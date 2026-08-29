/*
 * tunnel_srv.h — TCP tunnel server for Node B (spec §6). Single client: a new
 * connection replaces the old (handles half-open sockets after a B reboot).
 * All socket writes happen on the tunnel task; other tasks enqueue via these.
 */
#ifndef TUNNEL_SRV_H
#define TUNNEL_SRV_H

#include <stdint.h>
#include <stdbool.h>
#include "tunnel_proto.h"

void tunnel_srv_start(void);

/* Outbound helpers (thread-safe; enqueue a pre-framed message). */
void tunnel_send_link(uint8_t bms_id, tunnel_link_state_t state);
void tunnel_send_write_result(uint8_t bms_id, uint8_t idx, tunnel_write_status_t st);
void tunnel_send_read_cache(uint8_t bms_id, uint8_t idx, const uint8_t *data, uint16_t len);

/* Re-send TABLE + IDENT + LINK for all harvested units (e.g. after a new
 * harvest) so Node B can advertise them without waiting for a reconnect. */
void tunnel_srv_announce(void);

/* IDENT + LINK only (no TABLE): the periodic self-heal for Node B's RAM-only
 * advertising state. Supervisor calls it every ~30 s while the tunnel is up. */
void tunnel_srv_refresh(void);

bool tunnel_is_up(void);

#endif /* TUNNEL_SRV_H */
