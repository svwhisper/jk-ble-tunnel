/* Production callbacks, timeout sweep and request gate with simulated NimBLE
 * procedure results. No radio/network/serial I/O exists in these adapters. */
#include <assert.h>
#include "../node_a/main/ble_owner.c"

static int response_token;
QueueHandle_t g_q_bms_response = &response_token;
QueueHandle_t g_q_arb_in, g_q_bms_request, g_q_notify, g_q_decode;
EventGroupHandle_t g_evt;
struct ble_hs_cfg_stub ble_hs_cfg;
static int64_t test_now;
static int svc_rc, chr_rc, write_rc, mtu_rc, terminate_rc;
static unsigned svc_calls, chr_calls, write_calls, terminate_calls, responses, state_calls;
static bms_response_t last_response;
static tunnel_link_state_t last_state;
static bool last_held;
static void *svc_arg, *chr_arg;
static ble_gatt_disc_svc_fn *svc_cb;
static ble_gatt_chr_fn *chr_cb;
static const struct ble_gatt_error ok = {0}, done = { .status = BLE_HS_EDONE },
                                   error = { .status = 5 };
int64_t esp_timer_get_time(void) { return test_now; }
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{ (void)ticks; assert(q == g_q_bms_response); last_response = *(const bms_response_t *)item; responses++; return pdTRUE; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b)
{ (void)g; return b; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b)
{ (void)g; (void)b; return 0; }
void state_set_link_state(uint8_t id, tunnel_link_state_t st, bool held, int64_t seen)
{ assert(id < CFG_NUM_UNITS); (void)seen; last_state = st; last_held = held; state_calls++; }
void state_note_frame(uint8_t id, int64_t now) { (void)id; (void)now; assert(0); }
void state_get_runtime(uint8_t id, bms_runtime_t *out)
{ (void)id; (void)out; assert(0); }
int ble_gattc_exchange_mtu(uint16_t ch, ble_gatt_mtu_fn *cb, void *arg)
{ (void)ch; assert(!cb && !arg); return mtu_rc; }
int ble_gattc_disc_svc_by_uuid(uint16_t ch, const ble_uuid_t *uuid, ble_gatt_disc_svc_fn *cb, void *arg)
{ (void)ch; assert(ble_uuid_u16(uuid) == JK_SVC_UUID); svc_calls++; svc_cb = cb; svc_arg = arg; return svc_rc; }
int ble_gattc_disc_all_chrs(uint16_t ch, uint16_t start, uint16_t end, ble_gatt_chr_fn *cb, void *arg)
{ (void)ch; assert(start == 1 && end == 30); chr_calls++; chr_cb = cb; chr_arg = arg; return chr_rc; }
int ble_gattc_write_flat(uint16_t ch, uint16_t handle, const void *value, uint16_t len, ble_gatt_attr_fn *cb, void *arg)
{ (void)ch; (void)handle; (void)value; (void)len; (void)cb; (void)arg; write_calls++; return write_rc; }
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
void mqtt_publish_llevent(const char *k, uint8_t id, int r) { (void)k; (void)id; (void)r; }
void mqtt_publish_raw(uint8_t id, const uint8_t *d, uint16_t n) { (void)id; (void)d; (void)n; assert(0); }
bool net_wifi_up(void) { return true; }
int64_t net_wifi_down_ms(void) { return 0; }

static link_t *setup(uint16_t ch)
{
    memset(s_links, 0, sizeof(s_links));
    s_connecting = s_conn_inflight = NULL;
    svc_rc = chr_rc = write_rc = mtu_rc = terminate_rc = 0;
    svc_calls = chr_calls = write_calls = terminate_calls = responses = state_calls = 0;
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
    struct ble_gatt_chr c = { .val_handle = handle,
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
int main(void)
{
    s_mtx_link_pool = xSemaphoreCreateMutex();
    /* Immediate start failure, errors, missing service/characteristic. */
    link_t *l = setup(7); svc_rc = 6; start(l); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); svc_cb(7, &error, NULL, svc_arg); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); svc_cb(7, NULL, NULL, svc_arg); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); svc_cb(7, &done, NULL, svc_arg); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); chr_rc = 6; service(l); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); service(l); chr_cb(7, &error, NULL, chr_arg); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); service(l); chr_cb(7, &done, NULL, chr_arg); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, UINT16_MAX);
    chr_cb(7, &done, NULL, chr_arg); failed(l, RESP_GATT_ERR);
    l = setup(7); start(l); service(l); characteristic(l, JK_CHR_UUID, 10); write_rc = 6;
    chr_cb(7, &done, NULL, chr_arg); failed(l, RESP_GATT_ERR);

    /* Normal discovery (including optional FFE2) remains one successful txn.
     * Duplicate and stale procedure completions must not resubscribe/respond. */
    for (unsigned extra = 0; extra < 2; extra++) {
        l = setup(7); mtu_rc = 6; start(l); service(l); service(l);
        assert(chr_calls == 1);
        svc_cb(7, &done, NULL, svc_arg);
        characteristic(l, JK_CHR_UUID, 10);
        assert(!l->table_ready && responses == 0);
        if (extra) characteristic(l, JK_CHR2_UUID, 20);
        chr_cb(7, &done, NULL, chr_arg);
        assert(l->table_ready && !l->discovery_pending && last_held && last_state == LINK_UP);
        assert(responses == 1 && last_response.status == RESP_OK && write_calls == 1 + extra);
        chr_cb(7, &done, NULL, chr_arg); svc_cb(7, &error, NULL, svc_arg);
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
    responses = 0; chr_cb(7, &done, NULL, chr_arg);
    assert(last_response.cmd_id == 42 && last_response.status == RESP_OK);
    l->txn_active = true; l->txn.kind = TXN_POLL; test_now = l->txn_deadline_us + 1;
    sweep_timeouts(); assert(terminate_calls == 0 && l->table_ready && l->timeout_strikes == 1);
    puts("PASS: production discovery callbacks, errors/deadlines/retry/stale-generation/request gates");
    return 0;
}
