#define main previous_test_main
#include "../../../tools/host_test_app_edges.c"
#undef main

int main(void)
{
    state_cache_init();
    test_now = 100000000;
    state_set_link_state(1, LINK_UP, true, test_now);
    bms_request_t app = {.bms_id=1,.kind=TXN_RAW_WRITE,.source=SRC_APP,.payload_len=20};
    bms_request_t mqtt = {.bms_id=1,.kind=TXN_BALANCE_WRITE,.source=SRC_MQTT,.payload_len=20};
    s_pend[1].busy = true;
    assert(ring_push(&s_pend[1], &app));
    assert(ring_push(&s_pend[1], &mqtt));
    incoming = (arb_msg_t){.kind=ARB_CLEAR,.bms_id=1};
    message_once();
    assert(!s_pend[1].count);
    puts("REPRODUCED: link-up clear discards queued app and MQTT writes");

    memset(s_pend,0,sizeof(s_pend)); ble_queue_sends=0;
    assert(ring_push(&s_pend[1], &app)); dispatch(1);
    uint16_t current = ble_queued.cmd_id;
    assert(s_pend[1].busy && ble_queue_sends==1);
    assert(ring_push(&s_pend[1], &app));
    bms_response_t stale={.bms_id=1,.cmd_id=(uint16_t)(current-1),.status=RESP_OK};
    on_response(&stale);
    assert(s_pend[1].busy && ble_queue_sends==2);
    puts("REPRODUCED: stale cmd_id releases busy and dispatches another request");

    memset(s_pend,0,sizeof(s_pend)); ble_queue_sends=0;
    state_set_app_connected(1,true,test_now);
    s_pend[1].busy=true; assert(ring_push(&s_pend[1], &app));
    on_app_conn(1,false);
    assert(s_pend[1].count==1 && !runtime(1).app_connected);
    s_pend[1].busy=false; dispatch(1);
    assert(ble_queue_sends==1 && ble_queued.source==SRC_APP);
    puts("REPRODUCED: queued app write dispatches after phone disconnect");

    memset(s_pend,0,sizeof(s_pend)); ble_queue_sends=0;
    state_set_app_connected(1,true,test_now);
    assert(ring_push(&s_pend[1], &mqtt)); dispatch(1);
    assert(ble_queue_sends==1 && ble_queued.kind==TXN_BALANCE_WRITE);
    puts("REPRODUCED: previously admitted MQTT write dispatches during app ownership");
    return 0;
}
