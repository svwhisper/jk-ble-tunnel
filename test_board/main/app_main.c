/*
 * app_main.c — bench test board. Brings up WiFi + the TCP control server, then
 * waits for a `role` command to initialise BLE as either the app or a BMS
 * emulator (see ctl_server.h). One role per boot; reboot to switch.
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "config_secret.h"
#include "net_util.h"
#include "ctl_server.h"

static const char *TAG = "test_board";

void app_main(void)
{
    ESP_LOGI(TAG, "JK BLE tunnel — bench test board");
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND)
        { nvs_flash_erase(); nvs_flash_init(); }

    net_wifi_start(WIFI_SSID, WIFI_PASS);
    net_wifi_wait(20000);

    ctl_server_start();   /* role selected over TCP; BLE inits on demand */
    ESP_LOGI(TAG, "ready — connect to :%d and send 'role app' or 'role bms <name>'", CTL_PORT);
}
