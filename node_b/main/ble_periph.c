/*
 * ble_periph.c — single shared GATT table + per-identity routing (spec §5/§6).
 * NIMBLE-PASS markers flag lines whose exact NimBLE signatures/structs must be
 * confirmed against the pinned IDF next week.
 *
 * WHY ONE TABLE: a BLE peripheral has exactly one attribute table shared by
 * every connection. Registering four copies would expose four 0xFFE0 services
 * and the app would bind the first 0xFFE1 it found — wrong unit. All four JK
 * units share an identical layout (asserted at harvest), so we register the
 * table ONCE and let the advertising set (address) the client connected on
 * decide which identity — and therefore which cache — its traffic maps to.
 */
#include <string.h>
#include "ble_periph.h"
#include "config.h"
#include "nb_state.h"
#include "adv_mgr.h"
#include "tunnel_cli.h"
#include "esp_log.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "ble_periph";
static uint16_t s_val_handle;     /* 0xFFE1 value handle (idx 0) */

/* ---- identity resolution ------------------------------------------------ */
static int identity_for_conn(uint16_t handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(handle, &desc) != 0) return -1;
    /* The set's per-instance random address is the OTA (over-the-air) local
     * address; our_id_addr may be the device identity address instead. */
    int id = adv_mgr_identity_for_addr(desc.our_ota_addr.val);
    if (id < 0) id = adv_mgr_identity_for_addr(desc.our_id_addr.val);
    if (id < 0)
        ESP_LOGW(TAG, "conn %u no identity: ota=%02x:%02x:%02x:%02x:%02x:%02x "
                 "id=%02x:%02x:%02x:%02x:%02x:%02x", handle,
                 desc.our_ota_addr.val[5], desc.our_ota_addr.val[4], desc.our_ota_addr.val[3],
                 desc.our_ota_addr.val[2], desc.our_ota_addr.val[1], desc.our_ota_addr.val[0],
                 desc.our_id_addr.val[5], desc.our_id_addr.val[4], desc.our_id_addr.val[3],
                 desc.our_id_addr.val[2], desc.our_id_addr.val[1], desc.our_id_addr.val[0]);
    return id;
}

/* ---- GATT access callback (synchronous, host task) --------------------- */
static int chr_access(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int id = identity_for_conn(conn);
    if (id < 0) return BLE_ATT_ERR_UNLIKELY;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        /* Answer from this identity's cache — never a LAN round trip here
         * (spec §6: the access callback blocks the host task). */
        nb_cache_t c; nb_get_cache(id, 0, &c);
        return os_mbuf_append(ctxt->om, c.data, c.len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES; /* NIMBLE-PASS */
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint8_t buf[256];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(buf)) len = sizeof(buf);
        ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);        /* NIMBLE-PASS */
        /* Complete the ATT write immediately; forward to A (spec §6). The app
         * confirms at the frame level via notifications, not ATT status. */
        bool with_resp = (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR);
        tunnel_cli_send_write(id, 0, with_resp, buf, len);
        return 0;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ---- mirrored standard services (spec §5 fidelity) ---------------------- */
/* The real JK BLE module (gattdump 2026-08-28, bank 3) exposes GAP name, a
 * full Device Information Service, a Battery Service and a second FFE2
 * write-no-rsp characteristic. The official app's "device information" step
 * READS the DIS — without it the app pops "request device information
 * failure" no matter how good the FFE1 stream is. Mirror them. */
#define UUID_DIS        0x180A
#define UUID_BATT       0x180F
#define CHR_MANUFACTURER 0x2A29
#define CHR_MODEL        0x2A24
#define CHR_SERIAL       0x2A25
#define CHR_HW_REV       0x2A27
#define CHR_FW_REV       0x2A26
#define CHR_SW_REV       0x2A28
#define CHR_SYSTEM_ID    0x2A23
#define CHR_REG_CERT     0x2A2A
#define CHR_PNP_ID       0x2A50
#define CHR_BATT_LEVEL   0x2A19
#define JK_CHR2_UUID     0xFFE2

/* Fleet constants from the real units' 0x03 device-info frames. Serials are
 * per-identity where captured; placeholders otherwise (presence of the read
 * is what the app needs; values are display-only). */
static const char *DIS_SERIAL[4] = {
    "504185749000000",            /* unit 0 (parked) — placeholder            */
    "504185749007323",            /* BMS 1-01 (captured)                      */
    "504185749000000",            /* BMS 2-02 — placeholder                   */
    "504185749007494",            /* BMS_3-03 (captured)                      */
};

static int dis_access(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    int id = identity_for_conn(conn);
    uint16_t u = ble_uuid_u16(ctxt->chr->uuid);
    const char *str = NULL;
    switch (u) {
    case CHR_MANUFACTURER: str = "JK-BMS";        break;
    case CHR_MODEL:        str = "JK-PB2A16S20P"; break;
    case CHR_SERIAL:       str = DIS_SERIAL[(id >= 0 && id < 4) ? id : 0]; break;
    case CHR_HW_REV:       str = "19A";           break;
    case CHR_FW_REV:       str = "19.31";         break;
    case CHR_SW_REV:       str = "19.31";         break;
    case CHR_SYSTEM_ID: {
        static const uint8_t sysid[8] = {0};
        return os_mbuf_append(ctxt->om, sysid, sizeof(sysid)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    case CHR_REG_CERT: {
        static const uint8_t cert[4] = {0};
        return os_mbuf_append(ctxt->om, cert, sizeof(cert)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    case CHR_PNP_ID: {
        static const uint8_t pnp[7] = {0x01, 0, 0, 0, 0, 0, 0};
        return os_mbuf_append(ctxt->om, pnp, sizeof(pnp)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    default: return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, (const uint8_t *)str, strlen(str))
           ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static int batt_access(uint16_t conn, uint16_t attr,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    /* Serve the REAL SoC: the identity's cache holds the last complete FFE1
     * frame; a 0x02 cell-info record carries SoC at offset 173. */
    uint8_t soc = 100;
    int id = identity_for_conn(conn);
    if (id >= 0) {
        nb_cache_t c; nb_get_cache((uint8_t)id, 0, &c);
        if (c.len >= 174 && c.data[4] == 0x02) soc = c.data[173];
    }
    return os_mbuf_append(ctxt->om, &soc, 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

/* FFE2: write-no-response command characteristic (mirrors the real module).
 * Forwarded with idx=1 so Node A writes it to the real FFE2. */
static int chr2_access(uint16_t conn, uint16_t attr,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    int id = identity_for_conn(conn);
    if (id < 0) return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[256];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(buf)) len = sizeof(buf);
    ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
    tunnel_cli_send_write((uint8_t)id, 1, false, buf, len);

    /* Warm-start replay (2026-08-30): the app's opener is a command frame
     * AA 55 90 EB <op> ... on FFE2 — 0x97 asks for device-info, 0x96 for cell
     * info, each expecting a NOTIFY back. If the real module is asleep, A
     * can't answer in time and the app fails with "request device information
     * failure". Answer instantly from B's warm cache so the app stays
     * connected; the live stream takes over once A wakes the module. */
    if (len >= 5 && buf[0]==0xAA && buf[1]==0x55 && buf[2]==0x90 && buf[3]==0xEB) {
        uint8_t rec = buf[4]==0x97 ? 0x03 : buf[4]==0x96 ? 0x02 : 0x00;
        if (rec) {
            nb_cache_t w; nb_get_warm((uint8_t)id, rec, &w);
            nb_identity_t it; nb_get_identity((uint8_t)id, &it);
            if (w.len && it.notify_enabled) {
                ble_periph_forward_notify((uint8_t)id, 0, w.data, w.len);
            } else if (w.len) {
                /* The app writes its opener BEFORE subscribing (measured with
                 * app_probe 2026-08-30), so an instant replay here would be
                 * dropped by the CCCD gate. Owe it; the tunnel task's
                 * replay tick delivers once notifications come up. */
                nb_mark_replay((uint8_t)id, rec == 0x03 ? NB_REPLAY_DEVINFO
                                                        : NB_REPLAY_CELLINFO);
            }
        }
    }
    return 0;
}

/* Deliver replays owed by chr2_access. Called ~10 Hz from the tunnel task —
 * a safe notify context (it already forwards the raw stream); NEVER call
 * from the subscribe callback (that wedged the live stream, 2026-08-30). */
void ble_periph_replay_tick(void)
{
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        nb_identity_t it; nb_get_identity(id, &it);
        if (!it.connected || !it.notify_enabled || !it.pending_replay) continue;
        uint8_t bits = nb_take_replay(id);
        for (int r = 0; r < 2; r++) {
            uint8_t bit = r == 0 ? NB_REPLAY_DEVINFO : NB_REPLAY_CELLINFO;
            if (!(bits & bit)) continue;
            nb_cache_t w; nb_get_warm(id, r == 0 ? 0x03 : 0x02, &w);
            if (w.len) ble_periph_forward_notify(id, 0, w.data, w.len);
        }
    }
}

static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = BLE_UUID16_DECLARE(JK_SVC_UUID),
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = BLE_UUID16_DECLARE(JK_CHR2_UUID),   /* FFE2 first: matches
                                                          * real module order */
            .access_cb = chr2_access,
            .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
          { .uuid = BLE_UUID16_DECLARE(JK_CHR_UUID),
            .access_cb = chr_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                     BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &s_val_handle },
          { 0 }
      } },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = BLE_UUID16_DECLARE(UUID_DIS),
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = BLE_UUID16_DECLARE(CHR_MANUFACTURER), .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
          { .uuid = BLE_UUID16_DECLARE(CHR_MODEL),        .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
          { .uuid = BLE_UUID16_DECLARE(CHR_SERIAL),       .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
          { .uuid = BLE_UUID16_DECLARE(CHR_HW_REV),       .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
          { .uuid = BLE_UUID16_DECLARE(CHR_FW_REV),       .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
          { .uuid = BLE_UUID16_DECLARE(CHR_SW_REV),       .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
          { .uuid = BLE_UUID16_DECLARE(CHR_SYSTEM_ID),    .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
          { .uuid = BLE_UUID16_DECLARE(CHR_REG_CERT),     .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
          { .uuid = BLE_UUID16_DECLARE(CHR_PNP_ID),       .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
          { 0 }
      } },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = BLE_UUID16_DECLARE(UUID_BATT),
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = BLE_UUID16_DECLARE(CHR_BATT_LEVEL),
            .access_cb = batt_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
          { 0 }
      } },
    { 0 }
};

/* ---- notify fan-in (from tunnel) --------------------------------------- */
void ble_periph_forward_notify(uint8_t id, uint8_t idx, const uint8_t *data, uint16_t len)
{
    (void)idx;
    nb_identity_t it; nb_get_identity(id, &it);
    if (!it.connected || !it.notify_enabled) return;   /* CCCD filter (spec §6) */

    uint16_t mtu = ble_att_mtu(it.conn_handle);        /* NIMBLE-PASS */
    if (mtu < 23) mtu = 23;
    uint16_t chunk = mtu - 3;                           /* re-chunk (spec §6)   */
    for (uint16_t off = 0; off < len; off += chunk) {
        uint16_t n = (len - off < chunk) ? (len - off) : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + off, n); /* NIMBLE-PASS */
        if (!om) break;
        ble_gatts_notify_custom(it.conn_handle, s_val_handle, om);  /* NIMBLE-PASS */
    }
}

void ble_periph_on_write_result(uint8_t id, uint8_t idx, uint8_t status)
{
    (void)idx;
    nb_identity_t it; nb_get_identity(id, &it);
    if (!it.connected) return;
    if (status == TUN_WR_LINK_DOWN) {
        ESP_LOGW(TAG, "id %u write link-down — dropping app conn (spec §6)", id);
        ble_gap_terminate(it.conn_handle, BLE_ERR_REM_USER_CONN_TERM);   /* NIMBLE-PASS */
    }
    /* WRITE_FAIL_LIMIT consecutive failures also drop; counter lives in
     * nb_state and is reset on CLIENT transitions (spec §6 hygiene). */
}

void ble_periph_drop_all(void)
{
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        nb_identity_t it; nb_get_identity(id, &it);
        if (it.connected) ble_gap_terminate(it.conn_handle, BLE_ERR_REM_USER_CONN_TERM); /* NIMBLE-PASS */
    }
}

/* ---- GAP events --------------------------------------------------------- */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) return 0;
        uint16_t h = event->connect.conn_handle;
        int id = identity_for_conn(h);
        if (id < 0) { ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM); return 0; }

        /* One app connection at a time: accept-then-terminate a second
         * (BLE has no reject primitive) — spec §5. */
        if (nb_active_conn_count() >= CFG_MAX_APP_CONNS) {
            ESP_LOGW(TAG, "2nd connection on id %u — terminating", id);
            ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        nb_set_conn(id, true, h);
        adv_mgr_on_connect(id);
        /* GAP device name = this identity (the real module's 2a00 returns the
         * unit's own name; apps cross-check it against the advertised name).
         * Safe as a global because only one app connection is allowed. */
        { nb_identity_t it; nb_get_identity(id, &it);
          if (it.have_name) ble_svc_gap_device_name_set(it.name); }
        tunnel_cli_send_client(id, true);       /* drives A's connect-on-demand */
        ESP_LOGI(TAG, "app connected -> identity %u", id);
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t h = event->disconnect.conn.conn_handle;
        int id = nb_identity_for_conn(h);
        if (id >= 0) {
            nb_set_conn(id, false, 0);
            adv_mgr_on_disconnect(id);
            tunnel_cli_send_client(id, false);
            ESP_LOGI(TAG, "app disconnected from identity %u", id);
        }
        return 0;
    }
    case BLE_GAP_EVENT_SUBSCRIBE: {
        int id = nb_identity_for_conn(event->subscribe.conn_handle);
        if (id >= 0) nb_set_notify(id, event->subscribe.cur_notify);  /* CCCD */
        /* NOTE: do NOT send notifications from inside this handler — replaying
         * warm frames here (2026-08-30 attempt) wedged the live stream to the
         * app (bank 1 connected but showed no data). Warm replay stays on the
         * FFE2 write path (chr2_access) only. */
        return 0;
    }
    case BLE_GAP_EVENT_MTU:
        return 0;
    default:
        return 0;
    }
}
/* adv_mgr's ext-adv sets are configured with this same gap_event callback so
 * connections route here. Exposed for adv_mgr via a shared declaration. */
int ble_periph_gap_event(struct ble_gap_event *e, void *a) { return gap_event(e, a); }

/* ---- table registration ------------------------------------------------- */
void ble_periph_rebuild_table(void)
{
    /* JK's layout is a single service/characteristic, identical across units,
     * so the static definition above already is the shared table. If a future
     * fleet needs a data-driven table, build s_svcs from the blueprint here and
     * rotate the sets' random addresses so iOS re-discovers (spec §5 sticky
     * cache). For now, log that the blueprint arrived. */
    nb_blueprint_t bp; nb_get_blueprint(&bp);
    ESP_LOGI(TAG, "blueprint received: %u char(s)", bp.char_count);
}

void ble_periph_start(void)
{
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("JK-Tunnel");   /* generic GAP name (spec §5) */

    ble_gatts_count_cfg(s_svcs);                 /* NIMBLE-PASS */
    ble_gatts_add_svcs(s_svcs);                  /* NIMBLE-PASS */
    ESP_LOGI(TAG, "shared replica GATT table registered");
}
