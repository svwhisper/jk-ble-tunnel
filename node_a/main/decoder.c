#include <time.h>
#include "decoder.h"
#include "config.h"
#include "queues.h"
#include "state_cache.h"
#include "mqtt_task.h"
#include "arbiter.h"
#include "jk_proto.h"
#include "esp_log.h"

static const char *TAG = "decoder";

/* One 0x6C clock-set per BLE session (it also arms the module's stay-awake —
 * see the block below), sent only AFTER the session's first cell frame
 * decodes. Supervisor resets the flag at link-up. */
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
                    /* 0x6C DECODED (2026-08-29 TUN capture experiment): it is
                     * SET-RTC — reg 6C, u32 LE seconds since 2020-01-01 00:00
                     * AEDT (the app's epoch; offset measured against its own
                     * frames), 9 don't-care bytes, 8-bit-sum checksum. The
                     * old replayed constant carried a STALE morning timestamp:
                     * banks 1/3 rejected it and slept ~25 s post-96 (their
                     * whole "churn"), and every link-up stomped their RTCs.
                     * A FRESH clock (the app's opener always works) doubles
                     * as an NTP-grade RTC sync. Skip entirely until SNTP has
                     * real time — never write a garbage clock. */
                    time_t now = time(NULL);
                    if (now > 1700000000) {
                        /* Full app-style opener trilogy, ALL on FFE2 (idx 1,
                         * write-no-rsp): the app sends 97+96+6C down FFE2 and
                         * its sessions latch stay-awake; our FFE1-path 97/96
                         * polls demonstrably don't (and a 6C on FFE1 is
                         * C8-acked and ignored). The 97/96 value bytes are
                         * buffer garbage in the app's frames — zeros here. */
                        uint32_t jk = (uint32_t)(now - 1577797125);
                        static const uint8_t cmds[3][2] = {
                            {0x97,0x00}, {0x96,0x00}, {0x6C,0x04} };
                        for (int c = 0; c < 3; c++) {
                            uint8_t f[20] = { 0xAA,0x55,0x90,0xEB,
                                              cmds[c][0], cmds[c][1] };
                            if (cmds[c][0] == 0x6C) {
                                f[6]=(uint8_t)jk;       f[7]=(uint8_t)(jk>>8);
                                f[8]=(uint8_t)(jk>>16); f[9]=(uint8_t)(jk>>24);
                            }
                            uint8_t sum = 0;
                            for (int i = 0; i < 19; i++) sum += f[i];
                            f[19] = sum;
                            arbiter_app_write(it.bms_id, 1, false, f, sizeof(f));
                        }
                        ESP_LOGI(TAG, "bms %u: first cell frame — FFE2 trilogy + clock (jk=%lu)",
                                 it.bms_id, (unsigned long)jk);
                    } else {
                        s_stream_armed[it.bms_id] = false;  /* retry next frame */
                    }
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
