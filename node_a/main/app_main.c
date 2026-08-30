/*
 * app_main.c — Node A bring-up (spec §4). Order matters:
 *   NVS -> restore any interrupted measurement -> WiFi/SNTP -> queues ->
 *   state cache -> tasks. BLE and the tunnel come up last so the arbiter and
 *   caches are ready before any consumer can drive them.
 */
#include "esp_log.h"
#include "log_udp.h"
#include "esp_task_wdt.h"
#include "nvs_store.h"
#include "queues.h"
#include "state_cache.h"
#include "config.h"
#include "net_util.h"

#include "arbiter.h"
#include "ble_owner.h"
#include "decoder.h"
#include "tunnel_srv.h"
#include "mqtt_task.h"
#include "measure.h"
#include "supervisor.h"
#include "display.h"
#include "ota.h"

static const char *TAG = "node_a";

void app_main(void)
{
    ESP_LOGI(TAG, "JK BLE tunnel — Node A (garage/central) starting");

    nvs_store_init();
    measure_init();
    measure_check_boot();          /* restore a crash-interrupted measurement (§9) */

    net_wifi_start(WIFI_SSID, WIFI_PASS, "jk-node-a");   /* DHCP hostname (spec §12) */
    net_wifi_set_txpower(CFG_WIFI_MAX_TX_QDBM);          /* marginal-supply guard */
    net_wifi_wait(20000);
    net_sntp_start(CFG_NTP_SERVER);
    log_udp_start();               /* mirror logs to UDP :3766 (no console here) */
    nightly_reboot_start(CFG_TZ, 1, 0, NULL);  /* 01:00, unconditional */               /* mirror logs to UDP :3766 (no console here) */

    /* Push-OTA receiver: started HERE, before NimBLE + the task fleet, while
     * internal RAM is plentiful. Started at the tail it lost the resource
     * race on some builds: httpd_start bound :CFG_OTA_PORT, failed its later
     * task-create, LEAKED the bound listener (phantom socket that accepts
     * and never serves), and every retry then died EADDRINUSE — the node was
     * un-updatable over the air for the whole boot (2026-08-29). Binds
     * 0.0.0.0, so late WiFi still gets served. The supervisor retries it
     * every 30 s should it ever still fail. */
    ota_start(CFG_OTA_PORT);

    queues_init();
    state_cache_init();

    /* Task watchdog: long-running tasks subscribe (spec §11). */
    esp_task_wdt_config_t wdt = { .timeout_ms = 15000, .idle_core_mask = 0, .trigger_panic = true };
    esp_task_wdt_init(&wdt);

    arbiter_start();               /* serialise + policy                     */
    decoder_start();               /* frames -> state cache -> MQTT          */
    ble_owner_start();             /* NimBLE central (the only BLE task)     */
    mqtt_start();                  /* automation (MQTT) path + LWT           */
    tunnel_srv_start();            /* Node B tunnel                          */
    supervisor_start();            /* harvest, probes, idle-disc, meas guard */
    display_start();               /* onboard OLED: role + status            */

    /* Confirm the running image only once WiFi is actually up; a bad build
     * that can't join WiFi is left unconfirmed and self-reverts on the next
     * reset (§addendum). */
    if (net_wifi_up()) ota_mark_valid();
    else ESP_LOGW(TAG, "no WiFi at bringup — image left unconfirmed (rollback armed)");

    ESP_LOGI(TAG, "Node A up");
}
