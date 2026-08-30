/*
 * arbiter.h — per-BMS serialisation, response/timeout gate, §10 policy.
 *
 * Every BMS interaction funnels through here (spec §3). The arbiter is the
 * only writer of app-connected/link bookkeeping and the only place the
 * balance-write validation pipeline (§10) runs.
 */
#ifndef ARBITER_H
#define ARBITER_H

#include "na_types.h"

void arbiter_start(void);

/* Submissions (thread-safe; enqueue onto g_q_arb_in). */
void arbiter_submit(const bms_request_t *req);

/* Convenience wrappers used by the other tasks. */
void arbiter_app_write(uint8_t bms_id, uint8_t idx, bool with_resp,
                       const uint8_t *data, uint8_t len);
void arbiter_poll(uint8_t bms_id, uint8_t opcode);          /* internal poll */
void arbiter_set_app_connected(uint8_t bms_id, bool connected); /* from tunnel */
void arbiter_notify_settings(uint8_t bms_id);  /* decoder: a settings frame decoded */
void arbiter_clear_pending(uint8_t bms_id);  /* flush stale poll ring (link-up) */

/* MQTT command entry points (validated inside the arbiter, §10). */
void arbiter_balance_set(uint8_t bms_id, const char *json, const char *id);
void arbiter_measure(uint8_t bms_id, const char *id);
void arbiter_refresh(uint8_t bms_id, const char *id);
void arbiter_bounce(uint8_t bms_id);      /* drop + let next cmd re-raise the link */

#endif /* ARBITER_H */
