/*
 * jk_proto.h — JK BMS frame logic, single source of truth for both nodes.
 *
 *   Spec §8, §13: "Two firmware images sharing a common jk_proto component
 *   ported from syssi/esphome-jk-bms — single source of truth for frame logic."
 *
 * WHAT IS REAL HERE:
 *   - Frame reassembly (chunk accumulation by start-magic + fixed length).
 *   - Checksum validation.
 *   - The read-request command builder (poll cell/device info).
 *
 * WHAT IS DELIBERATELY NOT FINISHED (open items O-1/O-2, spec §8):
 *   - Exact decode offsets. The constants in jk_proto.c marked `VERIFY` are
 *     best-known values that MUST be confirmed against the reference component
 *     and a bench unit before they are trusted.
 *   - The balance-parameter WRITE builder and the login/auth handshake. These
 *     are compile-gated behind JK_ENABLE_WRITES (default 0) and return -1 until
 *     ported. Guessing register layouts on a 60 kWh protection device is a
 *     safety violation — do not enable until bench-verified.
 */
#ifndef JK_PROTO_H
#define JK_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "jk_ble_defs.h"

/* Master gate for anything that writes device state. Leave 0 until O-2 done. */
#ifndef JK_ENABLE_WRITES
#define JK_ENABLE_WRITES 0
#endif

#define JK_FRAME_MAX        320   /* largest reassembled record (VERIFY: ~300) */
#define JK_SETTINGS_RAW_MAX 320
#define JK_CMD_FRAME_LEN    20    /* command frame written to 0xFFE1 (VERIFY)  */

/* Record type byte (5th byte of a JK02 response). VERIFY against reference. */
typedef enum {
    JK_REC_DEVICE_INFO = 0x03,
    JK_REC_CELL_INFO   = 0x02,
    JK_REC_SETTINGS    = 0x01,
    JK_REC_NONE        = 0xFF,
} jk_record_t;

/* ---- Decoded structures ------------------------------------------------- */

typedef struct {
    int      n;         /* cell number, 1-based                               */
    uint16_t mv;        /* voltage, millivolts (0 => not populated)           */
    int32_t  r_mohm;    /* wire resistance, milliohm. <0 => NULL/failed.       */
                        /* Spec §9: raw 0 means "measurement failed", stored   */
                        /* as null (-1 here), never as a low resistance.       */
} jk_cell_t;

typedef struct {
    jk_frame_ver_t ver;
    uint8_t   cell_count;
    jk_cell_t cells[JK_MAX_CELLS];
    int32_t   pack_mv;
    int32_t   current_ma;        /* signed; + = charge                        */
    int32_t   power_mw;
    uint8_t   soc_pct;
    int16_t   temp_dc[4];        /* deci-°C: [0]=T1 [1]=T2 [2]=power tube      */
    uint8_t   temp_count;
    uint32_t  cycle_count;       /* u32 @182 (bench-verified)                  */
    bool      balancing;
    bool      charging;
    bool      discharging;
    uint32_t  error_bitmask;     /* main error mask; bit0 = wire resistance    */
    uint32_t  wire_warn_bitmask; /* per-wire warnings @ offset 114 (spec §8/9) */
} jk_cell_info_t;

typedef struct {
    jk_frame_ver_t ver;
    float    balance_trigger_v;  /* balance_trigger_voltage (writable)         */
    float    balance_current_a;  /* balance current limit                      */
    bool     balancing_enabled;
    uint8_t  raw[JK_SETTINGS_RAW_MAX]; /* retained verbatim for readback diff  */
    uint16_t raw_len;
} jk_settings_t;

typedef struct {
    jk_frame_ver_t ver;
    char name[32];
    char fw_version[16];
    char hw_version[16];
} jk_device_info_t;

/* ---- Reassembly --------------------------------------------------------- */
/*
 * Notifications arrive as chunks (spec §8: frames span multiple notifications).
 * Feed every inbound chunk to jk_reasm_push; it returns a pointer to a complete,
 * checksum-valid frame exactly once per frame, else NULL.
 */
typedef struct {
    uint8_t  buf[JK_FRAME_MAX];
    uint16_t have;          /* bytes accumulated                              */
    uint16_t want;          /* target frame length once locked, else 0        */
    jk_frame_ver_t ver;     /* variant this reassembler is tuned to           */
} jk_reasm_t;

void jk_reasm_init(jk_reasm_t *r, jk_frame_ver_t ver);
void jk_reasm_reset(jk_reasm_t *r);

/*
 * Returns a completed frame (r->buf, length in *out_len) or NULL if more data
 * is needed. On return of a frame, the reassembler auto-resets for the next.
 */
const uint8_t *jk_reasm_push(jk_reasm_t *r, const uint8_t *data, size_t len,
                             uint16_t *out_len);

/* ---- Frame identification + decode -------------------------------------- */

jk_record_t jk_frame_record(const uint8_t *frame, uint16_t len);
bool jk_checksum_ok(const uint8_t *frame, uint16_t len);

/* Detect variant from a device-info frame (harvest, O-1). */
jk_frame_ver_t jk_detect_version(const uint8_t *frame, uint16_t len);

/* Each returns 0 on success, <0 on malformed/unsupported frame. */
int jk_decode_cell_info(jk_frame_ver_t ver, const uint8_t *frame, uint16_t len,
                        jk_cell_info_t *out);
int jk_decode_settings(jk_frame_ver_t ver, const uint8_t *frame, uint16_t len,
                       jk_settings_t *out);
int jk_decode_device_info(const uint8_t *frame, uint16_t len,
                          jk_device_info_t *out);

/* ---- Command builders --------------------------------------------------- */

/*
 * Build a poll/read request (safe: a wrong format just gets ignored by the
 * BMS, it changes no state). `opcode` is JK_CMD_CELL_INFO / JK_CMD_DEVICE_INFO.
 * Writes JK_CMD_FRAME_LEN bytes to out (must be >= JK_CMD_FRAME_LEN).
 * Returns bytes written, or <0 on error.
 */
int jk_build_read_cmd(uint8_t opcode, uint8_t *out, size_t out_cap);

/*
 * GATED write path. Returns -1 (ENOTSUP) unless JK_ENABLE_WRITES==1 AND the
 * offsets/format have been ported (O-2). `key` is a §10 whitelist name.
 */
int jk_build_balance_write(jk_frame_ver_t ver, const char *key, double value,
                           uint8_t *out, size_t out_cap);
int jk_build_login(const char *pin, uint8_t *out, size_t out_cap);

#endif /* JK_PROTO_H */
