#include <stdio.h>
#include <string.h>
#include "display.h"
#include "oled.h"
#include "net_util.h"
#include "queues.h"        /* g_evt, EVT_MQTT_UP */
#include "na_types.h"
#include "tunnel_srv.h"    /* tunnel_is_up */
#include "state_cache.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* "192.168.3.241" -> "3.241" (subnet.host), to fit 72 px. */
static void ip_last2(char *out, int n)
{
    char ip[24]; net_wifi_ip_str(ip, sizeof(ip));
    int dots = 0; char *p = ip;
    for (; *p; p++) if (*p == '.' && ++dots == 2) { p++; break; }
    snprintf(out, n, "%s", dots >= 2 ? p : ip);
}

static int bms_up_count(void)
{
    int n = 0;
    for (uint8_t i = 0; i < CFG_NUM_UNITS; i++) {
        bms_runtime_t rt; state_get_runtime(i, &rt);
        if (rt.link == LINK_UP) n++;
    }
    return n;
}

static void disp_task(void *arg)
{
    char ip[16], l4[16];
    for (;;) {
        bool wifi = net_wifi_up();
        bool mqtt = xEventGroupGetBits(g_evt) & EVT_MQTT_UP;
        bool tun  = tunnel_is_up();
        ip_last2(ip, sizeof(ip));
        snprintf(l4, sizeof(l4), "BMS %d/%d", bms_up_count(), CFG_NUM_UNITS);

        oled_line(0, "NODE A");
        oled_line(1, wifi ? "WIFI UP" : "WIFI --");
        oled_line(2, wifi ? ip : "-");
        oled_line(3, mqtt ? "MQTT UP" : "MQTT --");
        oled_line(4, tun  ? "TUN UP"  : l4);   /* B connected? else BMS count */
        oled_show();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void display_start(void)
{
    if (!oled_init()) return;   /* panel absent — carry on headless */
    xTaskCreate(disp_task, "display", 3072, NULL, 2, NULL);
}
