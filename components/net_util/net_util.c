#include <string.h>
#include "net_util.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "net_util";
static EventGroupHandle_t s_evt;
static esp_netif_t *s_netif;
static char s_hostname[33];
#define GOT_IP (1 << 0)

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* The scan + hostname + first connect are driven by netstart_task so a
         * blocking scan cannot deadlock this event-loop task. */
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected (reason=%d), retrying", e ? e->reason : -1);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_evt, GOT_IP);
    }
}

/* One-shot diagnostic: scan all channels and dump every AP with the details
 * that explain an auth-stage failure (authmode/cipher/11ax/BSSID count). */
static void dump_scan(void)
{
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) { ESP_LOGW(TAG, "scan failed"); return; }
    uint16_t n = 0; esp_wifi_scan_get_ap_num(&n);
    static wifi_ap_record_t recs[24];   /* ~2 KB — keep off the task stack */
    uint16_t got = (n < 24) ? n : 24;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) return;
    ESP_LOGW(TAG, "scan: %u AP(s) visible (auth: 3=WPA2 4=WPA/WPA2 7=WPA2/WPA3; cipher: 3=TKIP 4=CCMP)", n);
    for (int i = 0; i < got; i++) {
        wifi_ap_record_t *r = &recs[i];
        ESP_LOGW(TAG, "  '%s' %02x:%02x:%02x:%02x:%02x:%02x ch%d rssi=%d auth=%d pair=%d grp=%d b=%d g=%d n=%d ax=%d",
                 (char *)r->ssid, r->bssid[0], r->bssid[1], r->bssid[2],
                 r->bssid[3], r->bssid[4], r->bssid[5], r->primary, r->rssi,
                 r->authmode, r->pairwise_cipher, r->group_cipher,
                 r->phy_11b, r->phy_11g, r->phy_11n, r->phy_11ax);
    }
}

static void netstart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(300));          /* let STA_START settle */
    dump_scan();                              /* evidence, then connect */
    if (s_hostname[0] && s_netif) esp_netif_set_hostname(s_netif, s_hostname);
    esp_wifi_connect();
    vTaskDelete(NULL);
}

void net_wifi_start(const char *ssid, const char *pass, const char *hostname)
{
    s_evt = xEventGroupCreate();
    if (hostname) strlcpy(s_hostname, hostname, sizeof(s_hostname));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.pmf_cfg.capable = true;   /* IDF default; PMF-optional WPA2 APs need it */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(netstart_task, "netstart", 6144, NULL, 5, NULL);
}

bool net_wifi_wait(int timeout_ms)
{
    return xEventGroupWaitBits(s_evt, GOT_IP, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(timeout_ms)) & GOT_IP;
}

void net_wifi_set_txpower(int8_t max_qdbm)
{
    esp_wifi_set_max_tx_power(max_qdbm);
    int8_t actual = 0; esp_wifi_get_max_tx_power(&actual);
    ESP_LOGW(TAG, "WiFi max TX power capped at %d (%.2f dBm)", actual, actual * 0.25);
}

void net_sntp_start(const char *server)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server);
    esp_sntp_init();
}
