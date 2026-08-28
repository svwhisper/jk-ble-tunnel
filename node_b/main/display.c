#include <stdio.h>
#include <string.h>
#include "display.h"
#include "oled.h"
#include "net_util.h"
#include "tunnel_cli.h"    /* tunnel_cli_up */
#include "nb_state.h"      /* nb_active_conn_count */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void ip_last2(char *out, int n)
{
    char ip[24]; net_wifi_ip_str(ip, sizeof(ip));
    int dots = 0; char *p = ip;
    for (; *p; p++) if (*p == '.' && ++dots == 2) { p++; break; }
    snprintf(out, n, "%s", dots >= 2 ? p : ip);
}

static void disp_task(void *arg)
{
    char ip[16], l4[16];
    for (;;) {
        bool wifi = net_wifi_up();
        bool tun  = tunnel_cli_up();
        ip_last2(ip, sizeof(ip));
        snprintf(l4, sizeof(l4), "APP %d", nb_active_conn_count());

        oled_line(0, "NODE B");
        oled_line(1, wifi ? "WIFI UP" : "WIFI --");
        oled_line(2, wifi ? ip : "-");
        oled_line(3, tun  ? "TUN UP" : "TUN --");
        oled_line(4, l4);
        oled_show();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void display_start(void)
{
    if (!oled_init()) return;   /* panel absent — carry on headless */
    xTaskCreate(disp_task, "display", 3072, NULL, 2, NULL);
}
