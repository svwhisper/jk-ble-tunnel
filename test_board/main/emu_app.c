/*
 * emu_app.c — BLE central that impersonates the JK iOS app, so Node B can be
 * driven without a phone. Scans for advertised names, connects by name,
 * discovers 0xFFE0/0xFFE1, subscribes, reads (served from Node B's cache),
 * writes (relayed to Node A), and reports notifications. NIMBLE-PASS markers.
 */
#include <string.h>
#include <stdio.h>
#include "roles.h"
#include "ctl_server.h"
#include "jk_ble_defs.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"

static const char *TAG = "emu_app";
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_val_handle, s_cccd_handle;
static char     s_target[32];
static ble_addr_t s_target_addr;
static bool     s_have_target;

static int gap_event(struct ble_gap_event *e, void *arg);

/* ---- scan --------------------------------------------------------------- */
void emu_app_scan(void)
{
    struct ble_gap_disc_params dp = { .passive = 0, .itvl = 0, .window = 0 };
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 5000, &dp, gap_event, NULL);   /* NIMBLE-PASS */
}

/* Parse a complete-name AD field; emit and remember matches. */
static void on_adv(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return; /* NIMBLE-PASS */
    if (f.name_len) {
        char name[32] = {0}; int n = f.name_len < 31 ? f.name_len : 31;
        memcpy(name, f.name, n);
        ctl_emit("EVT adv %s", name);
        if (s_have_target && !strcmp(name, s_target)) {
            s_target_addr = d->addr;
            ble_gap_disc_cancel();
            ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &s_target_addr, 5000, NULL,
                            gap_event, NULL);                         /* NIMBLE-PASS */
        }
    }
}

bool emu_app_connect(const char *name)
{
    if (!name) return false;
    strlcpy(s_target, name, sizeof(s_target));
    s_have_target = true;
    emu_app_scan();     /* scan, then connect on match in on_adv */
    return true;
}

/* ---- discovery ---------------------------------------------------------- */
static int on_chr(uint16_t c, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (chr && ble_uuid_u16(&chr->uuid.u) == JK_CHR_UUID) {
        s_val_handle = chr->val_handle; s_cccd_handle = chr->val_handle + 1;
        ctl_emit("EVT chr 0x%04x val_handle=%u", JK_CHR_UUID, s_val_handle);
    }
    return 0;
}
static int on_svc(uint16_t c, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    if (svc && ble_uuid_u16(&svc->uuid.u) == JK_SVC_UUID)
        ble_gattc_disc_all_chrs(c, svc->start_handle, svc->end_handle, on_chr, NULL); /* NIMBLE-PASS */
    return 0;
}

/* ---- ops ---------------------------------------------------------------- */
void emu_app_subscribe(void)
{
    uint8_t v[2] = {0x01, 0x00};
    ble_gattc_write_flat(s_conn, s_cccd_handle, v, sizeof(v), NULL, NULL);  /* NIMBLE-PASS */
}
static int on_read(uint16_t c, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *attr, void *arg)
{
    if (attr && attr->om) {
        uint8_t b[320]; uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (n > sizeof(b)) n = sizeof(b);
        ble_hs_mbuf_to_flat(attr->om, b, n, NULL);
        char hex[641]; for (int i=0;i<n && i<320;i++) sprintf(hex+i*2, "%02x", b[i]);
        ctl_emit("EVT read %s", hex);
    }
    return 0;
}
void emu_app_read(void)
{ ble_gattc_read(s_conn, s_val_handle, on_read, NULL); }                    /* NIMBLE-PASS */
void emu_app_write(const uint8_t *d, int n)
{ ble_gattc_write_flat(s_conn, s_val_handle, d, n, NULL, NULL); }           /* NIMBLE-PASS */
void emu_app_disconnect(void)
{ if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM); }

/* ---- events ------------------------------------------------------------- */
static int gap_event(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_DISC:      on_adv(&e->disc); return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            s_conn = e->connect.conn_handle; ctl_emit("EVT connected");
            ble_uuid16_t u = BLE_UUID16_INIT(JK_SVC_UUID);
            ble_gattc_disc_svc_by_uuid(s_conn, &u.u, on_svc, NULL);         /* NIMBLE-PASS */
        } else ctl_emit("EVT connect_failed %d", e->connect.status);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE; ctl_emit("EVT disconnected"); return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t b[320]; uint16_t n = OS_MBUF_PKTLEN(e->notify_rx.om);
        if (n > sizeof(b)) n = sizeof(b);
        ble_hs_mbuf_to_flat(e->notify_rx.om, b, n, NULL);
        char hex[641]; for (int i=0;i<n && i<320;i++) sprintf(hex+i*2, "%02x", b[i]);
        ctl_emit("EVT notify %s", hex);
        return 0;
    }
    default: return 0;
    }
}

static void on_sync(void) { ESP_LOGI(TAG, "central ready"); }
static void host_task(void *arg) { nimble_port_run(); nimble_port_freertos_deinit(); }

void emu_app_start(void)
{
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}
