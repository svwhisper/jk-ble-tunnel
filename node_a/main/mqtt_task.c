#include <string.h>
#include <stdio.h>
#include "mqtt_task.h"
#include "config.h"
#include "queues.h"
#include "state_cache.h"
#include "arbiter.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_cli;

/* ---- topic helpers ------------------------------------------------------ */
static void topic(char *out, size_t n, uint8_t id, const char *leaf)
{ snprintf(out, n, "%s/%u/%s", CFG_MQTT_BASE, id, leaf); }

static void pub(const char *t, const char *payload, int retain)
{
    if (s_cli) esp_mqtt_client_publish(s_cli, t, payload, 0, 1, retain);
}

/* ---- inbound command routing -------------------------------------------- */
/* topic form: jkbms/<id>/cmd/<what> */
static void on_cmd(const char *t, int tlen, const char *data, int dlen)
{
    /* parse the numeric id and the trailing command */
    unsigned id; char what[32] = {0};
    /* CFG_MQTT_BASE is "jkbms" */
    if (sscanf(t, CFG_MQTT_BASE "/%u/cmd/%31s", &id, what) != 2) return;
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
        .broker.address.uri = MQTT_URI,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
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
