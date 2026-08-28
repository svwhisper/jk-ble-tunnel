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

    printf("\n%s (%d failure%s)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
