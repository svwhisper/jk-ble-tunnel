/* R4a on the live R1c baseline: exercise production start_connect without
 * importing later GAP-token or owned-command stages. No hardware I/O. */
#define main discovery_test_main
#include "host_test_discovery.c"
#undef main

int main(void)
{
    s_mtx_link_pool=xSemaphoreCreateMutex();
    assert(s_mtx_link_pool);
    for (unsigned resource=0; resource<3; resource++) {
        link_t *requester=setup(0), *owner=link_alloc(2);
        assert(owner);
        s_ble_enabled=true;
        s_scan_active=resource==2;
        s_connecting=resource==0?owner:NULL;
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
        assert(s_connecting==(resource==0?owner:NULL));
        assert(s_conn_inflight==(resource==1?owner:NULL));
        assert(s_scan_active==(resource==2));
        s_connecting=s_conn_inflight=NULL;
        s_scan_active=false;
        link_t *retry=link_alloc(1);
        assert(retry);
        retry->txn_active=true;
        retry->txn=(bms_request_t){.bms_id=1,.kind=TXN_CONNECT,.cmd_id=43};
        allow_connect=true;
        xSemaphoreTake(s_mtx_link_pool,portMAX_DELAY);
        start_connect(retry);
        xSemaphoreGive(s_mtx_link_pool);
        assert(s_connecting==retry && scan_calls==1 && responses==1);
    }
    puts("PASS: 3 production radio-contention/release scenarios");
    return 0;
}
