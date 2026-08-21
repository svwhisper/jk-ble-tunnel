/*
 * tunnel_cli.h — TCP client to Node A (spec §6). Owns resync (TABLE_REQ ->
 * blueprint/idents/link/caches -> CLIENT replay) and the grace window: on
 * tunnel loss, app connections are held for CFG_TUNNEL_GRACE_MS while
 * reconnecting; beyond that they are dropped and advertising is paused.
 */
#ifndef TUNNEL_CLI_H
#define TUNNEL_CLI_H

#include <stdint.h>
#include <stdbool.h>

void tunnel_cli_start(void);
bool tunnel_cli_up(void);

/* Called by ble_periph. Enqueue app-originated events to A. */
void tunnel_cli_send_write(uint8_t bms_id, uint8_t idx, bool with_resp,
                           const uint8_t *data, uint16_t len);
void tunnel_cli_send_client(uint8_t bms_id, bool connected);

#endif /* TUNNEL_CLI_H */
