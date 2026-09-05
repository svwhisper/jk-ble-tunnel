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

void state_set_app_connected(uint8_t id, bool connected, int64_t now_us)
{
    if (id >= CFG_NUM_UNITS) return;
    lock();
    if (s_state[id].rt.app_connected != connected) s_state[id].rt.idle_epoch++;
    s_state[id].rt.app_connected = connected;
    s_state[id].rt.app_left_us = connected ? 0 : now_us;
    unlock();
}

void state_set_link_state(uint8_t id, tunnel_link_state_t link, bool held,
                          int64_t seen_us)
{
    if (id >= CFG_NUM_UNITS) return;
    lock();
    bms_runtime_t *rt = &s_state[id].rt;
    if (rt->link_held != held) {
        rt->idle_epoch++;
        if (held && seen_us > rt->link_up_us) rt->link_up_us = seen_us;
    }
    rt->link = link;
    rt->link_held = held;
    /* Legacy activity drives reconnect/idle policies; do not change those
     * alongside boot evidence. Link-up never advances last_frame_us.
     * A timestamp sampled before taking the mutex must not move time back. */
    if (seen_us > rt->last_seen_us) rt->last_seen_us = seen_us;
    unlock();
}

void state_note_frame(uint8_t id, int64_t seen_us)
{
    if (id >= CFG_NUM_UNITS) return;
    lock();
    if (seen_us > s_state[id].rt.last_seen_us) s_state[id].rt.last_seen_us = seen_us;
    if (seen_us > s_state[id].rt.last_frame_us) s_state[id].rt.last_frame_us = seen_us;
    unlock();
}

bool state_promote_unreachable(uint8_t id)
{
    if (id >= CFG_NUM_UNITS) return false;
    lock();
    bms_runtime_t *rt = &s_state[id].rt;
    bool changed = !rt->link_held && rt->link == LINK_UNREACHABLE;
    if (changed) rt->link = LINK_REACHABLE_IDLE;
    unlock();
    return changed;
}

bool state_mark_idle_if_unheld(uint8_t id)
{
    if (id >= CFG_NUM_UNITS) return false;
    lock();
    bool allowed = !s_state[id].rt.link_held;
    if (allowed) s_state[id].rt.link = LINK_REACHABLE_IDLE;
    unlock();
    return allowed;
}

void state_clear_app_left_if(uint8_t id, int64_t expected_us)
{
    if (id >= CFG_NUM_UNITS) return;
    lock();
    bms_runtime_t *rt = &s_state[id].rt;
    if (!rt->app_connected && rt->app_left_us == expected_us) rt->app_left_us = 0;
    unlock();
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
