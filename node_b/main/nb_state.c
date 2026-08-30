#include <string.h>
#include <stdio.h>
#include "nb_state.h"
#include "config.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "nb_state";

static nb_blueprint_t  s_bp;
static nb_identity_t   s_id[CFG_NUM_UNITS];
static SemaphoreHandle_t s_mtx;
static nvs_handle_t    s_nvs;      /* "warm" namespace — devinfo blobs */

static inline void lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_mtx); }

static void devinfo_key(uint8_t id, int page, char *k)
{ sprintf(k, "w%c%u", page ? 'b' : 'a', id); }

/* Devinfo page classifier: page A carries hw/sw version strings at bytes
 * 22..29; page B zeros them (passcode + mfg date live further in). */
static int devinfo_page(const uint8_t *f, uint16_t len)
{
    if (len < 30) return 0;
    for (int i = 22; i < 30; i++) if (f[i]) return 0;
    return 1;
}

void nb_state_init(void)
{
    memset(&s_bp, 0, sizeof(s_bp));
    memset(s_id, 0, sizeof(s_id));
    for (int i = 0; i < CFG_NUM_UNITS; i++) s_id[i].link = LINK_UNREACHABLE;
    s_mtx = xSemaphoreCreateMutex();

    /* Restore persisted device-info frames: they are static per unit, and
     * having them from boot kills the app's "request device information
     * failure" even before a bank has streamed this uptime (2026-08-30). */
    if (nvs_open("warm", NVS_READWRITE, &s_nvs) == ESP_OK) {
        for (uint8_t i = 0; i < CFG_NUM_UNITS; i++) {
            for (int p = 0; p < 2; p++) {
                char k[8]; devinfo_key(i, p, k);
                size_t len = NB_CACHE_MAX;
                if (nvs_get_blob(s_nvs, k, s_id[i].warm_dev[p].data, &len) == ESP_OK) {
                    s_id[i].warm_dev[p].len = (uint16_t)len;
                    ESP_LOGI(TAG, "devinfo[%u] page %c restored (%u B)", i,
                             p ? 'B' : 'A', (unsigned)len);
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "nvs open failed — devinfo cache is RAM-only");
        s_nvs = 0;
    }
}

void nb_get_blueprint(nb_blueprint_t *out) { lock(); *out = s_bp; unlock(); }
void nb_set_blueprint(const nb_blueprint_t *bp) { lock(); s_bp = *bp; s_bp.valid = true; unlock(); }

void nb_get_identity(uint8_t id, nb_identity_t *out)
{ if (id >= CFG_NUM_UNITS) { memset(out, 0, sizeof(*out)); return; } lock(); *out = s_id[id]; unlock(); }

void nb_set_ident_name(uint8_t id, const char *name)
{ if (id >= CFG_NUM_UNITS) return; lock(); strlcpy(s_id[id].name, name, sizeof(s_id[id].name)); s_id[id].have_name = true; unlock(); }

void nb_set_link(uint8_t id, tunnel_link_state_t s)
{ if (id >= CFG_NUM_UNITS) return; lock(); s_id[id].link = s; unlock(); }

void nb_set_cache(uint8_t id, uint8_t idx, const uint8_t *data, uint16_t len)
{
    if (id >= CFG_NUM_UNITS || idx >= NB_MAX_CHARS) return;
    if (len > NB_CACHE_MAX) len = NB_CACHE_MAX;
    lock(); s_id[id].cache[idx].len = len; memcpy(s_id[id].cache[idx].data, data, len); unlock();
}
void nb_get_cache(uint8_t id, uint8_t idx, nb_cache_t *out)
{
    if (id >= CFG_NUM_UNITS || idx >= NB_MAX_CHARS) { out->len = 0; return; }
    lock(); *out = s_id[id].cache[idx]; unlock();
}

/* rec: JK record type — 0x03 device-info, 0x02 cell-info (others ignored). */
void nb_set_warm(uint8_t id, uint8_t rec, const uint8_t *frame, uint16_t len)
{
    if (id >= CFG_NUM_UNITS) return;
    if (len > NB_CACHE_MAX) len = NB_CACHE_MAX;
    bool persist = false; int page = 0;
    lock();
    nb_cache_t *w;
    if (rec == 0x03) {
        page = devinfo_page(frame, len);
        w = &s_id[id].warm_dev[page];
        /* Persist only on identity change. Devinfo carries live fields —
         * frame counter (byte 5), module uptime, checksum — that differ
         * every frame; the stable identity is bytes 6..37 (model + the
         * page's fixed region). That is what write-once means here. */
        persist = !(w->len == len && len >= 38 &&
                    memcmp(w->data + 6, frame + 6, 32) == 0);
    } else {
        w = rec == 0x02 ? &s_id[id].warm_cellinfo
          : rec == 0x01 ? &s_id[id].warm_settings : NULL;
        if (rec == 0x02) s_id[id].warm_cell_us = esp_timer_get_time();
    }
    if (w) { w->len = len; memcpy(w->data, frame, len); }
    unlock();
    if (persist && s_nvs) {
        char k[8]; devinfo_key(id, page, k);
        if (nvs_set_blob(s_nvs, k, frame, len) == ESP_OK) nvs_commit(s_nvs);
        ESP_LOGI(TAG, "devinfo[%u] page %c persisted (%u B)", id,
                 page ? 'B' : 'A', len);
    }
}
void nb_get_warm(uint8_t id, uint8_t rec, nb_cache_t *out)
{
    out->len = 0;
    if (id >= CFG_NUM_UNITS) return;
    lock();
    nb_cache_t *w = rec == 0x03 ? &s_id[id].warm_dev[0]   /* page A */
                  : rec == 0x02 ? &s_id[id].warm_cellinfo
                  : rec == 0x01 ? &s_id[id].warm_settings : NULL;
    if (w) *out = *w;
    /* Age gate: never replay old voltages as if live (see NB_CELL_REPLAY_
     * MAX_AGE_US). Applies here so every replay path inherits it. */
    if (rec == 0x02 &&
        esp_timer_get_time() - s_id[id].warm_cell_us > NB_CELL_REPLAY_MAX_AGE_US)
        out->len = 0;
    unlock();
}

void nb_get_warm_dev(uint8_t id, int page, nb_cache_t *out)
{
    out->len = 0;
    if (id >= CFG_NUM_UNITS || page < 0 || page > 1) return;
    lock(); *out = s_id[id].warm_dev[page]; unlock();
}

void nb_mark_replay(uint8_t id, uint8_t bits)
{
    if (id >= CFG_NUM_UNITS) return;
    lock();
    if (!s_id[id].pending_replay) s_id[id].pending_since_us = esp_timer_get_time();
    s_id[id].pending_replay |= bits;
    unlock();
}

uint8_t nb_take_replay(uint8_t id)
{
    if (id >= CFG_NUM_UNITS) return 0;
    lock(); uint8_t b = s_id[id].pending_replay; s_id[id].pending_replay = 0; unlock();
    return b;
}

bool nb_notify_ready(uint8_t id)
{
    if (id >= CFG_NUM_UNITS) return false;
    lock(); bool r = s_id[id].connected && s_id[id].notify_enabled; unlock();
    return r;
}
bool nb_replay_ready(uint8_t id)
{
    if (id >= CFG_NUM_UNITS) return false;
    lock();
    bool r = s_id[id].connected && s_id[id].notify_enabled &&
             s_id[id].pending_replay != 0;
    /* Expire past the opener window: a replay held back by live traffic
     * must never fire into a session that has long since moved on. */
    if (r && esp_timer_get_time() - s_id[id].pending_since_us > 5000000LL) {
        s_id[id].pending_replay = 0;
        r = false;
    }
    unlock();
    return r;
}

int nb_conn_handle(uint8_t id)
{
    if (id >= CFG_NUM_UNITS) return -1;
    lock();
    int h = s_id[id].connected ? (int)s_id[id].conn_handle : -1;
    unlock();
    return h;
}

bool nb_get_name(uint8_t id, char *out, size_t out_len)
{
    if (id >= CFG_NUM_UNITS || !out_len) return false;
    lock();
    bool have = s_id[id].have_name;
    if (have) strlcpy(out, s_id[id].name, out_len);
    unlock();
    return have;
}

/* Plain volatile per-id state: written only by the single tunnel task,
 * read by the same task's tick — no locking needed. */
static volatile int64_t s_last_raw_us[CFG_NUM_UNITS];
static volatile int32_t s_raw_rem[CFG_NUM_UNITS];   /* bytes left of frame */

void nb_note_raw_chunk(uint8_t id, const uint8_t *d, uint16_t n)
{
    if (id >= CFG_NUM_UNITS) return;
    s_last_raw_us[id] = esp_timer_get_time();
    int32_t rem = s_raw_rem[id];
    if (rem <= 0) {
        /* At a boundary: a 55AAEB90 chunk starts a 300 B record; anything
         * else (AT heartbeat, AA5590EBC8 ack) is an atomic chunk. */
        rem = (n >= 4 && d[0]==0x55 && d[1]==0xAA && d[2]==0xEB && d[3]==0x90)
              ? 300 - (int32_t)n : 0;
    } else {
        rem -= n;
    }
    s_raw_rem[id] = rem > 0 ? rem : 0;
}

bool nb_at_frame_boundary(uint8_t id)
{
    if (id >= CFG_NUM_UNITS) return true;
    /* A stalled mid-frame stream counts as a boundary after 500 ms of
     * silence (desync/death guard — the frame will never complete). */
    if (esp_timer_get_time() - s_last_raw_us[id] > 500000) return true;
    return s_raw_rem[id] <= 0;
}

void nb_set_conn(uint8_t id, bool c, uint16_t h)
{
    if (id >= CFG_NUM_UNITS) return;
    lock();
    s_id[id].connected = c; s_id[id].conn_handle = h;
    if (!c) { s_id[id].notify_enabled = false; s_id[id].write_fail_count = 0;
              s_id[id].pending_replay = 0; }   /* owed replays die with the conn */
    unlock();
}
void nb_set_notify(uint8_t id, bool en)
{ if (id >= CFG_NUM_UNITS) return; lock(); s_id[id].notify_enabled = en; unlock(); }

int nb_identity_for_conn(uint16_t h)
{
    int r = -1; lock();
    for (int i = 0; i < CFG_NUM_UNITS; i++)
        if (s_id[i].connected && s_id[i].conn_handle == h) { r = i; break; }
    unlock(); return r;
}
int nb_active_conn_count(void)
{
    int n = 0; lock();
    for (int i = 0; i < CFG_NUM_UNITS; i++) if (s_id[i].connected) n++;
    unlock(); return n;
}
