#include <string.h>
#include "supervisor.h"
#include "config.h"
#include "net_util.h"
#include "queues.h"
#include "state_cache.h"
#include "arbiter.h"
#include "ble_owner.h"
#include "measure.h"
#include "mqtt_task.h"
#include "tunnel_srv.h"
#include "nvs_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
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
                ESP_LOGE(TAG, "bms %u layout MISMATCH — excluding identity", id);
                pub_bridge_alert(id, "layout_mismatch");
                continue;
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
    bool ble_quiesce = net_wifi_down_ms() > CFG_WIFI_QUIESCE_MS;

    /* Round-robin one reachable-idle unit per tick for fresh telemetry.
     * BENCH 2026-08-28 (fw 19.31): the 0x02 cell-info stream only starts after
     * the BMS has seen DEVICE_INFO (0x97) FOLLOWED BY CELL_INFO (0x96) on the
     * session — the esphome-jk-bms bootstrap order. Either alone elicits
     * 0x01/0x03 replies but never a 0x02 (both single-opcode variants were
     * tried and failed). The bootstrap pair is sent on every link-up below;
     * this round-robin 0x96 is the keep-alive. */
    if (!ble_quiesce)
        for (uint8_t k = 0; k < CFG_NUM_UNITS; k++) {
            uint8_t id = (s_rr + k) % CFG_NUM_UNITS;
            bms_runtime_t rt; state_get_runtime(id, &rt);
            /* Skip units an app holds: the BMS streams on its own once
             * bootstrapped, and our polls would inject C8 acks the app never
             * requested into its (now transparent, TUN_RAW) stream. */
            if (rt.app_connected) continue;
            if (cfg_name_for(id)[0] == '\0') continue;   /* parked unit */
            if (rt.link != LINK_UNREACHABLE) { arbiter_poll(id, JK_CMD_CELL_INFO); s_rr = id + 1; break; }
        }

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
                arbiter_poll(id, JK_CMD_DEVICE_INFO);
                arbiter_poll(id, JK_CMD_CELL_INFO);
            }
            s_last_link[id] = rt.link;
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

        /* Publish link state (retained). */
        mqtt_publish_link(id);
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
    for (;;) {
        esp_task_wdt_reset();
        harvest_tick();      /* idempotent: skips units already in NVS */
        maintenance_tick();
        { static int hb; if (++hb >= 15) { hb = 0; mqtt_publish_health(); } }
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
