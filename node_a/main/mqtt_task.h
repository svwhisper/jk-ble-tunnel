/*
 * mqtt_task.h — Mosquitto client (spec §7). LWT on jkbms/bridge/status so a
 * dead Node A reads offline, not stale. Publishes decoded state per unit;
 * subscribes to the three cmd topics and routes them into the arbiter.
 */
#ifndef MQTT_TASK_H
#define MQTT_TASK_H

#include <stdint.h>

void mqtt_start(void);

/* Publishers (read the state cache, build JSON, publish). */
void mqtt_publish_cells(uint8_t bms_id);
void mqtt_publish_summary(uint8_t bms_id);
void mqtt_publish_settings(uint8_t bms_id);   /* retained */
void mqtt_publish_faults(uint8_t bms_id);      /* retained */
void mqtt_publish_link(uint8_t bms_id);        /* retained */
void mqtt_publish_meas(uint8_t bms_id, const char *json);  /* retained */
void mqtt_publish_scan(const char *json);   /* jkbms/bridge/scan (diagnostic) */
void mqtt_publish_raw(uint8_t bms_id, const uint8_t *data, uint16_t len); /* jkbms/<id>/raw hex */
void mqtt_publish_appwrite(uint8_t bms_id, const uint8_t *data, uint16_t len); /* jkbms/<id>/appwrite hex */
void mqtt_publish_gatt(const char *json);   /* jkbms/bridge/gatt (diagnostic) */
void mqtt_publish_verify(const char *json); /* jkbms/bridge/verify (boot round) */
void mqtt_publish_llevent(const char *kind, uint8_t bms_id, int reason); /* bridge/llevent */
void mqtt_publish_ka(uint8_t links, const char *rssi);                   /* bridge/ka      */
void mqtt_publish_health(void);             /* jkbms/bridge/health heap/rssi  */

/* Command ack (spec §7): jkbms/<id>/ack {cmd,id,status,detail,readback}. */
void mqtt_ack(uint8_t bms_id, const char *cmd, const char *id,
              const char *status, const char *detail, const char *readback);

#endif /* MQTT_TASK_H */
