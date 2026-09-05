/* Byte-exact stream regression, independent of ESP-IDF and BLE hardware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jk_proto.h"
#include "synth_frames.h"

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct {
    jk_reasm_t reasm;
    const uint8_t *expected;
    size_t expected_count;
    size_t count;
} stream_t;

static void init(stream_t *s, const uint8_t *expected, size_t count)
{
    memset(s, 0, sizeof(*s));
    s->expected = expected;
    s->expected_count = count;
    jk_reasm_init(&s->reasm, JK_FRAME_JK02_32S);
}

static void feed(stream_t *s, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        uint16_t flen = 0;
        size_t consumed;
        const uint8_t *frame = jk_reasm_push(&s->reasm, data + off, len - off,
                                            &flen, &consumed);
        CHECK(consumed > 0 && consumed <= len - off);
        off += consumed;
        if (!frame) { CHECK(flen == 0); continue; }
        CHECK(s->count < s->expected_count);
        CHECK(flen == 300);
        CHECK(memcmp(frame, s->expected + s->count * 300, 300) == 0);
        s->count++;
    }
}

int main(void)
{
    uint8_t frames[900];
    CHECK(synth_cell_info(frames, 300, 0) == 300);
    CHECK(synth_settings(frames + 300, 300) == 300);
    CHECK(synth_device_info(frames + 600, 300, "BMS test") == 300);
    stream_t s;
    init(&s, frames, 3);
    size_t consumed = 99; uint16_t flen = 99;
    CHECK(jk_reasm_push(&s.reasm, NULL, 0, &flen, &consumed) == NULL);
    CHECK(consumed == 0 && flen == 0);

    /* Original failure: continuous 128-byte notifications across records. */
    init(&s, frames, 3);
    for (size_t off = 0; off < sizeof(frames); off += 128) {
        size_t n = sizeof(frames) - off;
        feed(&s, frames + off, n < 128 ? n : 128);
    }
    CHECK(s.count == 3);

    /* All fixed chunk sizes, including one byte and several records/call. */
    for (size_t chunk = 1; chunk <= sizeof(frames); chunk++) {
        init(&s, frames, 3);
        for (size_t off = 0; off < sizeof(frames); off += chunk) {
            size_t n = sizeof(frames) - off;
            feed(&s, frames + off, n < chunk ? n : chunk);
        }
        CHECK(s.count == 3);
        CHECK(s.reasm.have == 0 && s.reasm.want == 0);
    }

    /* Every two-chunk split point, including within magic/checksum bytes. */
    for (size_t split = 0; split <= sizeof(frames); split++) {
        init(&s, frames, 3);
        feed(&s, frames, split);
        feed(&s, frames + split, sizeof(frames) - split);
        CHECK(s.count == 3);
    }

    /* Empty input must not clear an in-progress prefix/frame. */
    init(&s, frames, 3);
    feed(&s, frames, 303);  /* one frame plus three bytes of the next magic */
    CHECK(s.count == 1 && s.reasm.have == 3);
    CHECK(jk_reasm_push(&s.reasm, NULL, 0, &flen, &consumed) == NULL);
    CHECK(consumed == 0 && s.reasm.have == 3);
    feed(&s, frames + 303, sizeof(frames) - 303);
    CHECK(s.count == 3);

    /* Noise/partial magic, corrupted frame, then three valid frames; bad
     * checksum must not suppress the good record later in the same input. */
    uint8_t noisy[1207] = {0x55, 0x55, 0xaa, 0x01, 0xeb, 0x00, 0x90};
    memcpy(noisy + 7, frames, 300);
    noisy[7 + 299] ^= 1;
    memcpy(noisy + 307, frames, sizeof(frames));
    for (size_t chunk = 1; chunk <= sizeof(noisy); chunk++) {
        init(&s, frames, 3);
        for (size_t off = 0; off < sizeof(noisy); off += chunk) {
            size_t n = sizeof(noisy) - off;
            feed(&s, noisy + off, n < chunk ? n : chunk);
        }
        CHECK(s.count == 3);
    }

    /* A link reset discards partial state but accepts the next full stream. */
    init(&s, frames, 3);
    feed(&s, frames, 149);
    jk_reasm_reset(&s.reasm);
    feed(&s, frames, sizeof(frames));
    CHECK(s.count == 3);
    printf("PASS: %u byte-exact reassembly assertions\n", checks);
    return 0;
}
