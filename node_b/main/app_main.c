/*
 * app_main.c — Node B bring-up (spec §5). NimBLE peripheral (BLE 5.0 ext adv).
 * GATT table is registered before the host starts; advertising sets are
 * configured lazily by adv_mgr once the tunnel/link states arrive.
 */
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "config.h"
#include "net_util.h"
#include "nb_state.h"
#include "adv_mgr.h"
#include "ble_periph.h"
#include "tunnel_cli.h"
#include "supervisor.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

static const char *TAG = "node_b";

static void on_sync(void)
{
    /* Host up: adv sets are configured on demand by adv_mgr when a unit becomes
     * reachable and the tunnel is up. Nothing to advertise yet at boot. */
    ESP_LOGI(TAG, "NimBLE sync, peripheral ready");
}

static void host_task(void *arg) { nimble_port_run(); nimble_port_freertos_deinit(); }

void app_main(void)
{
    ESP_LOGI(TAG, "JK BLE tunnel — Node B (house/peripheral) starting");

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND)
        { nvs_flash_erase(); nvs_flash_init(); }

    net_wifi_start(WIFI_SSID, WIFI_PASS, "jk-node-b");   /* DHCP hostname (spec §12) */
    net_wifi_set_txpower(CFG_WIFI_MAX_TX_QDBM);          /* marginal-supply guard */
    net_wifi_wait(20000);

    nb_state_init();
    adv_mgr_init();               /* generate per-set static-random addresses  */

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_periph_start();           /* register the single shared GATT table     */
    nimble_port_freertos_init(host_task);

    esp_task_wdt_config_t wdt = { .timeout_ms = 15000, .idle_core_mask = 0, .trigger_panic = true };
    esp_task_wdt_init(&wdt);

    tunnel_cli_start();           /* connect to Node A, resync, grace window    */
    supervisor_start();

    ESP_LOGI(TAG, "Node B up");
}
