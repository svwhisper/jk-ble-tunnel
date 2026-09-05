/*
 * ota.c — push-model OTA receiver + rollback confirmation. See ota.h.
 *
 * The POST /ota handler streams the request body directly into esp_ota_write on
 * the next (inactive) OTA slot, so no full-image RAM buffer is needed. On a
 * clean end() the image is validated (magic + SHA), boot is switched, and the
 * device reboots ~1 s later (after the HTTP response has flushed to curl).
 *
 * Any failure — short/oversized upload, bad image, flash error — aborts the OTA
 * handle and leaves the running slot untouched, so a dropped WiFi transfer can
 * never brick the node. Combined with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE and
 * ota_mark_valid() (called only once WiFi is up), a bad *build* self-reverts.
 */
#include <string.h>
#include <stdio.h>
#include "ota.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";
static httpd_handle_t s_srv;

/* Deferred reboot so the caller's response/ack flushes first. esp_timer, NOT
 * xTaskCreate: the timer task and its stack exist from boot, so this cannot
 * fail at reboot time. The old 2048 B xTaskCreate failed SILENTLY once link
 * churn had exhausted internal heap — the node said "rebooting" over and over
 * and never did (2026-08-29; heap looked fine because PSRAM hid it). */
static void reboot_cb(void *arg) { esp_restart(); }
void ota_schedule_reboot(void)
{
    static esp_timer_handle_t s_tmr;
    if (!s_tmr) {
        const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "reboot" };
        esp_err_t e = esp_timer_create(&a, &s_tmr);
        if (e != ESP_OK) {          /* truly cornered: reboot from this task */
            ESP_LOGE(TAG, "reboot timer create failed (%d) — direct restart", e);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
    ESP_LOGW(TAG, "reboot scheduled (1 s)");
    esp_timer_start_once(s_tmr, 1000000);
}

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const char *code, const char *msg)
{
    if (h) esp_ota_abort(h);
    ESP_LOGE(TAG, "OTA failed: %s", msg);
    httpd_resp_set_status(req, code);
    httpd_resp_sendstr(req, msg);
    return ESP_FAIL;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
    if (!upd) return fail(req, 0, "500 Internal Server Error", "no OTA slot");
    if (req->content_len == 0 || req->content_len > upd->size)
        return fail(req, 0, "413 Content Too Large", "empty image or image exceeds OTA slot");
    ESP_LOGI(TAG, "OTA start -> %s (%d bytes announced)", upd->label, req->content_len);

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(upd, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
        return fail(req, 0, "500 Internal Server Error", "esp_ota_begin failed");

    char buf[1460];
    int remaining = req->content_len;   /* from Content-Length */
    size_t total = 0;
    int idle_timeouts = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            /* Bounded, NOT `continue` forever: a half-open upload (client died
             * mid-body, FIN lost) otherwise wedges httpd's single worker for
             * the rest of the boot — the port accepts but nothing is served,
             * and OTA looks dead until power-cycle (2026-08-29). 3 x 15 s of
             * dead air is far beyond any live client's stall. */
            if (++idle_timeouts >= 3)
                return fail(req, h, "408 Request Timeout", "upload stalled — aborting");
            continue;
        }
        idle_timeouts = 0;
        if (n <= 0) return fail(req, h, "400 Bad Request", "recv error / short upload");
        if (esp_ota_write(h, buf, n) != ESP_OK)
            return fail(req, h, "500 Internal Server Error", "flash write error");
        total += n;
        remaining -= n;
    }

    esp_err_t e = esp_ota_end(h);   /* validates the image */
    if (e != ESP_OK)
        return fail(req, 0, "400 Bad Request",
                    e == ESP_ERR_OTA_VALIDATE_FAILED ? "image validation failed"
                                                     : "esp_ota_end failed");
    if (esp_ota_set_boot_partition(upd) != ESP_OK)
        return fail(req, 0, "500 Internal Server Error", "set_boot_partition failed");

    ESP_LOGW(TAG, "OTA ok: %u bytes -> %s; rebooting", (unsigned)total, upd->label);
    char ok[96];
    snprintf(ok, sizeof(ok), "OK: %u bytes -> %s, rebooting\n", (unsigned)total, upd->label);
    httpd_resp_sendstr(req, ok);

    ota_schedule_reboot();
    return ESP_OK;
}

bool ota_is_up(void) { return s_srv != NULL; }

/* Read-only evidence for the updater. An open listener alone cannot distinguish
 * a successful update from the old image, an automatic rollback, or a boot
 * which has not yet cancelled rollback. No battery/network details exposed. */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    int st = esp_ota_get_state_partition(run, &state) == ESP_OK ? (int)state : -1;
    char sha[65];
    const esp_app_desc_t *app = esp_app_get_description();
    for (size_t i = 0; i < sizeof(app->app_elf_sha256); i++)
        snprintf(sha + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
    char body[192];
    snprintf(body, sizeof(body),
             "{\"elf_sha256\":\"%s\",\"ota_state\":%d,\"uptime_ms\":%lld}",
             sha, st, (long long)(esp_timer_get_time() / 1000));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

void ota_start(uint16_t port)
{
    if (s_srv) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = port;
    cfg.stack_size       = 8192;          /* esp_ota_write on the httpd task */
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 15;
    cfg.lru_purge_enable  = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed on port %u", port);
        s_srv = NULL;
        return;
    }
    static const httpd_uri_t u = {
        .uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler, .user_ctx = NULL,
    };
    static const httpd_uri_t status = {
        .uri = "/ota/status", .method = HTTP_GET, .handler = ota_status_handler,
    };
    if (httpd_register_uri_handler(srv, &u) != ESP_OK ||
        httpd_register_uri_handler(srv, &status) != ESP_OK) {
        ESP_LOGE(TAG, "OTA handler registration failed; stopping server for retry");
        httpd_stop(srv);
        return;
    }
    s_srv = srv;  /* advertise ready only when both routes exist */
    ESP_LOGI(TAG, "push-OTA receiver up: POST http://<host>:%u/ota", port);
}

void ota_mark_valid(void)
{
    if (!ota_is_up()) return;  /* never confirm an image with no recovery server */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        if (e == ESP_OK)
            ESP_LOGI(TAG, "running image confirmed valid (rollback cancelled)");
        else
            ESP_LOGE(TAG, "image confirmation failed: %s", esp_err_to_name(e));
    }
}
