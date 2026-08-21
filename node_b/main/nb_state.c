#include <string.h>
#include "nb_state.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static nb_blueprint_t  s_bp;
static nb_identity_t   s_id[CFG_NUM_UNITS];
static SemaphoreHandle_t s_mtx;

static inline void lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_mtx); }

void nb_state_init(void)
{
    memset(&s_bp, 0, sizeof(s_bp));
    memset(s_id, 0, sizeof(s_id));
    for (int i = 0; i < CFG_NUM_UNITS; i++) s_id[i].link = LINK_UNREACHABLE;
    s_mtx = xSemaphoreCreateMutex();
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

void nb_set_conn(uint8_t id, bool c, uint16_t h)
{
    if (id >= CFG_NUM_UNITS) return;
    lock();
    s_id[id].connected = c; s_id[id].conn_handle = h;
    if (!c) { s_id[id].notify_enabled = false; s_id[id].write_fail_count = 0; }
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
