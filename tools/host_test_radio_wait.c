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

    /* Stage D3: a device-info header in a chunk forwarded to an attached phone
     * notifies the arbiter (outside the mutex), wherever it sits in the chunk
     * and whether or not reassembly completes a frame. Other record types,
     * or no phone attached, do not. */
    const uint8_t at_hb[4] = { 'A', 'T', '\r', '\n' };
    const uint8_t devhdr[5] = { 0x55, 0xAA, 0xEB, 0x90, 0x03 };
    const uint8_t cellhdr[5] = { 0x55, 0xAA, 0xEB, 0x90, 0x02 };
    unsigned d3 = 0;
    for (unsigned attached = 0; attached < 2; attached++) {
        for (unsigned kind = 0; kind < 3; kind++) {           /* devinfo, cells, AT only */
            for (unsigned pos = 0; pos < 40; pos += 13) {
                link_t *l = setup(7);
                l->txn_active = false; l->table_ready = true;
                test_runtime[1].app_connected = attached;
                uint8_t chunk[64]; memset(chunk, 0x11, sizeof(chunk));
                memcpy(chunk + pos, at_hb, sizeof(at_hb));
                if (kind == 0) memcpy(chunk + pos + 4, devhdr, sizeof(devhdr));
                if (kind == 1) memcpy(chunk + pos + 4, cellhdr, sizeof(cellhdr));
                rx_bytes = chunk; capture_raw = true; raw_len = 0; devinfo_notes = 0;
                struct os_mbuf om = { .len = sizeof(chunk) };
                struct ble_gap_event ev = { .type = BLE_GAP_EVENT_NOTIFY_RX,
                    .notify_rx = { .conn_handle = 7, .om = &om } };
                gap_event(&ev, NULL);
                bool want = attached && kind == 0;
                assert(devinfo_notes == (want ? 1u : 0u));
                if (want) assert(devinfo_last_id == 1);
                assert(raw_len == (attached ? sizeof(chunk) : 0u));
                d3++;
            }
        }
    }
    test_runtime[1].app_connected = false; capture_raw = false;
    printf("PASS: stage D3 forwarded device-info header -> arbiter note, %u cases\n", d3);
    return 0;
}
