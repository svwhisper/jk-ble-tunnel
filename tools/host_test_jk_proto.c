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

    printf("\n%s (%d failure%s)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
