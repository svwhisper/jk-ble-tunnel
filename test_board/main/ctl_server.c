/*
 * ctl_server.c — line-oriented control channel over the USB **serial** port
 * (the board is permanently USB-tethered; no WiFi needed — it talks to the
 * real nodes over Bluetooth). Commands are read from stdin (the console UART),
 * events are printed to stdout. Same grammar as before (see ctl_server.h).
 *
 * Logs (ESP_LOGx) share this UART, so a driver line prints EVT/OK/ERR prefixes
 * a reader can filter. Drive it with tools/bench.py <port>, `idf.py monitor`,
 * or any serial terminal at 115200.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "ctl_server.h"
#include "roles.h"
#include "esp_log.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ctl";
static role_t s_role = ROLE_NONE;

void ctl_emit(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* hex string -> bytes; returns count. */
static int unhex(const char *s, uint8_t *out, int cap)
{
    int n = 0;
    while (s[0] && s[1] && n < cap) {
        char b[3] = { s[0], s[1], 0 };
        out[n++] = (uint8_t)strtol(b, NULL, 16);
        s += 2;
    }
    return n;
}

static void handle_line(char *line)
{
    char *cmd = strtok(line, " \t\r\n");
    if (!cmd) return;

    if (!strcmp(cmd, "role")) {
        char *which = strtok(NULL, " \t\r\n");
        if (!which) { ctl_emit("ERR role app|bms"); return; }
        if (s_role != ROLE_NONE) { ctl_emit("ERR role already %d, reboot to change", s_role); return; }
        if (!strcmp(which, "app")) { s_role = ROLE_APP; emu_app_start(); ctl_emit("OK role app"); }
        else if (!strcmp(which, "bms")) {
            char *name = strtok(NULL, " \t\r\n");
            emu_bms_start(name ? name : "JK-BENCH");
            s_role = ROLE_BMS; ctl_emit("OK role bms %s", name ? name : "JK-BENCH");
        } else ctl_emit("ERR role app|bms");
        return;
    }
    if (!strcmp(cmd, "status")) { ctl_emit("OK role=%d", s_role); return; }

    if (s_role == ROLE_APP) {
        if      (!strcmp(cmd, "scan"))       { emu_app_scan(); ctl_emit("OK scan"); }
        else if (!strcmp(cmd, "connect"))    { char *n = strtok(NULL, " \t\r\n");
                                               ctl_emit(emu_app_connect(n) ? "OK connect" : "ERR connect"); }
        else if (!strcmp(cmd, "sub"))        { emu_app_subscribe(); ctl_emit("OK sub"); }
        else if (!strcmp(cmd, "read"))       { emu_app_read(); ctl_emit("OK read"); }
        else if (!strcmp(cmd, "write"))      { char *h = strtok(NULL, " \t\r\n");
                                               uint8_t b[64]; int n = h ? unhex(h, b, sizeof(b)) : 0;
                                               emu_app_write(b, n); ctl_emit("OK write %d", n); }
        else if (!strcmp(cmd, "disconnect")) { emu_app_disconnect(); ctl_emit("OK disconnect"); }
        else ctl_emit("ERR unknown '%s'", cmd);
    } else if (s_role == ROLE_BMS) {
        if      (!strcmp(cmd, "push"))       { char *w = strtok(NULL, " \t\r\n");
                                               emu_bms_push(w ? w : "cell"); ctl_emit("OK push"); }
        else if (!strcmp(cmd, "autopush"))   { char *m = strtok(NULL, " \t\r\n");
                                               emu_bms_autopush(m ? atoi(m) : 0); ctl_emit("OK autopush"); }
        else ctl_emit("ERR unknown '%s'", cmd);
    } else {
        ctl_emit("ERR set role first: role app | role bms <name>");
    }
}

static void ctl_task(void *arg)
{
    char line[600];
    ctl_emit("OK bench ready (serial) — 'role app' or 'role bms <name>'");
    for (;;) {
        if (fgets(line, sizeof(line), stdin) != NULL) handle_line(line);
        else vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ctl_server_start(void)
{
    /* Make stdin a blocking, line-buffered reader over the console UART. */
    setvbuf(stdin, NULL, _IONBF, 0);
    const int u = CONFIG_ESP_CONSOLE_UART_NUM;
    uart_driver_install(u, 512, 0, 0, NULL, 0);
    esp_vfs_dev_uart_use_driver(u);
    esp_vfs_dev_uart_port_set_rx_line_endings(u, ESP_LINE_ENDINGS_CR);
    esp_vfs_dev_uart_port_set_tx_line_endings(u, ESP_LINE_ENDINGS_CRLF);
    xTaskCreate(ctl_task, "ctl", 6144, NULL, 5, NULL);
    ESP_LOGI(TAG, "serial control ready on console UART%d @115200", u);
}
