#ifndef HOST_BLE_HS_H
#define HOST_BLE_HS_H
/* Minimal native test declarations; firmware builds use pinned IDF headers.
 * These adapters emulate callback delivery, not the controller or radio. */
#include <stdint.h>
#include <stddef.h>
#define BLE_HS_EALREADY 2
#define BLE_HS_ENOTCONN 7
#define BLE_HS_EDONE 14
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_UUID_STR_LEN 37
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef union { ble_uuid_t u; ble_uuid16_t u16; } ble_uuid_any_t;
#define BLE_UUID16_INIT(v) { .u = {16}, .value = (v) }
#define BLE_UUID16_DECLARE(v) ((const ble_uuid_t *)&(const ble_uuid16_t)BLE_UUID16_INIT(v))
static inline uint16_t ble_uuid_u16(const ble_uuid_t *u)
{ return u->type == 16 ? ((const ble_uuid16_t *)u)->value : 0; }
char *ble_uuid_to_str(const ble_uuid_t *, char *);
typedef struct { uint8_t type, val[6]; } ble_addr_t;
struct os_mbuf { uint16_t len; };
#define OS_MBUF_PKTLEN(om) ((om)->len)
struct ble_hs_adv_fields { const uint8_t *name; uint8_t name_len; };
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *, const uint8_t *, uint8_t);
int ble_hs_mbuf_to_flat(const struct os_mbuf *, void *, uint16_t, uint16_t *);
int os_mbuf_append(struct os_mbuf *, const void *, uint16_t);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *, uint16_t);
uint16_t ble_att_mtu(uint16_t);
struct ble_hs_cfg_stub { void (*sync_cb)(void); };
extern struct ble_hs_cfg_stub ble_hs_cfg;
#endif
