#include <string.h>
#include "net_util.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "net_util";
static EventGroupHandle_t s_evt;
static esp_netif_t *s_netif;
static char s_hostname[33];
#define GOT_IP (1 << 0)

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* Set the DHCP hostname now the netif is started, before connecting, so
         * it goes out in the DHCP request and the server can register it. */
        if (s_hostname[0] && s_netif) esp_netif_set_hostname(s_netif, s_hostname);
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_evt, GOT_IP);
    }
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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool net_wifi_wait(int timeout_ms)
{
    return xEventGroupWaitBits(s_evt, GOT_IP, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(timeout_ms)) & GOT_IP;
}

void net_sntp_start(const char *server)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server);
    esp_sntp_init();
}
