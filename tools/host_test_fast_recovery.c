/* Stage D2 on live D1: the real arbiter task/handlers with the app-edges
 * queue adapters. No BLE, network, serial or battery I/O. */
#define main app_edges_main
#include "host_test_app_edges.c"
#undef main

static void arb_message(arb_kind_t kind, uint8_t id)
{
    incoming = (arb_msg_t){ .kind = kind, .bms_id = id };
    message_once();
}
static void fail_active(uint8_t id, bms_cmd_id_t cmd, resp_status_t st)
{
    s_pend[id].busy = true;
    s_pend[id].active_cmd_id = cmd;
    bms_response_t r = { .bms_id = id, .cmd_id = cmd, .status = st };
    on_response(&r);
}

int main(void)
{
    state_cache_init();
    unsigned cases = 0;

    /* D2a: first failure with a phone waiting retries after 250 ms; the next
     * failure resumes 2 s doubling; RESP_OK resets; no phone = unchanged. */
    const resp_status_t fails[3] = { RESP_LINK_DOWN, RESP_TIMEOUT, RESP_GATT_ERR };
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        for (unsigned f = 0; f < 3; f++) {
            memset(s_pend, 0, sizeof(s_pend));
            state_set_link_state(id, LINK_REACHABLE_IDLE, false, test_now);
            state_set_app_connected(id, true, ++test_now);
            fail_active(id, 5, fails[f]);
            assert(s_pend[id].backoff_ms == 1);
            assert(s_pend[id].connect_after_us == test_now + CFG_APP_FAST_RETRY_MS * 1000LL);
            fail_active(id, 6, fails[f]);
            assert(s_pend[id].backoff_ms == 2000);
            assert(s_pend[id].connect_after_us == test_now + 2000000LL);
            fail_active(id, 7, fails[f]);
            assert(s_pend[id].backoff_ms == 4000);
            fail_active(id, 8, RESP_OK);
            assert(s_pend[id].backoff_ms == 0);
            fail_active(id, 9, fails[f]);          /* healthy again: fast once more */
            assert(s_pend[id].backoff_ms == 1);

            memset(s_pend, 0, sizeof(s_pend));
            state_set_app_connected(id, false, ++test_now);
            fail_active(id, 5, fails[f]);          /* boot verify / background */
            assert(s_pend[id].backoff_ms == 2000);
            assert(s_pend[id].connect_after_us == test_now + 2000000LL);
            cases += 2;
        }
    }
    /* Local radio contention is not a failure: no fast retry consumed. */
    memset(s_pend, 0, sizeof(s_pend));
    state_set_app_connected(1, true, ++test_now);
    fail_active(1, 5, RESP_CONNECT_WAIT);
    assert(s_pend[1].backoff_ms == 0 && s_pend[1].connect_after_us == 0);
    cases++;

    /* D2b: app 0x97 on a held link. */
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        memset(s_pend, 0, sizeof(s_pend));
        state_set_link_state(id, LINK_UP, true, test_now);
        client_message(id, true);                  /* resets per-session D2 state */
        assert(!s_pend[id].devreq_deadline_us && !s_pend[id].devreq_refreshed);

        /* Answered: arm, then a device-info record disarms; expiry is inert. */
        arb_message(ARB_APP_DEVREQ, id);
        assert(s_pend[id].devreq_deadline_us == test_now + CFG_APP_DEVINFO_WAIT_MS * 1000LL);
        test_now += 400000;
        arb_message(ARB_DEVINFO, id);
        assert(!s_pend[id].devreq_deadline_us);
        unsigned before = ble_queue_sends;
        test_now += 2000000;
        arb_message(ARB_DEVINFO, id);              /* any tick */
        assert(ble_queue_sends == before && !s_pend[id].devreq_refreshed);
        cases++;

        /* Unanswered: at expiry, disconnect then re-raise, once per session. */
        arb_message(ARB_APP_DEVREQ, id);
        int64_t armed = s_pend[id].devreq_deadline_us;
        assert(armed == test_now + CFG_APP_DEVINFO_WAIT_MS * 1000LL);
        arb_message(ARB_APP_DEVREQ, id);           /* repeat 0x97 does not extend */
        assert(s_pend[id].devreq_deadline_us == armed);
        test_now = armed;                          /* not yet expired */
        arb_message(ARB_SETTINGS, id);
        assert(s_pend[id].devreq_deadline_us == armed && ble_queue_sends == before);
        test_now = armed + 1;
        arb_message(ARB_SETTINGS, id);
        assert(!s_pend[id].devreq_deadline_us && s_pend[id].devreq_refreshed);
        assert(ble_queue_sends == before + 1 && ble_queued.kind == TXN_DISCONNECT);
        assert(ble_queued.bms_id == id && ble_queued.source == SRC_INTERNAL);
        assert(s_pend[id].count == 1 && s_pend[id].ring[s_pend[id].head].kind == TXN_POLL);
        assert(s_pend[id].ring[s_pend[id].head].opcode == JK_CMD_DEVICE_INFO);
        arb_message(ARB_APP_DEVREQ, id);           /* same session: never again */
        assert(!s_pend[id].devreq_deadline_us);
        cases++;

        /* Cold attach (link not held): the fresh link's bootstrap answers. */
        memset(s_pend, 0, sizeof(s_pend));
        state_set_link_state(id, LINK_REACHABLE_IDLE, false, test_now);
        client_message(id, true);
        arb_message(ARB_APP_DEVREQ, id);
        assert(!s_pend[id].devreq_deadline_us);
        cases++;

        /* Phone leaves before expiry: nothing is refreshed. */
        memset(s_pend, 0, sizeof(s_pend));
        state_set_link_state(id, LINK_UP, true, test_now);
        client_message(id, true);
        arb_message(ARB_APP_DEVREQ, id);
        client_message(id, false);
        assert(!s_pend[id].devreq_deadline_us && !s_pend[id].devreq_refreshed);
        before = ble_queue_sends;
        test_now += 3000000;
        arb_message(ARB_SETTINGS, id);
        assert(!s_pend[id].devreq_refreshed);
        cases++;

        /* Link lost before expiry: no refresh (the attach driver owns it). */
        memset(s_pend, 0, sizeof(s_pend));
        state_set_link_state(id, LINK_UP, true, test_now);
        client_message(id, true);
        arb_message(ARB_APP_DEVREQ, id);
        state_set_link_state(id, LINK_REACHABLE_IDLE, false, test_now);
        test_now += 2000000;
        s_pend[id].link_wait_deadline_us = 0;      /* keep the link-up guard out */
        arb_message(ARB_SETTINGS, id);
        assert(!s_pend[id].devreq_deadline_us && !s_pend[id].devreq_refreshed);
        cases++;
        client_message(id, false);
    }
    printf("PASS: stage D2 fast first retry and held-link 0x97 refresh, %u production arbiter cases\n", cases);
    return 0;
}
