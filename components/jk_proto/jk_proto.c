/*
 * jk_proto.c — see jk_proto.h for the contract and the honesty boundary.
 *
 * PORTING NOTE (read before trusting any decode value): every constant tagged
 * `VERIFY` below is a best-known layout figure. Confirm each against
 * syssi/esphome-jk-bms components/jk_bms_ble/ and a captured bench frame
 * (Test plan §14.1 "decode parity") before relying on it. Do NOT flip
 * JK_ENABLE_WRITES on until the write-frame format and login handshake are
 * ported and bench-checked (O-2).
 */
#include <string.h>
#include "jk_proto.h"

/* JK02 response start magic. VERIFY. */
static const uint8_t JK02_MAGIC[4] = {0x55, 0xAA, 0xEB, 0x90};
/* JK02 command frame start magic (host->BMS). VERIFY. */
static const uint8_t JK02_CMD_MAGIC[4] = {0xAA, 0x55, 0x90, 0xEB};

/* Fixed record length for JK02 variants. VERIFY (reference uses ~300). */
#define JK02_RECORD_LEN 300

/* --- Decode offsets (JK02_32S). ALL VERIFY. These are the single most
 *     important thing to confirm on the bench; a wrong offset silently
 *     mis-reads a protection device. ------------------------------------- */
#define OFF_RECORD_TYPE     4    /* record type byte                          */
#define OFF_CELL_V_BASE     6    /* first cell voltage, u16 LE mV, 2B stride   */
#define OFF_CELL_R_BASE     80   /* first cell wire resistance, u16 LE mOhm    */
#define OFF_WIRE_WARN       114  /* per-wire warning bitmask (spec §8/§9)      */
#define OFF_PACK_V          118  /* pack voltage, u32 LE, 1mV                  */
#define OFF_PACK_I          126  /* pack current, s32 LE, 1mA (+ = charge)     */
#define OFF_SOC             141  /* state of charge, u8 %                      */
#define OFF_ERROR_MASK      134  /* main error bitmask, u32 LE                 */
#define OFF_CYCLE_COUNT     150  /* cycle count, u16 LE                        */

/* ---- little-endian readers --------------------------------------------- */
static inline uint16_t rd_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t rd_s32(const uint8_t *p) { return (int32_t)rd_u32(p); }

/* ---- checksum ----------------------------------------------------------- */
/* VERIFY: reference CRC is the 8-bit sum of all bytes except the last, which
 * holds the checksum. Confirm the exact span. */
bool jk_checksum_ok(const uint8_t *frame, uint16_t len)
{
    if (len < 5) return false;
    uint8_t sum = 0;
    for (uint16_t i = 0; i < len - 1; i++) sum += frame[i];
    return sum == frame[len - 1];
}

/* ---- reassembly --------------------------------------------------------- */
void jk_reasm_init(jk_reasm_t *r, jk_frame_ver_t ver)
{
    memset(r, 0, sizeof(*r));
    r->ver = ver;
}
void jk_reasm_reset(jk_reasm_t *r) { r->have = 0; r->want = 0; }

/* How long a record is, once we recognise the start. VERIFY per variant. */
static uint16_t frame_target_len(jk_frame_ver_t ver)
{
    (void)ver;
    /* JK04 has a different magic/length; add its case when O-1 pins the
     * fleet's variants. For JK02_* the record is fixed. */
    return JK02_RECORD_LEN;
}

static bool starts_with_magic(const uint8_t *p, uint16_t n)
{
    return n >= 4 && memcmp(p, JK02_MAGIC, 4) == 0;
}

const uint8_t *jk_reasm_push(jk_reasm_t *r, const uint8_t *data, size_t len,
                             uint16_t *out_len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        /* Hunt for the 4-byte start magic while unlocked. */
        if (r->want == 0) {
            uint8_t expect = JK02_MAGIC[r->have];
            if (b == expect) {
                r->buf[r->have++] = b;
                if (r->have == 4) r->want = frame_target_len(r->ver);
            } else {
                /* Resync: keep the longest magic prefix that still matches. */
                r->have = (b == JK02_MAGIC[0]) ? 1 : 0;
                if (r->have) r->buf[0] = b;
            }
            continue;
        }

        /* Locked: accumulate to target length. */
        if (r->have < JK_FRAME_MAX) r->buf[r->have] = b;
        r->have++;

        if (r->have >= r->want) {
            uint16_t flen = r->want;
            jk_reasm_reset(r);
            if (flen <= JK_FRAME_MAX && jk_checksum_ok(r->buf, flen)) {
                if (out_len) *out_len = flen;
                return r->buf;
            }
            /* Bad checksum: drop and keep scanning subsequent bytes. */
        }
    }
    return NULL;
}

/* ---- identification ----------------------------------------------------- */
jk_record_t jk_frame_record(const uint8_t *frame, uint16_t len)
{
    if (!starts_with_magic(frame, len) || len <= OFF_RECORD_TYPE)
        return JK_REC_NONE;
    switch (frame[OFF_RECORD_TYPE]) {
        case JK_REC_DEVICE_INFO: return JK_REC_DEVICE_INFO;
        case JK_REC_CELL_INFO:   return JK_REC_CELL_INFO;
        case JK_REC_SETTINGS:    return JK_REC_SETTINGS;
        default:                 return JK_REC_NONE;
    }
}

jk_frame_ver_t jk_detect_version(const uint8_t *frame, uint16_t len)
{
    /* VERIFY: the reference distinguishes JK04 vs JK02_24S vs JK02_32S from
     * device-info fields. Until O-1 pins the fleet, assume 32S for JK02 magic
     * and flag anything else as unknown for the operator to resolve. */
    if (starts_with_magic(frame, len)) return JK_FRAME_JK02_32S;
    return JK_FRAME_UNKNOWN;
}

/* ---- decode: cell info -------------------------------------------------- */
int jk_decode_cell_info(jk_frame_ver_t ver, const uint8_t *frame, uint16_t len,
                        jk_cell_info_t *out)
{
    if (jk_frame_record(frame, len) != JK_REC_CELL_INFO) return -1;
    if (len < JK02_RECORD_LEN) return -1;
    memset(out, 0, sizeof(*out));
    out->ver = ver;

    /* Cell count differs by variant; VERIFY. 32S carries up to 32. */
    uint8_t n = (ver == JK_FRAME_JK02_24S) ? 24 : 32;
    if (n > JK_MAX_CELLS) n = JK_MAX_CELLS;
    out->cell_count = n;

    for (uint8_t i = 0; i < n; i++) {
        out->cells[i].n  = i + 1;
        out->cells[i].mv = rd_u16(&frame[OFF_CELL_V_BASE + i * 2]);
        uint16_t raw_r   = rd_u16(&frame[OFF_CELL_R_BASE + i * 2]);
        /* Spec §9: raw 0 == measurement failed -> store as null (-1). */
        out->cells[i].r_mohm = (raw_r == 0) ? -1 : (int32_t)raw_r;
    }

    out->wire_warn_bitmask = rd_u32(&frame[OFF_WIRE_WARN]);
    out->error_bitmask     = rd_u32(&frame[OFF_ERROR_MASK]);
    out->pack_mv           = (int32_t)rd_u32(&frame[OFF_PACK_V]);
    out->current_ma        = rd_s32(&frame[OFF_PACK_I]);
    out->soc_pct           = frame[OFF_SOC];
    out->cycle_count       = rd_u16(&frame[OFF_CYCLE_COUNT]);
    out->power_mw          = (int32_t)(((int64_t)out->pack_mv * out->current_ma) / 1000);
    out->charging          = out->current_ma > 0;
    out->discharging       = out->current_ma < 0;
    /* balancing bool + temps: VERIFY offsets, wire in once bench-confirmed. */
    return 0;
}

/* ---- decode: settings --------------------------------------------------- */
int jk_decode_settings(jk_frame_ver_t ver, const uint8_t *frame, uint16_t len,
                       jk_settings_t *out)
{
    if (jk_frame_record(frame, len) != JK_REC_SETTINGS) return -1;
    memset(out, 0, sizeof(*out));
    out->ver = ver;
    out->raw_len = (len < JK_SETTINGS_RAW_MAX) ? len : JK_SETTINGS_RAW_MAX;
    memcpy(out->raw, frame, out->raw_len);
    /*
     * VERIFY (O-2): port balance_trigger_voltage, balance current limit and
     * the balancing-enabled bit offsets from the reference settings parser.
     * Left unpopulated until then so nothing downstream trusts a guessed
     * number. raw[] is captured so readback-compare (spec §10.5) still works
     * byte-for-byte even before named fields are decoded.
     */
    return 0;
}

/* ---- decode: device info ------------------------------------------------ */
int jk_decode_device_info(const uint8_t *frame, uint16_t len,
                          jk_device_info_t *out)
{
    if (jk_frame_record(frame, len) != JK_REC_DEVICE_INFO) return -1;
    memset(out, 0, sizeof(*out));
    out->ver = jk_detect_version(frame, len);
    /* VERIFY: device name / fw / hw string offsets from reference. */
    return 0;
}

/* ---- command builders --------------------------------------------------- */
int jk_build_read_cmd(uint8_t opcode, uint8_t *out, size_t out_cap)
{
    if (out_cap < JK_CMD_FRAME_LEN) return -1;
    memset(out, 0, JK_CMD_FRAME_LEN);
    memcpy(out, JK02_CMD_MAGIC, 4);
    out[4] = opcode;             /* JK_CMD_CELL_INFO / JK_CMD_DEVICE_INFO      */
    out[5] = 0x00;               /* length/value fields zero for a plain read  */
    /* VERIFY: reference sets bytes 6..18 (value + reserved) then a trailing
     * checksum at [19]. A wrong read command is harmless (BMS ignores it). */
    uint8_t sum = 0;
    for (int i = 0; i < JK_CMD_FRAME_LEN - 1; i++) sum += out[i];
    out[JK_CMD_FRAME_LEN - 1] = sum;
    return JK_CMD_FRAME_LEN;
}

int jk_build_balance_write(jk_frame_ver_t ver, const char *key, double value,
                           uint8_t *out, size_t out_cap)
{
    (void)ver; (void)key; (void)value; (void)out; (void)out_cap;
#if JK_ENABLE_WRITES
    /*
     * PORT ME (O-2): build the settings-write frame for `key` using the
     * reference write-frame opcode/format. Enforce the §10 whitelist upstream
     * in the arbiter; this builder only serialises an already-validated write.
     */
#error "JK_ENABLE_WRITES=1 but jk_build_balance_write is not ported yet (O-2)."
#else
    return -1; /* ENOTSUP: writes disabled until bench-verified. */
#endif
}

int jk_build_login(const char *pin, uint8_t *out, size_t out_cap)
{
    (void)pin; (void)out; (void)out_cap;
#if JK_ENABLE_WRITES
#error "JK_ENABLE_WRITES=1 but jk_build_login is not ported yet (O-5)."
#else
    return -1;
#endif
}
