/* Production callbacks, timeout sweep and request gate with simulated NimBLE
 * procedure results. No radio/network/serial I/O exists in these adapters. */
#include <assert.h>
#include "synth_frames.h"
#include "../node_a/main/ble_owner.c"

static int response_token, notify_token, decode_token;
QueueHandle_t g_q_bms_response = &response_token;
QueueHandle_t g_q_arb_in, g_q_bms_request;
QueueHandle_t g_q_notify = &notify_token, g_q_decode = &decode_token;
EventGroupHandle_t g_evt;
struct ble_hs_cfg_stub ble_hs_cfg;
static int64_t test_now;
static int svc_rc, chr_rc, write_rc, optional_write_rc, mtu_rc, terminate_rc;
static unsigned svc_calls, chr_calls, write_calls, terminate_calls, responses, state_calls;
static bms_response_t last_response;
static tunnel_link_state_t last_state;
static bool last_held;
static void *svc_arg, *chr_arg;
static ble_gatt_disc_svc_fn *svc_cb;
static ble_gatt_chr_fn *chr_cb;
static ble_gatt_attr_fn *write_cb;
static void *write_arg;
static ble_gatt_dsc_fn *dsc_cb;
static void *dsc_arg;
static uint16_t dsc_start, dsc_end, written_handle, optional_written_handle;
static unsigned dsc_calls;
static unsigned frame_notes, frame_copies;
static int64_t frame_time;
static bms_runtime_t test_runtime[CFG_NUM_UNITS];
static int dsc_rc, optional_dsc_rc;
static const struct ble_gatt_error ok = {0}, done = { .status = BLE_HS_EDONE },
                                   error = { .status = 5 };
int64_t esp_timer_get_time(void) { return test_now; }
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)ticks;
    if (q == g_q_notify || q == g_q_decode) {
        assert(!((const notify_item_t *)item)->raw);
        frame_copies++;
        return pdTRUE;
    }
    assert(q == g_q_bms_response);
    last_response = *(const bms_response_t *)item; responses++; return pdTRUE;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b)
{ (void)g; return b; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b)
{ (void)g; (void)b; return 0; }
void state_set_link_state(uint8_t id, tunnel_link_state_t st, bool held, int64_t seen)
{ assert(id < CFG_NUM_UNITS); (void)seen; last_state = st; last_held = held; state_calls++; }
void state_note_frame(uint8_t id, int64_t now)
{ assert(id == 1); frame_notes++; frame_time = now; }
void state_get_runtime(uint8_t id, bms_runtime_t *out)
{ assert(id < CFG_NUM_UNITS); *out = test_runtime[id]; }
int ble_gattc_exchange_mtu(uint16_t ch, ble_gatt_mtu_fn *cb, void *arg)
{ (void)ch; assert(!cb && !arg); return mtu_rc; }
int ble_gattc_disc_svc_by_uuid(uint16_t ch, const ble_uuid_t *uuid, ble_gatt_disc_svc_fn *cb, void *arg)
{ (void)ch; assert(ble_uuid_u16(uuid) == JK_SVC_UUID); svc_calls++; svc_cb = cb; svc_arg = arg; return svc_rc; }
int ble_gattc_disc_all_chrs(uint16_t ch, uint16_t start, uint16_t end, ble_gatt_chr_fn *cb, void *arg)
{ (void)ch; assert(start == 1 && end == 30); chr_calls++; chr_cb = cb; chr_arg = arg; return chr_rc; }
int ble_gattc_disc_all_dscs(uint16_t ch, uint16_t start, uint16_t end, ble_gatt_dsc_fn *cb, void *arg)
{
    (void)ch; assert(start < end);
    dsc_calls++; dsc_start = start; dsc_end = end; dsc_cb = cb; dsc_arg = arg;
    return start == 20 ? optional_dsc_rc : dsc_rc;
}
int ble_gattc_write_flat(uint16_t ch, uint16_t handle, const void *value, uint16_t len, ble_gatt_attr_fn *cb, void *arg)
{
    (void)ch; (void)handle; (void)value; (void)len;
    if (cb) { write_cb = cb; write_arg = arg; written_handle = handle; }
    else optional_written_handle = handle;
    write_calls++; return handle == 21 ? optional_write_rc : write_rc;
}
int ble_gattc_write_no_rsp_flat(uint16_t ch, uint16_t h, const void *v, uint16_t n)
{ (void)ch; (void)h; (void)v; (void)n; assert(0); return 0; }
int ble_gap_terminate(uint16_t ch, uint8_t reason)
{ (void)ch; assert(reason == BLE_ERR_REM_USER_CONN_TERM); terminate_calls++; return terminate_rc; }
int ble_gap_conn_find(uint16_t ch, struct ble_gap_conn_desc *d)
{ (void)ch; (void)d; return BLE_HS_ENOTCONN; }
int ble_gap_disc_cancel(void) { return 0; }
int ble_gap_disc(uint8_t a, int32_t t, const struct ble_gap_disc_params *p, ble_gap_event_fn *cb, void *arg)
{ (void)a; (void)t; (void)p; (void)cb; (void)arg; assert(0); return 0; }
int ble_gap_connect(uint8_t a, const ble_addr_t *b, int32_t t, const struct ble_gap_conn_params *p, ble_gap_event_fn *cb, void *arg)
{ (void)a; (void)b; (void)t; (void)p; (void)cb; (void)arg; assert(0); return 0; }
int ble_hs_mbuf_to_flat(const struct os_mbuf *o, void *p, uint16_t n, uint16_t *out)
{ (void)o; (void)p; (void)n; (void)out; assert(0); return 0; }
void mqtt_publish_llevent(const char *k, uint8_t id, int r)
{
    (void)k; (void)id; (void)r;
    assert(pthread_mutex_trylock(s_mtx_link_pool) == 0);
    assert(pthread_mutex_unlock(s_mtx_link_pool) == 0);
}
void mqtt_publish_raw(uint8_t id, const uint8_t *d, uint16_t n) { (void)id; (void)d; (void)n; assert(0); }
bool net_wifi_up(void) { return true; }
int64_t net_wifi_down_ms(void) { return 0; }

static link_t *setup(uint16_t ch)
{
    memset(s_links, 0, sizeof(s_links));
    s_connecting = s_conn_inflight = NULL;
    svc_rc = chr_rc = write_rc = optional_write_rc = mtu_rc = terminate_rc = 0;
    svc_calls = chr_calls = write_calls = terminate_calls = responses = state_calls = 0;
    write_cb = NULL; write_arg = NULL;
    dsc_cb = NULL; dsc_arg = NULL; dsc_calls = 0;
    dsc_rc = optional_dsc_rc = 0; written_handle = optional_written_handle = 0;
    last_held = false; last_state = LINK_REACHABLE_IDLE;
    test_now = 1000000;
    link_t *l = link_alloc(1);
    assert(l);
    l->conn_handle = ch;
    l->txn_active = true;
    l->txn = (bms_request_t){ .bms_id = 1, .kind = TXN_CONNECT, .cmd_id = 42 };
    l->txn_deadline_us = test_now + 9000000;
    return l;
}
static void start(link_t *l)
{
    xSemaphoreTake(s_mtx_link_pool, portMAX_DELAY);
    discovery_start(l);
    xSemaphoreGive(s_mtx_link_pool);
}
static void service(link_t *l)
{
    struct ble_gatt_svc s = { .start_handle = 1, .end_handle = 30,
                             .uuid.u16 = BLE_UUID16_INIT(JK_SVC_UUID) };
    svc_cb(l->conn_handle, &ok, &s, svc_arg);
}
static void characteristic(link_t *l, uint16_t uuid, uint16_t handle)
{
    struct ble_gatt_chr c = { .def_handle = handle - 1, .val_handle = handle,
        .properties = BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_NOTIFY,
        .uuid.u16 = BLE_UUID16_INIT(uuid) };
    chr_cb(l->conn_handle, &ok, &c, chr_arg);
}
static void failed(link_t *l, resp_status_t status)
{
    assert(l->discovery_failed && !l->discovery_pending && !l->table_ready);
    assert(!l->txn_active && !l->val_handle && !last_held);
    assert(responses == 1 && last_response.cmd_id == 42 && last_response.status == status);
    assert(terminate_calls == 1);
}
static void descriptor(uint16_t ch, uint16_t value, uint16_t handle, uint16_t uuid, void *arg)
{
    struct ble_gatt_dsc d = { .handle = handle, .uuid.u16 = BLE_UUID16_INIT(uuid) };
    dsc_cb(ch, &ok, value, &d, arg);
}
static void finish_characteristics(link_t *l)
{
    chr_cb(l->conn_handle, &done, NULL, chr_arg);
    /* Default emulator layout. Dedicated cases below drive non-adjacent and
     * missing descriptors directly through the captured production callback. */
    for (unsigned i = 0; i < 2 && dsc_cb && !write_cb; i++) {
        uint16_t value = dsc_start;
        void *arg = dsc_arg;
        descriptor(l->conn_handle, value, value + 1, 0x2902, arg);
        dsc_cb(l->conn_handle, &done, value, NULL, arg);
    }
}
static void *complete_worker(void *arg)
{
    link_t *l = arg;
    finish_characteristics(l);
    if (write_cb) {
        struct ble_gatt_attr attr = { .handle = written_handle };
        write_cb(l->conn_handle, &ok, &attr, write_arg);
    }
    return NULL;
}
static void ack(link_t *l)
{
    struct ble_gatt_attr attr = { .handle = written_handle };
    assert(write_cb);
    write_cb(l->conn_handle, &ok, &attr, write_arg);
}
static void *timeout_worker(void *arg)
{ (void)arg; sweep_timeouts(); return NULL; }
static int test_idle_fence(void)
{
    link_t *l = setup(7);
    l->table_ready = true; l->val_handle = 10; l->txn_active = false;
    test_runtime[1] = (bms_runtime_t){ .app_connected = true, .link_held = true,
                                     .link = LINK_UP, .idle_epoch = 8 };
    /* Request waited in either queue while the phone acquired this bank. */
    bms_request_t r = { .bms_id = 1, .cmd_id = 77, .kind = TXN_DISCONNECT,
                       .source = SRC_INTERNAL, .idle_only = true, .idle_epoch = 7 };
    exec_request(&r);
    if (terminate_calls || responses != 1 || last_response.status != RESP_REJECTED) {
        fprintf(stderr, "FAIL: stale idle release terminated=%u responses=%u status=%d\n",
                terminate_calls, responses, last_response.status);
        return 1;
    }
    unsigned cases = 1;
    const uint64_t epochs[] = {0, 1, UINT32_MAX, (uint64_t)UINT32_MAX + 1, UINT64_MAX};
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++)
    for (unsigned held = 0; held < 2; held++)
    for (unsigned app = 0; app < 2; app++)
    for (unsigned source = SRC_APP; source <= SRC_INTERNAL; source++)
    for (unsigned kind = TXN_POLL; kind <= TXN_DISCONNECT; kind++)
    for (unsigned e = 0; e < sizeof(epochs) / sizeof(epochs[0]); e++)
    for (unsigned match = 0; match < 2; match++) {
        l = setup(7); l->bms_id = id;
        l->table_ready = true; l->val_handle = 10; l->txn_active = false;
        test_runtime[id] = (bms_runtime_t){ .app_connected = app, .link_held = held,
                                          .link = held ? LINK_UP : LINK_REACHABLE_IDLE,
                                          .idle_epoch = epochs[e] };
        r = (bms_request_t){ .bms_id = id, .cmd_id = 77, .kind = kind,
                            .source = source, .idle_only = true,
                            .idle_epoch = match ? epochs[e] : epochs[e] ^ 1u };
        link_t before = *l;
        exec_request(&r);
        bool allowed = held && !app && source == SRC_INTERNAL &&
                       kind == TXN_DISCONNECT && match;
        assert(responses == 1 && last_response.bms_id == id && last_response.cmd_id == 77);
        assert(last_response.status == (allowed ? RESP_OK : RESP_REJECTED));
        assert(terminate_calls == (unsigned)allowed && write_calls == 0);
        assert(memcmp(l, &before, sizeof(before)) == 0);
        cases++;
    }
    /* Explicit bounces are NOT idle releases: leave their policy unchanged. */
    l = setup(7); l->table_ready = true; l->val_handle = 10; l->txn_active = false;
    test_runtime[1] = (bms_runtime_t){ .app_connected = true, .idle_epoch = 99 };
    r = (bms_request_t){ .bms_id = 1, .cmd_id = 78, .kind = TXN_DISCONNECT,
                        .source = SRC_INTERNAL };
    exec_request(&r);
    assert(terminate_calls == 1 && responses == 1 && last_response.status == RESP_OK);
    cases++;
    memset(test_runtime, 0, sizeof(test_runtime));
    printf("PASS: final BLE idle fence, %u stale/app/link/type/source/64-bit epoch cases\n", cases);
    return 0;
}

int main(void)
{
    s_mtx_link_pool = xSemaphoreCreateMutex();
    if (test_idle_fence()) return 1;
    /* Immediate start failure, errors, missing service/characteristic. */
    link_t *l = setup(7); svc_rc = 6; start(l); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); svc_cb(7, &error, NULL, svc_arg); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); svc_cb(7, NULL, NULL, svc_arg); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); svc_cb(7, &done, NULL, svc_arg); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); chr_rc = 6; service(l); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); service(l); chr_cb(7, &error, NULL, chr_arg); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); service(l); finish_characteristics(l); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, UINT16_MAX);
    finish_characteristics(l); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10); write_rc = 6;
    finish_characteristics(l); failed(l, RESP_GATT_ERR);

    /* Normal discovery (including optional FFE2) remains one successful txn.
     * Duplicate and stale procedure completions must not resubscribe/respond. */
    for (unsigned extra = 0; extra < 2; extra++) {
        l = setup(7); mtu_rc = 6; start(l); service(l); service(l);
        assert(chr_calls == 1);
        svc_cb(7, &done, NULL, svc_arg);
        characteristic(l, JK_CHR_UUID, 10);
        assert(!l->table_ready && responses == 0);
        if (extra) { characteristic(l, JK_CHR2_UUID, 20); optional_write_rc = 5; }
        finish_characteristics(l);
        assert(!l->table_ready && !last_held && responses == 0);
        assert(write_cb && write_calls == 1 + extra);
        finish_characteristics(l); /* duplicate before ack must do nothing */
        assert(write_calls == 1 + extra && responses == 0);
        ack(l);
        ack(l); /* duplicate acknowledgement must not produce a second result */
        assert(l->table_ready && !l->discovery_pending && last_held && last_state == LINK_UP);
        assert(responses == 1 && last_response.status == RESP_OK && write_calls == 1 + extra);
        finish_characteristics(l); svc_cb(7, &error, NULL, svc_arg);
        assert(responses == 1 && terminate_calls == 0 && write_calls == 1 + extra);
    }

    /* Deadline, one response, bounded terminate retries, no healthy-link kill. */
    l = setup(0); start(l); test_now = l->txn_deadline_us; sweep_timeouts();
    assert(responses == 0);
    test_now++; terminate_rc = 6; sweep_timeouts(); failed(l, RESP_TIMEOUT);
    void *old_arg = svc_arg;
    assert(l->in_use); sweep_timeouts(); assert(terminate_calls == 1);
    test_now += 1000000; terminate_rc = BLE_HS_EALREADY; sweep_timeouts();
    assert(terminate_calls == 2 && responses == 1 && l->in_use);
    svc_cb(0, &done, NULL, old_arg); assert(responses == 1);
    test_now += 1000000; terminate_rc = BLE_HS_ENOTCONN; sweep_timeouts();
    assert(l->in_use && responses == 1); /* ENOTCONN may precede GAP callback */
    struct ble_gap_event lost = { .type = BLE_GAP_EVENT_DISCONNECT,
                                  .disconnect.conn.conn_handle = 0 };
    gap_event(&lost, l);
    assert(!l->in_use && responses == 1);

    /* Same handle and same slot reused: old generation cannot fail new work. */
    l = setup(0); start(l); on_svc_disc(0, &error, NULL, old_arg);
    assert(l->discovery_pending && responses == 0);
    on_svc_disc(99, &error, NULL, svc_arg); assert(responses == 0);
    struct ble_gap_event e = { .type = BLE_GAP_EVENT_DISCONNECT,
                               .disconnect.conn.conn_handle = 0 };
    gap_event(&e, NULL);
    assert(!l->in_use && responses == 1 && last_response.status == RESP_LINK_DOWN);
    on_svc_disc(0, &error, NULL, svc_arg); assert(responses == 1);

    l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
    bms_request_t req = { .bms_id = 1, .kind = TXN_CONNECT, .cmd_id = 77 };
    exec_request(&req);
    assert(last_response.cmd_id == 77 && last_response.status == RESP_LINK_DOWN && l->txn.cmd_id == 42);
    req.kind = TXN_POLL; exec_request(&req);
    assert(last_response.status == RESP_LINK_DOWN && write_calls == 0 && l->txn.cmd_id == 42);
    responses = 0; finish_characteristics(l); ack(l);
    assert(last_response.cmd_id == 42 && last_response.status == RESP_OK);
    l->txn_active = true; l->txn.kind = TXN_POLL; test_now = l->txn_deadline_us + 1;
    sweep_timeouts(); assert(terminate_calls == 0 && l->table_ready && l->timeout_strikes == 1);

    /* An allocated scan slot has handle 0 too; do not let it steal a real
     * handle-0 connection's disconnect/notify callback. */
    l = setup(0); link_t *scanning = l;
    scanning->bms_id = 0; scanning->txn_active = false;
    l = link_alloc(1); assert(l && l != scanning);
    l->conn_handle = 0; l->txn_active = true;
    l->txn = (bms_request_t){ .bms_id = 1, .kind = TXN_CONNECT, .cmd_id = 42 };
    start(l); assert(link_by_conn(0) == l);
    /* NimBLE fails pending GATT procedures before emitting GAP DISCONNECT. */
    struct ble_gatt_error disconnected = { .status = BLE_HS_ENOTCONN };
    svc_cb(0, &disconnected, NULL, svc_arg);
    assert(l->in_use && l->discovery_failed);
    gap_event(&lost, l);
    assert(!l->in_use && scanning->in_use && responses == 1);

    l = setup(0); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
    finish_characteristics(l); ack(l);
    responses = 0; req.kind = TXN_CONNECT; exec_request(&req);
    assert(responses == 1 && last_response.status == RESP_OK && svc_calls == 1);

    /* ATT rejection, timeout, malformed callback and stale acknowledgement.
     * No error status, missing attribute or wrong attribute can publish UP. */
    struct ble_gatt_attr sub_attr = { .handle = 11 };
    for (unsigned status = 1; status <= 260; status++) {
        l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
        finish_characteristics(l);
        struct ble_gatt_error rejected = { .status = status };
        write_cb(7, &rejected, &sub_attr, write_arg); failed(l, RESP_GATT_ERR);
        ack(l); assert(responses == 1 && !last_held);
    }
    for (unsigned malformed = 0; malformed < 3; malformed++) {
        l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
        finish_characteristics(l);
        struct ble_gatt_attr wrong = { .handle = 12 };
        write_cb(7, malformed == 0 ? NULL : &ok,
                 malformed == 1 ? NULL : &wrong, write_arg);
        failed(l, RESP_GATT_ERR);
    }
    l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
    finish_characteristics(l);
    old_arg = write_arg;
    test_now = l->txn_deadline_us + 1; sweep_timeouts(); failed(l, RESP_TIMEOUT);
    ack(l); assert(responses == 1 && !last_held);

    /* Real descriptor handles, not value+1. Never cross the next declaration.
     * Primary value 10 ends at 18; secondary value 20 ends at service end 30. */
    l = setup(7); start(l); service(l);
    characteristic(l, JK_CHR_UUID, 10); characteristic(l, JK_CHR2_UUID, 20);
    chr_cb(7, &done, NULL, chr_arg);
    assert(dsc_start == 10 && dsc_end == 18 && write_calls == 0);
    descriptor(7, 10, 11, 0x2901, dsc_arg); /* user description, not CCCD */
    assert(l->cccd_handle == 0);
    descriptor(7, 10, 15, 0x2902, dsc_arg);
    dsc_cb(7, &done, 10, NULL, dsc_arg);
    assert(dsc_start == 20 && dsc_end == 30 && write_calls == 0);
    descriptor(7, 10, 16, 0x2902, dsc_arg); /* stale primary-phase result */
    descriptor(7, 20, 25, 0x2902, dsc_arg);
    dsc_cb(7, &done, 20, NULL, dsc_arg);
    assert(written_handle == 15 && optional_written_handle == 25 && write_calls == 2);
    ack(l); assert(last_held && responses == 1);
    /* Declaration order may be FFE2 then FFE1; lookup still follows UUID,
     * with each descriptor search bounded by that characteristic's end. */
    l = setup(7); start(l); service(l);
    characteristic(l, JK_CHR2_UUID, 10); characteristic(l, JK_CHR_UUID, 20);
    chr_cb(7, &done, NULL, chr_arg); assert(dsc_start == 20 && dsc_end == 30);
    descriptor(7, 20, 25, 0x2902, dsc_arg); dsc_cb(7, &done, 20, NULL, dsc_arg);
    assert(dsc_start == 10 && dsc_end == 18);
    descriptor(7, 10, 15, 0x2902, dsc_arg); dsc_cb(7, &done, 10, NULL, dsc_arg);
    assert(written_handle == 25 && optional_written_handle == 15);
    ack(l); assert(last_held && responses == 1);

    for (unsigned handle = 11; handle <= 30; handle++) {
        l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
        chr_cb(7, &done, NULL, chr_arg);
        descriptor(7, 10, handle, 0x2902, dsc_arg);
        descriptor(7, 10, handle, 0x2902, dsc_arg); /* identical replay */
        assert(write_calls == 0);
        dsc_cb(7, &done, 10, NULL, dsc_arg);
        assert(written_handle == handle && write_calls == 1);
        ack(l); assert(responses == 1 && last_held);
    }
    const uint16_t bad_handles[] = {0, 10, 14, 31, UINT16_MAX};
    l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
    characteristic(l, 0x1234, 11); /* declaration overlaps previous value */
    failed(l, RESP_GATT_ERR); assert(write_calls == 0);
    for (unsigned i = 0; i < sizeof(bad_handles) / sizeof(bad_handles[0]); i++) {
        l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
        characteristic(l, 0x1234, 15); /* unrelated declaration at 14 */
        chr_cb(7, &done, NULL, chr_arg); assert(dsc_end == 13);
        descriptor(7, 10, bad_handles[i], 0x2902, dsc_arg);
        failed(l, RESP_GATT_ERR); assert(write_calls == 0);
    }
    l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
    characteristic(l, 0x1234, 12); /* no descriptor space before next char */
    chr_cb(7, &done, NULL, chr_arg); failed(l, RESP_GATT_ERR); assert(dsc_calls == 0);
    for (unsigned failure = 0; failure < 5; failure++) {
        l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
        if (failure == 0) dsc_rc = 6;
        chr_cb(7, &done, NULL, chr_arg);
        if (failure == 1) dsc_cb(7, &error, 10, NULL, dsc_arg);
        if (failure == 2) dsc_cb(7, &done, 10, NULL, dsc_arg); /* missing */
        if (failure == 3) dsc_cb(7, NULL, 10, NULL, dsc_arg);
        if (failure == 4) {
            descriptor(7, 10, 11, 0x2902, dsc_arg);
            descriptor(7, 10, 12, 0x2902, dsc_arg); /* ambiguous duplicate */
        }
        failed(l, RESP_GATT_ERR); assert(write_calls == 0);
    }
    for (unsigned optional_failure = 0; optional_failure < 3; optional_failure++) {
        l = setup(7); start(l); service(l);
        characteristic(l, JK_CHR_UUID, 10); characteristic(l, JK_CHR2_UUID, 20);
        if (optional_failure == 0) optional_dsc_rc = 6;
        chr_cb(7, &done, NULL, chr_arg);
        descriptor(7, 10, 15, 0x2902, dsc_arg);
        dsc_cb(7, &done, 10, NULL, dsc_arg);
        if (optional_failure == 1) dsc_cb(7, &error, 20, NULL, dsc_arg);
        if (optional_failure == 2) dsc_cb(7, &done, 20, NULL, dsc_arg);
        assert(write_calls == 1 && written_handle == 15 && optional_written_handle == 0);
        ack(l); assert(last_held && responses == 1);
    }
    l = setup(7); start(l); service(l);
    characteristic(l, JK_CHR_UUID, 10); characteristic(l, JK_CHR2_UUID, 20);
    optional_dsc_rc = BLE_HS_ENOTCONN;
    chr_cb(7, &done, NULL, chr_arg);
    descriptor(7, 10, 15, 0x2902, dsc_arg);
    dsc_cb(7, &done, 10, NULL, dsc_arg);
    failed(l, RESP_GATT_ERR); assert(write_calls == 0);
    l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
    chr_cb(7, &done, NULL, chr_arg); old_arg = dsc_arg;
    test_now = l->txn_deadline_us + 1; sweep_timeouts(); failed(l, RESP_TIMEOUT);
    descriptor(7, 10, 11, 0x2902, old_arg); assert(write_calls == 0);
    gap_event(&lost, l);
    l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
    chr_cb(7, &done, NULL, chr_arg);
    dsc_cb(7, &error, 10, NULL, old_arg); assert(responses == 0);
    descriptor(7, 999, 11, 0x2902, dsc_arg); assert(l->cccd_handle == 0);
    descriptor(7, 10, 11, 0x2902, dsc_arg);
    dsc_cb(7, &done, 10, NULL, dsc_arg); ack(l);
    assert(responses == 1 && last_held);
    lost.disconnect.conn.conn_handle = 7; gap_event(&lost, l);
    l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
    finish_characteristics(l);
    write_cb(7, &ok, &sub_attr, old_arg); assert(responses == 0 && !l->table_ready);
    write_cb(99, &ok, &sub_attr, write_arg); assert(responses == 0);
    req.kind = TXN_POLL; exec_request(&req);
    assert(responses == 1 && last_response.status == RESP_LINK_DOWN && l->txn.cmd_id == 42);
    responses = 0; gap_event(&lost, l);
    assert(responses == 1 && last_response.status == RESP_LINK_DOWN);
    ack(l); assert(responses == 1 && !last_held);

    /* Competing completion/deadline outcomes are serialized by the real
     * production callback mutex: exactly one result, never UP after failure. */
    for (unsigned phase = 0; phase < 2; phase++)
    for (unsigned i = 0; i < 1000; i++) {
        l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10);
        if (phase) finish_characteristics(l); /* race while waiting for ACK */
        test_now = l->txn_deadline_us + 1;
        pthread_t a, b;
        assert(pthread_create(&a, NULL, i & 1 ? complete_worker : timeout_worker, l) == 0);
        assert(pthread_create(&b, NULL, i & 1 ? timeout_worker : complete_worker, l) == 0);
        assert(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
        assert(responses == 1 && !l->txn_active && !l->discovery_pending);
        assert(l->table_ready != l->discovery_failed);
        assert(last_response.status == (l->table_ready ? RESP_OK : RESP_TIMEOUT));
        assert(terminate_calls == (l->discovery_failed ? 1u : 0u));
    }
    /* Trace production notify -> reassembly -> evidence. Neither heartbeats,
     * partial frames nor a complete corrupt frame count as fresh evidence. */
    l = setup(7); l->txn_active = false;
    uint8_t frame[JK_FRAME_MAX];
    int frame_len = synth_cell_info(frame, sizeof(frame), 1);
    assert(frame_len > 128);
    static const uint8_t junk[] = "AT\r\n";
    on_notify(l, junk, sizeof(junk) - 1);
    assert(frame_notes == 0 && frame_copies == 0);
    frame[frame_len - 1] ^= 1;
    on_notify(l, frame, frame_len);
    assert(frame_notes == 0 && frame_copies == 0);
    frame[frame_len - 1] ^= 1;
    jk_reasm_init(&l->reasm, JK_FRAME_JK02_32S);
    on_notify(l, frame, 128);
    assert(frame_notes == 0 && frame_copies == 0);
    test_now++;
    on_notify(l, frame + 128, frame_len - 128);
    assert(frame_notes == 1 && frame_copies == 2 && frame_time == test_now);
    puts("PASS: frame evidence only after complete checksum-valid production reassembly");
    puts("PASS: production discovery callbacks, errors/deadlines/retry/stale-generation/request gates");
    puts("PASS: 2000 concurrent discovery/ACK/deadline races; disconnect ordering and handle-0 isolation");
    puts("PASS: subscription ACK gating, 260 rejected statuses, malformed/duplicate/stale/missing ACKs");
    puts("PASS: discovered CCCD handles/ranges, optional fallback, missing/error/stale descriptor callbacks");
    return 0;
}
