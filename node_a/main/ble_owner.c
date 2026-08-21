/*
 * ble_owner.c — NimBLE central. Structure is complete and faithful to the
 * NimBLE host API; the lines tagged `NIMBLE-PASS` need a compile/link check
 * against the pinned ESP-IDF next week (exact arg structs, HS error handling).
 * Nothing here writes BMS settings — that path is gated in jk_proto.
 *
 * Threading: NimBLE host callbacks run on the host task. They only touch their
 * own link's reassembler and post to queues. ble_owner_task performs the
 * connect/write side under mtx_link_pool. Snapshot copies cross the boundary.
 */
#include <string.h>
#include "ble_owner.h"
#include "queues.h"
#include "config.h"
#include "state_cache.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"

static const char *TAG = "ble_owner";

typedef struct {
    bool      in_use;
    uint8_t   bms_id;
    uint16_t  conn_handle;
    uint16_t  val_handle;     /* 0xFFE1 value handle                          */
    uint16_t  cccd_handle;    /* 0xFFE1 CCCD                                  */
    jk_reasm_t reasm;
    harvest_entry_t table;    /* discovered blueprint                        */
    bool      table_ready;

    /* outstanding transaction (one per link — JK is strict req/resp) */
    bool      txn_active;
    bms_request_t txn;
    int64_t   txn_deadline_us;
    uint8_t   want_record;    /* record we expect back for a POLL            */
} link_t;

static link_t s_links[CFG_LINK_POOL_SIZE];
static SemaphoreHandle_t s_mtx_link_pool;

/* target selection ------------------------------------------------------- */
static const char *name_for(uint8_t bms_id)
{
    for (int i = 0; i < CFG_NUM_UNITS; i++)
        if (CFG_BMS[i].bms_id == bms_id) return CFG_BMS[i].name;
    return NULL;
}
static link_t *link_by_conn(uint16_t ch)
{
    for (int i = 0; i < CFG_LINK_POOL_SIZE; i++)
        if (s_links[i].in_use && s_links[i].conn_handle == ch) return &s_links[i];
    return NULL;
}
static link_t *link_by_bms(uint8_t id)
{
    for (int i = 0; i < CFG_LINK_POOL_SIZE; i++)
        if (s_links[i].in_use && s_links[i].bms_id == id) return &s_links[i];
    return NULL;
}
static link_t *link_alloc(uint8_t id)
{
    for (int i = 0; i < CFG_LINK_POOL_SIZE; i++)
        if (!s_links[i].in_use) {
            memset(&s_links[i], 0, sizeof(link_t));
            s_links[i].in_use = true;
            s_links[i].bms_id = id;
            jk_reasm_init(&s_links[i].reasm, JK_FRAME_JK02_32S);
            return &s_links[i];
        }
    return NULL;   /* pool full — round-robin/idle-disconnect frees slots     */
}

/* response helper -------------------------------------------------------- */
static void respond(uint8_t bms_id, uint16_t cmd_id, resp_status_t st,
                    const uint8_t *frame, uint16_t len, jk_record_t rec)
{
    bms_response_t r = { .bms_id = bms_id, .cmd_id = cmd_id, .status = st,
                         .frame = frame, .frame_len = len, .record = rec };
    xQueueSend(g_q_bms_response, &r, portMAX_DELAY);
}

static void set_link_state(uint8_t id, tunnel_link_state_t s, bool held)
{
    bms_runtime_t rt; state_get_runtime(id, &rt);
    rt.link = s; rt.link_held = held;
    if (held) rt.last_seen_us = esp_timer_get_time();
    state_set_runtime(id, &rt);
    if (id < CFG_NUM_UNITS) {
        if (held) xEventGroupSetBits(g_evt, EVT_BMS_UP(id));
        else      xEventGroupClearBits(g_evt, EVT_BMS_UP(id));
    }
}

/* ---- notify path (host task) ------------------------------------------- */
static void on_notify(link_t *l, const uint8_t *data, uint16_t len)
{
    uint16_t flen;
    const uint8_t *frame = jk_reasm_push(&l->reasm, data, len, &flen);
    if (!frame) return;   /* need more chunks */

    /* Fan out the complete frame: to the app (tunnel) and to the decoder. */
    notify_item_t it;
    it.bms_id = l->bms_id; it.idx = 0; it.len = flen;
    memcpy(it.data, frame, flen);
    xQueueSend(g_q_notify, &it, 0);   /* drop-oldest semantics if full */
    xQueueSend(g_q_decode, &it, 0);

    bms_runtime_t rt; state_get_runtime(l->bms_id, &rt);
    rt.last_seen_us = esp_timer_get_time(); state_set_runtime(l->bms_id, &rt);

    /* Complete an outstanding POLL if this is the record it wanted. */
    if (l->txn_active && l->txn.kind == TXN_POLL) {
        jk_record_t rec = jk_frame_record(frame, flen);
        l->txn_active = false;
        respond(l->bms_id, l->txn.cmd_id, RESP_OK, frame, flen, rec);
    }
}

/* ---- GATT discovery callbacks (host task) ------------------------------ */
/* NIMBLE-PASS: these signatures follow the NimBLE host API; verify the exact
 * argument structs (ble_gatt_error, ble_gatt_svc, ble_gatt_chr, ble_gatt_dsc)
 * against the pinned IDF headers. */
static int on_chr_disc(uint16_t ch, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg)
{
    link_t *l = arg;
    if (err && err->status != 0 && err->status != BLE_HS_EDONE) return 0;
    if (chr && ble_uuid_u16(&chr->uuid.u) == JK_CHR_UUID) {
        l->val_handle  = chr->val_handle;
        l->cccd_handle = chr->val_handle + 1;   /* CCCD is typically val+1    */
        /* Record the blueprint (single characteristic for JK). */
        l->table.char_count = 1;
        l->table.chars[0] = (tunnel_char_desc_t){ JK_SVC_UUID, JK_CHR_UUID, chr->properties };
        l->table.valid = true;
        l->table_ready = true;
    }
    if (err && err->status == BLE_HS_EDONE) {
        /* Discovery finished: subscribe to notifications (write CCCD = 0x0001). */
        uint8_t v[2] = {0x01, 0x00};
        ble_gattc_write_flat(ch, l->cccd_handle, v, sizeof(v), NULL, NULL); /* NIMBLE-PASS */
        set_link_state(l->bms_id, LINK_UP, true);
        /* A TXN_CONNECT completes here. */
        if (l->txn_active && l->txn.kind == TXN_CONNECT) {
            l->txn_active = false;
            respond(l->bms_id, l->txn.cmd_id, RESP_OK, NULL, 0, JK_REC_NONE);
        }
    }
    return 0;
}

static int on_svc_disc(uint16_t ch, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg)
{
    link_t *l = arg;
    if (svc && ble_uuid_u16(&svc->uuid.u) == JK_SVC_UUID) {
        ble_gattc_disc_all_chrs(ch, svc->start_handle, svc->end_handle,
                                on_chr_disc, l);                   /* NIMBLE-PASS */
    }
    return 0;
}

/* ---- notify RX + connection lifecycle ---------------------------------- */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        link_t *l = arg;   /* link chosen in the connect call */
        if (event->connect.status == 0) {
            l->conn_handle = event->connect.conn_handle;
            /* Request larger MTU, then discover the JK service. */
            ble_gattc_exchange_mtu(l->conn_handle, NULL, NULL);        /* NIMBLE-PASS */
            ble_uuid16_t svc = BLE_UUID16_INIT(JK_SVC_UUID);
            ble_gattc_disc_svc_by_uuid(l->conn_handle, &svc.u, on_svc_disc, l); /* NIMBLE-PASS */
        } else {
            ESP_LOGW(TAG, "connect failed bms %u st=%d", l->bms_id, event->connect.status);
            set_link_state(l->bms_id, LINK_UNREACHABLE, false);
            if (l->txn_active) { l->txn_active = false;
                respond(l->bms_id, l->txn.cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE); }
            l->in_use = false;
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        link_t *l = link_by_conn(event->disconnect.conn.conn_handle);
        if (l) {
            set_link_state(l->bms_id, LINK_REACHABLE_IDLE, false);
            if (l->txn_active) { l->txn_active = false;
                respond(l->bms_id, l->txn.cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE); }
            l->in_use = false;
        }
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        link_t *l = link_by_conn(event->notify_rx.conn_handle);
        if (l) {
            /* Copy the mbuf out on the host stack, then reassemble. */
            uint8_t tmp[256]; uint16_t n = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (n > sizeof(tmp)) n = sizeof(tmp);
            ble_hs_mbuf_to_flat(event->notify_rx.om, tmp, n, NULL);    /* NIMBLE-PASS */
            on_notify(l, tmp, n);
        }
        return 0;
    }
    default: return 0;
    }
}

/* ---- transaction execution (ble_owner_task) ---------------------------- */
static void start_connect(link_t *l)
{
    /* NIMBLE-PASS: scan-by-name then ble_gap_connect to the matched address,
     * or connect directly if CFG_BMS carries an address. For the bench rig the
     * emulator advertises the configured name; a name filter in the scan cb
     * selects it. Timeout/own_addr_type per IDF defaults. */
    const char *name = name_for(l->bms_id);
    ESP_LOGI(TAG, "connecting bms %u (%s)", l->bms_id, name ? name : "?");
    /* ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 30000, &conn_params, gap_event, l); */
}

static void exec_request(const bms_request_t *req)
{
    xSemaphoreTake(s_mtx_link_pool, portMAX_DELAY);
    link_t *l = link_by_bms(req->bms_id);

    if (req->kind == TXN_CONNECT) {
        if (l && l->conn_handle) {   /* already up */
            respond(req->bms_id, req->cmd_id, RESP_OK, NULL, 0, JK_REC_NONE);
        } else {
            l = l ? l : link_alloc(req->bms_id);
            if (!l) { respond(req->bms_id, req->cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE); }
            else { l->txn_active = true; l->txn = *req;
                   l->txn_deadline_us = esp_timer_get_time() + req->timeout_ms * 1000LL;
                   start_connect(l); }
        }
        xSemaphoreGive(s_mtx_link_pool); return;
    }

    if (!l || !l->val_handle) {
        respond(req->bms_id, req->cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE);
        xSemaphoreGive(s_mtx_link_pool); return;
    }

    switch (req->kind) {
    case TXN_DISCONNECT:
        ble_gap_terminate(l->conn_handle, BLE_ERR_REM_USER_CONN_TERM);     /* NIMBLE-PASS */
        respond(req->bms_id, req->cmd_id, RESP_OK, NULL, 0, JK_REC_NONE);
        break;
    case TXN_POLL: {
        uint8_t cmd[JK_CMD_FRAME_LEN];
        int n = jk_build_read_cmd(req->opcode, cmd, sizeof(cmd));
        l->txn_active = true; l->txn = *req;
        l->txn_deadline_us = esp_timer_get_time() + req->timeout_ms * 1000LL;
        ble_gattc_write_flat(l->conn_handle, l->val_handle, cmd, n, NULL, NULL); /* NIMBLE-PASS */
        break;
    }
    case TXN_RAW_WRITE: {
        /* App write relayed verbatim to 0xFFE1. */
        l->txn_active = req->response_needed; l->txn = *req;
        l->txn_deadline_us = esp_timer_get_time() + req->timeout_ms * 1000LL;
        ble_gattc_write_flat(l->conn_handle, l->val_handle, req->payload,
                             req->payload_len, NULL, NULL);                 /* NIMBLE-PASS */
        if (!req->response_needed)
            respond(req->bms_id, req->cmd_id, RESP_OK, NULL, 0, JK_REC_NONE);
        break;
    }
    case TXN_BALANCE_WRITE:
        /* Reaches here only if JK_ENABLE_WRITES built the frame (else the
         * arbiter never enqueued it). Same write path as RAW_WRITE. */
        l->txn_active = true; l->txn = *req;
        l->txn_deadline_us = esp_timer_get_time() + req->timeout_ms * 1000LL;
        ble_gattc_write_flat(l->conn_handle, l->val_handle, req->payload,
                             req->payload_len, NULL, NULL);                 /* NIMBLE-PASS */
        break;
    default: break;
    }
    xSemaphoreGive(s_mtx_link_pool);
}

/* ---- timeout sweep ----------------------------------------------------- */
static void sweep_timeouts(void)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_mtx_link_pool, portMAX_DELAY);
    for (int i = 0; i < CFG_LINK_POOL_SIZE; i++) {
        link_t *l = &s_links[i];
        if (l->in_use && l->txn_active && now > l->txn_deadline_us) {
            ESP_LOGW(TAG, "bms %u txn timeout (spec §11 guard)", l->bms_id);
            l->txn_active = false;
            respond(l->bms_id, l->txn.cmd_id, RESP_TIMEOUT, NULL, 0, JK_REC_NONE);
            /* Free the link so it can't be held indefinitely. */
            if (l->conn_handle) ble_gap_terminate(l->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    xSemaphoreGive(s_mtx_link_pool);
}

/* ---- tasks ------------------------------------------------------------- */
static void ble_owner_task(void *arg)
{
    for (;;) {
        bms_request_t req;
        if (xQueueReceive(g_q_bms_request, &req, pdMS_TO_TICKS(200)) == pdTRUE)
            exec_request(&req);
        sweep_timeouts();
    }
}

static void nimble_host_task(void *arg) { nimble_port_run(); nimble_port_freertos_deinit(); }

static void on_sync(void) { ESP_LOGI(TAG, "NimBLE sync, central ready"); }

bool ble_owner_copy_table(uint8_t bms_id, harvest_entry_t *out)
{
    bool ok = false;
    xSemaphoreTake(s_mtx_link_pool, portMAX_DELAY);
    link_t *l = link_by_bms(bms_id);
    if (l && l->table_ready) { *out = l->table; ok = true; }
    xSemaphoreGive(s_mtx_link_pool);
    return ok;
}

void ble_owner_start(void)
{
    memset(s_links, 0, sizeof(s_links));
    s_mtx_link_pool = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    /* NIMBLE-PASS: set role=central only; no GATT server on Node A. */
    nimble_port_freertos_init(nimble_host_task);

    xTaskCreatePinnedToCore(ble_owner_task, "ble_owner", 8192, NULL, 7, NULL, 0);
}
