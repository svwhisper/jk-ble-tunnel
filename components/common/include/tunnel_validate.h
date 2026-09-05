#ifndef TUNNEL_VALIDATE_H
#define TUNNEL_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>
#include "tunnel_proto.h"

/* Pure wire-boundary checks, shared with native malformed-frame tests. A
 * length is checked before every payload access; reject rather than truncate. */
static inline bool tunnel_to_a_valid(uint8_t type, uint8_t id, const uint8_t *p,
                                     size_t len, uint8_t units)
{
    if (len > TUNNEL_MAX_PAYLOAD || (len && !p)) return false;
    switch (type) {
    case TUN_PING:
    case TUN_TABLE_REQ:
        return id == TUNNEL_BMS_ID_LINK && len == 0;
    case TUN_CLIENT:
        return id < units && len == 1 && p[0] <= 1;
    case TUN_WRITE:
        return id < units && len >= 3 && len <= 2 + TUNNEL_MAX_WRITE_DATA &&
               p[0] <= 1 && p[1] <= 1;
    default:
        return false;
    }
}

static inline bool tunnel_to_b_valid(uint8_t type, uint8_t id, const uint8_t *p,
                                     size_t len, uint8_t units, uint8_t chars,
                                     size_t cache_max)
{
    if (len > TUNNEL_MAX_PAYLOAD || (len && !p)) return false;
    if (type == TUN_PING) return id == TUNNEL_BMS_ID_LINK && len == 0;
    if (type == TUN_TABLE)
        return id == TUNNEL_BMS_ID_LINK && len >= 1 && p[0] > 0 && p[0] <= chars &&
               len == 1 + p[0] * sizeof(tunnel_char_desc_t);
    if (id >= units) return false;
    switch (type) {
    case TUN_IDENT: return len > 0 && len < 32;
    case TUN_READ_CACHE:
    case TUN_NOTIFY:
    case TUN_RAW:
        return len >= 1 && len <= cache_max + 1 && p[0] < chars;
    case TUN_WRITE_RESULT:
        return len == 2 && p[0] < chars && p[1] <= TUN_WR_GATT_ERR;
    case TUN_LINK: return len == 1 && p[0] <= LINK_UP;
    default: return false;
    }
}
#endif
