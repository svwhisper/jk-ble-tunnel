#include <string.h>
#include "supervisor.h"
#include "config.h"
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
        if (nvs_get_harvest(id, &h) && h.valid) continue;  /* already have it */
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
            nvs_put_harvest(id, &live);
        } else {
            /* Not harvested yet: attempt a connect (also the reachability probe). */
            arbiter_poll(id, JK_CMD_DEVICE_INFO);
        }
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

    /* Round-robin one reachable-idle unit per tick for fresh telemetry. */
    for (uint8_t k = 0; k < CFG_NUM_UNITS; k++) {
        uint8_t id = (s_rr + k) % CFG_NUM_UNITS;
        bms_runtime_t rt; state_get_runtime(id, &rt);
        if (rt.link != LINK_UNREACHABLE) { arbiter_poll(id, JK_CMD_CELL_INFO); s_rr = id + 1; break; }
    }

    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        bms_runtime_t rt; state_get_runtime(id, &rt);

        /* Reachability-probe floor for unreachable units (spec §4). */
        if (rt.link == LINK_UNREACHABLE &&
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
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void supervisor_start(void)
{
    memset(s_last_probe_us, 0, sizeof(s_last_probe_us));
    xTaskCreatePinnedToCore(supervisor_task, "supervisor", 6144, NULL, 3, NULL, 1);
}
