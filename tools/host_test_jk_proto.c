/*
 * host_test_jk_proto.c — native round-trip test for the pure-logic core
 * (framing, reassembly, checksum, decode). No ESP-IDF needed. This is the one
 * part that is fully testable off-hardware, so it has a real test.
 *
 * Build & run on a Mac:
 *   cc -I components/common/include -I components/jk_proto/include \
 *      -I test_board/main tools/host_test_jk_proto.c \
 *      components/jk_proto/jk_proto.c test_board/main/synth_frames.c \
 *      -o /tmp/jkt && /tmp/jkt
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "jk_proto.h"
#include "synth_frames.h"

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
                              else printf("ok:   %s\n", msg); } while (0)

int main(void)
{
    /* 1. Build a synthetic cell-info frame and verify its checksum. */
    uint8_t frame[320];
    int flen = synth_cell_info(frame, sizeof(frame), 0);
    CHECK(flen == 300, "synth cell-info is 300 bytes");
    CHECK(jk_checksum_ok(frame, flen), "synth frame checksum validates");
    CHECK(jk_frame_record(frame, flen) == JK_REC_CELL_INFO, "record type = cell-info");

    /* 2. Feed it to the reassembler in awkward 17-byte chunks (like BLE
     *    notifications) and confirm exactly one complete frame comes out. */
    jk_reasm_t r; jk_reasm_init(&r, JK_FRAME_JK02_32S);
    const uint8_t *got = NULL; uint16_t got_len = 0;
    for (int off = 0; off < flen; off += 17) {
        int n = (flen - off < 17) ? (flen - off) : 17;
        uint16_t out_len;
        const uint8_t *f = jk_reasm_push(&r, frame + off, n, &out_len);
        if (f) { got = f; got_len = out_len; }
    }
    CHECK(got != NULL && got_len == 300, "reassembly across 17-byte chunks");

    /* 3. Prepend garbage so the magic-hunt/resync path is exercised. */
    jk_reasm_init(&r, JK_FRAME_JK02_32S);
    uint8_t junk[5] = {0x00, 0x11, 0x55, 0x22, 0x33};
    jk_reasm_push(&r, junk, sizeof(junk), NULL);
    got = NULL;
    for (int off = 0; off < flen; off += 40) {
        int n = (flen - off < 40) ? (flen - off) : 40;
        uint16_t out_len;
        const uint8_t *f = jk_reasm_push(&r, frame + off, n, &out_len);
        if (f) { got = f; got_len = out_len; }
    }
    CHECK(got != NULL && got_len == 300, "resync after leading garbage");

    /* 4. Decode and spot-check the values synth_frames put in. */
    jk_cell_info_t ci;
    int rc = jk_decode_cell_info(JK_FRAME_JK02_32S, frame, flen, &ci);
    CHECK(rc == 0, "decode_cell_info succeeds");
    CHECK(ci.cells[0].mv >= 3295 && ci.cells[0].mv <= 3305, "cell 1 ~3.30 V");
    CHECK(ci.cells[8].mv < ci.cells[7].mv, "cell 9 reads low (imbalance signal)");
    /* spec §9: synth set cell 3's resistance to 0 -> must decode to null (<0). */
    CHECK(ci.cells[2].r_mohm < 0, "failed resistance (0) decodes to null");
    CHECK(ci.cells[0].r_mohm > 0, "good resistance decodes to a value");
    CHECK(ci.soc_pct == 74, "SOC decodes to 74%");
    CHECK(ci.cycle_count == 128, "cycle count decodes to 128");

    /* 5. A corrupted checksum must be rejected. */
    frame[10] ^= 0xFF;
    CHECK(!jk_checksum_ok(frame, flen), "corrupted frame rejected by checksum");

    /* 6. REAL capture regression: a complete 0x02 frame captured live from
     * JK-PB2A16S20P "BMS 1-01" (fw 19.31) on 2026-08-28, with values verified
     * against the unit and the .90 aggregator at capture time. If this decode
     * ever drifts, the offsets are wrong — not the frame. */
    {
        static const char *HX =
            "55AAEB9002230F0D100D0F0D100D100D110D110D130D110D100D110D130D130D"
            "110D100D130D0000000000000000000000000000000000000000000000000000"
            "000000000000FFFF0000110D05000700480055004800510045005F0051005B00"
            "4F00530046005400460053004600540000000000000000000000000000000000"
            "00000000000000000000000000000000AC000000000017D10000CF1B00007BFF"
            "FFFFA000A4000000000000000062EAB3040090CA040000000000674F03006400"
            "000046DBEC0101010000000000000000000000000000FF000100000010270000"
            "010071A03F4000000000E8140000000101010006000013BBF20400000000AC00"
            "9B009D00000056B3850C250000008051010000000102000000000000000000FE"
            "FF7FDD2F0121B00F070000DC";
        uint8_t rf[300]; int n = 0;
        for (const char *p = HX; p[0] && p[1] && n < 300; p += 2) {
            unsigned b; sscanf(p, "%2X", &b); rf[n++] = (uint8_t)b;
        }
        CHECK(n == 300, "real capture is 300 bytes");
        CHECK(jk_checksum_ok(rf, n), "real capture checksum validates");
        jk_cell_info_t rc2;
        CHECK(jk_decode_cell_info(JK_FRAME_JK02_32S, rf, n, &rc2) == 0,
              "real capture decodes");
        CHECK(rc2.cell_count == 16, "real: 16 cells from mask");
        CHECK(rc2.cells[0].mv == 3343, "real: cell 1 = 3343 mV");
        CHECK(rc2.cells[7].mv == 3347, "real: cell 8 = 3347 mV");
        CHECK(rc2.pack_mv == 53527, "real: pack = 53.527 V");
        CHECK(rc2.current_ma == -133, "real: current = -133 mA");
        CHECK(rc2.soc_pct == 98, "real: SOC = 98%");
        CHECK(rc2.cycle_count == 0, "real: cycles = 0");
        CHECK(rc2.temp_dc[0] == 160 && rc2.temp_dc[1] == 164,
              "real: T1/T2 = 16.0/16.4 C");
        CHECK(rc2.wire_warn_bitmask == 0, "real: no wire warnings");
        CHECK(rc2.cells[0].r_mohm == 72, "real: cell 1 wire = 72 mOhm");
    }

#if JK_ENABLE_WRITES
    /* Balance-write frame builder vs the official app's OWN captured frames
     * (JK-PB2A16S20P fw 19.31, 2026-08-30). The value+checksum bytes must
     * match byte-for-byte; the app's junk tail (bytes 10..18) is don't-care,
     * so compare only the header, register, value, and checksum. */
    {
        uint8_t f[20];
        /* trigger voltage 0.015 V -> reg 0x06, 0x0F mV. (The app's captured
         * frame carried a junk tail summing to checksum 0x94; ours zeros the
         * tail, so the self-consistent checksum is 0x93 — the BMS validates
         * the sum over whatever tail is sent, proven by our 0x6C set-RTC.) */
        CHECK(jk_build_balance_write(JK_FRAME_JK02_32S, "balance_trigger_voltage",
                                     0.015, f, sizeof(f)) == 20, "write: trig builds");
        CHECK(f[4]==0x06 && f[5]==0x04 && f[6]==0x0F && f[7]==0 && f[8]==0 && f[9]==0,
              "write: trig 0.015V -> reg06 val 15mV");
        {   uint8_t sum = 0; for (int i = 0; i < 19; i++) sum += f[i];
            CHECK(f[19] == sum, "write: trig checksum self-consistent"); }
        /* balance current 1.5 A -> reg 0x13, 0x05DC mA */
        CHECK(jk_build_balance_write(JK_FRAME_JK02_32S, "balance_current",
                                     1.5, f, sizeof(f)) == 20, "write: curr builds");
        CHECK(f[4]==0x13 && f[6]==0xDC && f[7]==0x05 && f[8]==0 && f[9]==0,
              "write: curr 1.5A -> reg13 val 1500mA");
        /* balancer enable = 1 -> reg 0x1F, value 1 */
        CHECK(jk_build_balance_write(JK_FRAME_JK02_32S, "balancing_enabled",
                                     1.0, f, sizeof(f)) == 20, "write: enable builds");
        CHECK(f[4]==0x1F && f[6]==0x01 && f[7]==0 && f[8]==0 && f[9]==0,
              "write: enable=1 -> reg1F val 1");
        /* unknown key rejected */
        CHECK(jk_build_balance_write(JK_FRAME_JK02_32S, "cell_ovp",
                                     3.65, f, sizeof(f)) < 0, "write: unknown key rejected");
        /* login is a no-op on this firmware (no auth on the wire) */
        CHECK(jk_build_login("123456", f, sizeof(f)) == 0, "write: login is no-op (0 bytes)");
    }
#endif

    printf("\n%s (%d failure%s)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
