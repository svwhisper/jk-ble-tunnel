#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mqtt_task.h"
#include "config.h"
#include "queues.h"
#include "state_cache.h"
#include "arbiter.h"
#include "ble_owner.h"
#include "nvs_store.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_cli;

/* ---- topic helpers ------------------------------------------------------ */
static void topic(char *out, size_t n, uint8_t id, const char *leaf)
{ snprintf(out, n, "%s/%u/%s", CFG_MQTT_BASE, id, leaf); }

static void pub(const char *t, const char *payload, int retain)
{
    if (s_cli) esp_mqtt_client_publish(s_cli, t, payload, 0, 1, retain);
}

void mqtt_publish_scan(const char *json)   /* diagnostic, not retained */
{
    pub(CFG_MQTT_BASE "/bridge/scan", json, 0);
}

void mqtt_publish_gatt(const char *json)   /* diagnostic, not retained */
{
    pub(CFG_MQTT_BASE "/bridge/gatt", json, 0);
}

static void pub_hex(char *hex, size_t cap, uint8_t bms_id, const char *leaf,
                    const uint8_t *data, uint16_t len)
{
    int o = 0;
    for (int i = 0; i < len && o < (int)cap - 3; i++)
        o += snprintf(hex + o, cap - o, "%02X", data[i]);
    hex[o] = 0;
    char t[40]; snprintf(t, sizeof(t), "%s/%u/%s", CFG_MQTT_BASE, bms_id, leaf);
    pub(t, hex, 0);
}

/* Separate static buffers: raw runs on the NimBLE host task, appwrite on the
 * tunnel task — they can fire concurrently, so they must not share a buffer. */
void mqtt_publish_raw(uint8_t bms_id, const uint8_t *data, uint16_t len)
{ static char hex[600]; pub_hex(hex, sizeof(hex), bms_id, "raw", data, len); }

void mqtt_publish_appwrite(uint8_t bms_id, const uint8_t *data, uint16_t len)
{ static char hex[600]; pub_hex(hex, sizeof(hex), bms_id, "appwrite", data, len); }

/* Deferred reboot so the caller's MQTT publish/ack can flush first. */
static void reboot_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }

/* ---- inbound command routing -------------------------------------------- */
/* topic form: jkbms/<id>/cmd/<what>, plus system commands under jkbms/bridge/cmd/ */
static void on_cmd(const char *t, int tlen, const char *data, int dlen)
{
    /* esp-mqtt topics are not NUL-terminated — copy before any string op. */
    char topic[64] = {0};
    int tl = tlen < (int)sizeof(topic) - 1 ? tlen : (int)sizeof(topic) - 1;
    memcpy(topic, t, tl);

    /* System (bridge-level) commands are not tied to a bms id. Require a
     * non-empty payload so a zero-length retained-clear can't re-trigger them,
     * and clear any retained copy before acting so a retained command can't
     * boot-loop the node on every reconnect. */
    if (!strcmp(topic, CFG_MQTT_BASE "/bridge/cmd/reboot")) {
        if (dlen == 0) return;
        pub(CFG_MQTT_BASE "/bridge/cmd/reboot", "", 1);   /* clear retained */
        ESP_LOGW(TAG, "reboot requested via MQTT");
        xTaskCreate(reboot_task, "mqtt_reboot", 2048, NULL, 5, NULL);
        return;
    }
    if (!strcmp(topic, CFG_MQTT_BASE "/bridge/cmd/scan")) {
        if (dlen == 0) return;
        pub(CFG_MQTT_BASE "/bridge/cmd/scan", "", 1);      /* clear retained */
        ESP_LOGW(TAG, "BLE scan dump requested via MQTT");
        ble_owner_scan_dump();
        return;
    }
    if (!strcmp(topic, CFG_MQTT_BASE "/bridge/cmd/rawcap")) {
        if (dlen == 0) return;
        pub(CFG_MQTT_BASE "/bridge/cmd/rawcap", "", 1);    /* clear retained */
        char sec[8] = {0}; int sn = dlen < 7 ? dlen : 7; memcpy(sec, data, sn);
        ble_owner_rawcap(atoi(sec));   /* payload = seconds (0 -> default 20) */
        return;
    }
    if (!strcmp(topic, CFG_MQTT_BASE "/bridge/cmd/gattdump")) {
        if (dlen == 0) return;
        pub(CFG_MQTT_BASE "/bridge/cmd/gattdump", "", 1); /* clear retained */
        char idb[8] = {0}; int in = dlen < 7 ? dlen : 7; memcpy(idb, data, in);
        ble_owner_gattdump((uint8_t)atoi(idb));
        return;
    }
    if (!strcmp(topic, CFG_MQTT_BASE "/bridge/cmd/nvsclear")) {
        if (dlen == 0) return;
        pub(CFG_MQTT_BASE "/bridge/cmd/nvsclear", "", 1);  /* clear retained */
        ESP_LOGW(TAG, "clearing harvest NVS + rebooting (re-harvest fresh)");
        nvs_clear_harvest_all();
        xTaskCreate(reboot_task, "nvs_reboot", 2048, NULL, 5, NULL);
        return;
    }

    /* parse the numeric id and the trailing command */
    unsigned id; char what[32] = {0};
    /* CFG_MQTT_BASE is "jkbms" */
    if (sscanf(topic, CFG_MQTT_BASE "/%u/cmd/%31s", &id, what) != 2) return;
    if (id >= CFG_NUM_UNITS) return;

    char body[192] = {0}; int n = dlen < (int)sizeof(body) - 1 ? dlen : (int)sizeof(body) - 1;
    memcpy(body, data, n);

    /* optional correlation id */
    char cid[32] = {0};
    cJSON *j = cJSON_Parse(body);
    if (j) { cJSON *ji = cJSON_GetObjectItem(j, "id");
             if (cJSON_IsString(ji)) strlcpy(cid, ji->valuestring, sizeof(cid)); }

    if      (!strcmp(what, "balance/set")) arbiter_balance_set(id, body, cid);
    else if (!strcmp(what, "measure"))     arbiter_measure(id, cid);
    else if (!strcmp(what, "refresh"))     arbiter_refresh(id, cid);
    if (j) cJSON_Delete(j);
}

static void ev_handler(void *arg, esp_event_base_t base, int32_t ev, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)ev) {
    case MQTT_EVENT_CONNECTED: {
        xEventGroupSetBits(g_evt, EVT_MQTT_UP);
        /* bridge/status online (retained), and subscribe to commands. */
        pub(CFG_MQTT_BRIDGE_STATUS, "{\"online\":true}", 1);
        char sub[64]; snprintf(sub, sizeof(sub), "%s/+/cmd/#", CFG_MQTT_BASE);
        esp_mqtt_client_subscribe(s_cli, sub, 1);
        for (uint8_t i = 0; i < CFG_NUM_UNITS; i++) mqtt_publish_link(i);
        ESP_LOGI(TAG, "connected");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(g_evt, EVT_MQTT_UP);
        break;
    case MQTT_EVENT_DATA:
        on_cmd(e->topic, e->topic_len, e->data, e->data_len);
        break;
    default: break;
    }
}

/* ---- publishers --------------------------------------------------------- */
void mqtt_publish_cells(uint8_t id)
{
    bms_state_t s; if (!state_snapshot(id, &s) || !s.have_cells) return;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s.cells.cell_count; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "n", s.cells.cells[i].n);
        cJSON_AddNumberToObject(c, "v", s.cells.cells[i].mv / 1000.0);
        /* spec §9: failed resistance is null, never 0/low. */
        if (s.cells.cells[i].r_mohm < 0) cJSON_AddNullToObject(c, "r");
        else cJSON_AddNumberToObject(c, "r", s.cells.cells[i].r_mohm / 1000.0);
        cJSON_AddItemToArray(arr, c);
    }
    char *out = cJSON_PrintUnformatted(arr);
    char t[48]; topic(t, sizeof(t), id, "state/cells"); pub(t, out, 0);
    cJSON_free(out); cJSON_Delete(arr);
}

void mqtt_publish_summary(uint8_t id)
{
    bms_state_t s; if (!state_snapshot(id, &s) || !s.have_cells) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "pack_v",  s.cells.pack_mv / 1000.0);
    cJSON_AddNumberToObject(o, "current", s.cells.current_ma / 1000.0);
    cJSON_AddNumberToObject(o, "power",   s.cells.power_mw / 1000.0);
    cJSON_AddNumberToObject(o, "soc",     s.cells.soc_pct);
    cJSON_AddNumberToObject(o, "cycles",  s.cells.cycle_count);
    cJSON_AddBoolToObject(o, "balancing",   s.cells.balancing);
    cJSON_AddBoolToObject(o, "charging",    s.cells.charging);
    cJSON_AddBoolToObject(o, "discharging", s.cells.discharging);
    char *out = cJSON_PrintUnformatted(o);
    char t[48]; topic(t, sizeof(t), id, "state/summary"); pub(t, out, 0);
    cJSON_free(out); cJSON_Delete(o);
}

void mqtt_publish_settings(uint8_t id)
{
    bms_state_t s; if (!state_snapshot(id, &s) || !s.have_settings) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "balance_trigger_voltage", s.settings.balance_trigger_v);
    cJSON_AddNumberToObject(o, "balance_current",         s.settings.balance_current_a);
    cJSON_AddBoolToObject(o,   "balancing_enabled",       s.settings.balancing_enabled);
    char *out = cJSON_PrintUnformatted(o);
    char t[48]; topic(t, sizeof(t), id, "state/settings"); pub(t, out, 1);  /* retained */
    cJSON_free(out); cJSON_Delete(o);
}

void mqtt_publish_faults(uint8_t id)
{
    bms_state_t s; if (!state_snapshot(id, &s) || !s.have_cells) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "error_bitmask",     s.cells.error_bitmask);
    cJSON_AddNumberToObject(o, "wire_warn_bitmask", s.cells.wire_warn_bitmask);
    char *out = cJSON_PrintUnformatted(o);
    char t[48]; topic(t, sizeof(t), id, "state/faults"); pub(t, out, 1);
    cJSON_free(out); cJSON_Delete(o);
}

void mqtt_publish_link(uint8_t id)
{
    bms_runtime_t rt; state_get_runtime(id, &rt);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "reachability",  rt.link);
    cJSON_AddBoolToObject(o,   "app_connected", rt.app_connected);
    cJSON_AddNumberToObject(o, "last_seen",     rt.last_seen_us / 1000000);
    char *out = cJSON_PrintUnformatted(o);
    char t[48]; topic(t, sizeof(t), id, "state/link"); pub(t, out, 1);
    cJSON_free(out); cJSON_Delete(o);
}

void mqtt_publish_meas(uint8_t id, const char *json)
{
    char t[48]; topic(t, sizeof(t), id, "state/meas"); pub(t, json, 1);
}

void mqtt_ack(uint8_t id, const char *cmd, const char *cid,
              const char *status, const char *detail, const char *readback)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", cmd);
    if (cid && cid[0]) cJSON_AddStringToObject(o, "id", cid);
    cJSON_AddStringToObject(o, "status", status);
    if (detail)   cJSON_AddStringToObject(o, "detail", detail);
    if (readback) cJSON_AddRawToObject(o, "readback", readback);
    char *out = cJSON_PrintUnformatted(o);
    char t[48]; topic(t, sizeof(t), id, "ack"); pub(t, out, 0);
    cJSON_free(out); cJSON_Delete(o);
}

void mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_URI,   /* hostname; broker allows anonymous */
        /* LWT: retained offline on ungraceful disconnect (spec §7). */
        .session.last_will.topic = CFG_MQTT_BRIDGE_STATUS,
        .session.last_will.msg = "{\"online\":false}",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };
    s_cli = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_cli, ESP_EVENT_ANY_ID, ev_handler, NULL);
    esp_mqtt_client_start(s_cli);
}
