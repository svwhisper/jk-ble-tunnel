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

/* --- Decode offsets (JK02_32S layout, 300-byte record).
 *     BENCH-VERIFIED 2026-08-28 against live frames captured from a real
 *     JK-PB2A16S20P (fw 19.31, "BMS 1-01") via Node A rawcap: every field
 *     below was cross-checked against physical reality (16 cells @ ~3.345 V,
 *     pack 53.527 V, -133 mA idle, 16.0/16.4 C garage temps, SOC 98 %,
 *     314 Ah nominal = the EVE MB31 fleet). Matches syssi/esphome-jk-bms
 *     JK02_32S. 16S units populate cells 1-16 and OFF_CELL_MASK=0x0000FFFF. */
#define OFF_RECORD_TYPE     4    /* record type byte                          */
#define OFF_CELL_V_BASE     6    /* 32 x u16 LE cell mV                        */
#define OFF_CELL_MASK       70   /* enabled-cell bitmask, u32 LE               */
#define OFF_CELL_AVG        74   /* average cell mV, u16 LE                    */
#define OFF_CELL_DELTA      76   /* max-min delta mV, u16 LE                   */
#define OFF_CELL_R_BASE     80   /* 32 x u16 LE wire resistance, mOhm          */
#define OFF_MOS_TEMP        144  /* power-tube temp, s16 LE, 0.1 C             */
#define OFF_WIRE_WARN       146  /* per-wire warning bitmask, u32 LE           */
#define OFF_PACK_V          150  /* pack voltage, u32 LE, 1 mV                 */
#define OFF_PACK_P          154  /* pack power, u32 LE, 1 mW (unsigned)        */
#define OFF_PACK_I          158  /* pack current, s32 LE, 1 mA (+ = charge)    */
#define OFF_TEMP1           162  /* T1, s16 LE, 0.1 C                          */
#define OFF_TEMP2           164  /* T2, s16 LE, 0.1 C                          */
#define OFF_ERROR_MASK      166  /* error bitmask, u16 LE                      */
#define OFF_BAL_CURRENT     168  /* balance current, s16 LE, 1 mA              */
#define OFF_BAL_ACTION      170  /* 0 = off, 1 = charge-bal, 2 = discharge-bal */
#define OFF_SOC             173  /* state of charge, u8 %                      */
#define OFF_CAP_REMAIN      174  /* remaining capacity, u32 LE, mAh            */
#define OFF_CAP_NOMINAL     178  /* nominal capacity, u32 LE, mAh              */
#define OFF_CYCLE_COUNT     182  /* cycle count, u32 LE                        */

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

    /* Cell count comes from the enabled-cell bitmask (0x0000FFFF on the 16S
     * fleet), so 17-32 are never emitted as phantom cells. */
    uint32_t mask = rd_u32(&frame[OFF_CELL_MASK]);
    uint8_t k = 0;
    for (uint8_t i = 0; i < 32 && k < JK_MAX_CELLS; i++) {
        if (!(mask & (1u << i))) continue;
        out->cells[k].n  = i + 1;
        out->cells[k].mv = rd_u16(&frame[OFF_CELL_V_BASE + i * 2]);
        uint16_t raw_r   = rd_u16(&frame[OFF_CELL_R_BASE + i * 2]);
        /* Spec §9: raw 0 == measurement failed -> store as null (-1). */
        out->cells[k].r_mohm = (raw_r == 0) ? -1 : (int32_t)raw_r;
        k++;
    }
    out->cell_count = k;

    out->wire_warn_bitmask = rd_u32(&frame[OFF_WIRE_WARN]);
    out->error_bitmask     = rd_u16(&frame[OFF_ERROR_MASK]);
    out->pack_mv           = (int32_t)rd_u32(&frame[OFF_PACK_V]);
    out->current_ma        = rd_s32(&frame[OFF_PACK_I]);
    out->soc_pct           = frame[OFF_SOC];
    out->cycle_count       = rd_u32(&frame[OFF_CYCLE_COUNT]);
    out->power_mw          = (int32_t)(((int64_t)out->pack_mv * out->current_ma) / 1000);
    out->charging          = out->current_ma > 0;
    out->discharging       = out->current_ma < 0;
    out->balancing         = frame[OFF_BAL_ACTION] != 0;
    out->temp_dc[0] = (int16_t)rd_u16(&frame[OFF_TEMP1]);      /* 0.1 C */
    out->temp_dc[1] = (int16_t)rd_u16(&frame[OFF_TEMP2]);
    out->temp_dc[2] = (int16_t)rd_u16(&frame[OFF_MOS_TEMP]);   /* power tube */
    out->temp_count = 3;
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
    /* BENCH-VERIFIED 2026-08-28 from a live 0x01 frame (values matched the
     * unit's real config: trigger 0.010 V, max balance 2.000 A, balancer on,
     * UVP 2.500/OVP 3.650, cell count 16 @ offset 114). Layout = esphome
     * JK02_32S settings record. */
    out->balance_trigger_v = rd_u32(&frame[26]) / 1000.0f;
    out->balance_current_a = rd_u32(&frame[78]) / 1000.0f;
    out->balancing_enabled = rd_u32(&frame[126]) != 0;
    return 0;
}

/* ---- decode: device info ------------------------------------------------ */
int jk_decode_device_info(const uint8_t *frame, uint16_t len,
                          jk_device_info_t *out)
{
    if (jk_frame_record(frame, len) != JK_REC_DEVICE_INFO) return -1;
    if (len < 62) return -1;
    memset(out, 0, sizeof(*out));
    out->ver = jk_detect_version(frame, len);
    /* BENCH-VERIFIED 2026-08-28 from a live 0x03 frame: vendor/model @6
     * ("JK-PB2A16S20P"), hw @22, sw @30 ("19.31"), device name @46 ("BMS 1"). */
    memcpy(out->hw_version, &frame[22], 8);  out->hw_version[8]  = 0;
    memcpy(out->fw_version, &frame[30], 8);  out->fw_version[8]  = 0;
    memcpy(out->name,       &frame[46], 16); out->name[16]       = 0;
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
    /* BENCH OBSERVATION 2026-08-28 (fw 19.31): the official app's 0x97 fills
     * bytes 6..18 with what looks like a random nonce (two captured samples
     * differed, both accepted), and the cell-info (0x02) stream only started
     * after such a 0x97 — our zero-filled 0x97 got a 0x03 reply but no stream.
     * Mimic the app: fill 6..18 with pseudo-random bytes for 0x97. Harmless
     * either way (it is a read command). xorshift, not esp_random, so this
     * still builds for the native host test. */
    if (opcode == JK_CMD_DEVICE_INFO) {
        static uint32_t s = 0x9E3779B9u;
        for (int i = 6; i <= 18; i++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            out[i] = (uint8_t)s;
        }
    }
    uint8_t sum = 0;
    for (int i = 0; i < JK_CMD_FRAME_LEN - 1; i++) sum += out[i];
    out[JK_CMD_FRAME_LEN - 1] = sum;
    return JK_CMD_FRAME_LEN;
}

/* Settings-register map — DECODED from the official app's own write frames,
 * captured through the transparent clones on a live JK-PB2A16S20P (fw 19.31),
 * 2026-08-30. Each write is the universal 20-byte register frame:
 *   AA 55 90 EB | reg | 0x04 | u32 LE value | 8 don't-care bytes | 8-bit sum
 * (same shape as the 0x6C set-RTC), sent to FFE2. The `scale` turns the
 * whitelist's human unit (V, A, bool) into the register's integer unit. */
typedef struct { const char *key; uint8_t reg; double scale; } jk_setting_reg_t;
static const jk_setting_reg_t JK_SETTING_REGS[] = {
    { "balance_trigger_voltage", 0x06, 1000.0 },  /* millivolts (a DELTA, not absolute) */
    { "balance_current",         0x13, 1000.0 },  /* milliamps                          */
    { "balancing_enabled",       0x1F,    1.0 },  /* 0/1                                 */
};
#define JK_SETTING_REGS_N (sizeof(JK_SETTING_REGS)/sizeof(JK_SETTING_REGS[0]))

int jk_build_balance_write(jk_frame_ver_t ver, const char *key, double value,
                           uint8_t *out, size_t out_cap)
{
    (void)ver;   /* register map is identical across the fleet's frame versions */
#if JK_ENABLE_WRITES
    if (out_cap < JK_CMD_FRAME_LEN) return -1;
    const jk_setting_reg_t *r = NULL;
    for (size_t i = 0; i < JK_SETTING_REGS_N; i++)
        if (strcmp(JK_SETTING_REGS[i].key, key) == 0) { r = &JK_SETTING_REGS[i]; break; }
    if (!r) return -1;   /* not a known register — the §10 whitelist gates this too */

    /* Scale to the register's integer unit; round to nearest, reject negatives
     * and anything that would overflow u32 (the arbiter range-clamps first, so
     * this is defense in depth). */
    double scaled = value * r->scale;
    if (scaled < 0.0 || scaled > 4294967295.0) return -1;
    uint32_t iv = (uint32_t)(scaled + 0.5);

    memset(out, 0, JK_CMD_FRAME_LEN);
    memcpy(out, JK02_CMD_MAGIC, 4);
    out[4] = r->reg;
    out[5] = 0x04;                       /* value length, per the app's frames  */
    out[6] = (uint8_t)iv;        out[7] = (uint8_t)(iv >> 8);
    out[8] = (uint8_t)(iv >> 16); out[9] = (uint8_t)(iv >> 24);
    /* bytes 10..18 left zero (the app sends recycled buffer junk; zero works). */
    uint8_t sum = 0;
    for (int i = 0; i < JK_CMD_FRAME_LEN - 1; i++) sum += out[i];
    out[JK_CMD_FRAME_LEN - 1] = sum;
    return JK_CMD_FRAME_LEN;
#else
    (void)key; (void)value; (void)out; (void)out_cap;
    return -1; /* ENOTSUP: writes disabled until bench-verified. */
#endif
}

int jk_build_login(const char *pin, uint8_t *out, size_t out_cap)
{
    (void)pin; (void)out; (void)out_cap;
    /* No-op by design: captures of the app's full settings-write sessions on
     * fw 19.31 show NO login/auth frame on the wire — the PIN never appears
     * (the app's password prompt is a local UI gate). Kept as a stub so the
     * arbiter's optional login step compiles; returns "nothing to send". */
    return 0;
}
