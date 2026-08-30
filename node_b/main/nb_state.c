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

static void devinfo_key(uint8_t id, char *k) { sprintf(k, "wd%u", id); }

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
            char k[8]; devinfo_key(i, k);
            size_t len = NB_CACHE_MAX;
            if (nvs_get_blob(s_nvs, k, s_id[i].warm_devinfo.data, &len) == ESP_OK) {
                s_id[i].warm_devinfo.len = (uint16_t)len;
                ESP_LOGI(TAG, "devinfo[%u] restored (%u B)", i, (unsigned)len);
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
    bool persist = false;
    lock();
    nb_cache_t *w = rec == 0x03 ? &s_id[id].warm_devinfo
                  : rec == 0x02 ? &s_id[id].warm_cellinfo : NULL;
    if (w) {
        if (rec == 0x03) {
            /* Persist only on real change. Byte 5 is a rolling frame counter
             * and the last byte its checksum — both differ every frame while
             * the payload is static, so compare bytes 6..len-2 to keep this
             * a write-once (NVS wear). */
            persist = !(w->len == len && len > 7 &&
                        memcmp(w->data + 6, frame + 6, len - 7) == 0);
        } else {
            s_id[id].warm_cell_us = esp_timer_get_time();
        }
        w->len = len; memcpy(w->data, frame, len);
    }
    unlock();
    if (persist && s_nvs) {
        char k[8]; devinfo_key(id, k);
        if (nvs_set_blob(s_nvs, k, frame, len) == ESP_OK) nvs_commit(s_nvs);
        ESP_LOGI(TAG, "devinfo[%u] persisted (%u B)", id, len);
    }
}
void nb_get_warm(uint8_t id, uint8_t rec, nb_cache_t *out)
{
    out->len = 0;
    if (id >= CFG_NUM_UNITS) return;
    lock();
    nb_cache_t *w = rec == 0x03 ? &s_id[id].warm_devinfo
                  : rec == 0x02 ? &s_id[id].warm_cellinfo : NULL;
    if (w) *out = *w;
    /* Age gate: never replay old voltages as if live (see NB_CELL_REPLAY_
     * MAX_AGE_US). Applies here so every replay path inherits it. */
    if (rec == 0x02 &&
        esp_timer_get_time() - s_id[id].warm_cell_us > NB_CELL_REPLAY_MAX_AGE_US)
        out->len = 0;
    unlock();
}

void nb_mark_replay(uint8_t id, uint8_t bits)
{ if (id >= CFG_NUM_UNITS) return; lock(); s_id[id].pending_replay |= bits; unlock(); }

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
    unlock();
    return r;
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
