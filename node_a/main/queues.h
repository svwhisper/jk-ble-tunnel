/*
 * queues.h — inter-task queues + event group (spec §4 "Queues / sync").
 * Created once in app_main before any task starts.
 */
#ifndef QUEUES_H
#define QUEUES_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

extern QueueHandle_t g_q_arb_in;       /* anyone   -> arbiter (submits+events)  */
extern QueueHandle_t g_q_bms_request;  /* arbiter  -> ble_owner                 */
extern QueueHandle_t g_q_bms_response; /* ble_owner-> arbiter                   */
extern QueueHandle_t g_q_notify;       /* ble_owner-> tunnel_srv (NOTIFY to app) */
extern QueueHandle_t g_q_decode;       /* ble_owner-> decoder (decode -> state)  */
extern EventGroupHandle_t g_evt;

void queues_init(void);

#endif /* QUEUES_H */
