/*
 * app_main.c — bench test board. No WiFi: the board is USB-tethered and driven
 * over the serial control channel (ctl_server.h), and it talks to the real
 * nodes over Bluetooth. Waits for a `role` command to init BLE as either the
 * app or a BMS emulator. One role per boot; reboot to switch.
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "ctl_server.h"

static const char *TAG = "test_board";

void app_main(void)
{
    ESP_LOGI(TAG, "JK BLE tunnel — bench test board (serial control, no WiFi)");
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND)
        { nvs_flash_erase(); nvs_flash_init(); }

    ctl_server_start();   /* role selected over serial; BLE inits on demand */
    ESP_LOGI(TAG, "ready — type 'role app' or 'role bms <name>' on this serial port");
}
