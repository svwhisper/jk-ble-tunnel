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
#include <stdarg.h>
#include <stdio.h>
#include "ble_owner.h"
#include "mqtt_task.h"
#include "queues.h"
#include "config.h"
#include "state_cache.h"
#include "net_util.h"
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
    uint16_t  ffe2_handle;    /* 0xFFE2 value handle (0 if absent)            */
    uint16_t  cccd_handle;    /* 0xFFE1 CCCD                                  */
    uint8_t   ffe1_props;     /* discovered char properties — pick the ATT   */
    uint8_t   ffe2_props;     /*   write op each char actually permits        */
    jk_reasm_t reasm;
    harvest_entry_t table;    /* discovered blueprint                        */
    bool      table_ready;

    /* outstanding transaction (one per link — JK is strict req/resp) */
    bool      txn_active;
    bms_request_t txn;
    int64_t   txn_deadline_us;
    uint8_t   want_record;    /* record we expect back for a POLL            */
    uint8_t   timeout_strikes;/* consecutive txn timeouts on this link       */
    int64_t   renegotiate_at_us; /* central-initiated param update pending    */
} link_t;

static link_t s_links[CFG_LINK_POOL_SIZE];
static SemaphoreHandle_t s_mtx_link_pool;

/* One scan/connect in flight at a time (scanning is a global radio resource).
 * The arbiter serialises per-BMS work, so this is rarely contended.
 * s_connecting covers the SCAN phase only; once ble_gap_connect is issued the
 * link moves to s_conn_inflight until its CONNECT event resolves. Keeping one
 * flag for both phases let burst-duplicate DISC events free an in-flight link
 * (multi-connect race — bank 0 held 4 phantom connections, 2026-08-30). */
static link_t *s_connecting;
static link_t *s_conn_inflight;
static char    s_connect_name[32];
static uint8_t s_connect_addr[6];
static int gap_event(struct ble_gap_event *event, void *arg);
static int scan_event(struct ble_gap_event *event, void *arg);
static link_t *link_by_bms(uint8_t id);

/* ---- diagnostic scan dump (jkbms/bridge/cmd/scan) ----------------------- */
#define SCAN_DUMP_MAX 24
typedef struct { ble_addr_t addr; int8_t rssi; char name[32]; } scan_rec_t;
static scan_rec_t     s_scan[SCAN_DUMP_MAX];
static int            s_scan_n;
static volatile bool  s_scan_req;      /* set by MQTT cmd, serviced on BLE task */
static volatile bool  s_scan_active;   /* a dump scan owns the radio            */
static int scan_dump_event(struct ble_gap_event *event, void *arg);

void ble_owner_scan_dump(void) { s_scan_req = true; }

/* ---- BLE master switch + connect/disconnect counters -------------------- */
static volatile bool s_ble_enabled;       /* restored from NVS by supervisor */
static volatile uint32_t s_conn_events, s_disc_events;
bool ble_owner_ble_enabled(void) { return s_ble_enabled; }
uint32_t ble_owner_conn_count(void) { return s_conn_events; }
uint32_t ble_owner_disc_count(void) { return s_disc_events; }
void ble_owner_set_ble(bool on)
{
    s_ble_enabled = on;
    nvs_put_ble_enabled(on);   /* survives power cycles; default OFF */
    ESP_LOGW(TAG, "BLE master switch: %s (persisted)", on ? "ON" : "OFF");
    if (!on) {
        /* Drop everything held so the units go quiet immediately. */
        xSemaphoreTake(s_mtx_link_pool, portMAX_DELAY);
        for (int i = 0; i < CFG_LINK_POOL_SIZE; i++)
            if (s_links[i].in_use && s_links[i].conn_handle)
                ble_gap_terminate(s_links[i].conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        xSemaphoreGive(s_mtx_link_pool);
    }
}

/* ---- full GATT dump (jkbms/bridge/cmd/gattdump <id>) -------------------- */
/* Walk the COMPLETE service/characteristic table of a connected unit and
 * publish it to jkbms/bridge/gatt. The harvest only records the JK FFE0/FFE1
 * pair, so the clone may lack services the official app checks (GAP name,
 * Device Information Service, extra chars) — this shows what a real unit
 * actually exposes so Node B can mirror it. */
#define GD_MAX_SVCS 12
typedef struct { struct ble_gatt_svc svcs[GD_MAX_SVCS]; int n, cur;
                 uint16_t conn; uint8_t bms_id; bool active;
                 char json[1792]; int jo; } gatt_dump_t;
static gatt_dump_t s_gd;
static volatile int s_gd_req = -1;      /* bms_id to dump, -1 = none */
void ble_owner_gattdump(uint8_t bms_id) { s_gd_req = bms_id; }

static void gd_append(const char *fmt, ...)
{
    int left = (int)sizeof(s_gd.json) - s_gd.jo - 1;
    if (left <= 0) return;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(s_gd.json + s_gd.jo, left, fmt, ap);
    va_end(ap);
    if (w > 0) s_gd.jo += (w < left) ? w : left;
}

static void gd_finish(void)
{
    gd_append("]}]}");
    mqtt_publish_gatt(s_gd.json);
    ESP_LOGI(TAG, "gattdump: published (%d svcs)", s_gd.n);
    s_gd.active = false;
}

static int gd_chr_cb(uint16_t ch, const struct ble_gatt_error *err,
                     const struct ble_gatt_chr *chr, void *arg);

static void gd_next_svc(void)
{
    s_gd.cur++;
    if (s_gd.cur >= s_gd.n) { gd_finish(); return; }
    struct ble_gatt_svc *sv = &s_gd.svcs[s_gd.cur];
    char u[BLE_UUID_STR_LEN];
    ble_uuid_to_str(&sv->uuid.u, u);
    gd_append("%s{\"svc\":\"%s\",\"chrs\":[", s_gd.cur ? "]}," : "", u);
    if (ble_gattc_disc_all_chrs(s_gd.conn, sv->start_handle, sv->end_handle,
                                gd_chr_cb, NULL) != 0)
        gd_next_svc();   /* skip a service we can't walk */
}

static int gd_chr_cb(uint16_t ch, const struct ble_gatt_error *err,
                     const struct ble_gatt_chr *chr, void *arg)
{
    (void)ch; (void)arg;
    if (chr) {
        char u[BLE_UUID_STR_LEN];
        ble_uuid_to_str(&chr->uuid.u, u);
        gd_append("%s{\"u\":\"%s\",\"p\":%d}",
                  (s_gd.jo && s_gd.json[s_gd.jo - 1] == '}') ? "," : "",
                  u, chr->properties);
    }
    if (err && err->status == BLE_HS_EDONE) gd_next_svc();
    else if (err && err->status != 0) gd_next_svc();
    return 0;
}

static int gd_svc_cb(uint16_t ch, const struct ble_gatt_error *err,
                     const struct ble_gatt_svc *svc, void *arg)
{
    (void)ch; (void)arg;
    if (svc && s_gd.n < GD_MAX_SVCS) s_gd.svcs[s_gd.n++] = *svc;
    if (err && (err->status == BLE_HS_EDONE || err->status != 0)) {
        if (err->status == BLE_HS_EDONE && s_gd.n) {
            s_gd.cur = -1;
            gd_next_svc();
        } else if (err->status != BLE_HS_EDONE) {
            ESP_LOGW(TAG, "gattdump: svc disc err %d", err->status);
            s_gd.active = false;
        }
    }
    return 0;
}

/* Runs on ble_owner_task. */
static void do_gatt_dump(int bms_id)
{
    xSemaphoreTake(s_mtx_link_pool, portMAX_DELAY);
    link_t *l = link_by_bms((uint8_t)bms_id);
    uint16_t conn = (l && l->conn_handle) ? l->conn_handle : 0;
    xSemaphoreGive(s_mtx_link_pool);
    if (!conn) { ESP_LOGW(TAG, "gattdump: bms %d not connected", bms_id); return; }

    memset(&s_gd, 0, sizeof(s_gd));
    s_gd.conn = conn; s_gd.bms_id = (uint8_t)bms_id; s_gd.active = true;
    gd_append("{\"id\":%d,\"svcs\":[", bms_id);
    if (ble_gattc_disc_all_svcs(conn, gd_svc_cb, NULL) != 0) {
        ESP_LOGW(TAG, "gattdump: disc_all_svcs busy/failed");
        s_gd.active = false;
    }
}

/* Link vitals — the GENTLE CLIENT replacement for the keepalive GATT read
 * (2026-08-29 CPUAux reframe). The old 15 s ble_gattc_read per link was our
 * only ATT-layer traffic to a streaming module, and the prime suspect for
 * crash-looping the JK's aux CPU (the phone app and esphome-jk-bms never
 * read; both survive 24/7). "Keepalive is life support" was disproven: reads
 * don't keep modules alive. So: NOTHING goes on the air here. RSSI comes
 * from the local controller (last-received-packet HCI read, zero radio
 * traffic) and the 0x02 stream itself is the liveness signal. */
void ble_owner_keepalive_read(void)
{
    uint8_t links = 0;
    char rssi_frag[64] = ""; int rf = 0;
    xSemaphoreTake(s_mtx_link_pool, portMAX_DELAY);
    for (int i = 0; i < CFG_LINK_POOL_SIZE; i++) {
        link_t *l = &s_links[i];
        if (!(l->in_use && l->conn_handle && l->val_handle)) continue;
        links++;
        int8_t rssi;
        if (ble_gap_conn_rssi(l->conn_handle, &rssi) == 0)
            rf += snprintf(rssi_frag + rf, sizeof(rssi_frag) - rf, "%s\"%u\":%d",
                           rf ? "," : "", l->bms_id, rssi);
    }
    xSemaphoreGive(s_mtx_link_pool);
    mqtt_publish_ka(links, rssi_frag);
}

/* ---- raw-frame capture (jkbms/bridge/cmd/rawcap) ------------------------ */
/* While armed, publish every raw notify chunk to jkbms/<id>/raw as hex, so the
 * real JK frame layout can be captured remotely to pin decode offsets (O-1). */
static volatile int64_t s_rawcap_until_us;
void ble_owner_rawcap(int seconds)
{
    if (seconds < 1)   seconds = 20;
    if (seconds > 120) seconds = 120;
    s_rawcap_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
    ESP_LOGW(TAG, "raw-frame capture armed for %ds", seconds);
}

/* target selection ------------------------------------------------------- */
static const char *name_for(uint8_t bms_id)
{
    for (int i = 0; i < CFG_NUM_UNITS; i++)
        if (CFG_BMS[i].bms_id == bms_id) return CFG_BMS[i].name;
    return NULL;
}
static const uint8_t *addr_for(uint8_t bms_id)
{
    static const uint8_t zeros[6] = {0};
    for (int i = 0; i < CFG_NUM_UNITS; i++)
        if (CFG_BMS[i].bms_id == bms_id)
            return memcmp(CFG_BMS[i].addr, zeros, 6) ? CFG_BMS[i].addr : NULL;
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
    /* Bounded so the BLE task never blocks forever; the arbiter drains this
     * promptly now that its own sends are bounded, so this effectively never
     * times out. */
    if (xQueueSend(g_q_bms_response, &r, pdMS_TO_TICKS(200)) != pdTRUE)
        ESP_LOGW(TAG, "bms_response full — dropped (bms %u)", bms_id);
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
    /* Raw capture (O-1): dump the chunk as received, before reassembly — so we
     * see the real bytes even if reassembly itself is mismatched. */
    if (esp_timer_get_time() < s_rawcap_until_us)
        mqtt_publish_raw(l->bms_id, data, len);

    /* App transparency (spec §6): while an app holds this identity, forward
     * the chunk VERBATIM (TUN_RAW) before reassembly, preserving wire order.
     * The real stream carries AT heartbeats and AA5590EB C8 command-acks that
     * reassembly strips — the official app stalls without them. */
    bms_runtime_t apprt; state_get_runtime(l->bms_id, &apprt);
    if (apprt.app_connected && len <= JK_FRAME_MAX) {
        notify_item_t rw;
        rw.bms_id = l->bms_id; rw.idx = 0; rw.raw = true; rw.len = len;
        memcpy(rw.data, data, len);
        xQueueSend(g_q_notify, &rw, 0);
    }

    uint16_t flen;
    const uint8_t *frame = jk_reasm_push(&l->reasm, data, len, &flen);
    if (!frame) return;   /* need more chunks */

    l->timeout_strikes = 0;   /* the unit is talking — clear the §11 strikes */

    /* Fan out the complete frame: to Node B's read cache and to the decoder. */
    notify_item_t it;
    it.bms_id = l->bms_id; it.idx = 0; it.raw = false; it.len = flen;
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

/* ---- write-op selection ------------------------------------------------- */
/* Unit 0's Telink/Nordic module (A4:C1:38, gattdump 2026-08-29 evening)
 * exposes FFE1 as notify+write-NO-rsp (0x14) and FFE2 as write+notify (0x18)
 * — the exact INVERSE of the C8:47:80 trio's write types. ATT silently drops
 * a Write Request to a char without the write property AND a Write Command to
 * one without write-no-rsp, so every command this client (and the frozen-
 * constant era before it) ever sent to unit 0 was discarded on arrival —
 * the whole "connects but never streams" record. Choose the op the char
 * permits; where a char permits the trio's proven op (or props are unknown,
 * 0), keep current behavior. */
static bool ffe1_needs_write_cmd(const link_t *l)
{ return !(l->ffe1_props & BLE_GATT_CHR_PROP_WRITE) &&
          (l->ffe1_props & BLE_GATT_CHR_PROP_WRITE_NO_RSP); }
static bool ffe2_needs_write_req(const link_t *l)
{ return !(l->ffe2_props & BLE_GATT_CHR_PROP_WRITE_NO_RSP) &&
          (l->ffe2_props & BLE_GATT_CHR_PROP_WRITE); }

/* ---- GATT discovery callbacks (host task) ------------------------------ */
/* NIMBLE-PASS: these signatures follow the NimBLE host API; verify the exact
 * argument structs (ble_gatt_error, ble_gatt_svc, ble_gatt_chr, ble_gatt_dsc)
 * against the pinned IDF headers. */
static int on_chr_disc(uint16_t ch, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg)
{
    link_t *l = arg;
    if (err && err->status != 0 && err->status != BLE_HS_EDONE) return 0;
    if (chr && ble_uuid_u16(&chr->uuid.u) == JK_CHR2_UUID) {
        l->ffe2_handle = chr->val_handle;   /* idx-1 app writes route here */
        l->ffe2_props  = chr->properties;
    }
    if (chr && ble_uuid_u16(&chr->uuid.u) == JK_CHR_UUID) {
        l->val_handle  = chr->val_handle;
        l->ffe1_props  = chr->properties;
        l->cccd_handle = chr->val_handle + 1;   /* CCCD is typically val+1    */
        /* Record the blueprint (single characteristic for JK). */
        l->table.char_count = 1;
        l->table.chars[0] = (tunnel_char_desc_t){ JK_SVC_UUID, JK_CHR_UUID, chr->properties };
        l->table.valid = true;
        l->table_ready = true;
    }
    if (err && err->status == BLE_HS_EDONE) {
        /* Discovery finished: subscribe to notifications (write CCCD = 0x0001).
         * Back-to-back GATT procedures are safe — NimBLE queues them (the
         * connect path already stacks MTU-exchange + svc discovery). */
        uint8_t v[2] = {0x01, 0x00};
        if (l->cccd_handle)
            ble_gattc_write_flat(ch, l->cccd_handle, v, sizeof(v), NULL, NULL); /* NIMBLE-PASS */
        /* Unit 0's module carries notify on FFE2 too — its replies may
         * surface there. Same val+1 CCCD convention; a module without that
         * descriptor just returns a harmless ATT error. */
        if (l->ffe2_handle && (l->ffe2_props & BLE_GATT_CHR_PROP_NOTIFY))
            ble_gattc_write_flat(ch, l->ffe2_handle + 1, v, sizeof(v), NULL, NULL);
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
        /* Only the connect we believe is in flight may be adopted; anything
         * else is a stale completion whose slot was freed (and possibly
         * reused) — adopting those is how phantom connections piled onto
         * bank 0 and corrupted its state (2026-08-30). */
        bool expected = (l == s_conn_inflight) && l->in_use && !l->conn_handle;
        s_conn_inflight = NULL;
        /* (s_connecting is NOT cleared here: it now belongs solely to the
         * scan phase, which may already be running for a DIFFERENT bank.) */
        if (event->connect.status == 0) {
            if (!expected) {
                ESP_LOGW(TAG, "orphan connect handle=%u — terminating",
                         event->connect.conn_handle);
                ble_gap_terminate(event->connect.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);          /* NIMBLE-PASS */
                return 0;
            }
            s_conn_events++;   /* the number that can't lie (each = one chirp) */
            mqtt_publish_llevent("connect", l->bms_id, 0);
            l->conn_handle = event->connect.conn_handle;
            /* Request larger MTU, then discover the JK service. */
            ble_gattc_exchange_mtu(l->conn_handle, NULL, NULL);        /* NIMBLE-PASS */
            ble_uuid16_t svc = BLE_UUID16_INIT(JK_SVC_UUID);
            ble_gattc_disc_svc_by_uuid(l->conn_handle, &svc.u, on_svc_disc, l); /* NIMBLE-PASS */
        } else if (expected) {
            ESP_LOGW(TAG, "connect failed bms %u st=%d", l->bms_id, event->connect.status);
            set_link_state(l->bms_id, LINK_UNREACHABLE, false);
            if (l->txn_active) { l->txn_active = false;
                respond(l->bms_id, l->txn.cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE); }
            l->in_use = false;
        }   /* failed AND unexpected: nothing of ours to clean up */
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        s_disc_events++;
        /* reason 0x08=supervision timeout, 0x13=peer terminated, 0x16=us */
        ESP_LOGW(TAG, "LL disconnect conn=%u reason=0x%02x",
                 event->disconnect.conn.conn_handle,
                 event->disconnect.reason);
        link_t *l = link_by_conn(event->disconnect.conn.conn_handle);
        mqtt_publish_llevent("disconnect", l ? l->bms_id : 0xFF,
                             event->disconnect.reason);
        if (l) {
            set_link_state(l->bms_id, LINK_REACHABLE_IDLE, false);
            if (l->txn_active) { l->txn_active = false;
                respond(l->bms_id, l->txn.cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE); }
            l->in_use = false;
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONN_UPDATE_REQ: {
        /* LL-path request: counter-propose our envelope via self_params. */
        struct ble_gap_upd_params *self = event->conn_update_req.self_params;
        if (self) {
            self->itvl_min = CFG_CONN_ITVL_MIN_MS * 4 / 5;
            self->itvl_max = CFG_CONN_ITVL_MAX_MS * 4 / 5;
            self->latency  = CFG_CONN_LATENCY;
            self->supervision_timeout = CFG_CONN_SUPERVISION_MS / 10;
        }
        return 0;
    }
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: {
        /* Plan B (16:35): REJECTING the peer's param request makes the JK
         * module hang up (constant chirping, disc climbing). So ACCEPT its
         * 15-20 ms request — then, 2 s later, renegotiate from the central
         * side with our latency envelope. A master-initiated LL connection
         * update cannot be refused by the peripheral. */
        /* Final policy (16:38): the module re-requests 16/0/600 forever and a
         * renegotiation war chirps ~every 20 s. ACCEPT ITS PARAMS, FULL STOP —
         * at 14:56 all three banks streamed for minutes at native params once
         * the terminate policy, scan storms, and starvation were fixed. */
        ESP_LOGI(TAG, "accepting peer params (no renegotiation)");
        return 0;
    }
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0)
            ESP_LOGI(TAG, "conn params now: itvl=%u lat=%u sup=%u (status %d)",
                     d.conn_itvl, d.conn_latency, d.supervision_timeout,
                     event->conn_update.status);
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

/* GATT write completion for app/balance writes (host task). The ATT-layer ack
 * IS the response for a write-with-response — JK's protocol-level reply (C8
 * ack, records) arrives as notifications, which the transparent TUN_RAW path
 * forwards to the app. Without this completion the txn could never finish:
 * the §11 sweeper then terminated the BMS link at timeout_ms, and the repeated
 * WRITE_RESULT timeouts tripped Node B's WRITE_FAIL_LIMIT, dropping the app
 * ("cell info briefly, then blank", 2026-08-28). */
static int on_gatt_write_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    xSemaphoreTake(s_mtx_link_pool, portMAX_DELAY);
    link_t *l = link_by_conn(conn_handle);
    if (l && l->txn_active &&
        (l->txn.kind == TXN_RAW_WRITE || l->txn.kind == TXN_BALANCE_WRITE)) {
        l->txn_active = false;
        respond(l->bms_id, l->txn.cmd_id,
                (error && error->status != 0) ? RESP_GATT_ERR : RESP_OK,
                NULL, 0, JK_REC_NONE);
    }
    xSemaphoreGive(s_mtx_link_pool);
    return 0;
}

/* ---- transaction execution (ble_owner_task) ---------------------------- */
/* Scan callback: match the target BMS by advertised name, then connect to
 * whatever address it advertised (spec §5 — match on name, never MAC). */
static int scan_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (!s_connecting) return 0;
        /* PASSIVE scan + ADDRESS match: no SCAN_REQ traffic (scan-reqs CHIRP
         * the units — parked bank 3 chirped during scans that never touched
         * it), and public-address matching keeps the clone self-loop
         * impossible (clones use static-random addresses). */
        if (event->disc.addr.type != BLE_ADDR_PUBLIC) return 0;
        if (memcmp(event->disc.addr.val, s_connect_addr, 6) != 0) return 0;
        ble_gap_disc_cancel();
        ble_addr_t addr = event->disc.addr;
        link_t *l = s_connecting;
        /* Resolve the scan phase BEFORE connecting: DISC events arrive in
         * bursts (no duplicate filtering — and unit 0's Telink advertises
         * fast, even while connected). Leaving s_connecting set until the
         * CONNECT event let a queued duplicate DISC re-enter here, fail
         * ble_gap_connect (busy), and free the link while the first connect
         * was still pending; the orphan then completed into a freed slot and
         * every arbiter retry stacked another connection onto the module. */
        s_connecting = NULL;
        s_conn_inflight = l;
        /* Long-interval, long-supervision connection (see config.h rationale). */
        static const struct ble_gap_conn_params cp = {
            .scan_itvl = 0x0010, .scan_window = 0x0010,
            .itvl_min = CFG_CONN_ITVL_MIN_MS * 4 / 5,   /* ms -> 1.25 ms units */
            .itvl_max = CFG_CONN_ITVL_MAX_MS * 4 / 5,
            .latency  = CFG_CONN_LATENCY,
            .supervision_timeout = CFG_CONN_SUPERVISION_MS / 10, /* 10 ms units */
            .min_ce_len = 0, .max_ce_len = 0,
        };
        if (ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 5000, &cp, gap_event, l) != 0) {
            s_conn_inflight = NULL;
            l->txn_active = false;
            respond(l->bms_id, l->txn.cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE);
            l->in_use = false;
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* Scan window ended with no match -> unreachable. */
        if (s_connecting) {
            link_t *l = s_connecting; s_connecting = NULL;
            set_link_state(l->bms_id, LINK_UNREACHABLE, false);
            if (l->txn_active) { l->txn_active = false;
                respond(l->bms_id, l->txn.cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE); }
            l->in_use = false;
        }
        return 0;
    default: return 0;
    }
}

static void start_connect(link_t *l)
{
    const char *name = name_for(l->bms_id);
    const uint8_t *addr = addr_for(l->bms_id);
    if (!name || !addr || !s_ble_enabled || s_connecting || s_conn_inflight ||
        s_scan_active || net_wifi_down_ms() > CFG_WIFI_QUIESCE_MS) {
        /* No target, another scan/connect in flight, a diagnostic scan owns
         * the radio, or WiFi is re-associating (shared radio — scanning now
         * would starve the 802.11 handshake). Fail the txn; the arbiter
         * retries. */
        l->txn_active = false;
        respond(l->bms_id, l->txn.cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE);
        l->in_use = false;
        return;
    }
    ESP_LOGI(TAG, "scanning for bms %u ('%s')", l->bms_id, name);
    s_connecting = l;
    strlcpy(s_connect_name, name, sizeof(s_connect_name));
    memcpy(s_connect_addr, addr, 6);
    /* DUTY-CYCLED scan: 30 ms window / 100 ms interval (~30%% radio), NOT the
     * NimBLE default continuous scan. The C3 shares one radio: continuous
     * connect-scans starved WiFi of even null-frame airtime ("wifi:m f null"
     * flood -> zombie association -> MQTT dead, 2026-08-28). JK units
     * advertise ~1/s, so a 5 s window at 30%% still catches them. */
    /* ACTIVE scan required: JK modules carry the device name in the SCAN
     * RESPONSE, so passive scanning never matches (verified 16:31: 93 s of
     * passive windows, zero connects). Chirp audio correlated with LL
     * connect/disconnect events, not scan-reqs, so active costs nothing. */
    struct ble_gap_disc_params dp = { .passive = 1, .itvl = 160, .window = 48 };
    if (ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 5000, &dp, scan_event, NULL) != 0) {
        s_connecting = NULL;
        l->txn_active = false;
        respond(l->bms_id, l->txn.cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE);
        l->in_use = false;
    }
}

static void exec_request(const bms_request_t *req)
{
    xSemaphoreTake(s_mtx_link_pool, portMAX_DELAY);
    link_t *l = link_by_bms(req->bms_id);

    if (req->kind == TXN_CONNECT) {
        if (l && l->conn_handle) {   /* already up */
            respond(req->bms_id, req->cmd_id, RESP_OK, NULL, 0, JK_REC_NONE);
        } else if (l && (l == s_connecting || l == s_conn_inflight)) {
            /* This bank is already mid scan/connect: don't stomp its txn or
             * free its slot via start_connect's refuse path. Fail this
             * request; the arbiter's retry lands after resolution. */
            respond(req->bms_id, req->cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE);
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
        int rc = ffe1_needs_write_cmd(l)
               ? ble_gattc_write_no_rsp_flat(l->conn_handle, l->val_handle, cmd, n)
               : ble_gattc_write_flat(l->conn_handle, l->val_handle, cmd, n, NULL, NULL); /* NIMBLE-PASS */
        if (rc) {   /* proc-pool exhaustion etc: fail LOUDLY, don't fake a txn */
            l->txn_active = false;
            ESP_LOGW(TAG, "poll 0x%02x write bms %u rc=%d", req->opcode, req->bms_id, rc);
            respond(req->bms_id, req->cmd_id, RESP_GATT_ERR, NULL, 0, JK_REC_NONE);
        }
        break;
    }
    case TXN_RAW_WRITE: {
        /* App write relayed verbatim. idx 1 = the FFE2 command characteristic
         * (the clone mirrors it); everything else = FFE1. FFE1 write-with-rsp
         * completion = ATT write ack (on_gatt_write_done). Op per discovered
         * props — see ffe1_needs_write_cmd/ffe2_needs_write_req. */
        if (req->idx == 1 && l->ffe2_handle) {
            int wrc = ffe2_needs_write_req(l)
                    ? ble_gattc_write_flat(l->conn_handle, l->ffe2_handle,
                                           req->payload, req->payload_len, NULL, NULL)
                    : ble_gattc_write_no_rsp_flat(l->conn_handle, l->ffe2_handle,
                                                  req->payload, req->payload_len); /* NIMBLE-PASS */
            if (wrc) ESP_LOGW(TAG, "ffe2 write bms %u rc=%d", req->bms_id, wrc);
            respond(req->bms_id, req->cmd_id, RESP_OK, NULL, 0, JK_REC_NONE);
            break;
        }
        if (ffe1_needs_write_cmd(l)) {      /* no ATT ack exists for a Write Command */
            int wrc = ble_gattc_write_no_rsp_flat(l->conn_handle, l->val_handle,
                                                  req->payload, req->payload_len);
            respond(req->bms_id, req->cmd_id, wrc ? RESP_GATT_ERR : RESP_OK,
                    NULL, 0, JK_REC_NONE);
            break;
        }
        l->txn_active = req->response_needed; l->txn = *req;
        l->txn_deadline_us = esp_timer_get_time() + req->timeout_ms * 1000LL;
        int rc = ble_gattc_write_flat(l->conn_handle, l->val_handle, req->payload,
                             req->payload_len, on_gatt_write_done, NULL);   /* NIMBLE-PASS */
        if (rc) {
            l->txn_active = false;
            ESP_LOGW(TAG, "raw write bms %u rc=%d", req->bms_id, rc);
            respond(req->bms_id, req->cmd_id, RESP_GATT_ERR, NULL, 0, JK_REC_NONE);
        } else if (!req->response_needed)
            respond(req->bms_id, req->cmd_id, RESP_OK, NULL, 0, JK_REC_NONE);
        break;
    }
    case TXN_BALANCE_WRITE: {
        /* Reaches here only if JK_ENABLE_WRITES built the frame (else the
         * arbiter never enqueued it). Settings writes ride FFE1 — the channel
         * the app and esphome-jk-bms use on every unit. FFE2 worked on the
         * trio but unit 0's Telink ACKS FFE2 writes at ATT level and silently
         * drops them (proved 2026-08-30: cell_count via FFE2 acked+ignored;
         * the app's FFE1 writes recalibrated). Op per discovered props.
         * No ATT ack on write-cmd, so the arbiter confirms by settings
         * readback (§10), not write status. */
        if (!l->val_handle) {
            respond(req->bms_id, req->cmd_id, RESP_GATT_ERR, NULL, 0, JK_REC_NONE);
            break;
        }
        int wrc = ffe1_needs_write_cmd(l)
                ? ble_gattc_write_no_rsp_flat(l->conn_handle, l->val_handle,
                                              req->payload, req->payload_len)
                : ble_gattc_write_flat(l->conn_handle, l->val_handle,
                                       req->payload, req->payload_len, NULL, NULL); /* NIMBLE-PASS */
        if (wrc) ESP_LOGW(TAG, "balance write bms %u rc=%d", req->bms_id, wrc);
        respond(req->bms_id, req->cmd_id, wrc ? RESP_GATT_ERR : RESP_OK,
                NULL, 0, JK_REC_NONE);
        break;
    }
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
            l->txn_active = false;
            respond(l->bms_id, l->txn.cmd_id, RESP_TIMEOUT, NULL, 0, JK_REC_NONE);
            /* NEVER terminate a healthy LL link for slow data (audio-correlated
             * 2026-08-28: chirps == LL connect/disconnect events, count-matched
             * — the 3-strike terminate WAS the chirp machine). An unarmed link
             * held silently costs one chirp ever; the supervisor re-arms it
             * in place. The 8 s supervision timeout handles truly dead links. */
            l->timeout_strikes++;
            ESP_LOGW(TAG, "bms %u txn timeout (strike %u, link held)",
                     l->bms_id, l->timeout_strikes);
        }
    }
    xSemaphoreGive(s_mtx_link_pool);
}

/* ---- diagnostic scan dump ---------------------------------------------- */
static void publish_scan_dump(void)
{
    static char buf[2048];   /* static: keep off the host-task stack */
    int o = snprintf(buf, sizeof(buf), "{\"n\":%d,\"devs\":[", s_scan_n);
    for (int i = 0; i < s_scan_n && o < (int)sizeof(buf) - 96; i++) {
        const scan_rec_t *r = &s_scan[i];
        char safe[32]; int so = 0;         /* minimal JSON-string sanitising */
        for (int k = 0; r->name[k] && so < (int)sizeof(safe) - 1; k++) {
            char c = r->name[k];
            safe[so++] = (c == '"' || c == '\\' || (unsigned char)c < 0x20) ? '.' : c;
        }
        safe[so] = 0;
        o += snprintf(buf + o, sizeof(buf) - o,
            "%s{\"a\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"t\":%d,\"rssi\":%d,\"name\":\"%s\"}",
            i ? "," : "", r->addr.val[5], r->addr.val[4], r->addr.val[3],
            r->addr.val[2], r->addr.val[1], r->addr.val[0], r->addr.type, r->rssi, safe);
    }
    if (o < (int)sizeof(buf) - 3) o += snprintf(buf + o, sizeof(buf) - o, "]}");
    mqtt_publish_scan(buf);
    ESP_LOGI(TAG, "scan dump: %d device(s) published to jkbms/bridge/scan", s_scan_n);
}

/* Runs on the host task (NimBLE callback). */
static int scan_dump_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields f;
        char nm[32] = {0};
        if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) == 0
            && f.name_len) {
            uint8_t n = f.name_len < 31 ? f.name_len : 31;
            memcpy(nm, f.name, n);
        }
        ble_addr_t a = event->disc.addr;
        for (int i = 0; i < s_scan_n; i++)          /* dedup by address */
            if (s_scan[i].addr.type == a.type && !memcmp(s_scan[i].addr.val, a.val, 6)) {
                if (nm[0] && !s_scan[i].name[0]) strlcpy(s_scan[i].name, nm, sizeof(s_scan[i].name));
                if (event->disc.rssi > s_scan[i].rssi) s_scan[i].rssi = event->disc.rssi;
                return 0;
            }
        if (s_scan_n < SCAN_DUMP_MAX) {
            s_scan[s_scan_n].addr = a;
            s_scan[s_scan_n].rssi = event->disc.rssi;
            strlcpy(s_scan[s_scan_n].name, nm, sizeof(s_scan[s_scan_n].name));
            s_scan_n++;
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        publish_scan_dump();
        s_scan_active = false;
        return 0;
    default: return 0;
    }
}

/* Runs on ble_owner_task. Takes the radio (cancelling any in-flight connect
 * scan) and starts an 8 s active discovery reported by scan_dump_event. */
static void do_scan_dump(void)
{
    xSemaphoreTake(s_mtx_link_pool, portMAX_DELAY);
    if (s_conn_inflight) {
        /* A connect is pending in the controller — scanning would fail and
         * cancelling isn't ours to do. Skip; the operator can re-request. */
        ESP_LOGW(TAG, "scan dump: connect in flight — skipped");
        xSemaphoreGive(s_mtx_link_pool);
        return;
    }
    if (s_connecting) {
        ble_gap_disc_cancel();
        link_t *l = s_connecting; s_connecting = NULL;
        if (l->txn_active) { l->txn_active = false;
            respond(l->bms_id, l->txn.cmd_id, RESP_LINK_DOWN, NULL, 0, JK_REC_NONE); }
        l->in_use = false;
    }
    s_scan_n = 0;
    s_scan_active = true;
    xSemaphoreGive(s_mtx_link_pool);

    ESP_LOGI(TAG, "scan dump: starting 8 s discovery");
    struct ble_gap_disc_params dp = { .passive = 0, .itvl = 160, .window = 48 };
    if (ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 8000, &dp, scan_dump_event, NULL) != 0) {
        ESP_LOGW(TAG, "scan dump: ble_gap_disc failed");
        s_scan_active = false;
    }
}

/* ---- tasks ------------------------------------------------------------- */
static void ble_owner_task(void *arg)
{
    for (;;) {
        if (s_scan_req && !s_scan_active) { s_scan_req = false; do_scan_dump(); }
        {   /* central-initiated param renegotiation (see L2CAP_UPDATE_REQ) */
            int64_t nowus = esp_timer_get_time();
            xSemaphoreTake(s_mtx_link_pool, portMAX_DELAY);
            for (int i = 0; i < CFG_LINK_POOL_SIZE; i++) {
                link_t *l = &s_links[i];
                if (l->in_use && l->conn_handle && l->renegotiate_at_us &&
                    nowus > l->renegotiate_at_us) {
                    l->renegotiate_at_us = 0;
                    struct ble_gap_upd_params up = {
                        .itvl_min = CFG_CONN_ITVL_MIN_MS * 4 / 5,
                        .itvl_max = CFG_CONN_ITVL_MAX_MS * 4 / 5,
                        .latency  = CFG_CONN_LATENCY,
                        .supervision_timeout = CFG_CONN_SUPERVISION_MS / 10,
                        .min_ce_len = 0, .max_ce_len = 0,
                    };
                    int rc = ble_gap_update_params(l->conn_handle, &up);
                    ESP_LOGI(TAG, "bms %u central param update rc=%d", l->bms_id, rc);
                }
            }
            xSemaphoreGive(s_mtx_link_pool);
        }
        if (s_gd_req >= 0 && !s_gd.active) { int id = s_gd_req; s_gd_req = -1; do_gatt_dump(id); }

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
