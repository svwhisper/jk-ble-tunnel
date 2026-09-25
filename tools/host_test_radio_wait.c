/* R4a on the live R1c baseline, updated for stage D1 direct connect:
 * exercise production start_connect without importing later GAP-token or
 * owned-command stages. No hardware I/O. */
#define main discovery_test_main
#include "host_test_discovery.c"
#undef main

int main(void)
{
    s_mtx_link_pool=xSemaphoreCreateMutex();
    assert(s_mtx_link_pool);
    /* resource 1: another bank's connect in flight; 2: operator scan dump. */
    for (unsigned resource=1; resource<3; resource++) {
        link_t *requester=setup(0), *owner=link_alloc(2);
        assert(owner);
        s_ble_enabled=true;
        s_scan_active=resource==2;
        s_conn_inflight=resource==1?owner:NULL;
        link_t saved=*owner;
        scan_calls=connect_calls=0;
        allow_connect=false;
        xSemaphoreTake(s_mtx_link_pool,portMAX_DELAY);
        start_connect(requester);
        xSemaphoreGive(s_mtx_link_pool);
        assert(responses==1 && last_response.status==RESP_CONNECT_WAIT);
        assert(last_response.cmd_id==42 && !requester->in_use && !requester->txn_active);
        assert(!scan_calls && !connect_calls && !state_calls && !terminate_calls);
        assert(!memcmp(owner,&saved,sizeof(saved)));
        assert(s_conn_inflight==(resource==1?owner:NULL));
        assert(s_scan_active==(resource==2));
        s_conn_inflight=NULL;
        s_scan_active=false;
        link_t *retry=link_alloc(1);
        assert(retry);
        retry->txn_active=true;
        retry->txn=(bms_request_t){.bms_id=1,.kind=TXN_CONNECT,.cmd_id=43};
        allow_connect=true;
        xSemaphoreTake(s_mtx_link_pool,portMAX_DELAY);
        start_connect(retry);
        xSemaphoreGive(s_mtx_link_pool);
        assert(s_conn_inflight==retry && connect_calls==1 && !scan_calls && responses==1);

        /* The bank is never heard: NimBLE completes the 5 s connect as a
         * failed CONNECT event. It must end exactly as an empty scan window
         * did: unreachable, LINK_DOWN to the arbiter, slot freed. */
        struct ble_gap_event timeout={.type=BLE_GAP_EVENT_CONNECT,
            .connect={.status=BLE_HS_ETIMEOUT}};
        gap_event(&timeout,retry);
        assert(!s_conn_inflight && !retry->in_use && !retry->txn_active);
        assert(responses==2 && last_response.cmd_id==43);
        assert(last_response.status==RESP_LINK_DOWN);
        assert(state_calls==1 && last_state==LINK_UNREACHABLE);
        assert(!terminate_calls);
    }
    puts("PASS: 2 production radio-contention scenarios; direct connect targets the public address; connect timeout -> unreachable");
    return 0;
}
