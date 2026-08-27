/*
 * arbiter.c — per-BMS serialisation + §10 policy + balance-write validation.
 *
 * All pending-queue manipulation happens on the single arbiter task, so no
 * lock is needed on the per-unit rings: other tasks only ever post messages
 * onto g_q_arb_in.
 */
#include <string.h>
#include "arbiter.h"
#include "queues.h"
#include "config.h"
#include "state_cache.h"
#include "tunnel_srv.h"
#include "mqtt_task.h"
#include "measure.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "arbiter";

/* ---- inbound message union --------------------------------------------- */
typedef enum { ARB_REQ, ARB_APP_CONN, ARB_MQTT } arb_kind_t;
typedef enum { MQTT_BALANCE, MQTT_MEASURE, MQTT_REFRESH } arb_mqtt_t;

typedef struct {
    arb_kind_t kind;
    uint8_t    bms_id;
    union {
        bms_request_t req;
        bool          connected;
        struct { arb_mqtt_t type; char json[192]; char id[32]; } mqtt;
    };
} arb_msg_t;

/* ---- per-unit pending ring --------------------------------------------- */
#define PEND_DEPTH 8
typedef struct {
    bms_request_t ring[PEND_DEPTH];
    uint8_t head, tail, count;
    bool     busy;              /* a transaction is outstanding on the link  */
    uint16_t next_cmd_id;
    int64_t  link_wait_deadline_us; /* app-write link-up guard (0 = inactive) */
    int64_t  connect_after_us;  /* backoff: don't attempt a connect before this */
} pend_t;

#define CONNECT_BACKOFF_US (2 * 1000000LL)  /* per-unit retry gap after a failed
                                             * connect — stops a tight retry loop
                                             * while another unit holds the scan */

static pend_t s_pend[CFG_NUM_UNITS];

static bool ring_push(pend_t *p, const bms_request_t *r)
{
    if (p->count >= PEND_DEPTH) return false;
    p->ring[p->tail] = *r;
    p->tail = (p->tail + 1) % PEND_DEPTH;
    p->count++;
    return true;
}
static bool ring_pop(pend_t *p, bms_request_t *out)
{
    if (!p->count) return false;
    *out = p->ring[p->head];
    p->head = (p->head + 1) % PEND_DEPTH;
    p->count--;
    return true;
}

/* ---- runtime helpers ---------------------------------------------------- */
static void rt_set_app(uint8_t id, bool app)
{
    bms_runtime_t rt; state_get_runtime(id, &rt);
    rt.app_connected = app; state_set_runtime(id, &rt);
    if (app) xEventGroupSetBits(g_evt, EVT_APP_ACTIVE);
}
static bool rt_app_connected(uint8_t id)
{ bms_runtime_t rt; state_get_runtime(id, &rt); return rt.app_connected; }
static bool rt_link_held(uint8_t id)
{ bms_runtime_t rt; state_get_runtime(id, &rt); return rt.link_held; }

/* ---- dispatch ----------------------------------------------------------- */
static void dispatch(uint8_t id)
{
    pend_t *p = &s_pend[id];
    if (p->busy || !p->count) return;

    /* Peek the head to decide if it may run now. */
    bms_request_t *r = &p->ring[p->head];

    /* Connection ops and polls can run without a prior link; writes/polls to a
     * live unit require link_held. TXN_CONNECT brings the link up. */
    bool needs_link = (r->kind != TXN_CONNECT && r->kind != TXN_DISCONNECT);
    if (needs_link && !rt_link_held(id)) {
        /* Respect the post-failure backoff so we don't spin retrying a connect
         * that keeps failing instantly (e.g. while another unit holds the scan
         * slot). Leaves the request pending; a later tick retries. */
        if (esp_timer_get_time() < p->connect_after_us) return;
        /* Not up yet. If nothing is establishing it, request a connect.
         * Bounded send: never block the arbiter task (else g_q_arb_in fills and
         * its producers — incl. the watchdog-fed supervisor — block forever). */
        bms_request_t c = { .bms_id = id, .kind = TXN_CONNECT,
                            .source = SRC_INTERNAL, .response_needed = true,
                            .timeout_ms = 4000, .cmd_id = ++p->next_cmd_id };
        if (xQueueSend(g_q_bms_request, &c, pdMS_TO_TICKS(20)) == pdTRUE)
            p->busy = true;   /* retry next tick if the link queue was full */
        return;
    }

    bms_request_t out;
    ring_pop(p, &out);
    out.cmd_id = ++p->next_cmd_id;
    if (xQueueSend(g_q_bms_request, &out, pdMS_TO_TICKS(20)) == pdTRUE)
        p->busy = true;
    else {
        ring_push(p, &out);   /* couldn't dispatch; requeue, retry next tick */
        ESP_LOGW(TAG, "bms %u req queue full, requeued", id);
    }
}

/* ---- §10 balance-write validation -------------------------------------- */
static const cfg_range_t *whitelist_lookup(const char *key)
{
    for (size_t i = 0; i < CFG_BALANCE_RANGE_N; i++)
        if (strcmp(CFG_BALANCE_RANGE[i].key, key) == 0) return &CFG_BALANCE_RANGE[i];
    return NULL;
}

static void handle_balance_set(uint8_t id, const char *json, const char *cid)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) { mqtt_ack(id, "balance/set", cid, "bad_json", NULL, NULL); return; }

    /* 1. whitelist + 2. range clamp — reject the whole command on any miss. */
    cJSON *it;
    cJSON_ArrayForEach(it, root) {
        const cfg_range_t *rg = whitelist_lookup(it->string);
        if (!rg) {
            mqtt_ack(id, "balance/set", cid, "rejected_out_of_scope", it->string, NULL);
            cJSON_Delete(root); return;
        }
        double v = cJSON_IsBool(it) ? (cJSON_IsTrue(it) ? 1 : 0) : it->valuedouble;
        if (v < rg->min || v > rg->max) {
            ESP_LOGW(TAG, "range reject %s=%f", it->string, v);
            mqtt_ack(id, "balance/set", cid, "out_of_range", it->string, NULL);
            cJSON_Delete(root); return;
        }
    }

    /* Arbitration (§10): app takes precedence. */
    if (rt_app_connected(id)) {
        /* CFG_ARB_MODE_QUEUE=1 would stash for apply-on-disconnect; block mode
         * simply defers. */
        mqtt_ack(id, "balance/set", cid, "deferred_app_active", NULL, NULL);
        cJSON_Delete(root); return;
    }

    /* 6. rate limit. */
    bms_runtime_t rt; state_get_runtime(id, &rt);
    if (CFG_BALANCE_RATE_PER_CYCLE && rt.writes_this_cycle >= CFG_BALANCE_RATE_PER_CYCLE) {
        mqtt_ack(id, "balance/set", cid, "rate_limited", NULL, NULL);
        cJSON_Delete(root); return;
    }

    /* 3/5. Build login + write frame. GATED: returns -1 until O-2/O-5 ported. */
    uint8_t frame[JK_FRAME_MAX];
    cJSON *first = root->child;
    double v = cJSON_IsBool(first) ? (cJSON_IsTrue(first) ? 1 : 0) : first->valuedouble;
    int n = jk_build_balance_write(JK_FRAME_JK02_32S, first->string, v,
                                   frame, sizeof(frame));
    if (n < 0) {
        /* Honest: the write path is compile-disabled until bench-verified. */
        mqtt_ack(id, "balance/set", cid, "write_path_disabled", first->string, NULL);
        cJSON_Delete(root); return;
    }

    /* Enqueue the validated write; readback happens in the response handler. */
    bms_request_t req = { .bms_id = id, .kind = TXN_BALANCE_WRITE, .source = SRC_MQTT,
                          .with_response = true, .response_needed = true,
                          .timeout_ms = 3000, .payload_len = (uint8_t)n };
    memcpy(req.payload, frame, n);
    arbiter_submit(&req);
    /* NOTE: on the response, re-read settings, compare, publish readback,
     * update retained state/settings + push READ_CACHE, bump writes_this_cycle. */
    cJSON_Delete(root);
}

/* ---- response routing --------------------------------------------------- */
static void on_response(const bms_response_t *rsp)
{
    uint8_t id = rsp->bms_id;
    pend_t *p = &s_pend[id];
    p->busy = false;

    switch (rsp->status) {
        case RESP_OK:
            /* Connection just came up? flush pending. Decode handled elsewhere. */
            break;
        case RESP_TIMEOUT:
        case RESP_LINK_DOWN:
        case RESP_GATT_ERR:
            /* Arbiter timeout guard freed the link (spec §11). Back off before
             * the next connect so a persistently-unreachable unit can't spin. */
            p->connect_after_us = esp_timer_get_time() + CONNECT_BACKOFF_US;
            ESP_LOGW(TAG, "bms %u txn cmd=%u status=%d", id, rsp->cmd_id, rsp->status);
            break;
        default: break;
    }
    dispatch(id);
}

/* ---- app-connect handling ---------------------------------------------- */
static void on_app_conn(uint8_t id, bool connected)
{
    rt_set_app(id, connected);
    pend_t *p = &s_pend[id];
    if (connected) {
        /* Bring the real link up; start the link-up guard (spec §4). */
        p->link_wait_deadline_us = esp_timer_get_time() + CFG_APP_LINK_TIMEOUT_MS * 1000LL;
        bms_request_t c = { .bms_id = id, .kind = TXN_CONNECT, .source = SRC_APP,
                            .response_needed = true, .timeout_ms = 4000 };
        arbiter_submit(&c);
    } else {
        /* App left: post-session settings re-read (spec §4), then idle timer
         * (owned by supervisor) will drop the link. */
        p->link_wait_deadline_us = 0;
        arbiter_poll(id, JK_CMD_DEVICE_INFO); /* placeholder for settings re-read */
        bms_runtime_t rt; state_get_runtime(id, &rt);
        rt.app_left_us = esp_timer_get_time(); state_set_runtime(id, &rt);
    }
}

/* ---- link-up guard check (called each loop tick) ----------------------- */
static void check_link_guards(void)
{
    int64_t now = esp_timer_get_time();
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        pend_t *p = &s_pend[id];
        if (p->link_wait_deadline_us && now > p->link_wait_deadline_us
            && !rt_link_held(id)) {
            ESP_LOGW(TAG, "bms %u app link-up timed out -> unreachable", id);
            p->link_wait_deadline_us = 0;
            /* Flush queued app writes as link-down. */
            bms_request_t r;
            while (ring_pop(p, &r)) {
                if (r.source == SRC_APP)
                    tunnel_send_write_result(id, r.idx, TUN_WR_LINK_DOWN);
            }
            bms_runtime_t rt; state_get_runtime(id, &rt);
            rt.link = LINK_UNREACHABLE; state_set_runtime(id, &rt);
            tunnel_send_link(id, LINK_UNREACHABLE);  /* B drops the app (spec §5) */
        }
    }
}

/* ---- public submit API -------------------------------------------------- */
/* Bounded enqueue onto g_q_arb_in: producers (tunnel, mqtt, and the
 * watchdog-fed supervisor) must never block forever if the arbiter is slow.
 * Dropping the odd internal poll under overload is fine; it is re-issued. */
static void arb_in_send(const arb_msg_t *m)
{
    if (xQueueSend(g_q_arb_in, m, pdMS_TO_TICKS(20)) != pdTRUE)
        ESP_LOGW(TAG, "arb_in full — dropped (bms %u kind %d)", m->bms_id, m->kind);
}

void arbiter_submit(const bms_request_t *req)
{
    arb_msg_t m = { .kind = ARB_REQ, .bms_id = req->bms_id, .req = *req };
    arb_in_send(&m);
}
void arbiter_app_write(uint8_t id, uint8_t idx, bool wr, const uint8_t *d, uint8_t n)
{
    bms_request_t r = { .bms_id = id, .kind = TXN_RAW_WRITE, .source = SRC_APP,
                        .idx = idx, .with_response = wr, .response_needed = wr,
                        .timeout_ms = 3000, .payload_len = n };
    if (n > REQ_PAYLOAD_MAX) n = REQ_PAYLOAD_MAX;
    memcpy(r.payload, d, n);
    arbiter_submit(&r);
}
void arbiter_poll(uint8_t id, uint8_t opcode)
{
    bms_request_t r = { .bms_id = id, .kind = TXN_POLL, .source = SRC_INTERNAL,
                        .opcode = opcode, .response_needed = true, .timeout_ms = 3000 };
    arbiter_submit(&r);
}
void arbiter_set_app_connected(uint8_t id, bool connected)
{
    arb_msg_t m = { .kind = ARB_APP_CONN, .bms_id = id, .connected = connected };
    arb_in_send(&m);
}
static void submit_mqtt(uint8_t id, arb_mqtt_t t, const char *json, const char *cid)
{
    arb_msg_t m = { .kind = ARB_MQTT, .bms_id = id };
    m.mqtt.type = t;
    if (json) strlcpy(m.mqtt.json, json, sizeof(m.mqtt.json));
    if (cid)  strlcpy(m.mqtt.id, cid, sizeof(m.mqtt.id));
    arb_in_send(&m);
}
void arbiter_balance_set(uint8_t id, const char *json, const char *cid) { submit_mqtt(id, MQTT_BALANCE, json, cid); }
void arbiter_measure(uint8_t id, const char *cid)                       { submit_mqtt(id, MQTT_MEASURE, NULL, cid); }
void arbiter_refresh(uint8_t id, const char *cid)                       { submit_mqtt(id, MQTT_REFRESH, NULL, cid); }

/* ---- task --------------------------------------------------------------- */
static void arbiter_task(void *arg)
{
    /* Set length must be >= sum of member queue lengths (24 + 12). */
    QueueSetHandle_t qs = xQueueCreateSet(24 + 12);
    xQueueAddToSet(g_q_arb_in, qs);
    xQueueAddToSet(g_q_bms_response, qs);

    for (;;) {
        QueueSetMemberHandle_t m = xQueueSelectFromSet(qs, pdMS_TO_TICKS(500));
        if (m == g_q_arb_in) {
            arb_msg_t msg;
            xQueueReceive(g_q_arb_in, &msg, 0);
            switch (msg.kind) {
                case ARB_REQ:
                    if (ring_push(&s_pend[msg.bms_id], &msg.req)) dispatch(msg.bms_id);
                    else ESP_LOGW(TAG, "bms %u pending full, dropped", msg.bms_id);
                    break;
                case ARB_APP_CONN:
                    on_app_conn(msg.bms_id, msg.connected);
                    break;
                case ARB_MQTT:
                    if (msg.mqtt.type == MQTT_BALANCE)
                        handle_balance_set(msg.bms_id, msg.mqtt.json, msg.mqtt.id);
                    else if (msg.mqtt.type == MQTT_MEASURE) {
                        if (rt_app_connected(msg.bms_id))
                            mqtt_ack(msg.bms_id, "measure", msg.mqtt.id, "deferred_app_active", NULL, NULL);
                        else
                            measure_start(msg.bms_id, msg.mqtt.id);   /* §9 sequence */
                    } else { /* MQTT_REFRESH */
                        arbiter_poll(msg.bms_id, JK_CMD_CELL_INFO);
                        if (rt_app_connected(msg.bms_id))
                            mqtt_ack(msg.bms_id, "refresh", msg.mqtt.id, "deferred_app_active", "settings", NULL);
                    }
                    break;
            }
        } else if (m == g_q_bms_response) {
            bms_response_t rsp;
            xQueueReceive(g_q_bms_response, &rsp, 0);
            on_response(&rsp);
        }
        check_link_guards();
    }
}

void arbiter_start(void)
{
    memset(s_pend, 0, sizeof(s_pend));
    /* Item size must equal sizeof(arb_msg_t) exactly (private to this file). */
    g_q_arb_in = xQueueCreate(24, sizeof(arb_msg_t));
    /* tskNO_AFFINITY: C3 is single-core; on a dual-core part this floats. */
    xTaskCreatePinnedToCore(arbiter_task, "arbiter", 6144, NULL, 6, NULL, tskNO_AFFINITY);
}
