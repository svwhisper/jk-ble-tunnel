#include <string.h>
#include "state_cache.h"
#include "config.h"
#include "freertos/semphr.h"

static bms_state_t s_state[CFG_NUM_UNITS];
static SemaphoreHandle_t s_mtx;

void state_cache_init(void)
{
    memset(s_state, 0, sizeof(s_state));
    for (int i = 0; i < CFG_NUM_UNITS; i++)
        s_state[i].rt.link = LINK_REACHABLE_IDLE;   /* optimistic boot (spec §4) */
    s_mtx = xSemaphoreCreateMutex();
}

static inline bool lock(void)   { return xSemaphoreTake(s_mtx, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_mtx); }

void state_set_cells(uint8_t id, const jk_cell_info_t *ci)
{
    if (id >= CFG_NUM_UNITS) return;
    lock();
    s_state[id].cells = *ci;
    s_state[id].have_cells = true;
    unlock();
}

void state_set_settings(uint8_t id, const jk_settings_t *s)
{
    if (id >= CFG_NUM_UNITS) return;
    lock();
    s_state[id].settings = *s;
    s_state[id].have_settings = true;
    unlock();
}

void state_set_runtime(uint8_t id, const bms_runtime_t *rt)
{
    if (id >= CFG_NUM_UNITS) return;
    lock(); s_state[id].rt = *rt; unlock();
}

void state_get_runtime(uint8_t id, bms_runtime_t *out)
{
    if (id >= CFG_NUM_UNITS) { memset(out, 0, sizeof(*out)); return; }
    lock(); *out = s_state[id].rt; unlock();
}

bool state_snapshot(uint8_t id, bms_state_t *out)
{
    if (id >= CFG_NUM_UNITS) return false;
    lock(); *out = s_state[id]; unlock();
    return true;
}
