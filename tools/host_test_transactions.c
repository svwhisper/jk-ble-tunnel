/* Uses production BLE callbacks, submit/timeout gates and finite host stubs.
 * No radio/network/USB/BMS operations are possible in these adapters. */
#define main discovery_suite_main
#include "host_test_discovery.c"
#undef main

static unsigned cases;
static link_t *ready(void)
{
    link_t *l=setup(7);
    l->table_ready=true; l->txn_active=false;
    l->val_handle=10; l->ffe2_handle=20;
    l->ffe1_props=l->ffe2_props=BLE_GATT_CHR_PROP_WRITE;
    frame_notes=frame_copies=0;
    test_runtime[1]=(bms_runtime_t){.link=LINK_UP,.link_held=true};
    allow_no_rsp=true;
    return l;
}
static bms_request_t request(txn_kind_t kind, bms_cmd_id_t id)
{
    return (bms_request_t){.bms_id=1,.cmd_id=id,.kind=kind,.source=SRC_APP,
        .opcode=JK_CMD_DEVICE_INFO,.payload_len=20,.response_needed=true,
        .timeout_ms=3000};
}
static void notify_bytes(const uint8_t *data, uint16_t len)
{
    rx_bytes=data;
    struct os_mbuf om={.len=len};
    struct ble_gap_event e={.type=BLE_GAP_EVENT_NOTIFY_RX,
        .notify_rx={.conn_handle=7,.om=&om}};
    gap_event(&e,NULL);
}
static void frame(const uint8_t *data, uint16_t len)
{
    notify_bytes(data,128); notify_bytes(data+128,len-128);
}
static uint8_t race_frame[JK_FRAME_MAX];
static int race_len;
static void *frame_worker(void *unused)
{ (void)unused; notify_bytes(race_frame+128,race_len-128); return NULL; }
static void *ack_worker(void *arg) { ack(arg); return NULL; }

int main(void)
{
    s_mtx_link_pool=xSemaphoreCreateMutex();
    uint8_t cell[JK_FRAME_MAX], dev[JK_FRAME_MAX], settings[JK_FRAME_MAX];
    int nc=synth_cell_info(cell,sizeof(cell),1);
    int nd=synth_device_info(dev,sizeof(dev),"TEST BMS");
    memcpy(settings,cell,nc); settings[4]=JK_REC_SETTINGS;
    unsigned sum=0; for (int i=0;i<nc-1;i++) sum+=settings[i];
    settings[nc-1]=(uint8_t)sum;
    assert(nc==300 && nd==300);

    /* Cell/settings telemetry must not complete device-info, but must still
     * reach both frame consumers and frame-evidence accounting. */
    link_t *l=ready(); bms_request_t r=request(TXN_POLL,UINT64_C(0x100000001));
    exec_request(&r); frame(cell,nc); frame(settings,nc);
    assert(l->txn_active && responses==0 && frame_notes==2 && frame_copies==4);
    ack(l); assert(l->txn_active && responses==0); /* ATT ACK isn't JK reply */
    frame(dev,nd);
    assert(!l->txn_active && responses==1 && last_response.cmd_id==r.cmd_id);
    assert(last_response.record==JK_REC_DEVICE_INFO);
    frame(dev,nd); assert(responses==1); cases++;

    l=ready(); r=request(TXN_POLL,2); r.opcode=JK_CMD_CELL_INFO;
    exec_request(&r); frame(dev,nd); frame(settings,nc);
    assert(l->txn_active && responses==0);
    frame(cell,nc); assert(l->txn_active && responses==0);
    ack(l); assert(responses==1 && last_response.record==JK_REC_CELL_INFO); cases++;

    l=ready(); r=request(TXN_POLL,3); exec_request(&r);
    assert(write_cb); write_cb(7,&error,NULL,write_arg);
    assert(!l->txn_active && responses==1 && last_response.status==RESP_GATT_ERR);
    frame(dev,nd); assert(responses==1); cases++;

    /* Write op follows discovered properties, not the phone's ATT preference.
     * Write Requests wait for their own ACK; Commands report submission only. */
    for (unsigned kind=TXN_RAW_WRITE; kind<=TXN_BALANCE_WRITE; kind++)
    for (unsigned idx=0; idx<2; idx++)
    for (unsigned props=0; props<4; props++)
    for (unsigned wanted=0; wanted<2; wanted++)
    for (unsigned failure=0; failure<2; failure++) {
        l=ready(); r=request((txn_kind_t)kind,100+cases);
        r.idx=idx; r.response_needed=wanted;
        l->ffe1_props=l->ffe2_props=((props&1) ? BLE_GATT_CHR_PROP_WRITE : 0) |
                                   ((props&2) ? BLE_GATT_CHR_PROP_WRITE_NO_RSP : 0);
        bool ffe2=kind==TXN_RAW_WRITE && idx==1;
        bool no_rsp=ffe2 ? !(!(props&2) && (props&1)) : (!(props&1) && (props&2));
        write_rc=failure ? BLE_HS_ENOTCONN : 0;
        exec_request(&r);
        assert(written_handle==(kind==TXN_RAW_WRITE && idx==1 ? 20 : 10));
        if (failure || no_rsp) {
            assert(!l->txn_active && responses==1);
            assert(last_response.status==(failure ? RESP_GATT_ERR : RESP_OK));
        } else {
            assert(l->txn_active && responses==0);
            ack(l); assert(!l->txn_active && responses==1 && last_response.status==RESP_OK);
        }
        assert(last_response.cmd_id==r.cmd_id);
        cases++;
    }

    /* A late callback cannot acknowledge a replacement transaction, including
     * a reused slot and connection handle. No heap callback contexts to leak. */
    for (unsigned reuse=0; reuse<2; reuse++) {
        l=ready(); r=request(TXN_RAW_WRITE,40); exec_request(&r);
        void *old_arg=write_arg; ble_gatt_attr_fn *old_cb=write_cb;
        assert(old_arg && old_cb);
        test_now=l->txn_deadline_us+1; sweep_timeouts();
        assert(responses==1 && last_response.status==RESP_TIMEOUT);
        if (reuse) l=ready();
        else {
            bms_request_t overlap=request(TXN_RAW_WRITE,41); exec_request(&overlap);
            assert(responses==2 && last_response.status==RESP_REJECTED && l->txn.cmd_id==40);
            struct ble_gatt_attr old_attr={.handle=10};
            old_cb(7,&ok,&old_attr,old_arg);
            assert(responses==2); /* retire procedure, no second completion */
            responses=0;
        }
        r.cmd_id=41; exec_request(&r);
        old_cb(7,&ok,NULL,old_arg);
        assert(l->txn_active && responses==0 && l->txn.cmd_id==41);
        ack(l); assert(responses==1 && last_response.cmd_id==41);
        old_cb(7,&error,NULL,old_arg); assert(responses==1); cases++;
    }

    for (unsigned kind=TXN_POLL; kind<=TXN_DISCONNECT; kind++) {
        l=ready(); r=request(TXN_RAW_WRITE,50); exec_request(&r);
        unsigned writes=write_calls;
        bms_request_t extra=request((txn_kind_t)kind,51); exec_request(&extra);
        assert(l->txn_active && l->txn.cmd_id==50 && write_calls==writes && !terminate_calls);
        assert(responses==1 && last_response.cmd_id==51 && last_response.status==RESP_REJECTED);
        ack(l); assert(responses==2 && last_response.cmd_id==50); cases++;
    }

    /* Missing/wrong ACK metadata never means success. */
    for (unsigned malformed=0; malformed<3; malformed++) {
        l=ready(); r=request(TXN_RAW_WRITE,60); exec_request(&r);
        struct ble_gatt_attr a={.handle=malformed==2 ? 99 : 10};
        write_cb(7,malformed==0 ? NULL : &ok,malformed==1 ? NULL : &a,write_arg);
        assert(responses==1 && last_response.status==RESP_GATT_ERR && !l->txn_active); cases++;
    }
    l=ready(); r=request(TXN_POLL,70); r.opcode=0x55; exec_request(&r);
    assert(!write_calls && responses==1 && last_response.status==RESP_REJECTED); cases++;
    l=ready(); l->ffe1_props=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    r=request(TXN_POLL,71); exec_request(&r);
    assert(l->txn_active && !write_cb && !responses);
    frame(dev,nd); assert(!l->txn_active && responses==1 && last_response.status==RESP_OK); cases++;
    l=ready(); l->ffe2_handle=0; r=request(TXN_RAW_WRITE,72); r.idx=1; exec_request(&r);
    assert(written_handle==10 && l->txn_active && !responses);
    ack(l); assert(responses==1 && last_response.status==RESP_OK); cases++;

    /* Actual GAP notify versus actual timeout: exactly one terminal result. */
    race_len=synth_device_info(race_frame,sizeof(race_frame),"RACE");
    for (unsigned i=0;i<1000;i++) {
        l=ready(); r=request(TXN_POLL,80+i); exec_request(&r); ack(l);
        notify_bytes(race_frame,128); test_now=l->txn_deadline_us+1;
        pthread_t a,b;
        assert(pthread_create(&a,NULL,i&1 ? frame_worker : timeout_worker,l)==0);
        assert(pthread_create(&b,NULL,i&1 ? timeout_worker : frame_worker,l)==0);
        assert(pthread_join(a,NULL)==0 && pthread_join(b,NULL)==0);
        assert(responses==1 && !l->txn_active && !terminate_calls);
        assert(last_response.status==RESP_OK || last_response.status==RESP_TIMEOUT);
        assert(frame_notes==1 && frame_copies==2); cases++;
    }
    /* Data before ACK cannot release the gate; ACK/deadline races still
     * produce only one result. A timeout leaves the ATT lease until callback. */
    for (unsigned i=0;i<1000;i++) {
        l=ready(); r=request(TXN_POLL,2000+i); exec_request(&r); frame(dev,nd);
        assert(l->txn_active && responses==0 && l->write_cookie);
        test_now=l->txn_deadline_us+1;
        pthread_t a,b;
        assert(pthread_create(&a,NULL,i&1 ? ack_worker : timeout_worker,l)==0);
        assert(pthread_create(&b,NULL,i&1 ? timeout_worker : ack_worker,l)==0);
        assert(pthread_join(a,NULL)==0 && pthread_join(b,NULL)==0);
        assert(responses==1 && !l->txn_active && !l->write_cookie && !terminate_calls);
        assert(last_response.status==RESP_OK || last_response.status==RESP_TIMEOUT); cases++;
    }
    /* Diagnostic publication cannot hold the pool mutex added to notify RX. */
    l=ready(); allow_raw_publish=true; ble_owner_rawcap(1); frame(dev,nd);
    assert(raw_publishes==2 && frame_notes==1 && !responses);
    test_now+=1000001; frame(dev,nd); assert(raw_publishes==2); cases++;

    /* Cookie exhaustion must not reuse an old callback token. Command writes
     * do not need a token and retain their submission-only result. */
    l=ready(); s_write_cookie=UINT32_MAX;
    r=request(TXN_RAW_WRITE,90); exec_request(&r);
    assert(!write_calls && !l->txn_active && responses==1 && last_response.status==RESP_REJECTED);
    l->ffe1_props=BLE_GATT_CHR_PROP_WRITE_NO_RSP; r.cmd_id=91; exec_request(&r);
    assert(write_calls==1 && responses==2 && last_response.status==RESP_OK && !l->write_cookie);
    cases++;
    printf("PASS: %u production transaction/frame/callback/timeout cases\n",cases);
    return 0;
}
