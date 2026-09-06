/* Actual B cache selector; synthetic frames only, no NVS/device/network I/O.
 * Reproduce current hazards without silently changing selection policy. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define HOST_ESP_LOG_H
static void test_log(const char *, const char *, ...);
#define ESP_LOGI test_log
#define ESP_LOGW test_log
#include "../node_b/main/nb_state.c"
static char last_log[400];
static void test_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    assert(pthread_mutex_trylock(s_mtx) == 0);
    assert(pthread_mutex_unlock(s_mtx) == 0);
    va_list ap; va_start(ap, fmt); vsnprintf(last_log, sizeof(last_log), fmt, ap); va_end(ap);
}
static int64_t test_now;
static unsigned writes;
int64_t esp_timer_get_time(void) { return test_now; }
int nvs_open(const char *name, int mode, nvs_handle_t *out)
{ (void)name; (void)mode; *out = 1; return 0; }
int nvs_get_blob(nvs_handle_t h, const char *key, void *buf, size_t *len)
{ (void)h; (void)key; (void)buf; (void)len; return -1; }
int nvs_set_blob(nvs_handle_t h, const char *key, const void *buf, size_t len)
{ (void)key; (void)buf; assert(h == 1 && len == 300); writes++; return 0; }
int nvs_commit(nvs_handle_t h) { assert(h == 1); return 0; }
static void checksum(uint8_t *f)
{ uint8_t sum = 0; for (unsigned k = 0; k < 299; k++) sum += f[k]; f[299] = sum; }
static void frame(uint8_t *f)
{
    memset(f, 0, 300); memcpy(f, "\x55\xAA\xEB\x90\x03", 5);
    memcpy(f + 6, "JK-PB2A16S20P", 12);
    memcpy(f + 22, "19A", 3); memcpy(f + 30, "19.31", 5);
    memcpy(f + 46, "TEST UNIT", 9); memcpy(f + 86, "SYNTH000001", 11);
    checksum(f);
}
int main(void)
{
    uint8_t old[300], next[300]; nb_cache_t result;
    nb_state_init(); frame(old); old[38] = old[39] = 255; old[40] = 1;
    checksum(old); nb_set_warm(1, 3, old, 300);
    memcpy(next, old, 300); memset(next + 38, 0, 4); next[40] = 2;
    next[5] = 2; checksum(next);
    /* A larger numeric uptime with fewer nonzero bytes loses the score. */
    assert(devinfo_score(next, 300) < devinfo_score(old, 300));
    nb_set_warm(1, 3, next, 300); nb_get_warm_dev(1, 0, &result);
    assert(!memcmp(result.data, old, 300));
    assert(strstr(last_log, "keep=1") && strstr(last_log, "public_same=1 stable_same=1"));
    /* Equal-richness identity changes replace RAM but skip persistence when
     * public model/HW/SW bytes6..37 are unchanged. Separate latent flaw. */
    nb_state_init(); writes = 0; frame(old); nb_set_warm(2, 3, old, 300);
    assert(writes == 1); memcpy(next, old, 300); next[96] = '2'; checksum(next);
    nb_set_warm(2, 3, next, 300); nb_get_warm_dev(2, 0, &result);
    assert(!memcmp(result.data, next, 300) && writes == 1);
    assert(strstr(last_log, "keep=0") && strstr(last_log, "stable_same=0 persist=0"));
    /* Byte-for-byte oracle of pre-diagnostic selector across every bounded
     * length, all record classes, empty/full previous cache, all identities.
     * Persistence disabled; oracle explicitly encodes the original policy. */
    unsigned cases = 0;
    for (unsigned id = 0; id < CFG_NUM_UNITS; id++)
    for (unsigned prior = 0; prior < 2; prior++)
    for (unsigned len = 0; len <= NB_CACHE_MAX + 1; len++)
    for (unsigned rec = 0; rec < 5; rec++) {
        uint8_t incoming[NB_CACHE_MAX + 1];
        for (unsigned k = 0; k < sizeof(incoming); k++) incoming[k] = (k * 7 + len) % 11;
        memset(&s_id[id], 0, sizeof(s_id[id])); s_nvs = 0;
        if (prior) {
            s_id[id].warm_dev[0].len = 300;
            memcpy(s_id[id].warm_dev[0].data, old, 300);
        }
        nb_identity_t expected = s_id[id];
        unsigned n = len > NB_CACHE_MAX ? NB_CACHE_MAX : len;
        nb_cache_t *slot = rec == 3 ? &expected.warm_dev[0] :
                           rec == 2 ? &expected.warm_cellinfo :
                           rec == 1 ? &expected.warm_settings : NULL;
        bool keep = false;
        if (rec == 3) {
            unsigned old_score = 0, new_score = 0;
            for (unsigned k = 38; k < 160; k++) {
                if (k < n && incoming[k]) new_score++;
                if (k < slot->len && slot->data[k]) old_score++;
            }
            keep = slot->len && new_score < old_score;
        }
        test_now = 2000000 + cases;
        if (rec == 2) expected.warm_cell_us = test_now;
        if (slot && !keep) { slot->len = n; memcpy(slot->data, incoming, n); }
        nb_set_warm(id, rec, incoming, len);
        assert(!memcmp(&s_id[id], &expected, sizeof(expected))); cases++;
    }
    puts("PASS: reproduced B uptime-sensitive cache selection and same-score identity persistence omission (not fixes)");
    printf("PASS: %u diagnostic cache-selection equivalence cases; logging outside state mutex\n", cases);
}
