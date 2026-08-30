/*
 * arbiter.c — per-BMS serialisation + §10 policy + balance-write validation.
 *
 * All pending-queue manipulation happens on the single arbiter task, so no
 * lock is needed on the per-unit rings: other tasks only ever post messages
 * onto g_q_arb_in.
 */
#include <string.h>
#include <math.h>
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
typedef enum { ARB_REQ, ARB_APP_CONN, ARB_MQTT, ARB_CLEAR, ARB_SETTINGS } arb_kind_t;
typedef enum { MQTT_BALANCE, MQTT_MEASURE, MQTT_REFRESH } arb_mqtt_t;

/* ---- balance-write readback (§10) --------------------------------------- */
/* A settings write to FFE2 gets no ATT ack (write-no-rsp on the majority
 * module), and the BMS pushes fresh settings frames only sparsely — so the
 * arbiter confirms a write by actively re-reading and comparing the target
 * key against the next decoded settings frame, within a deadline. */
typedef struct {
    bool     active;
    uint8_t  which;          /* 0=trigger_v, 1=current_a, 2=enabled */
    double   target;         /* value in human units, for the compare */
    char     key[28];
    char     cid[32];
    int64_t  deadline_us;    /* hard: past this, ack written_unverified */
    int64_t  next_nudge_us;  /* when to issue the next settings re-read */
    uint8_t  nudges;         /* re-read polls issued so far (capped) */
} rb_t;
static rb_t s_rb[CFG_NUM_UNITS];
static int64_t s_last_write_us[CFG_NUM_UNITS];   /* debounce, set on submit */

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
    uint32_t backoff_ms;        /* current per-unit backoff, doubles on failure */
    int64_t  dispatch_after_us; /* failure-path rate limit: after ANY failed
                                 * response, gate the next dispatch briefly.
                                 * Without this, polls queued during a unit's
                                 * connect/discovery window fail instantly
                                 * (no val_handle yet) and the arbiter<->
                                 * ble_owner ping-pong runs at full speed,
                                 * starving the prio-3 supervisor -> task WDT
                                 * (garage/indoor crash loop, 2026-08-28). */
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
    pend_t *pgate = &s_pend[id];
    if (esp_timer_get_time() < pgate->dispatch_after_us) return;  /* rate limit */
    pend_t *p = &s_pend[id];
    if (p->busy || !p->count) return;

    /* Peek the head to decide if it may run now. */
    bms_request_t *r = &p->ring[p->head];

    /* Connection ops and polls can run without a prior link; writes/polls to a
     * live unit require link_held. TXN_CONNECT brings the link up. */
    bool needs_link = (r->kind != TXN_CONNECT && r->kind != TXN_DISCONNECT);
    if (needs_link && !rt_link_held(id)) {
        /* (dispatch_after_us is checked by the caller-side gate below) */
        /* Respect the post-failure backoff so we don't spin retrying a connect
         * that keeps failing instantly (e.g. while another unit holds the scan
         * slot). Leaves the request pending; a later tick retries. */
        if (esp_timer_get_time() < p->connect_after_us) return;
        /* Not up yet. If nothing is establishing it, request a connect.
         * Bounded send: never block the arbiter task (else g_q_arb_in fills and
         * its producers — incl. the watchdog-fed supervisor — block forever). */
        bms_request_t c = { .bms_id = id, .kind = TXN_CONNECT,
                            .source = SRC_INTERNAL, .response_needed = true,
                            /* > the 5 s scan window + discovery, else the §11
                             * sweep kills connects that are still scanning */
                            .timeout_ms = 9000, .cmd_id = ++p->next_cmd_id };
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

    /* One register per command (the app writes one register per frame, and a
     * single-key command keeps readback unambiguous). Reject multi-key. */
    if (!root->child || root->child->next) {
        mqtt_ack(id, "balance/set", cid, "one_key_per_command", NULL, NULL);
        cJSON_Delete(root); return;
    }

    /* 1. whitelist + 2. range clamp. */
    cJSON *it = root->child;
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

    /* Arbitration (§10): app takes precedence. */
    if (rt_app_connected(id)) {
        /* CFG_ARB_MODE_QUEUE=1 would stash for apply-on-disconnect; block mode
         * simply defers. */
        mqtt_ack(id, "balance/set", cid, "deferred_app_active", NULL, NULL);
        cJSON_Delete(root); return;
    }

    /* 6. rate limit — a minimum interval between writes per unit (debounce
     * against rapid-fire / accidental double-writes). Replaces the original
     * "one per charge cycle" counter: writes are occasional human operator
     * commands, not an automation loop, and per-cycle needed a charge-cycle
     * detector that never existed (the counter had no reset → a permanent
     * lock after the first write). */
    if (s_last_write_us[id] &&
        esp_timer_get_time() - s_last_write_us[id] < CFG_BALANCE_MIN_INTERVAL_MS * 1000LL) {
        mqtt_ack(id, "balance/set", cid, "rate_limited", NULL, NULL);
        cJSON_Delete(root); return;
    }

    /* 3/5. Build the write frame (no login: fw 19.31 needs none on the wire,
     * confirmed by capture). Register map lives in jk_proto (decoded from the
     * app's own frames). */
    uint8_t frame[JK_FRAME_MAX];
    int n = jk_build_balance_write(JK_FRAME_JK02_32S, it->string, v,
                                   frame, sizeof(frame));
    if (n < 0) {
        mqtt_ack(id, "balance/set", cid, "write_path_disabled", it->string, NULL);
        cJSON_Delete(root); return;
    }

    /* Enqueue the validated write to FFE2. */
    bms_request_t req = { .bms_id = id, .kind = TXN_BALANCE_WRITE, .source = SRC_MQTT,
                          .with_response = true, .response_needed = true,
                          .timeout_ms = 3000, .payload_len = (uint8_t)n };
    memcpy(req.payload, frame, n);
    arbiter_submit(&req);
    s_last_write_us[id] = esp_timer_get_time();   /* debounce from submit time */

    /* Arm the readback: FFE2 gives no ATT ack, so confirm by comparing the
     * next decoded settings frame against the target. */
    rb_t *rb = &s_rb[id];
    rb->active = true;
    rb->which  = !strcmp(it->string, "balance_current") ? 1
               : !strcmp(it->string, "balancing_enabled") ? 2
               : !strcmp(it->string, "balance_start_voltage") ? 3
               : !strcmp(it->string, "cell_count") ? 4 : 0;
    rb->target = v;
    strlcpy(rb->key, it->string, sizeof(rb->key));
    strlcpy(rb->cid, cid ? cid : "", sizeof(rb->cid));
    /* Generous window: a write to a sleeping bank waits for dispatch to raise
     * the link (~5 s connect+discovery) before the frame is even sent, then a
     * settings frame must arrive. 15 s covers a cold-link start. */
    int64_t nw = esp_timer_get_time();
    rb->deadline_us   = nw + 15000000LL;
    rb->next_nudge_us = nw + 5000000LL;   /* first re-read at ~5 s (post-write) */
    rb->nudges = 0;
    ESP_LOGI(TAG, "bms %u balance write %s=%.3f (%d bytes), readback armed",
             id, it->string, v, n);
    cJSON_Delete(root);
}

/* Compare a decoded settings snapshot to the armed target; publish the ack. */
static void rb_finish(uint8_t id, const char *status, double readval)
{
    rb_t *rb = &s_rb[id];
    char rbjson[64];
    snprintf(rbjson, sizeof(rbjson), "{\"%s\":%.3f}", rb->key, readval);
    mqtt_ack(id, "balance/set", rb->cid[0] ? rb->cid : NULL, status, rb->key, rbjson);
    rb->active = false;
}

static void rb_on_settings(uint8_t id)
{
    rb_t *rb = &s_rb[id];
    if (!rb->active) return;
    bms_state_t st;
    if (!state_snapshot(id, &st) || !st.have_settings) return;
    double got = rb->which == 1 ? st.settings.balance_current_a
               : rb->which == 2 ? (st.settings.balancing_enabled ? 1.0 : 0.0)
               : rb->which == 3 ? st.settings.balance_start_v
               : rb->which == 4 ? (double)st.settings.cell_count_set
               : st.settings.balance_trigger_v;
    /* Tolerance: half the register's least count (0.5 mV / 0.5 mA), booleans exact. */
    double tol = rb->which == 2 ? 0.5 : 0.0005;
    /* Only a MATCH is definitive. A mismatch is ignored, not failed: a settings
     * frame can arrive between arming and the write actually landing (the write
     * may wait ~5 s for an idle link to connect), and that pre-write frame
     * carries the OLD value — failing on it is a false negative (seen live
     * 2026-08-30). The deadline nudge forces a post-write frame; if none ever
     * matches, rb_tick reports written_unverified. */
    if (fabs(got - rb->target) <= tol) {
        rb_finish(id, "ok", got);
        ESP_LOGI(TAG, "bms %u readback OK %s=%.3f", id, rb->key, got);
    } else {
        ESP_LOGD(TAG, "bms %u readback frame pre-write? %s=%.3f (want %.3f), waiting",
                 id, rb->key, got, rb->target);
    }
}

/* Deadline sweep (called each task tick): nudge a re-read at the half-way
 * point, and give up honestly after the deadline. */
static void rb_tick(void)
{
    int64_t now = esp_timer_get_time();
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        rb_t *rb = &s_rb[id];
        if (!rb->active) continue;
        /* Re-read to force a fresh settings frame, spaced every ~4 s (capped),
         * so at least one poll lands AFTER the write completes whether the link
         * started held (write instant) or idle (~5 s to connect first). Cell-
         * info (0x96), NOT device-info: only a 0x96 poll reliably elicits a
         * fresh settings (0x01) frame on fw 19.31 — proven live 2026-08-30
         * (cmd/refresh uses 0x96 and self-confirmed a readback that a 0x97
         * nudge had missed). */
        if (rb->nudges < 3 && now > rb->next_nudge_us) {
            rb->nudges++;
            rb->next_nudge_us = now + 4000000LL;
            arbiter_poll(id, JK_CMD_CELL_INFO);
        }
        if (now > rb->deadline_us) {
            /* The write was sent; we just never saw a confirming frame in time.
             * Don't claim success or failure — the next periodic settings frame
             * updates retained state/settings regardless. */
            mqtt_ack(id, "balance/set", rb->cid[0] ? rb->cid : NULL,
                     "written_unverified", rb->key, NULL);
            rb->active = false;
            ESP_LOGW(TAG, "bms %u readback timed out (%s)", id, rb->key);
        }
    }
}

/* ---- response routing --------------------------------------------------- */
static void on_response(const bms_response_t *rsp)
{
    uint8_t id = rsp->bms_id;
    pend_t *p = &s_pend[id];
    p->busy = false;

    switch (rsp->status) {
        case RESP_OK:
            p->backoff_ms = 0;              /* healthy again: reset backoff */
            /* Connection just came up? flush pending. Decode handled elsewhere. */
            break;
        case RESP_TIMEOUT:
        case RESP_LINK_DOWN:
        case RESP_GATT_ERR:
            /* Arbiter timeout guard freed the link (spec §11). Back off before
             * the next connect so a persistently-unreachable unit can't spin,
             * and rate-limit ALL dispatch for this unit briefly so instant
             * failures can't ping-pong at full speed (task-WDT guard).
             * Backoff is EXPONENTIAL (2 s doubling to CFG_RECONNECT_CAP_MS):
             * a marginal-range unit that fails every attempt must not keep
             * the shared radio busy with a 5 s scan every 7 s forever. */
            if (p->backoff_ms < 2000) p->backoff_ms = 2000;
            else { p->backoff_ms *= 2;
                   if (p->backoff_ms > CFG_RECONNECT_CAP_MS) p->backoff_ms = CFG_RECONNECT_CAP_MS; }
            p->connect_after_us  = esp_timer_get_time() + (int64_t)p->backoff_ms * 1000;
            p->dispatch_after_us = esp_timer_get_time() + 100000;  /* 100 ms */
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
                            .response_needed = true, .timeout_ms = 9000 };
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
            ESP_LOGW(TAG, "bms %u app link-up timed out -> reachable-idle", id);
            p->link_wait_deadline_us = 0;
            /* Flush queued app writes as link-down. */
            bms_request_t r;
            while (ring_pop(p, &r)) {
                if (r.source == SRC_APP)
                    tunnel_send_write_result(id, r.idx, TUN_WR_LINK_DOWN);
            }
            /* REACHABLE_IDLE, not UNREACHABLE (2026-08-30): this unit was
             * streaming seconds ago — it is dozy/mortal, not gone. Marking
             * UNREACHABLE pulled its TUN off the air, so the user's NEXT
             * attempt could not even find the clone (part of the bank-1
             * timeout cascade). B's warm replay carries the current app
             * session; the supervisor's probes will demote a truly dead
             * unit on their own evidence. */
            bms_runtime_t rt; state_get_runtime(id, &rt);
            rt.link = LINK_REACHABLE_IDLE; state_set_runtime(id, &rt);
            tunnel_send_link(id, LINK_REACHABLE_IDLE);
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
/* Bounce: force a disconnect/reconnect of the REAL link. Bank 0's Telink
 * unit answers commands only in the seconds after a fresh connect (deaf
 * thereafter, 2026-08-30) — queueing a write right after a bounce lands it
 * inside that window. The disconnect goes through the normal txn path; the
 * next queued command re-raises the link on dispatch. */
void arbiter_bounce(uint8_t id)
{
    bms_request_t r = { .bms_id = id, .kind = TXN_DISCONNECT,
                        .source = SRC_INTERNAL, .response_needed = true,
                        .timeout_ms = 3000 };
    arbiter_submit(&r);
}

void arbiter_set_app_connected(uint8_t id, bool connected)
{
    arb_msg_t m = { .kind = ARB_APP_CONN, .bms_id = id, .connected = connected };
    arb_in_send(&m);
}
void arbiter_clear_pending(uint8_t id)
{
    arb_msg_t m = { .kind = ARB_CLEAR, .bms_id = id };
    arb_in_send(&m);
}
void arbiter_notify_settings(uint8_t id)
{
    arb_msg_t m = { .kind = ARB_SETTINGS, .bms_id = id };
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
                case ARB_CLEAR: {
                    /* Link-up ring flush (supervisor): polls queued while the
                     * unit was down are stale, and dispatching an old 0x96
                     * before the bootstrap 0x97 breaks fw 19.31's strict
                     * 97-then-96 stream-arming order. */
                    pend_t *pc = &s_pend[msg.bms_id];
                    pc->head = pc->tail = pc->count = 0;
                    break;
                }
                case ARB_SETTINGS:
                    rb_on_settings(msg.bms_id);   /* balance-write readback (§10) */
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
        rb_tick();                 /* balance-write readback deadlines (§10) */
        /* Retry gated/pending work even when no message arrives (the select
         * timeout above bounds this to every 500 ms). Cheap: dispatch() is a
         * no-op for idle or gated units. Parked units (NULL name in CFG_BMS)
         * are skipped entirely — a connect for them can only insta-fail. */
        for (uint8_t i = 0; i < CFG_NUM_UNITS; i++) {
            bool parked = true;
            for (int k = 0; k < CFG_NUM_UNITS; k++)
                if (CFG_BMS[k].bms_id == i && CFG_BMS[k].name) { parked = false; break; }
            if (!parked) dispatch(i);
        }
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
