#include "decoder.h"
#include "queues.h"
#include "state_cache.h"
#include "mqtt_task.h"
#include "arbiter.h"
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
                ESP_LOGD(TAG, "bms %u: pack=%dmV I=%dmA soc=%d%% c1=%dmV "
                         "c9=%dmV r3=%ld warn=0x%08lx", it.bms_id, (int)ci.pack_mv,
                         (int)ci.current_ma, ci.soc_pct, ci.cells[0].mv, ci.cells[8].mv,
                         (long)ci.cells[2].r_mohm, (unsigned long)ci.wire_warn_bitmask);
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
        case JK_REC_DEVICE_INFO: {
            /* Stream arming (fw 19.31): the 0x02 stream starts only when 0x96
             * is received AFTER the 0x97 exchange completes. Sending the pair
             * blind back-to-back races the BMS's 0x01/0x03 reply burst and the
             * 0x96 can be swallowed (bank 3 never armed; bank 1 was timing
             * luck). So: whenever a device-info record lands and this unit has
             * no cell data yet, follow up with CELL_INFO now — strictly after
             * the 0x03. Self-limiting: stops once cells decode. */
            bms_state_t st;
            if (state_snapshot(it.bms_id, &st) && !st.have_cells)
                arbiter_poll(it.bms_id, JK_CMD_CELL_INFO);
            break;
        }
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
