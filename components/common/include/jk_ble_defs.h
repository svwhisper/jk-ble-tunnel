/*
 * jk_ble_defs.h — JK BMS BLE constants shared by all firmwares (spec §8).
 *
 * These are the stable, publicly-known BLE facts. The byte-level *payload*
 * layout (cell/settings offsets, write-frame format, login) is NOT here — it
 * lives in the jk_proto component and is an open item (O-1/O-2) pending
 * extraction from syssi/esphome-jk-bms and bench verification.
 */
#ifndef JK_BLE_DEFS_H
#define JK_BLE_DEFS_H

#include <stdint.h>

/* GATT identifiers (16-bit). */
#define JK_SVC_UUID          0xFFE0
#define JK_CHR_UUID          0xFFE1   /* notify + write, single characteristic */

/* Command opcodes written to 0xFFE1 (spec §8). */
#define JK_CMD_CELL_INFO     0x96
#define JK_CMD_DEVICE_INFO   0x97
#define JK_CMD_LOGBOOK       0xA1

/* Frame protocol version, detected per unit at harvest (spec §8, O-1). */
typedef enum {
    JK_FRAME_UNKNOWN  = 0x00,
    JK_FRAME_JK04     = 0x01,
    JK_FRAME_JK02_24S = 0x02,
    JK_FRAME_JK02_32S = 0x03,
} jk_frame_ver_t;

/* Cross-cutting project limits. */
#define JK_MAX_UNITS         4    /* four BMS units in scope (spec §1)          */
#define JK_MAX_CELLS         32   /* largest supported string (JK02_32S)        */

#endif /* JK_BLE_DEFS_H */
