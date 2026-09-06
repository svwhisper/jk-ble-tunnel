/* Actual B replay/notify production functions. Simulated mbufs/host submission:
 * no radio, network, serial, NVS or battery commands. Diagnostics must not fix
 * or otherwise change the existing loss/continuation/replay behaviour. */
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
static uint8_t received[1024], pending_data[320];
static struct os_mbuf mbuf;
static int64_t now_us = 10000000;
static nb_cache_t warm[4];
static int replay_action;
static uint8_t replay_bits;
static uint8_t replay_order[3];
static unsigned replay_gets;

int64_t esp_timer_get_time(void) { return now_us; }
void nb_get_identity(uint8_t id, nb_identity_t *out)
{ assert(id < CFG_NUM_UNITS); *out = identity; }
void nb_note_dev_forwarded(uint8_t id) { assert(id == 1); notes++; }
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
    test_mtu = 185; alloc_fail = notify_fail = -1;
    allocs = calls = notes = received_len = log_count = replay_gets = 0;
    last_log[0] = 0;
}

int main(void)
{
    static const uint16_t mtus[] = {0, 23, 64, 131, 185, 512};
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
    /* Independent B-side reproduction: CCCD toggling on mid-frame also
     * forwards only its tail, even when A supplied every original byte. */
    for (unsigned split = 1; split < 300; split++) {
        reset(); identity.notify_enabled = false;
        ble_periph_forward_notify(1, 0, data, split);
        assert(!calls);
        identity.notify_enabled = true;
        ble_periph_forward_notify(1, 0, data + split, 300 - split);
        assert(received_len == 300 - split);
        assert(!memcmp(received, data + split, received_len)); cases++;
    }
    puts("PASS: reproduced headerless live-session suffix at all299 CCCD-enable split points (not fixed)");
    printf("B notification diagnostic invariants: %u cases passed\n", cases);
}
