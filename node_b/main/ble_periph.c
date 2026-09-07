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
#include "stream_start.h"
#include "adv_mgr.h"
#include "tunnel_cli.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "ble_periph";
static uint16_t s_val_handle;     /* 0xFFE1 value handle (idx 0) */
static void forward_notify(uint8_t id, uint8_t idx, const uint8_t *data,
                           uint16_t len, bool replay);
static bool notify_session(uint8_t id, const uint8_t *data, uint16_t len,
                           bool replay, const nb_notify_session_t *session);
/* Only the tunnel task owns these buffers. GAP callbacks change the epoch
 * under nb_state's lock, never this parser or any notification buffers. */
static struct {
    uint64_t epoch;
    bool aligned;
    stream_start_t start;
} s_live[CFG_NUM_UNITS];

/* Compare only the documented public model/hardware/software bytes6..37.
 * Never fingerprint the remainder: it can contain passwords and passcodes.
 * Header chunks shorter than38 cannot supply these fields. Observation only;
 * this does not validate the frame or decide which cache to replay. */
static void diag_dev_metadata(uint8_t id, bool replay, const uint8_t *data, uint16_t len)
{
    if (len < 38) {
        ESP_LOGI(TAG, "diag dev id=%u src=%s fields=short len=%u",
                 id, replay ? "replay" : "live", len);
        return;
    }
    uint32_t sig = 2166136261u;
    unsigned model = 0, hw = 0, sw = 0;
    for (unsigned k = 6; k < 38; k++) {
        sig = (sig ^ data[k]) * 16777619u;
        if (data[k]) {
            if (k < 22) model++;
            else if (k < 30) hw++;
            else sw++;
        }
    }
    ESP_LOGI(TAG, "diag dev id=%u src=%s ctr=%u model_nz=%u hw_nz=%u sw_nz=%u public_sig=%08lX",
             id, replay ? "replay" : "live", data[5], model, hw, sw, (unsigned long)sig);
}

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
/* Opener warm replay, shared by BOTH write characteristics: the phone app
 * writes its 0x97/0x96 opener on FFE1 (proved live 2026-08-30 — replay
 * hooked only into FFE2 never fired for the phone), app_probe on FFE2.
 * 0x97 -> device-info; 0x96 -> settings THEN cell-info (the module streams
 * both after a 96, and the app appears to need settings before it paints). */
static void maybe_replay_opener(int id, const uint8_t *buf, uint16_t len)
{
    if (len < 5 || buf[0]!=0xAA || buf[1]!=0x55 || buf[2]!=0x90 || buf[3]!=0xEB)
        return;
    /* A live armed unit never answers a 97 with a lone devinfo — settings
     * and cell frames flow around it. Every replay rejection today was a
     * lone devinfo into dead air; every acceptance had a stream around it.
     * Answer 97 with the full synthetic burst. */
    static const uint8_t recs97[] = { 0x03, 0x01, 0x02, 0 };
    static const uint8_t recs96[] = { 0x01, 0x02, 0 };
    const uint8_t *recs = buf[4]==0x97 ? recs97
                        : buf[4]==0x96 ? recs96 : NULL;
    if (!recs) return;
    /* This callback runs on nimble_host; replay DELIVERY happens only on
     * the tunnel task's tick, which also forwards the live TUN_RAW stream —
     * one task, total ordering, mid-frame interleave structurally
     * impossible (the 2026-08-30 "device is not supported" class). The tick
     * fires within 100 ms and injects at a frame boundary. */
    ESP_LOGI(TAG, "id %d opener 0x%02X owed link=%d", id, buf[4],
             nb_link_state((uint8_t)id));
    for (int i = 0; recs[i]; i++) {
        nb_cache_t w;   /* flags/copies only — never nb_identity_t here */
        if (recs[i] == 0x03) nb_get_warm_dev((uint8_t)id, 0, &w);
        else                 nb_get_warm((uint8_t)id, recs[i], &w);
        if (!w.len) continue;
        nb_mark_replay((uint8_t)id,
                       recs[i] == 0x03 ? NB_REPLAY_DEVINFO
                     : recs[i] == 0x01 ? NB_REPLAY_SETTINGS
                                       : NB_REPLAY_CELLINFO);
    }
}

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
        uint8_t buf[TUNNEL_MAX_WRITE_DATA];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL) != 0)
            return BLE_ATT_ERR_UNLIKELY;
        /* Complete the ATT write immediately; forward to A (spec §6). The app
         * confirms at the frame level via notifications, not ATT status. */
        bool with_resp = (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR);
        if (!tunnel_cli_send_write(id, 0, with_resp, buf, len))
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        /* THE APP'S OPENER ARRIVES HERE, ON FFE1 — not FFE2 (proved live
         * 2026-08-30: appwrites flowed while the FFE2 handler logged
         * nothing). Replay-on-FFE2-only meant the phone NEVER got a warm
         * replay; only app_probe (which wrote FFE2) did. Hook both paths. */
        maybe_replay_opener(id, buf, len);
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
    ESP_LOGI(TAG, "id %d DIS read 0x%04X", id, u);
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
    uint8_t buf[TUNNEL_MAX_WRITE_DATA];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL) != 0)
        return BLE_ATT_ERR_UNLIKELY;
    if (!tunnel_cli_send_write((uint8_t)id, 1, false, buf, len))
        return BLE_ATT_ERR_INSUFFICIENT_RES;

    /* Warm-start replay (2026-08-30): the app's opener is a command frame
     * AA 55 90 EB <op> ... on FFE2 — 0x97 asks for device-info, 0x96 for cell
     * info, each expecting a NOTIFY back. If the real module is asleep, A
     * can't answer in time and the app fails with "request device information
     * failure". Answer instantly from B's warm cache so the app stays
     * connected; the live stream takes over once A wakes the module. */
    maybe_replay_opener(id, buf, len);
    return 0;
}

/* Deliver replays owed by chr2_access. Called ~10 Hz from the tunnel task —
 * a safe notify context (it already forwards the raw stream); NEVER call
 * from the subscribe callback (that wedged the live stream, 2026-08-30). */
void ble_periph_replay_tick(void)
{
    /* Static: only the single tunnel task calls this, and that task keeps
     * its own frames small (see serve()) — don't stack a 322 B cache under
     * the notify chain. */
    static nb_cache_t w;
    static uint8_t s_ctr = 0x40;   /* replay frame counter, see stamp below */
    /* Delivery order: devinfo, settings, cell-info — the app's own ladder. */
    static const struct { uint8_t bit, rec; } seq[] = {
        { NB_REPLAY_DEVINFO,  0x03 },
        { NB_REPLAY_SETTINGS, 0x01 },
        { NB_REPLAY_CELLINFO, 0x02 },
    };
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        /* Decision matrix (nb_replay_action): link down -> deliver now
         * (clean pipe); link up -> give the live unit 2 s to answer the
         * opener in-stream, cancel the debt if it does, deliver anyway if
         * it stays deaf (mortal modules can stream while their inbound is
         * dead — 14:09 proof). Delivery is tick-serialized with the live
         * relay; worst case one live frame resyncs at its next header. */
        int act = nb_replay_action(id);
        if (act != 1) {
            if (act == 2) ESP_LOGI(TAG, "diag id=%u replay cancelled: dev_seen advanced", id);
            continue;
        }
        uint8_t bits = nb_take_replay(id);
        ESP_LOGI(TAG, "id %u replay deliver bits=0x%02X", id, bits);
        for (unsigned r = 0; r < sizeof(seq)/sizeof(seq[0]); r++) {
            if (!(bits & seq[r].bit)) continue;
            if (seq[r].rec == 0x03) nb_get_warm_dev(id, 0, &w);
            else                     nb_get_warm(id, seq[r].rec, &w);
            if (!w.len) {
                ESP_LOGI(TAG, "diag id=%u replay rec=%u cache empty", id, seq[r].rec);
                continue;
            }
            /* Metadata only: never log payloads (devinfo can hold a passcode).
             * Observe before stamping; do not add a new validation gate. */
            uint8_t old_ctr = w.len >= 6 ? w.data[5] : 0;
            uint8_t old_sum = 0;
            for (uint16_t k = 0; k + 1 < w.len; k++) old_sum += w.data[k];
            bool header_ok = w.len >= 5 && w.data[0] == 0x55 &&
                w.data[1] == 0xAA && w.data[2] == 0xEB && w.data[3] == 0x90 &&
                w.data[4] == seq[r].rec;
            bool checksum_ok = old_sum == w.data[w.len - 1];
            /* Stamp a FRESH frame counter and re-checksum: the app dedupes
             * on byte 5 — a cached frame replayed twice is silently
             * discarded the second time (proved 14:28: identical replay
             * accepted right after a cache refresh, ignored before and
             * after). Every replay must look like a new frame. */
            if (w.len >= 6) {
                w.data[5] = s_ctr++;
                uint8_t sum = 0;
                for (uint16_t k = 0; k < w.len - 1; k++) sum += w.data[k];
                w.data[w.len - 1] = sum;
            }
            forward_notify(id, 0, w.data, w.len, true);
            ESP_LOGI(TAG, "diag id=%u cache rec=%u len=%u hdr=%d sum=%d ctr=%u->%u",
                     id, seq[r].rec, w.len, header_ok, checksum_ok, old_ctr,
                     w.len >= 6 ? w.data[5] : 0);
        }
    }
}

/* Controller-truth audit (supervisor, every 10 s): an app connection can die
 * without a DISCONNECT event reaching us (observed 2026-08-30: a hard-reset
 * central left identity 1 "connected" forever, its adv set stopped — TUN 1
 * vanished from the air until a B reboot). Any identity whose handle the
 * controller no longer knows gets the full disconnect cleanup. */
void ble_periph_audit_conns(void)
{
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        int h = nb_conn_handle(id);
        if (h < 0) continue;
        if (ble_gap_conn_find((uint16_t)h, NULL) == 0) continue;   /* alive */
        ESP_LOGW(TAG, "audit: identity %u conn %d is gone — cleaning up", id, h);
        nb_set_conn(id, false, 0);
        adv_mgr_on_disconnect(id);
        tunnel_cli_send_client(id, false);
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
    (void)idx; /* Replica has one notifying characteristic, as before. */
    if (id >= CFG_NUM_UNITS) return;
    nb_notify_session_t session;
    nb_get_notify_session(id, &session);
    if (s_live[id].epoch != session.epoch) {
        s_live[id].epoch = session.epoch;
        s_live[id].aligned = false;
        s_live[id].start.len = 0;
    }
    if (!session.connected || !session.notify_enabled) return;
    if (!s_live[id].aligned) {
        uint16_t consumed;
        if (!stream_start_push(&s_live[id].start, data, len, &consumed)) return;
        bool sent = notify_session(id, s_live[id].start.data,
                                   STREAM_START_RECORD_LEN, false, &session);
        s_live[id].start.len = 0;
        if (!sent) return; /* Never open the gate on a partial submission. */
        s_live[id].aligned = true;
        ESP_LOGI(TAG, "stream start id=%u aligned", id);
        data += consumed;
        len -= consumed;
    }
    /* Preserve AT/C8/unknown auxiliary bytes after this first boundary.
     * Cached replay must NOT align the independent incoming live stream. */
    if (len) notify_session(id, data, len, false, &session);
}

/* Same chunking, bytes, mbuf ownership and error continuation as before,
 * but abort remaining chunks when the session changes. Single tunnel-task caller;
 * live error logs capped at one per second, routine cell traffic not logged.
 * rc=0 is host submission, NOT over-air delivery or phone acceptance. */
static void forward_notify(uint8_t id, uint8_t idx, const uint8_t *data,
                           uint16_t len, bool replay)
{
    (void)idx;
    nb_notify_session_t session;
    nb_get_notify_session(id, &session);
    notify_session(id, data, len, replay, &session);
}

static bool notify_session(uint8_t id, const uint8_t *data, uint16_t len,
                           bool replay, const nb_notify_session_t *session)
{
    nb_notify_session_t it = *session;
    bool devinfo = len >= 5 && data[0]==0x55 && data[1]==0xAA &&
        data[2]==0xEB && data[3]==0x90 && data[4]==0x03;
    if (!it.connected || !it.notify_enabled) {
        if (replay || devinfo)
            ESP_LOGI(TAG, "diag id=%u src=%s filtered connected=%d cccd=%d len=%u",
                     id, replay ? "replay" : "live", it.connected, it.notify_enabled, len);
        return false;   /* CCCD filter (spec §6) */
    }

    /* Existing early-attempt replay cancellation policy is unchanged, apart
     * from fencing it to this session. This does not establish delivery. */
    if (devinfo)
        nb_note_dev_forwarded(id, it.epoch);

    uint16_t mtu = ble_att_mtu(it.conn_handle);        /* NIMBLE-PASS */
    if (mtu < 23) mtu = 23;
    uint16_t chunk = mtu - 3;                           /* re-chunk (spec §6)   */
    /* Cap at the real module's native chunk: the JK bridge never notifies
     * more than 128 B, and the app IGNORED our MTU-sized (182 B) replay
     * chunks while accepting the module's 128s (live-proved 2026-08-30 —
     * replay-now fired, phone still hit "Request device information
     * failure"). Only replays exceed 128; TUN_RAW chunks already fit. */
    if (chunk > 128) chunk = 128;
    unsigned attempts = 0, submitted = 0, errors = 0;
    int first_rc = 0, first_off = -1, oom_off = -1;
    for (uint16_t off = 0; off < len; off += chunk) {
        nb_notify_session_t current;
        nb_get_notify_session(id, &current);
        if (!current.connected || !current.notify_enabled ||
            current.epoch != it.epoch || current.conn_handle != it.conn_handle)
            return false;
        /* No state lock across NimBLE. This check authorizes this submission;
         * an already-authorized/submitted notification cannot be recalled. */
        uint16_t n = (len - off < chunk) ? (len - off) : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + off, n); /* NIMBLE-PASS */
        if (!om) { oom_off = off; break; }
        int rc = ble_gatts_notify_custom(it.conn_handle, s_val_handle, om);
        attempts++;
        if (!rc) submitted += n;
        else {
            if (!errors) { first_rc = rc; first_off = off; }
            errors++;
        }
    }
    static int64_t next_error_log_us;
    bool report = replay || devinfo;
    if ((errors || oom_off >= 0) && !report) {
        int64_t now = esp_timer_get_time();
        if (now >= next_error_log_us) { report = true; next_error_log_us = now + 1000000; }
    }
    if (devinfo) diag_dev_metadata(id, replay, data, len);
    if (report)
        ESP_LOGI(TAG, "diag id=%u src=%s h=%u mtu=%u len=%u calls=%u submitted=%u errors=%u rc=%d off=%d oom=%d",
                 id, replay ? "replay" : "live", it.conn_handle, mtu, len,
                 attempts, submitted, errors, first_rc, first_off, oom_off);
    return submitted == len && !errors && oom_off < 0;
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
/* Adopt a new app connection for identity `id`. Reached from CONNECT and,
 * as a belt, from ADV_COMPLETE(reason=connection) — a connection was
 * observed arriving with NO CONNECT event delivered here (2026-08-30: ATT
 * served, nothing logged, the adv set stranded dark until reboot). */
static void adopt_conn(uint8_t id, uint16_t h, const char *via)
{
    if (nb_identity_for_conn(h) >= 0) return;         /* already adopted */
    /* One app connection at a time: accept-then-terminate a second
     * (BLE has no reject primitive) — spec §5. */
    if (nb_active_conn_count() >= CFG_MAX_APP_CONNS) {
        ESP_LOGW(TAG, "2nd connection on id %u — terminating", id);
        ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    nb_set_conn(id, true, h);
    adv_mgr_on_connect(id);
    /* GAP device name = this identity (the real module's 2a00 returns the
     * unit's own name; apps cross-check it against the advertised name).
     * Safe as a global because only one app connection is allowed. Narrow
     * accessor — a whole nb_identity_t here is ~3.3 KB of nimble_host
     * stack (the 2026-08-30 overflow class). */
    char name[32];
    if (nb_get_name(id, name, sizeof(name)))
        ble_svc_gap_device_name_set(name);
    tunnel_cli_send_client(id, true);           /* drives A's connect-on-demand */
    ESP_LOGI(TAG, "app connected -> identity %u (%s)", id, via);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect event status=%d", event->connect.status);
            return 0;
        }
        uint16_t h = event->connect.conn_handle;
        int id = identity_for_conn(h);
        if (id < 0) {   /* LOUD — this used to be a silent terminate */
            ESP_LOGW(TAG, "connect %u: no identity — terminating", h);
            ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        adopt_conn((uint8_t)id, h, "connect");
        return 0;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE: {
        /* The controller stopped this instance (a connection arrived, or the
         * procedure ended). Ignoring this event let a set think it was still
         * advertising after a mishandled connect — dark TUN until reboot. */
        uint8_t inst = event->adv_complete.instance;
        int reason   = event->adv_complete.reason;
        adv_mgr_on_adv_stopped(inst, /*restart=*/reason != 0);
        if (reason == 0 && inst < CFG_NUM_UNITS) {
            /* Stopped by an incoming connection: adopt it here too, in case
             * the CONNECT event never arrives. instance == identity. */
            uint16_t h = event->adv_complete.conn_handle;
            if (ble_gap_conn_find(h, NULL) == 0)
                adopt_conn(inst, h, "adv_complete");
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t h = event->disconnect.conn.conn_handle;
        int id = nb_identity_for_conn(h);
        if (id >= 0) {
            nb_set_conn(id, false, 0);
            adv_mgr_on_disconnect(id);
            tunnel_cli_send_client(id, false);
            ESP_LOGI(TAG, "app disconnected from identity %u h=%u reason=0x%X",
                     id, h, event->disconnect.reason);
        }
        return 0;
    }
    case BLE_GAP_EVENT_SUBSCRIBE: {
        int id = nb_identity_for_conn(event->subscribe.conn_handle);
        if (id >= 0) {
            nb_set_notify(id, event->subscribe.cur_notify);           /* CCCD */
            ESP_LOGI(TAG, "id %d cccd=%d", id, event->subscribe.cur_notify);
        }
        /* NOTE: do NOT send notifications from inside this handler — replaying
         * warm frames here (2026-08-30 attempt) wedged the live stream to the
         * app (bank 1 connected but showed no data). Warm replay stays on the
         * FFE2 write path (chr2_access) only. */
        return 0;
    }
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "diag mtu h=%u value=%u", event->mtu.conn_handle, event->mtu.value);
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
