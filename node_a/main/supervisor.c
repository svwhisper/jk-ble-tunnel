#include <string.h>
#include "supervisor.h"
#include "config.h"
#include "net_util.h"
#include "queues.h"
#include "state_cache.h"
#include "arbiter.h"
#include "ble_owner.h"
#include "decoder.h"
#include "measure.h"
#include "mqtt_task.h"
#include "tunnel_srv.h"
#include "nvs_store.h"
#include "ota.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "supervisor";
static int64_t s_last_probe_us[CFG_NUM_UNITS];
static int64_t s_meas_start_us;

static void pub_bridge_alert(uint8_t id, const char *what);

static tunnel_link_state_t s_last_link[CFG_NUM_UNITS];  /* change detection */

/* The name Node A scans a unit by IS the name Node B clones (spec §5). */
static const char *cfg_name_for(uint8_t id)
{
    for (int i = 0; i < CFG_NUM_UNITS; i++)
        if (CFG_BMS[i].bms_id == id)
            return CFG_BMS[i].name ? CFG_BMS[i].name : "";   /* NULL = parked */
    return "";
}

/* ---- boot harvest coordinator (spec §4) -------------------------------- */
/* Incremental: serve NVS immediately; harvest a unit the first time it is
 * reachable and verify its layout against the first stored table. Here we only
 * kick a connect per not-yet-harvested unit; ble_owner fills the table and the
 * arbiter's connect path brings it up. Full device-info name/version capture is
 * completed once decode offsets are bench-verified (O-1). */
static void harvest_tick(void)
{
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        harvest_entry_t h;
        if (nvs_get_harvest(id, &h) && h.valid) {
            if (h.name[0] == '\0') {   /* legacy/nameless entry — fix + re-announce */
                strlcpy(h.name, cfg_name_for(id), sizeof(h.name));
                nvs_put_harvest(id, &h);
                tunnel_srv_announce();
            }
            continue;
        }
        harvest_entry_t live;
        if (ble_owner_copy_table(id, &live)) {
            /* Layout identity check vs the first stored table (spec §5/§O-9). */
            harvest_entry_t first; bool have_ref = false;
            for (uint8_t j = 0; j < CFG_NUM_UNITS; j++)
                if (nvs_get_harvest(j, &first) && first.valid) { have_ref = true; break; }
            if (have_ref && (first.char_count != live.char_count ||
                 memcmp(first.chars, live.chars, live.char_count * sizeof(live.chars[0])))) {
                /* Warn-and-ACCEPT (was exclude): Node B's clone table has been
                 * STATIC since the GATT mirror landed, so per-unit property
                 * differences are harmless — and unit 0's Telink module was
                 * being excluded every second for exactly this (2026-08-29).
                 * The phone app works identically on all four units. */
                ESP_LOGW(TAG, "bms %u layout differs from ref — accepting anyway", id);
                pub_bridge_alert(id, "layout_differs_accepted");
            }
            live.valid = true;
            strlcpy(live.name, cfg_name_for(id), sizeof(live.name));
            nvs_put_harvest(id, &live);
            /* Push TABLE+IDENT+LINK to Node B now so it can advertise this
             * identity without waiting for a tunnel reconnect. */
            tunnel_srv_announce();
        }
        /* If not yet harvested, do NOT poll here every tick — the round-robin
         * and the reachability probe (maintenance_tick) drive connects at a
         * sane rate; harvest just copies the table opportunistically once a
         * unit's link has come up. Polling here floods the arbiter. */
    }
}

static void pub_bridge_alert(uint8_t id, const char *what)
{
    mqtt_ack(id, "harvest", NULL, what, NULL, NULL);
}

/* ---- per-tick maintenance ---------------------------------------------- */
static uint8_t s_rr;   /* round-robin cursor for NR polling */

static void maintenance_tick(void)
{
    int64_t now = esp_timer_get_time();

    /* Quiesce ALL BLE initiation while WiFi is re-associating (shared radio;
     * see net_wifi_down_ms). Links already up are left alone — scans are the
     * radio hogs, and everything below only creates new radio work. */
    bool ble_quiesce = net_wifi_down_ms() > CFG_WIFI_QUIESCE_MS ||
                       !ble_owner_ble_enabled();

    /* NO keep-alive polling. THE BMS BEEPS ON EVERY COMMAND IT RECEIVES
     * (piezo command-ack — the day-long chirp mystery, confirmed by the
     * esphome-jk-bms community + live ears 2026-08-29). Once armed, the 0x02
     * stream flows on its own and IS the health signal; a streaming unit must
     * hear NOTHING from us. Commands are sent only to arm/re-arm (below) and
     * for app traffic. */

    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        bms_runtime_t rt; state_get_runtime(id, &rt);

        /* Push reachability changes to Node B so it starts/stops advertising
         * this identity (spec §4/§5). */
        if (rt.link != s_last_link[id]) {
            tunnel_send_link(id, rt.link);
            /* Session bootstrap on every link-up: 0x97 then 0x96 — the pair
             * (in this order) is what makes fw 19.31 start streaming 0x02
             * cell frames; see the round-robin note above. */
            if (rt.link == LINK_UP) {
                arbiter_clear_pending(id);   /* flush stale polls first, so the
                                              * bootstrap dispatches adjacent */
                arbiter_poll(id, JK_CMD_DEVICE_INFO);
                arbiter_poll(id, JK_CMD_CELL_INFO);
                /* 0x6C stream-enable (the app's third bootstrap command,
                 * captured 2026-08-29): WITHOUT it a module streams ~25 s
                 * after the last 96 and sleeps. But sent here, adjacent to
                 * 97/96, it fails to take on some boots/banks (paired-0x208
                 * churn). The decoder sends it, sequenced after the session's
                 * first cell frame — this just re-arms the one-shot. */
                decoder_session_reset(id);
            }
            s_last_link[id] = rt.link;
        }

        /* CONNECT DRIVER: a unit that is enabled but not connected gets a
         * pending poll every 20 s — dispatch sees the link down and runs the
         * connect-on-demand path. Costs no beeps: arbiter_clear_pending at
         * link-up flushes this pending before it can reach the BMS. (The old
         * round-robin keep-alive doubled as this driver; when it was removed
         * for beep silence, nothing initiated connections at all.) */
        if (!ble_quiesce && cfg_name_for(id)[0] != '\0' && !rt.link_held) {
            static int64_t s_conn_drive_us[CFG_NUM_UNITS];
            if (now - s_conn_drive_us[id] > 20000000LL) {
                s_conn_drive_us[id] = now;
                arbiter_poll(id, JK_CMD_DEVICE_INFO);
            }
        }

        /* Held link whose stream never armed OR went stale (>30 s since the
         * last good frame): re-send the 0x97 opener every 30 s WITHOUT
         * touching the link (the decoder's sequenced 0x96 completes the pair
         * when the 0x03 lands). Cost: <=2 command-ack beeps per 30 s for a
         * struggling unit, ZERO for a streaming one. */
        if (!ble_quiesce) {
            static int64_t s_rearm_us[CFG_NUM_UNITS];
            bool stale = rt.link_held &&
                         (now - rt.last_seen_us) > 30000000LL;
            bms_state_t bs;
            bool unarmed = rt.link_held && state_snapshot(id, &bs) && !bs.have_cells;
            /* 10 min between re-arm attempts: each costs ~2 command-ack beeps
             * at the unit, so a chronically unarmed bank (bank 3) stays
             * near-silent ambient. Investigation uses bounded rawcap windows. */
            if ((unarmed || stale) && now - s_rearm_us[id] > 600000000LL) {
                s_rearm_us[id] = now;
                arbiter_poll(id, JK_CMD_DEVICE_INFO);
            }
        }

        /* Reachability-probe floor for unreachable units (spec §4). */
        if (!ble_quiesce && cfg_name_for(id)[0] != '\0' &&
            rt.link == LINK_UNREACHABLE &&
            now - s_last_probe_us[id] > CFG_REACHABILITY_PROBE_S * 1000000LL) {
            s_last_probe_us[id] = now;
            arbiter_poll(id, JK_CMD_DEVICE_INFO);
        }

        /* Idle-disconnect: app gone, nothing pending, held past the timeout. */
        if (rt.link_held && !rt.app_connected && rt.app_left_us &&
            now - rt.app_left_us > CFG_IDLE_DISCONNECT_MS * 1000LL) {
            bms_request_t d = { .bms_id = id, .kind = TXN_DISCONNECT,
                                .source = SRC_INTERNAL };
            arbiter_submit(&d);
            rt.app_left_us = 0; state_set_runtime(id, &rt);
        }

        /* Publish link state (retained) on CHANGE plus a slow refresh — NOT
         * every tick: 4 QoS-1 publishes/s was pure outbox fuel whenever the
         * broker session degraded. (Reconnect re-announces all units.) */
        {
            static uint8_t s_pub_tick[CFG_NUM_UNITS];
            bool changed = (rt.link != s_last_link[id]);
            if (changed || ++s_pub_tick[id] >= 30) {
                s_pub_tick[id] = 0;
                mqtt_publish_link(id);
            }
        }
    }

    /* WiFi-zombie breaker: a dead association with no disconnect event keeps
     * net_wifi_up() true forever, so the quiesce never arms and BLE starves
     * the reassociation (observed 16:24: MQTT died, scans kept running). If
     * MQTT has been down >30 s while WiFi claims up, force a disconnect —
     * that fires the event machinery, starts down_ms, arms the quiesce, and
     * lets WiFi reassociate cleanly. At most once per minute. */
    {
        static int64_t s_zombie_kick_us;
        static int64_t s_mqtt_down_since_us;
        bool mqtt_up = (xEventGroupGetBits(g_evt) & EVT_MQTT_UP) != 0;
        if (mqtt_up) s_mqtt_down_since_us = 0;
        else if (!s_mqtt_down_since_us) s_mqtt_down_since_us = now;
        if (!mqtt_up && net_wifi_up() && s_mqtt_down_since_us &&
            now - s_mqtt_down_since_us > 30000000LL &&
            now - s_zombie_kick_us > 60000000LL) {
            s_zombie_kick_us = now;
            ESP_LOGW(TAG, "MQTT down 30s with WiFi 'up' — kicking zombie association");
            esp_wifi_disconnect();
        }
    }

    /* Measurement hard-timeout (spec §9). */
    uint8_t mb;
    if (measure_active(&mb)) {
        if (s_meas_start_us == 0) s_meas_start_us = now;
        if (now - s_meas_start_us > CFG_MEAS_TIMEOUT_MS * 1000LL)
            measure_force_restore(mb);
    } else s_meas_start_us = 0;
}

static void supervisor_task(void *arg)
{
    esp_task_wdt_add(NULL);
    /* Restore the operator's persisted BLE choice (default OFF). Replaced the
     * 30 s auto-arm 2026-08-29: OTA no longer needs a radio-quiet window, and
     * silent-until-told means a power blip can't chirp/crash the fleet. */
    if (nvs_get_ble_enabled()) {
        ESP_LOGW(TAG, "restoring persisted BLE state: ON");
        ble_owner_set_ble(true);
    }
    for (;;) {
        esp_task_wdt_reset();
        harvest_tick();      /* idempotent: skips units already in NVS */
        maintenance_tick();
        { static int ka; if (++ka >= 15) { ka = 0;
              if (ble_owner_ble_enabled()) ble_owner_keepalive_read(); } }
        { static int hb; if (++hb >= 15) { hb = 0; mqtt_publish_health(); } }
        /* IDENT+LINK refresh to Node B: repairs any tunnel frame lost to a
         * full out-queue, and rebuilds B's RAM-only adv state after B reboots
         * (B's one-shot TABLE_REQ resync was the only writer before, and a
         * lost frame stayed lost forever — 2026-08-29 "TUN 0+2 only"). */
        { static int rf; if (++rf >= 30) { rf = 0;
              if (tunnel_is_up()) tunnel_srv_refresh(); } }
        /* OTA receiver self-heal: httpd_start can lose the boot-time resource
         * race (observed failing at second ~5 on every boot of one image,
         * leaving the node un-updatable). ota_start is idempotent. */
        { static int oa; if (++oa >= 30) { oa = 0;
              if (!ota_is_up() && net_wifi_up()) ota_start(CFG_OTA_PORT); } }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void supervisor_start(void)
{
    memset(s_last_probe_us, 0, sizeof(s_last_probe_us));
    memset(s_last_link, 0xFF, sizeof(s_last_link));  /* force first push */
    /* Priority ABOVE the workers (ble_owner 7 / arbiter / decoder): the
     * supervisor's tick is tiny and bounded, but it feeds the task WDT — at
     * prio 3 it starved for 15 s whenever marginal-link churn + streaming
     * saturated the single core (repeated task-WDT panics, 2026-08-28). At 8
     * it always preempts, ticks in ms, and sleeps. */
    xTaskCreatePinnedToCore(supervisor_task, "supervisor", 6144, NULL, 8, NULL, tskNO_AFFINITY);
}
