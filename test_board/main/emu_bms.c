/*
 * emu_bms.c — BLE peripheral that impersonates a JK unit, so Node A has a
 * target on the bench. Serves 0xFFE0/0xFFE1; a write to 0xFFE1 (a JK read
 * command) triggers a synthetic response stream, and push/autopush emit
 * cell-info notifications on demand. NIMBLE-PASS markers as elsewhere.
 */
#include <string.h>
#include "roles.h"
#include "ctl_server.h"
#include "synth_frames.h"
#include "jk_ble_defs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "emu_bms";
static char     s_name[32] = "JK-BENCH";
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_val_handle;
static bool     s_notify;
static uint32_t s_seq;
static TimerHandle_t s_autopush;
static QueueHandle_t s_push_q;   /* codes: 0=cell 1=settings 2=devinfo */

/* Emit a frame as chunked notifications. Runs ONLY on push_task — never
 * re-entrantly from a GATT access callback or the FreeRTOS timer task, both of
 * which overflow/corrupt and panic (StoreProhibited). */
static void notify_frame(const uint8_t *frame, int len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_notify) return;
    for (int off = 0; off < len; off += 20) {
        int n = (len - off < 20) ? (len - off) : 20;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(frame + off, n);
        if (om) ble_gatts_notify_custom(s_conn, s_val_handle, om);
    }
    ctl_emit("EVT notified %d bytes", len);
}

static void push_task(void *arg)
{
    uint8_t what;
    for (;;) {
        if (xQueueReceive(s_push_q, &what, portMAX_DELAY) != pdTRUE) continue;
        uint8_t f[320]; int n = -1;
        if (what == 0)      n = synth_cell_info(f, sizeof(f), s_seq++);
        else if (what == 1) n = synth_settings(f, sizeof(f));
        else if (what == 2) n = synth_device_info(f, sizeof(f), s_name);
        if (n > 0) notify_frame(f, n);
    }
}

/* Deferred: just enqueue a request; push_task does the actual notify. Safe to
 * call from a GATT callback or a timer. */
void emu_bms_push(const char *what)
{
    uint8_t code = !strcmp(what, "settings") ? 1 : !strcmp(what, "devinfo") ? 2 : 0;
    if (s_push_q) xQueueSend(s_push_q, &code, 0);
}

static void autopush_cb(TimerHandle_t t) { emu_bms_push("cell"); }
void emu_bms_autopush(int ms)
{
    if (s_autopush) { xTimerStop(s_autopush, 0); }
    if (ms <= 0) return;
    if (!s_autopush) s_autopush = xTimerCreate("autopush", pdMS_TO_TICKS(ms), pdTRUE, NULL, autopush_cb);
    xTimerChangePeriod(s_autopush, pdMS_TO_TICKS(ms), 0);
    xTimerStart(s_autopush, 0);
}

static int chr_access(uint16_t c, uint16_t a, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t f[320]; int n = synth_cell_info(f, sizeof(f), s_seq);
        return os_mbuf_append(ctxt->om, f, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t b[64]; uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
        if (n > sizeof(b)) n = sizeof(b);
        ble_hs_mbuf_to_flat(ctxt->om, b, n, NULL);
        ctl_emit("EVT bms_write %u bytes (opcode 0x%02x)", n, n >= 5 ? b[4] : 0);
        /* A JK read command -> stream a synthetic cell-info response. */
        emu_bms_push("cell");
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = BLE_UUID16_DECLARE(JK_SVC_UUID),
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = BLE_UUID16_DECLARE(JK_CHR_UUID), .access_cb = chr_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                     BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &s_val_handle },
          { 0 } } },
    { 0 }
};

static int gap_event(struct ble_gap_event *e, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_name; f.name_len = strlen(s_name); f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);                                   /* NIMBLE-PASS */
    struct ble_gap_adv_params p = { .conn_mode = BLE_GAP_CONN_MODE_UND,
                                    .disc_mode = BLE_GAP_DISC_MODE_GEN };
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gap_event, NULL); /* NIMBLE-PASS */
    ESP_LOGI(TAG, "advertising '%s'", s_name);
}

static int gap_event(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) { s_conn = e->connect.conn_handle;
            ctl_emit("EVT connected"); }
        else advertise();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE; s_notify = false;
        ctl_emit("EVT disconnected"); advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        s_notify = e->subscribe.cur_notify;
        ctl_emit("EVT subscribe notify=%d", s_notify);
        return 0;
    default: return 0;
    }
}

static void on_sync(void) { advertise(); }
static void host_task(void *arg) { nimble_port_run(); nimble_port_freertos_deinit(); }

void emu_bms_start(const char *adv_name)
{
    if (adv_name) strlcpy(s_name, adv_name, sizeof(s_name));
    s_push_q = xQueueCreate(8, sizeof(uint8_t));
    xTaskCreate(push_task, "bms_push", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gap_device_name_set(s_name);
    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);
    nimble_port_freertos_init(host_task);
}
