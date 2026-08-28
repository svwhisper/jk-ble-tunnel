/*
 * ble_owner.h — the ONLY task that touches BLE (spec §4). NimBLE central.
 *
 * Owns a pool of up to CFG_LINK_POOL_SIZE concurrent central links, executes
 * the transactions the arbiter hands it on g_q_bms_request, reassembles JK
 * frames off the notify path, and fans completed frames to the decoder and the
 * tunnel. Reports every transaction outcome on g_q_bms_response.
 */
#ifndef BLE_OWNER_H
#define BLE_OWNER_H

#include "na_types.h"
#include "nvs_store.h"

void ble_owner_start(void);

/* Last discovered GATT blueprint for a unit (valid once its link has come up
 * and discovery completed). Used by the harvest coordinator. */
bool ble_owner_copy_table(uint8_t bms_id, harvest_entry_t *out);

/* Request a one-shot diagnostic BLE scan: report every advertiser seen (name,
 * address, RSSI) to MQTT topic jkbms/bridge/scan. Non-invasive; runs on the
 * BLE task, briefly preempting the connect loop. Triggered by the MQTT command
 * jkbms/bridge/cmd/scan. Use it to discover the units' real advertised names
 * and Node A's actual RSSI to each. */
void ble_owner_scan_dump(void);

/* Arm raw-frame capture for `seconds`: every notify chunk received from a unit
 * is published as hex to jkbms/<id>/raw. Triggered by jkbms/bridge/cmd/rawcap.
 * Used to capture real JK frames and pin the decode offsets (O-1). */
void ble_owner_rawcap(int seconds);

/* Walk a connected unit's FULL GATT table (all services/chars, not just the
 * harvested JK pair) and publish it to jkbms/bridge/gatt. Triggered by
 * jkbms/bridge/cmd/gattdump with the bms_id as payload. */
void ble_owner_gattdump(uint8_t bms_id);

/* BLE master switch (bench instrumentation): boots per CFG_BLE_ON_AT_BOOT;
 * MQTT jkbms/bridge/cmd/ble "on"/"off". Off drops all held links. */
void ble_owner_set_ble(bool on);
bool ble_owner_ble_enabled(void);
/* Cumulative LL connect/disconnect events since boot (each connect chirps a
 * real unit; a healthy soak holds these flat after the initial connects). */
uint32_t ble_owner_conn_count(void);
uint32_t ble_owner_disc_count(void);

#endif /* BLE_OWNER_H */
