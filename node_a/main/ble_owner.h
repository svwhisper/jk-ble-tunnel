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

#endif /* BLE_OWNER_H */
