#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "ctl_server.h"
#include "roles.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ctl";
static volatile int s_client = -1;
static role_t s_role = ROLE_NONE;

void ctl_emit(const char *fmt, ...)
{
    if (s_client < 0) return;
    char line[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    line[n++] = '\n';
    send(s_client, line, n, 0);
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
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                             .sin_port = htons(CTL_PORT) };
    bind(ls, (struct sockaddr *)&a, sizeof(a));
    listen(ls, 1);
    ESP_LOGI(TAG, "control server on :%d", CTL_PORT);

    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        s_client = c;
        ctl_emit("OK bench ready — 'role app' or 'role bms <name>'");
        char buf[600]; int fill = 0;
        for (;;) {
            int r = recv(c, buf + fill, sizeof(buf) - 1 - fill, 0);
            if (r <= 0) break;
            fill += r; buf[fill] = 0;
            char *nl;
            while ((nl = strchr(buf, '\n'))) {
                *nl = 0;
                char line[600]; strlcpy(line, buf, sizeof(line));
                handle_line(line);
                memmove(buf, nl + 1, fill - (nl + 1 - buf) + 1);
                fill -= (nl + 1 - buf);
            }
        }
        close(c); s_client = -1;
    }
}

void ctl_server_start(void)
{
    xTaskCreatePinnedToCore(ctl_task, "ctl", 6144, NULL, 5, NULL, 0);
}
