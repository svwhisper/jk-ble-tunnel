#include "decoder.h"
#include "queues.h"
#include "state_cache.h"
#include "mqtt_task.h"
#include "jk_proto.h"
#include "esp_log.h"

static const char *TAG = "decoder";

static void decoder_task(void *arg)
{
    notify_item_t it;
    for (;;) {
        if (xQueueReceive(g_q_decode, &it, portMAX_DELAY) != pdTRUE) continue;

        jk_record_t rec = jk_frame_record(it.data, it.len);
        jk_frame_ver_t ver = JK_FRAME_JK02_32S;   /* per-unit ver once O-1 pins it */

        switch (rec) {
        case JK_REC_CELL_INFO: {
            jk_cell_info_t ci;
            if (jk_decode_cell_info(ver, it.data, it.len, &ci) == 0) {
                state_set_cells(it.bms_id, &ci);
                mqtt_publish_cells(it.bms_id);
                mqtt_publish_summary(it.bms_id);
                mqtt_publish_faults(it.bms_id);
            }
            break;
        }
        case JK_REC_SETTINGS: {
            jk_settings_t s;
            if (jk_decode_settings(ver, it.data, it.len, &s) == 0) {
                state_set_settings(it.bms_id, &s);
                mqtt_publish_settings(it.bms_id);   /* retained (spec §7) */
            }
            break;
        }
        case JK_REC_DEVICE_INFO:
            /* harvest coordinator reads device info directly; nothing to cache
             * on the hot path. */
            break;
        default:
            ESP_LOGD(TAG, "bms %u unknown record", it.bms_id);
            break;
        }
    }
}

void decoder_start(void)
{
    xTaskCreatePinnedToCore(decoder_task, "decoder", 6144, NULL, 4, NULL, tskNO_AFFINITY);
}
