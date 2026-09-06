/* Actual B replay/notify production functions. Simulated mbufs/host submission:
 * no radio, network, serial, NVS or battery commands. Established-stream
 * loss/continuation/replay behaviour remains unchanged by the startup guard. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define HOST_ESP_LOG_H
static char last_log[512];
static char last_dev_log[512];
static unsigned log_count;
static void test_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    va_list ap; va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap); log_count++;
    if (!strncmp(last_log, "diag dev ", 9))
        memcpy(last_dev_log, last_log, sizeof(last_dev_log));
}
#define ESP_LOGI test_log
#define ESP_LOGW test_log
#include "../node_b/main/ble_periph.c"

static nb_identity_t identity;
static uint16_t test_mtu;
static int alloc_fail, notify_fail;
static unsigned allocs, calls, notes, received_len, cases;
static uint8_t received[2048], pending_data[320];
static struct os_mbuf mbuf;
static int64_t now_us = 10000000;
static nb_cache_t warm[4];
static int replay_action;
static uint8_t replay_bits;
static uint8_t replay_order[3];
static unsigned replay_gets;
static int change_session_after_call = -1;

int64_t esp_timer_get_time(void) { return now_us; }
void nb_get_identity(uint8_t id, nb_identity_t *out)
{ assert(id < CFG_NUM_UNITS); *out = identity; }
void nb_get_notify_session(uint8_t id, nb_notify_session_t *out)
{
    assert(id < CFG_NUM_UNITS);
    *out = (nb_notify_session_t){.epoch = identity.notify_epoch,
        .conn_handle = identity.conn_handle, .connected = identity.connected,
        .notify_enabled = identity.notify_enabled};
}
void nb_note_dev_forwarded(uint8_t id, uint64_t epoch)
{ assert(id == 1); if (epoch == identity.notify_epoch) notes++; }
uint16_t ble_att_mtu(uint16_t h) { assert(h == 7); return test_mtu; }
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t len)
{
    unsigned attempt = allocs++;
    if ((int)attempt == alloc_fail) return NULL;
    assert(len <= 128); memcpy(pending_data, data, len); mbuf.len = len;
    return &mbuf;
}
int ble_gatts_notify_custom(uint16_t h, uint16_t attr, struct os_mbuf *om)
{
    assert(h == 7 && attr == 10 && om == &mbuf);
    assert(received_len + om->len <= sizeof(received));
    memcpy(received + received_len, pending_data, om->len);
    received_len += om->len;
    if ((int)calls == change_session_after_call) identity.notify_epoch += 2;
    return (int)calls++ == notify_fail ? 6 : 0;
}
int nb_replay_action(uint8_t id) { return id == 1 ? replay_action : 0; }
uint8_t nb_take_replay(uint8_t id)
{ assert(id == 1); uint8_t bits = replay_bits; replay_bits = 0; return bits; }
void nb_get_warm(uint8_t id, uint8_t rec, nb_cache_t *out)
{
    assert(id == 1 && rec >= 1 && rec <= 3 && replay_gets < 3);
    replay_order[replay_gets++] = rec; *out = warm[rec];
}
void nb_get_warm_dev(uint8_t id, int page, nb_cache_t *out)
{ assert(page == 0); nb_get_warm(id, 3, out); }

static void reset(void)
{
    memset(&identity, 0, sizeof(identity));
    identity.connected = identity.notify_enabled = true;
    identity.conn_handle = 7; s_val_handle = 10;
    identity.notify_epoch = 1;
    memset(s_live, 0, sizeof(s_live));
    s_live[1].epoch = 1; s_live[1].aligned = true;
    s_live[1].replay_safe = true;
    change_session_after_call = -1;
    test_mtu = 185; alloc_fail = notify_fail = -1;
    allocs = calls = notes = received_len = log_count = replay_gets = 0;
    last_log[0] = 0;
}

int main(void)
{
    /* A tunnel tick must not replay between two raw pieces of a live record.
     * Before Stage10b this inserted the300 cached bytes at offset100. */
    uint8_t live[300] = {0x55, 0xAA, 0xEB, 0x90, 2};
    uint8_t live_sum = 0;
    for (unsigned i = 0; i < 299; i++) live_sum += live[i];
    live[299] = live_sum;
    reset(); replay_action = 1; replay_bits = NB_REPLAY_DEVINFO;
    warm[3].len = 300; memcpy(warm[3].data, live, 300);
    warm[3].data[4] = 3; warm[3].data[299]++;
    ble_periph_forward_notify(1, 0, live, 100);
    ble_periph_replay_tick();
    assert(received_len == 100 && replay_bits == NB_REPLAY_DEVINFO);
    ble_periph_forward_notify(1, 0, live + 100, 200);
    assert(received_len == 300 && !memcmp(received, live, 300));
    ble_periph_replay_tick();
    assert(received_len == 600 && replay_bits == 0);
    puts("PASS: cached device-info deferred to complete live cell record boundary");
    static const uint16_t mtus[] = {0, 23, 64, 131, 185, 512};
    for (unsigned r = 1; r <= 3; r++) {
        warm[r].len = 300; memcpy(warm[r].data, live, 300);
        warm[r].data[4] = r; warm[r].data[299] += r - 2;
    }
    static const uint8_t debts[] = {1, 2, 4, 7};
    for (unsigned m = 0; m < sizeof(mtus)/sizeof(mtus[0]); m++)
    for (unsigned d = 0; d < sizeof(debts); d++)
    for (unsigned split = 1; split < 300; split++) {
        reset(); test_mtu = mtus[m]; replay_action = 1; replay_bits = debts[d];
        ble_periph_forward_notify(1, 0, live, split);
        for (unsigned tick = 0; tick < 3; tick++) ble_periph_replay_tick();
        assert(received_len == split && replay_bits == debts[d] && !replay_gets);
        ble_periph_forward_notify(1, 0, live + split, 300 - split);
        assert(s_live[1].replay_safe && received_len == 300);
        ble_periph_replay_tick();
        assert(replay_bits == 0 && !memcmp(received, live, 300));
        unsigned frames = debts[d] == 7 ? 3 : 1;
        assert(received_len == 300 * (frames + 1));
        for (unsigned r = 0; r < frames; r++) {
            const uint8_t *out = received + 300 * (r + 1);
            assert(!memcmp(out, "\x55\xAA\xEB\x90", 4));
            assert(out[4] == replay_order[r]);
            uint8_t sum = 0;
            for (unsigned k = 0; k < 299; k++) sum += out[k];
            assert(out[299] == sum);
        }
        cases++;
    }
    /* Same raw notification may contain a heartbeat prefix, full record,
     * and an auxiliary/partial suffix. None is filtered or re-chunked for
     * the replay observer; only an exact valid end opens replay. */
    uint8_t mixed[320];
    for (unsigned prefix = 0; prefix <= 10; prefix++)
    for (unsigned suffix = 1; suffix <= 10; suffix++) {
        reset(); replay_action = 1; replay_bits = 1;
        memset(mixed, 0x66, sizeof(mixed));
        if (prefix >= 4) memcpy(mixed, "AT\r\n", 4);
        memcpy(mixed + prefix, live, 300);
        if (suffix >= 5) memcpy(mixed + prefix + 300, "\xAA\x55\x90\xEB\xC8", 5);
        unsigned n = prefix + 300 + suffix;
        ble_periph_forward_notify(1, 0, mixed, n);
        ble_periph_replay_tick();
        assert(!s_live[1].replay_safe && replay_bits == 1 && received_len == n);
        assert(!memcmp(received, mixed, n));
        ble_periph_forward_notify(1, 0, live, 300);
        ble_periph_replay_tick();
        assert(replay_bits == 0 && received_len == n + 600);
        assert(!memcmp(received + n, live, 300)); cases++;
    }
    /* Silence, malformed checksum and incomplete AT/JK prefixes cannot
     * manufacture a safe end. A later valid record recovers the observer. */
    reset(); replay_action = 1; replay_bits = 1;
    memcpy(mixed, live, 300); mixed[299]++;
    ble_periph_forward_notify(1, 0, mixed, 300);
    for (unsigned tick = 0; tick < 100; tick++) ble_periph_replay_tick();
    assert(replay_bits == 1 && received_len == 300);
    ble_periph_forward_notify(1, 0, live, 300); ble_periph_replay_tick();
    assert(replay_bits == 0 && received_len == 900);

    /* Failed submission cannot certify a boundary, including failed first
     * startup frame. Once a whole subsequent record succeeds, replay can run. */
    for (unsigned startup = 0; startup < 2; startup++)
    for (unsigned fail = 0; fail < 3; fail++)
    for (unsigned oom = 0; oom < 2; oom++) {
        reset(); s_live[1].aligned = !startup;
        if (oom) alloc_fail = fail; else notify_fail = fail;
        replay_action = 1; replay_bits = 1;
        ble_periph_forward_notify(1, 0, live, 300);
        ble_periph_replay_tick();
        assert(!s_live[1].replay_safe && replay_bits == 1 && !replay_gets);
        received_len = 0; notify_fail = alloc_fail = -1;
        ble_periph_forward_notify(1, 0, live, 300); ble_periph_replay_tick();
        assert(received_len == 600 && replay_bits == 0); cases++;
    }
    /* Reused handle/new CCCD epoch must discard an old closed boundary;
     * replay before any new live submission is still allowed at startup. */
    reset(); replay_action = 1; replay_bits = 1;
    ble_periph_forward_notify(1, 0, live, 100);
    identity.notify_epoch += 2;
    ble_periph_replay_tick();
    assert(replay_bits == 0 && received_len == 400);
    assert(!s_live[1].aligned && !s_live[1].start.len && s_live[1].replay_safe);
    reset(); s_live[1].aligned = false; replay_action = 1; replay_bits = 1;
    ble_periph_forward_notify(1, 0, live, 100); ble_periph_replay_tick();
    assert(received_len == 300 && replay_bits == 0 && !s_live[1].aligned);
    ble_periph_forward_notify(1, 0, live + 100, 200);
    assert(received_len == 600 && !memcmp(received + 300, live, 300));
    /* One chunk can finish a previous record AND carry a whole next record.
     * Partial magic after a valid end must carry over without opening replay. */
    uint8_t double_live[600];
    memcpy(double_live, live, 300); memcpy(double_live + 300, live, 300);
    for (unsigned split = 280; split < 300; split++) {
        reset(); replay_action = 1; replay_bits = 1;
        ble_periph_forward_notify(1, 0, double_live, split);
        ble_periph_replay_tick(); assert(replay_bits == 1);
        ble_periph_forward_notify(1, 0, double_live + split, 600 - split);
        ble_periph_replay_tick();
        assert(received_len == 900 && !memcmp(received, double_live, 600)); cases++;
    }
    for (unsigned prefix = 1; prefix <= 5; prefix++) {
        reset(); replay_action = 1; replay_bits = 1;
        ble_periph_forward_notify(1, 0, double_live, 300 + prefix);
        ble_periph_replay_tick(); assert(replay_bits == 1);
        ble_periph_forward_notify(1, 0, double_live + 300 + prefix, 300 - prefix);
        ble_periph_replay_tick();
        assert(received_len == 900 && !memcmp(received, double_live, 600)); cases++;
    }
    puts("PASS: all replay debts/MTUs/splits, auxiliary suffixes, errors and epoch boundary resets");
    uint8_t data[320] = {0x55, 0xAA, 0xEB, 0x90, 0x03};
    for (unsigned i = 5; i < sizeof(data); i++) data[i] = (uint8_t)i;
    for (unsigned m = 0; m < sizeof(mtus)/sizeof(mtus[0]); m++)
        for (unsigned len = 0; len <= sizeof(data); len++)
            for (int fail = -1; fail <= 16; fail++) {
                reset(); test_mtu = mtus[m]; alloc_fail = fail; notify_fail = 1;
                ble_periph_forward_notify(1, 0, data, len);
                unsigned chunk = (test_mtu < 23 ? 23 : test_mtu) - 3;
                if (chunk > 128) chunk = 128;
                unsigned want_calls = (len + chunk - 1) / chunk;
                bool oom = fail >= 0 && (unsigned)fail < want_calls;
                if (oom) want_calls = (unsigned)fail;
                unsigned want_len = want_calls * chunk;
                if (want_len > len) want_len = len;
                assert(calls == want_calls && allocs == want_calls + oom);
                assert(received_len == want_len && !memcmp(received, data, want_len));
                /* Existing early dev_seen remains even if allocation/notify fails. */
                assert(notes == (len >= 5));
                cases++;
            }
    for (unsigned c = 0; c < 2; c++) for (unsigned n = 0; n < 2; n++) {
        reset(); identity.connected = c; identity.notify_enabled = n;
        forward_notify(1, 0, data, sizeof(data), true);
        assert(calls == (c && n ? 3 : 0));
        assert(notes == (c && n)); cases++;
    }
    reset(); notify_fail = 1;
    forward_notify(1, 0, data, sizeof(data), true);
    assert(strstr(last_log, "calls=3 submitted=192 errors=1 rc=6 off=128 oom=-1"));
    reset(); alloc_fail = 1;
    forward_notify(1, 0, data, sizeof(data), true);
    assert(strstr(last_log, "calls=1 submitted=128 errors=0 rc=0 off=-1 oom=128"));
    /* Live non-devinfo successes stay quiet; repeated errors rate limited. */
    reset(); data[4] = 2; now_us += 2000000;
    ble_periph_forward_notify(1, 0, data, 128); assert(log_count == 0);
    for (unsigned i = 0; i < 20; i++) {
        received_len = 0;
        notify_fail = calls;
        ble_periph_forward_notify(1, 0, data, 128);
    }
    assert(log_count == 1);
    now_us += 1000000; notify_fail = calls;
    ble_periph_forward_notify(1, 0, data, 128); assert(log_count == 2);

    /* Full 97 burst still stamps counters/checksums and sends 03,01,02.
     * Test valid/invalid headers/checksums, short/empty caches: observe only. */
    for (unsigned len = 0; len <= 320; len++) {
        reset(); replay_action = 1; replay_bits = 7;
        for (unsigned r = 1; r <= 3; r++) {
            warm[r].len = len; memcpy(warm[r].data, data, len);
            if (len >= 5) warm[r].data[4] = r;
        }
        ble_periph_replay_tick();
        assert(replay_bits == 0 && replay_gets == 3);
        assert(replay_order[0] == 3 && replay_order[1] == 1 && replay_order[2] == 2);
        assert(received_len == len * 3);
        for (unsigned r = 0; r < 3; r++) {
            const uint8_t *out = received + r * len;
            const uint8_t *in = warm[replay_order[r]].data;
            for (unsigned k = 0; k < len; k++)
                if (len < 6 || (k != 5 && k != len - 1)) assert(out[k] == in[k]);
            if (len >= 6) {
                uint8_t sum = 0;
                for (unsigned k = 0; k + 1 < len; k++) sum += out[k];
                assert(sum == out[len - 1]);
                if (r && len > 6)
                    assert(out[5] == (uint8_t)(received[(r - 1) * len + 5] + 1));
            }
        }
        cases++;
    }
    reset(); replay_action = 2; replay_bits = 7;
    ble_periph_replay_tick(); assert(!allocs && !replay_gets);
    assert(strstr(last_log, "cancelled: dev_seen advanced"));
    for (unsigned corrupt = 0; corrupt < 3; corrupt++) {
        reset(); replay_action = 1; replay_bits = NB_REPLAY_DEVINFO;
        warm[3].len = 300; memcpy(warm[3].data, data, 300); warm[3].data[4] = 3;
        uint8_t sum = 0;
        for (unsigned k = 0; k < 299; k++) sum += warm[3].data[k];
        warm[3].data[299] = sum;
        if (corrupt == 1) warm[3].data[0] ^= 1;
        if (corrupt == 2) warm[3].data[299] ^= 1;
        ble_periph_replay_tick();
        assert(calls == 3 && received_len == 300);
        assert(strstr(last_log, corrupt == 0 ? "hdr=1 sum=1" :
                                corrupt == 1 ? "hdr=0 sum=0" : "hdr=1 sum=0"));
        cases++;
    }
    /* The metadata digest must never depend on byte38+ (potential secrets).
     * Known public field occupancy and live/replay matching are observed. */
    reset(); memset(data, 0, sizeof(data));
    memcpy(data, "\x55\xAA\xEB\x90\x03", 5);
    memcpy(data + 6, "JK-PB2A16S20P", 12);
    memcpy(data + 22, "19A", 3); memcpy(data + 30, "19.31", 5);
    forward_notify(1, 0, data, 128, true);
    assert(strstr(last_dev_log, "model_nz=12 hw_nz=3 sw_nz=5"));
    char metadata[512]; memcpy(metadata, last_dev_log, sizeof(metadata));
    for (unsigned value = 0; value < 256; value++) {
        reset(); memset(data + 38, value, sizeof(data) - 38);
        forward_notify(1, 0, data, sizeof(data), true);
        assert(!strcmp(metadata, last_dev_log)); cases++;
    }
    reset(); forward_notify(1, 0, data, 128, false);
    assert(!strcmp(strstr(metadata, "public_sig="), strstr(last_dev_log, "public_sig=")));
    for (unsigned len = 5; len < 38; len++) {
        reset(); forward_notify(1, 0, data, len, true);
        assert(strstr(last_dev_log, "fields=short")); cases++;
    }
    /* Both attach (A supplied only the tail) and CCCD enabling mid-frame
     * must wait for a complete next frame. Every possible split is tested. */
    memset(data + 5, 0, 295);
    uint8_t sum = 0;
    for (unsigned i = 0; i < 299; i++) sum += data[i];
    data[299] = sum;
    for (unsigned split = 1; split < 300; split++) {
        reset(); identity.notify_enabled = false;
        ble_periph_forward_notify(1, 0, data, split);
        assert(!calls);
        identity.notify_enabled = true; identity.notify_epoch++;
        ble_periph_forward_notify(1, 0, data + split, 300 - split);
        assert(!received_len && !s_live[1].aligned);
        ble_periph_forward_notify(1, 0, data, split);
        assert(!received_len);
        ble_periph_forward_notify(1, 0, data + split, 300 - split);
        assert(received_len == 300 && s_live[1].aligned);
        assert(!memcmp(received, data, 300)); cases++;
    }
    puts("PASS: all299 CCCD/attach suffixes suppressed; fragmented next record delivered intact");

    /* Concatenated AT / C8 and unrecognized startup bytes cannot unlock the
     * live gate. After alignment every byte (including such auxiliary data)
     * passes unmodified, even in the same input as the first frame's end. */
    uint8_t stream[640];
    memset(stream, 0x66, sizeof(stream));
    memcpy(stream, "AT\r\n\xAA\x55\x90\xEB\xC8", 9);
    memcpy(stream + 20, data, 300);
    memcpy(stream + 320, "AT\r\n\xAA\x55\x90\xEB\xC8", 9);
    for (unsigned rec = 1; rec <= 3; rec++) {
        stream[24] = rec; sum = 0;
        for (unsigned i = 20; i < 319; i++) sum += stream[i];
        stream[319] = sum;
        for (unsigned chunk = 1; chunk <= 320; chunk++) {
            reset(); s_live[1].aligned = false;
            for (unsigned off = 0; off < sizeof(stream); off += chunk) {
                unsigned n = sizeof(stream) - off;
                if (n > chunk) n = chunk;
                ble_periph_forward_notify(1, 0, stream + off, n);
            }
            assert(received_len == sizeof(stream) - 20);
            assert(!memcmp(received, stream + 20, received_len)); cases++;
        }
    }
    /* False/invalid candidate may contain the real header. Sliding recovery
     * must retain it, at every overlap offset and with one-byte inputs. */
    for (unsigned offset = 5; offset < 300; offset++) {
        reset(); s_live[1].aligned = false;
        memset(stream, 0, sizeof(stream)); memcpy(stream, data, 5);
        memcpy(stream + offset, data, 300);
        sum = 0; for (unsigned i = 0; i < 299; i++) sum += stream[i];
        /* Synthetic prefix chosen so the outer candidate is invalid. */
        if (sum == stream[299]) { stream[5]++; if (offset == 5) continue; }
        for (unsigned i = 0; i < offset + 300; i++)
            ble_periph_forward_notify(1, 0, stream + i, 1);
        assert(received_len == 300 && !memcmp(received, data, 300)); cases++;
    }
    /* Same-handle reconnect or off/on ABA between chunks resets pending
     * bytes. Cached replay during that wait never opens the live gate. */
    for (unsigned split = 1; split < 300; split++) {
        reset(); s_live[1].aligned = false;
        ble_periph_forward_notify(1, 0, data, split);
        identity.notify_epoch += 2;
        forward_notify(1, 0, data, 300, true);
        assert(received_len == 300 && !s_live[1].aligned);
        received_len = 0;
        ble_periph_forward_notify(1, 0, data + split, 300 - split);
        assert(!received_len);
        ble_periph_forward_notify(1, 0, data, 300);
        assert(received_len == 300 && !memcmp(received, data, 300)); cases++;
    }
    /* Session edge during a multi-chunk send cuts off remaining old chunks,
     * even when the new connection is ready and reuses the same handle. */
    for (unsigned fail = 0; fail < 2; fail++) {
        reset(); s_live[1].aligned = false; change_session_after_call = fail;
        ble_periph_forward_notify(1, 0, data, 300);
        assert(calls == fail + 1 && !s_live[1].aligned);
        assert(s_live[1].start.len == 0);
        change_session_after_call = -1; received_len = 0;
        ble_periph_forward_notify(1, 0, data, 300);
        assert(received_len == 300 && s_live[1].aligned); cases++;
    }
    for (unsigned fail = 0; fail < 3; fail++) {
        for (unsigned oom = 0; oom < 2; oom++) {
            reset(); s_live[1].aligned = false;
            if (oom) alloc_fail = fail; else notify_fail = fail;
            ble_periph_forward_notify(1, 0, data, 300);
            assert(!s_live[1].aligned && s_live[1].start.len == 0);
            alloc_fail = notify_fail = -1; received_len = 0;
            ble_periph_forward_notify(1, 0, data, 300);
            assert(received_len == 300 && s_live[1].aligned); cases++;
        }
    }
    for (unsigned m = 0; m < sizeof(mtus)/sizeof(mtus[0]); m++) {
        for (unsigned split = 1; split < 300; split++) {
            reset(); s_live[1].aligned = false; test_mtu = mtus[m];
            ble_periph_forward_notify(1, 0, data, split);
            assert(!received_len);
            ble_periph_forward_notify(1, 0, data + split, 300 - split);
            assert(received_len == 300 && !memcmp(received, data, 300)); cases++;
        }
    }
    reset(); s_live[1].aligned = false;
    memset(stream, 0x55, sizeof(stream));
    for (unsigned i = 0; i < 1000; i++)
        ble_periph_forward_notify(1, 0, stream, 320);
    assert(!received_len && s_live[1].start.len == 1);
    memcpy(stream, data, 300); stream[299]++;
    ble_periph_forward_notify(1, 0, stream, 300); assert(!received_len);
    ble_periph_forward_notify(1, 0, data, 300);
    assert(received_len == 300 && !memcmp(received, data, 300));

    /* Per-identity buffer isolation (mock connection state is shared, but
     * actual gate state is not). Incomplete data on bank0 cannot align1. */
    reset(); s_live[1].aligned = false;
    ble_periph_forward_notify(0, 0, data, 100);
    ble_periph_forward_notify(1, 0, data + 100, 200);
    assert(!received_len);
    ble_periph_forward_notify(1, 0, data, 300);
    assert(received_len == 300 && s_live[0].start.len == 100);
    printf("B notification diagnostic invariants: %u cases passed\n", cases);
}
