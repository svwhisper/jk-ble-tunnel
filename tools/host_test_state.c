/* Tests the production state_cache.c with pthread-backed FreeRTOS adapters. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "state_cache.h"

#define ITERATIONS 25000
static bms_runtime_t get(uint8_t id)
{
    bms_runtime_t rt;
    state_get_runtime(id, &rt);
    return rt;
}

static void *app_writer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= ITERATIONS; i++) {
        state_set_app_connected(0, true, 2 * i);
        bms_runtime_t rt = get(0);
        assert(rt.app_connected && rt.app_left_us == 0);
        state_set_app_connected(0, false, 2 * i + 1);
        rt = get(0);
        assert(!rt.app_connected && rt.app_left_us == 2 * i + 1);
    }
    return NULL;
}

static void *link_writer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= ITERATIONS; i++) {
        state_set_link_state(0, LINK_UP, true, i);
        bms_runtime_t rt = get(0);
        assert(rt.link_held && rt.link == LINK_UP);
        state_set_link_state(0, LINK_UNREACHABLE, false, 0);
        assert(!get(0).link_held);
    }
    return NULL;
}

static void *frame_writer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= ITERATIONS; i++) {
        state_note_frame(0, 1000000 + i);
        assert(get(0).last_seen_us >= 1000000 + i);
    }
    return NULL;
}

static void *reachability_writer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= ITERATIONS; i++) {
        state_promote_unreachable(0);
        state_mark_idle_if_unheld(0);
    }
    return NULL;
}

static void *decoder_writer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= ITERATIONS; i++) {
        jk_cell_info_t cells = {0};
        cells.cell_count = 1;
        cells.cells[0].mv = i;
        cells.pack_mv = i;
        state_set_cells(0, &cells);
        jk_settings_t settings = {0};
        settings.cell_count_set = i;
        settings.balance_current_a = i;
        state_set_settings(0, &settings);
    }
    return NULL;
}

static void *observer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= ITERATIONS; i++) {
        bms_state_t st;
        assert(state_snapshot(0, &st));
        assert(!st.rt.app_connected || st.rt.app_left_us == 0);
        assert(!st.rt.link_held || st.rt.link == LINK_UP);
        if (st.have_cells) assert(st.cells.cells[0].mv == st.cells.pack_mv);
        if (st.have_settings)
            assert(st.settings.cell_count_set == st.settings.balance_current_a);
    }
    return NULL;
}

int main(void)
{
    state_cache_init();

    /* Exact old failure: an unrelated notify update erased app_connected. */
    state_set_app_connected(0, true, 1);
    state_note_frame(0, 123);
    assert(get(0).app_connected && get(0).last_seen_us == 123);
    state_set_link_state(0, LINK_UP, true, 124);
    assert(get(0).app_connected);
    assert(!state_promote_unreachable(0));
    assert(!state_mark_idle_if_unheld(0));
    assert(get(0).link_held && get(0).link == LINK_UP);

    /* Older timestamp sampled before locking must not undo a newer update. */
    state_note_frame(0, 200);
    state_set_link_state(0, LINK_UP, true, 150);
    state_note_frame(0, 180);
    assert(get(0).last_seen_us == 200);

    /* Stale supervisor cleanup must preserve a subsequent app departure. */
    state_set_app_connected(0, false, 300);
    state_set_app_connected(0, true, 350);
    state_clear_app_left_if(0, 300);
    assert(get(0).app_connected && get(0).app_left_us == 0);
    state_set_app_connected(0, false, 400);
    state_clear_app_left_if(0, 300);
    assert(get(0).app_left_us == 400);
    state_clear_app_left_if(0, 400);
    assert(get(0).app_left_us == 0 && !get(0).app_connected);

    state_set_link_state(0, LINK_UNREACHABLE, false, 0);
    assert(state_promote_unreachable(0));
    assert(!state_promote_unreachable(0));
    assert(get(0).link == LINK_REACHABLE_IDLE && get(0).last_seen_us == 200);
    state_note_frame(3, 999);
    assert(get(3).last_seen_us == 999 && get(0).last_seen_us == 200);
    state_note_frame(255, 1000);
    state_set_app_connected(255, true, 1);
    state_set_link_state(255, LINK_UP, true, 1000);
    state_clear_app_left_if(255, 1);
    assert(!state_promote_unreachable(255) && !state_mark_idle_if_unheld(255));
    assert(get(255).last_seen_us == 0);

    state_note_frame(0, 1000000);
    pthread_t threads[6];
    void *(*workers[])(void *) = {
        app_writer, link_writer, frame_writer, reachability_writer,
        decoder_writer, observer
    };
    for (unsigned i = 0; i < 6; i++)
        assert(pthread_create(&threads[i], NULL, workers[i], NULL) == 0);
    for (unsigned i = 0; i < 6; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(get(0).last_seen_us == 1000000 + ITERATIONS);
    assert(get(0).app_left_us == 2 * ITERATIONS + 1 && !get(0).app_connected);
    assert(get(3).last_seen_us == 999);
    printf("PASS: production state cache, deterministic races + 6 threads x %d iterations\n",
           ITERATIONS);
    return 0;
}
