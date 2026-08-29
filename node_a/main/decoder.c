#include "decoder.h"
#include "config.h"
#include "queues.h"
#include "state_cache.h"
#include "mqtt_task.h"
#include "arbiter.h"
#include "jk_proto.h"
#include "esp_log.h"

static const char *TAG = "decoder";

/* One 0x6C stream-enable per BLE session, sent only AFTER the session's first
 * cell frame decodes. Supervisor resets the flag at link-up. */
static volatile bool s_stream_armed[CFG_NUM_UNITS];

void decoder_session_reset(uint8_t bms_id)
{ if (bms_id < CFG_NUM_UNITS) s_stream_armed[bms_id] = false; }

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
                /* 0x6C stream-enable, sequenced: sent adjacent to 97/96 at
                 * link-up it fails to take on some boots/banks (the module
                 * keeps its pre-6C habit of sleeping ~30 s after the last 96
                 * — the paired 0x208 cycle on banks 1+2, 2026-08-29). The
                 * app paces its trilogy against responses; do the same and
                 * hold the 6C until the session's first cell frame proves the
                 * module is awake and streaming. One beep per session, same
                 * as before, just later. */
                if (!s_stream_armed[it.bms_id]) {
                    s_stream_armed[it.bms_id] = true;
                    static const uint8_t stream_en[20] = {
                        0xAA,0x55,0x90,0xEB,0x6C,0x04,0xA5,0xD0,0x86,0x0C,
                        0xBD,0x7B,0x74,0xBE,0xF7,0x38,0x11,0x9D,0xE6,0x1E };
                    /* idx 1 = the FFE2 write-no-rsp path — the EXACT route the
                     * app's captured 6C took through the clone. On FFE1 (idx 0,
                     * write-with-rsp) the module C8-acks the 6C and ignores it:
                     * the session still dies ~30 s after bootstrap (rawcap
                     * 2026-08-29, bank 1: ack seen, 0x208 at +31 s). The
                     * stay-awake latch appears to arm only for FFE2 commands. */
                    arbiter_app_write(it.bms_id, 1, false, stream_en, sizeof(stream_en));
                    ESP_LOGI(TAG, "bms %u: first cell frame — sending 6C via FFE2", it.bms_id);
                }
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
