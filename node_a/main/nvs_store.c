#include <string.h>
#include <stdio.h>
#include "nvs_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "nvs_store";
static const char *NS  = "jkbridge";

void nvs_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* Blobs are stored under fixed keys; harvest_%u and the single meas record. */
static void harvest_key(uint8_t bms_id, char *k, size_t n)
{ snprintf(k, n, "harv_%u", bms_id); }

bool nvs_get_harvest(uint8_t bms_id, harvest_entry_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char k[16]; harvest_key(bms_id, k, sizeof(k));
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, k, out, &len);
    nvs_close(h);
    return err == ESP_OK && len == sizeof(*out) && out->valid;
}

void nvs_put_harvest(uint8_t bms_id, const harvest_entry_t *in)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) { ESP_LOGE(TAG, "open"); return; }
    char k[16]; harvest_key(bms_id, k, sizeof(k));
    ESP_ERROR_CHECK(nvs_set_blob(h, k, in, sizeof(*in)));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "harvest[%u] '%s' ver=%d stored", bms_id, in->name, in->ver);
}

bool nvs_get_meas(meas_record_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, "meas", out, &len);
    nvs_close(h);
    return err == ESP_OK && len == sizeof(*out) && out->active;
}

void nvs_put_meas(const meas_record_t *in)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) { ESP_LOGE(TAG, "open"); return; }
    ESP_ERROR_CHECK(nvs_set_blob(h, "meas", in, sizeof(*in)));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

void nvs_clear_meas(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "meas");
    nvs_commit(h);
    nvs_close(h);
}
