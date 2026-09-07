#define main previous_test_main
#include "../../../tools/host_test_discovery.c"
#undef main

int main(void)
{
    s_mtx_link_pool=xSemaphoreCreateMutex();
    link_t *l=setup(7); l->table_ready=true;
    l->txn.kind=TXN_POLL; l->txn.opcode=JK_CMD_DEVICE_INFO;
    uint8_t data[JK_FRAME_MAX]; int n=synth_cell_info(data,sizeof(data),1);
    on_complete_frame(l,data,n);
    assert(!l->txn_active && responses==1 && last_response.status==RESP_OK);
    assert(last_response.record==JK_REC_CELL_INFO);
    puts("REPRODUCED: CELL record completes DEVICE_INFO poll");

    l=setup(7); l->table_ready=true; l->val_handle=10;
    l->txn.kind=TXN_RAW_WRITE; l->txn.cmd_id=42;
    /* Old ATT completion arrives after request42 timed out and43 started. */
    l->txn.cmd_id=43;
    on_gatt_write_done(7,&ok,NULL,NULL);
    assert(!l->txn_active && last_response.cmd_id==43);
    puts("REPRODUCED: untagged late ATT completion acknowledges replacement transaction43");

    l=setup(7); l->table_ready=true; l->val_handle=10; l->ffe2_handle=20;
    l->ffe2_props=BLE_GATT_CHR_PROP_WRITE;
    write_rc=BLE_HS_ENOTCONN;
    bms_request_t r={.bms_id=1,.cmd_id=45,.kind=TXN_RAW_WRITE,.idx=1,.payload_len=20};
    exec_request(&r);
    assert(last_response.cmd_id==45 && last_response.status==RESP_OK);
    puts("REPRODUCED: FFE2 submission ENOTCONN reported RESP_OK");
    return 0;
}
