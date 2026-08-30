/*
 * app_probe — fake JK app for byte-level clone-session inspection.
 *
 * Runs on the retired C3 board over USB. Console keys (no newline needed):
 *   0..3  run one session against that bank's Node B clone (TUN x)
 *   o     toggle opener order: subscribe-first <-> write-first (app-like)
 *   q     abort the running session early
 *
 * A session mimics the app: scan for the clone by name, connect, exchange
 * MTU, then (depending on order) enable FFE1 notifications and write the
 * 0x97 device-info request to FFE2; when a device-info frame (55AAEB90 03)
 * is seen, follow with 0x96 like the app does. Every notify is printed as
 *   RX +<ms> len=<n> <hex>
 * with ms relative to session start, so B-side logs can be correlated.
 * Deliberately NEVER sends 0x6C (that would write a junk RTC into a live
 * BMS through the tunnel). 97/96 are routine read polls — safe.
 */
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"

static const char *TAG = "app_probe";

#define SVC_UUID   0xFFE0
#define CHR1_UUID  0xFFE1   /* notify stream */
#define CHR2_UUID  0xFFE2   /* command sink  */
#define SESSION_MS 25000

/* ---- session state (single session at a time; host task + main loop) ---- */
static volatile int      s_target   = -1;    /* bank being probed, -1 = idle */
static volatile bool     s_abort;
static volatile bool     s_write_first;      /* opener order toggle          */
static volatile uint16_t s_conn     = 0xFFFF;
static volatile uint16_t s_ffe1     = 0;     /* FFE1 value handle            */
static volatile uint16_t s_ffe2     = 0;     /* FFE2 value handle            */
static volatile bool     s_disc_done, s_found, s_connected, s_mtu_done;
static volatile bool     s_devinfo_seen, s_sent96;
static volatile int      s_rx_count;
static int64_t           s_t0;               /* session start, us            */
static ble_addr_t        s_peer;

static uint32_t ms_now(void) { return (uint32_t)((esp_timer_get_time() - s_t0) / 1000); }

/* AA 55 90 EB <op> <15 zero> <sum of first 19> — the JK command frame. */
static void jk_cmd(uint8_t *f, uint8_t op)
{
    memset(f, 0, 20);
    f[0]=0xAA; f[1]=0x55; f[2]=0x90; f[3]=0xEB; f[4]=op;
    uint8_t s = 0; for (int i = 0; i < 19; i++) s += f[i];
    f[19] = s;
}

/* ---- GATT callbacks ----------------------------------------------------- */
static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        uint16_t u = ble_uuid_u16(&chr->uuid.u);
        if (u == CHR1_UUID) s_ffe1 = chr->val_handle;
        if (u == CHR2_UUID) s_ffe2 = chr->val_handle;
        printf("CHR +%lu 0x%04X val_handle=%u\n",
               (unsigned long)ms_now(), u, chr->val_handle);
    }
    if (err->status != 0) s_disc_done = true;   /* BLE_HS_EDONE ends the list */
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc) {
        printf("SVC +%lu FFE0 handles %u..%u\n",
               (unsigned long)ms_now(), svc->start_handle, svc->end_handle);
        ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle,
                                on_chr, NULL);
    } else if (err->status != 0 && !s_ffe1 && !s_ffe2) {
        printf("SVC +%lu discovery ended without FFE0\n", (unsigned long)ms_now());
        s_disc_done = true;
    }
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t mtu, void *arg)
{
    printf("MTU +%lu = %u (st=%d)\n", (unsigned long)ms_now(), mtu, err->status);
    s_mtu_done = true;
    return 0;
}

/* ---- GAP ---------------------------------------------------------------- */
static int gap_ev(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        if (s_target == -1) return 0;
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data))
            return 0;
        if (!f.name || f.name_len < 5) return 0;
        const uint8_t *a = ev->disc.addr.val;
        /* Survey mode ('s'): print every named advert, connect to nothing. */
        if (s_target == -2) {
            printf("ADV %02X:%02X:%02X:%02X:%02X:%02X t%d rssi=%d '%.*s'\n",
                   a[5], a[4], a[3], a[2], a[1], a[0], ev->disc.addr.type,
                   ev->disc.rssi, f.name_len, f.name);
            return 0;
        }
        if (s_found) return 0;
        /* Clone names: TUN_0-00 / TUN 1-01 / ... — bank digit at offset 4. */
        if (memcmp(f.name, "TUN", 3) != 0 || f.name[4] != '0' + s_target)
            return 0;
        s_peer = ev->disc.addr; s_found = true;
        printf("ADV +%lu matched '%.*s' %02X:%02X:%02X:%02X:%02X:%02X t%d rssi=%d\n",
               (unsigned long)ms_now(), f.name_len, f.name,
               a[5], a[4], a[3], a[2], a[1], a[0], ev->disc.addr.type,
               ev->disc.rssi);
        ble_gap_disc_cancel();
        int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &s_peer, 5000, NULL,
                                 gap_ev, NULL);
        printf("CONNECTING +%lu rc=%d\n", (unsigned long)ms_now(), rc);
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        printf("CONNECT +%lu st=%d handle=%u\n", (unsigned long)ms_now(),
               ev->connect.status, ev->connect.conn_handle);
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_connected = true;
            ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        printf("DISCONNECT +%lu reason=0x%02x\n", (unsigned long)ms_now(),
               ev->disconnect.reason);
        s_connected = false; s_conn = 0xFFFF;
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t buf[260];
        uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(ev->notify_rx.om, 0, len, buf);
        s_rx_count++;
        printf("RX +%lu h=%u len=%u ", (unsigned long)ms_now(),
               ev->notify_rx.attr_handle, len);
        for (int i = 0; i < len; i++) printf("%02X", buf[i]);
        if (len >= 5 && buf[0]==0x55 && buf[1]==0xAA && buf[2]==0xEB && buf[3]==0x90) {
            printf("  <-- FRAME rec=0x%02X", buf[4]);
            if (buf[4] == 0x03) s_devinfo_seen = true;
        }
        printf("\n");
        return 0;
    }
    default: return 0;
    }
}

/* ---- session driver (runs on the app_main task) ------------------------- */
/* Sessions run on the console task itself, so 'q' must be polled here. */
static void poll_abort(void)
{
    int c = getchar();
    if (c == 'q') s_abort = true;
}

static bool waitf(volatile bool *flag, uint32_t to_ms)
{
    uint32_t start = ms_now();
    while (!*flag && !s_abort && ms_now() - start < to_ms)
        { poll_abort(); vTaskDelay(pdMS_TO_TICKS(10)); }
    return *flag;
}

static void write_cccd(void)
{
    /* B's table: FFE2 decl+val precede FFE1 decl+val, CCCD = FFE1 val+1.
     * (Descriptor discovery skipped: this probe only ever talks to Node B,
     * whose table is fixed — see node_b/main/ble_periph.c s_svcs.) */
    uint8_t v[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(s_conn, s_ffe1 + 1, v, sizeof(v), NULL, NULL);
    printf("SUBSCRIBE +%lu cccd=%u rc=%d\n", (unsigned long)ms_now(), s_ffe1 + 1, rc);
}

static void write_cmd(uint8_t op)
{
    /* FFE1, like the real app (proved live 2026-08-30: the phone's opener
     * arrives on FFE1, not FFE2 — writing FFE2 here masked a Node B bug). */
    uint8_t f[20]; jk_cmd(f, op);
    int rc = ble_gattc_write_no_rsp_flat(s_conn, s_ffe1, f, sizeof(f));
    printf("CMD +%lu op=0x%02X rc=%d\n", (unsigned long)ms_now(), op, rc);
}

static void run_session(int bank)
{
    s_abort = false; s_found = false; s_connected = false; s_mtu_done = false;
    s_disc_done = false; s_devinfo_seen = false; s_sent96 = false;
    s_ffe1 = s_ffe2 = 0; s_rx_count = 0; s_conn = 0xFFFF;
    s_target = bank; s_t0 = esp_timer_get_time();

    printf("\n=== SESSION bank %d order=%s ===\n", bank,
           s_write_first ? "WRITE-FIRST (app-like)" : "SUBSCRIBE-FIRST");

    ble_att_set_preferred_mtu(185);              /* mimic iOS */
    struct ble_gap_disc_params p = { .passive = 0, .itvl = 0x50, .window = 0x30 };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 10000, &p, gap_ev, NULL);
    if (rc) { printf("scan rc=%d — session aborted\n", rc); s_target = -1; return; }

    if (!waitf(&s_connected, 12000)) { printf("NO CONNECT — end\n"); goto out; }
    waitf(&s_mtu_done, 2000);

    ble_gattc_disc_svc_by_uuid(s_conn, BLE_UUID16_DECLARE(SVC_UUID), on_svc, NULL);
    { uint32_t st = ms_now();
      while ((!s_ffe1 || !s_ffe2) && !s_abort && ms_now() - st < 4000)
          vTaskDelay(pdMS_TO_TICKS(10)); }
    if (!s_ffe1 || !s_ffe2) { printf("DISCOVERY FAILED ffe1=%u ffe2=%u\n",
                                     s_ffe1, s_ffe2); goto out; }

    if (s_write_first) { write_cmd(0x97); vTaskDelay(pdMS_TO_TICKS(200)); write_cccd(); }
    else               { write_cccd(); vTaskDelay(pdMS_TO_TICKS(150)); write_cmd(0x97); }

    while (!s_abort && s_connected && ms_now() < SESSION_MS) {
        if (s_devinfo_seen && !s_sent96) {
            s_sent96 = true;
            vTaskDelay(pdMS_TO_TICKS(250));
            write_cmd(0x96);                      /* the app's follow-up */
        }
        poll_abort();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
out:
    /* Terminate and CONFIRM: a fire-and-forget terminate once left the clone
     * occupied forever — B stopped advertising TUN 1 until it was rebooted
     * (2026-08-30). Retry once, then say so loudly. */
    for (int t = 0; t < 2 && s_conn != 0xFFFF; t++) {
        int rc = ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0 && rc != BLE_HS_EALREADY)
            printf("TERMINATE rc=%d\n", rc);
        uint32_t st = ms_now();
        while (s_connected && ms_now() - st < 3000) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_connected) printf("WARNING: connection did not close — clone occupied!\n");
    ble_gap_disc_cancel();                        /* in case scan still runs */
    printf("=== END bank %d: rx=%d devinfo=%s dur=%lums ===\n\n", bank,
           s_rx_count, s_devinfo_seen ? "YES" : "NO", (unsigned long)ms_now());
    s_target = -1;
}

/* ---- boilerplate -------------------------------------------------------- */
static void host_task(void *arg) { nimble_port_run(); nimble_port_freertos_deinit(); }
static void on_sync(void) { printf("READY — keys: 0-3 session, o order, q abort\n"); }

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND)
        { nvs_flash_erase(); nvs_flash_init(); }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    /* Console loop: default non-blocking USB-SJ stdin, polled. */
    for (;;) {
        int c = getchar();
        if (c == 'o') {
            s_write_first = !s_write_first;
            printf("order = %s\n", s_write_first ? "WRITE-FIRST" : "SUBSCRIBE-FIRST");
        } else if (c == 's') {
            /* 10 s survey: every named advert with address, no connecting. */
            if (s_target == -1) {
                s_target = -2;
                struct ble_gap_disc_params sp =
                    { .passive = 0, .itvl = 0x50, .window = 0x30 };
                printf("--- survey start ---\n");
                ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 10000, &sp, gap_ev, NULL);
                vTaskDelay(pdMS_TO_TICKS(10500));
                s_target = -1;
                printf("--- survey end ---\n");
            }
        } else if (c == 'q') {
            s_abort = true;
        } else if (c >= '0' && c <= '3') {
            if (s_target < 0) run_session(c - '0');
            else printf("busy (bank %d) — q to abort\n", (int)s_target);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
