/*
 * adv_mgr.h — four BLE-5.0 extended-advertising sets, one per identity
 * (spec §5). Legacy-format connectable PDUs for app compatibility. A set
 * advertises only while its unit is reachable (LINK >= 1) AND the tunnel is up;
 * it stops while connected and resumes on disconnect. Each set uses a distinct
 * static-random address — that address is how a connection is mapped back to
 * its identity (iOS matches on name+UUID, never MAC — spec §5 iOS notes).
 */
#ifndef ADV_MGR_H
#define ADV_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "tunnel_proto.h"

void adv_mgr_init(void);                 /* generate addresses, configure sets */
void adv_mgr_set_name(uint8_t bms_id, const char *name);
void adv_mgr_on_link(uint8_t bms_id, tunnel_link_state_t state);
void adv_mgr_on_connect(uint8_t bms_id);
void adv_mgr_on_disconnect(uint8_t bms_id);
void adv_mgr_pause_all(void);            /* tunnel-down / grace expiry (spec §11) */

const uint8_t *adv_mgr_addr(uint8_t bms_id);      /* 6-byte static-random addr */
int  adv_mgr_identity_for_addr(const uint8_t *addr); /* -> bms_id or -1 */

#endif /* ADV_MGR_H */
