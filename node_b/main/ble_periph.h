/*
 * ble_periph.h — the single shared replica GATT table + per-identity routing
 * (spec §5). ONE attribute table is registered for all four identities;
 * identity comes from which advertising set (address) a connection landed on.
 * Reads are answered from that identity's local cache (spec §6, synchronous
 * NimBLE access callback); writes are forwarded to A over the tunnel.
 */
#ifndef BLE_PERIPH_H
#define BLE_PERIPH_H

#include <stdint.h>
#include <stdbool.h>

void ble_periph_start(void);
void ble_periph_rebuild_table(void);     /* (re)register from the blueprint */

/* Called by tunnel_cli. */
void ble_periph_forward_notify(uint8_t bms_id, uint8_t idx,
                               const uint8_t *data, uint16_t len);
void ble_periph_on_write_result(uint8_t bms_id, uint8_t idx, uint8_t status);
void ble_periph_replay_tick(void);       /* deliver owed warm replays (~10 Hz) */
void ble_periph_drop_all(void);          /* terminate all app connections */

#endif /* BLE_PERIPH_H */
