#include <string.h>
#include <stdlib.h>
#include "measure.h"
#include "config.h"
#include "nvs_store.h"
#include "state_cache.h"
#include "arbiter.h"
#include "mqtt_task.h"
#include "jk_proto.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "measure";
static volatile bool    s_active;
static volatile uint8_t s_bms;
static volatile int64_t s_start_us;

bool measure_active(uint8_t *out) { if (out) *out = s_bms; return s_active; }

/* Restore saved raw settings to the BMS. GATED: with writes disabled this is a
 * logged no-op — safe, because with writes disabled the floor was never
 * lowered either, so there is nothing to undo. */
static void restore_settings(uint8_t bms_id, const uint8_t *raw, uint16_t len)
{
#if JK_ENABLE_MEASURE_WRITES
    /* PORT ME (O-2): push `raw` back as a settings write via the arbiter. */
    (void)raw; (void)len;
    ESP_LOGW(TAG, "restore bms %u settings (%u bytes)", bms_id, len);
#else
    (void)raw; (void)len;
    ESP_LOGI(TAG, "restore bms %u: writes disabled, nothing to undo", bms_id);
#endif
}

void measure_check_boot(void)
{
    meas_record_t rec;
    if (!nvs_get_meas(&rec) || !rec.active) return;
    ESP_LOGW(TAG, "interrupted measurement found for bms %u — restoring", rec.bms_id);
    restore_settings(rec.bms_id, rec.saved, rec.saved_len);
    nvs_clear_meas();
    mqtt_ack(rec.bms_id, "measure", NULL, "measure_restored_after_reboot", NULL, NULL);
}

void measure_force_restore(uint8_t bms_id)
{
    meas_record_t rec;
    if (nvs_get_meas(&rec) && rec.active && rec.bms_id == bms_id) {
        ESP_LOGW(TAG, "MEAS_TIMEOUT hard restore bms %u", bms_id);
        restore_settings(bms_id, rec.saved, rec.saved_len);
        nvs_clear_meas();
        mqtt_ack(bms_id, "measure", NULL, "restore_failed_timeout", NULL, NULL);
    }
    s_active = false;
}

/* Sequence (spec §9). Steps 2/5 are writes → gated. */
static void run_sequence(uint8_t bms_id, const char *cid)
{
    bms_state_t st;
    if (!state_snapshot(bms_id, &st) || !st.have_settings) {
        mqtt_ack(bms_id, "measure", cid, "no_settings_cached", NULL, NULL);
        s_active = false; return;
    }

    /* Persist saved settings BEFORE altering anything (crash safety). */
    meas_record_t rec = { .active = true, .bms_id = bms_id,
                          .saved_len = st.settings.raw_len };
    memcpy(rec.saved, st.settings.raw, st.settings.raw_len);
    nvs_put_meas(&rec);

#if !JK_ENABLE_MEASURE_WRITES
    /* Measurement writes disabled: cannot lower the floor, so abort cleanly. The NVS record
     * we just wrote is cleared here; boot-restore covers the crash window. */
    nvs_clear_meas();
    mqtt_ack(bms_id, "measure", cid, "write_path_disabled", NULL, NULL);
    s_active = false;
    return;
#else /* JK_ENABLE_MEASURE_WRITES */
    /* 2. balance current -> floor, balancing enabled. (PORT ME O-2)          */
    /* 3. OPTIONAL cell-count re-init behind CFG_MEAS_CELLCOUNT_TOGGLE (O-4).  */
    vTaskDelay(pdMS_TO_TICKS(CFG_MEAS_SETTLE_MS));   /* 4. settle              */
    arbiter_poll(bms_id, JK_CMD_CELL_INFO);          /*    capture clean frame */
    /* 5. restore saved settings.                                             */
    restore_settings(bms_id, rec.saved, rec.saved_len);
    nvs_clear_meas();
    /* 6. publish state/meas with the SOC/balance-current/balancing conditions.*/
    mqtt_publish_meas(bms_id, "{\"note\":\"populate from captured frame\"}");
    mqtt_ack(bms_id, "measure", cid, "ok", NULL, NULL);
    s_active = false;
#endif
}

/* One-shot worker so the settle delay never blocks the arbiter. */
typedef struct { uint8_t bms_id; char cid[32]; } meas_arg_t;
static void worker(void *a)
{
    meas_arg_t *m = a;
    run_sequence(m->bms_id, m->cid);
    free(m);
    vTaskDelete(NULL);
}

void measure_start(uint8_t bms_id, const char *cid)
{
    if (s_active) { mqtt_ack(bms_id, "measure", cid, "busy", NULL, NULL); return; }
    s_active = true; s_bms = bms_id; s_start_us = esp_timer_get_time();
    meas_arg_t *m = calloc(1, sizeof(*m));
    m->bms_id = bms_id; if (cid) strlcpy(m->cid, cid, sizeof(m->cid));
    xTaskCreate(worker, "meas_worker", 4096, m, 4, NULL);
}

void measure_init(void) { s_active = false; }
