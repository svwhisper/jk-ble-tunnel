/*
 * scan_probe — house-side BLE observer on the retired C3 Node A board.
 *
 * Active-scans forever and prints one line per NAMED advert (plus scan rsp):
 *   ADV <addr> rssi=<dBm> evt=<type> name='<name>'
 * Safe in the house: the garage units are behind the Faraday wall, so the
 * probe's SCAN_REQs can only ever reach Node B's clones (which don't beep).
 * No WiFi, no MQTT, no connects — read via the USB console.
 */
#include <stdio.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

static const char *TAG = "scan_probe";

static int scan_event(struct ble_gap_event *ev, void *arg)
{
    if (ev->type != BLE_GAP_EVENT_DISC) return 0;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) != 0)
        return 0;
    if (!f.name || f.name_len == 0) return 0;
    const uint8_t *a = ev->disc.addr.val;
    printf("ADV %02X:%02X:%02X:%02X:%02X:%02X rssi=%d evt=%d name='%.*s'\n",
           a[5], a[4], a[3], a[2], a[1], a[0],
           ev->disc.rssi, ev->disc.event_type, f.name_len, f.name);
    return 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = {
        .passive = 0,               /* active: pull scan responses (names)   */
        .itvl = 0x50, .window = 0x30,
        .filter_duplicates = 0,     /* every repeat — we want liveness view  */
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p,
                          scan_event, NULL);
    ESP_LOGI(TAG, "scan start rc=%d", rc);
}

static void on_sync(void) { start_scan(); }
static void host_task(void *arg) { nimble_port_run(); nimble_port_freertos_deinit(); }

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND)
        { nvs_flash_erase(); nvs_flash_init(); }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}
