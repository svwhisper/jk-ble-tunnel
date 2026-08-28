/*
 * tunnel_proto.h — A<->B TCP tunnel wire format (spec §6).
 *
 * Shared verbatim by node_a, node_b and the test rig so all three agree on
 * the byte layout. LAN-only, TCP_NODELAY, length-prefixed binary frames:
 *
 *     [u8 type][u8 bms_id][u16 len little-endian][payload...]
 *
 * bms_id 0xFF is the link-level / identity-independent channel (PING, TABLE).
 * All multi-byte integers on the wire are little-endian (ESP32 is LE, so the
 * structs below map directly; keep it that way if a big-endian peer is ever
 * added).
 */
#ifndef TUNNEL_PROTO_H
#define TUNNEL_PROTO_H

#include <stdint.h>

#define TUNNEL_PORT_DEFAULT      3760
#define TUNNEL_BMS_ID_LINK       0xFF   /* link-level frames (PING, TABLE)      */
#define TUNNEL_MAX_PAYLOAD       512    /* largest real payload = one 300 B frame + idx */
#define TUNNEL_HDR_LEN           4

/* Frame types. Values are wire-stable — never renumber, only append. */
typedef enum {
    TUN_TABLE       = 0x01, /* A->B  shared replica blueprint (bms_id 0xFF)      */
    TUN_IDENT       = 0x02, /* A->B  one per unit: advertised name for this id   */
    TUN_READ_CACHE  = 0x03, /* A->B  [u8 idx][data] prime/refresh a read cache   */
    TUN_NOTIFY      = 0x04, /* A->B  [u8 idx][data] subscription data, BMS->app  */
    TUN_WRITE       = 0x05, /* B->A  [u8 idx][u8 withResponse][data] app write   */
    TUN_WRITE_RESULT= 0x06, /* A->B  [u8 idx][u8 status] see tun_write_status_t  */
    TUN_LINK        = 0x07, /* A->B  [u8 state] per-BMS reachability tri-state   */
    TUN_CLIENT      = 0x08, /* B->A  [u8 connected] app connected/left identity  */
    TUN_TABLE_REQ   = 0x09, /* B->A  (no payload) send blueprint + state         */
    TUN_PING        = 0x0A, /* both  (no payload) keepalive, bms_id 0xFF         */
    TUN_RAW         = 0x0B, /* A->B  [u8 idx][data] verbatim BMS notify chunk,
                             * sent only while an app holds the identity; B
                             * forwards it to the app untouched so the clone is
                             * byte-transparent (C8 acks, AT noise, natural
                             * chunking). TUN_NOTIFY stays cache-refresh only. */
} tunnel_type_t;

/* TUN_LINK state byte — the reachability tri-state (spec §4). */
typedef enum {
    LINK_UNREACHABLE   = 0, /* connect attempts failing, backoff running */
    LINK_REACHABLE_IDLE= 1, /* reachable, no real link currently held     */
    LINK_UP            = 2, /* real central link established               */
} tunnel_link_state_t;

/* TUN_WRITE_RESULT status byte. */
typedef enum {
    TUN_WR_OK        = 0,
    TUN_WR_TIMEOUT   = 1,
    TUN_WR_LINK_DOWN = 2,
    TUN_WR_GATT_ERR  = 3,
} tunnel_write_status_t;

/* Fixed 4-byte header, little-endian len. */
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  bms_id;
    uint16_t len;      /* payload byte count, little-endian on the wire */
} tunnel_hdr_t;

/*
 * TABLE payload: one shared GATT blueprint for all identities (spec §5 —
 * all four units share an identical attribute layout, asserted at harvest).
 *
 *   [u8 char_count]
 *   char_count x tunnel_char_desc_t   (idx = array position, 0-based)
 *
 * idx is the characteristic's position in this array and is the same index
 * used by READ_CACHE / NOTIFY / WRITE on both sides.
 */
typedef struct __attribute__((packed)) {
    uint16_t svc_uuid;   /* 16-bit service UUID (JK uses 0xFFE0)          */
    uint16_t chr_uuid;   /* 16-bit characteristic UUID (JK uses 0xFFE1)  */
    uint8_t  props;      /* BLE_GATT_CHR_PROP_* bitmask                   */
} tunnel_char_desc_t;

#endif /* TUNNEL_PROTO_H */
