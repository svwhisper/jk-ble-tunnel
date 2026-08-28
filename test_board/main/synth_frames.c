#include <string.h>
#include "synth_frames.h"

/* Must match jk_proto.c. BENCH-VERIFIED 2026-08-28 against real
 * JK-PB2A16S20P (fw 19.31) captures — JK02_32S layout. */
#define REC_LEN         300
#define OFF_TYPE        4
#define OFF_CELL_V      6
#define OFF_CELL_MASK   70
#define OFF_CELL_AVG    74
#define OFF_CELL_DELTA  76
#define OFF_CELL_R      80
#define OFF_MOS_TEMP    144
#define OFF_WIRE_WARN   146
#define OFF_PACK_V      150
#define OFF_PACK_P      154
#define OFF_PACK_I      158
#define OFF_TEMP1       162
#define OFF_TEMP2       164
#define OFF_ERR         166   /* u16 */
#define OFF_BAL_ACTION  170
#define OFF_SOC         173
#define OFF_CYCLES      182   /* u32 */

static const uint8_t MAGIC[4] = {0x55, 0xAA, 0xEB, 0x90};

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v)
{ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }

static void finalize(uint8_t *out, uint8_t type)
{
    memcpy(out, MAGIC, 4);
    out[OFF_TYPE] = type;
    uint8_t sum = 0;
    for (int i = 0; i < REC_LEN - 1; i++) sum += out[i];
    out[REC_LEN - 1] = sum;
}

int synth_cell_info(uint8_t *out, size_t cap, uint32_t seq)
{
    if (cap < REC_LEN) return -1;
    memset(out, 0, REC_LEN);

    /* 16 cells around 3.30 V, cell 9 nudged so imbalance work has a signal. */
    for (int i = 0; i < 16; i++) {
        uint16_t mv = 3300 + (i * 3) + (seq % 5);
        if (i == 8) mv -= 12;                 /* mimic the bank-0 cell-9 lag   */
        put_u16(&out[OFF_CELL_V + i * 2], mv);
        /* Wire resistance ~0.15 Ohm => 150 mOhm; cell 3 reads 0 (failed). */
        uint16_t r = (i == 2) ? 0 : (140 + i);
        put_u16(&out[OFF_CELL_R + i * 2], r);
    }
    put_u32(&out[OFF_CELL_MASK], 0x0000FFFFu);                  /* 16S        */
    put_u16(&out[OFF_CELL_AVG], 3305);
    put_u16(&out[OFF_CELL_DELTA], 12);
    put_u32(&out[OFF_WIRE_WARN], (seq & 1) ? 0x00000004u : 0);  /* cell3 warn */
    put_u32(&out[OFF_PACK_V], 52800 + (seq % 20));              /* mV         */
    put_u32(&out[OFF_PACK_P], 79000);                            /* ~1.5A*52.8 */
    put_u32(&out[OFF_PACK_I], (uint32_t)(int32_t)(1500));        /* +1.5 A     */
    put_u16(&out[OFF_TEMP1], 160);                               /* 16.0 C     */
    put_u16(&out[OFF_TEMP2], 164);
    put_u16(&out[OFF_MOS_TEMP], 172);
    put_u16(&out[OFF_ERR], 0);
    out[OFF_BAL_ACTION] = 0;
    out[OFF_SOC] = 74;
    put_u32(&out[OFF_CYCLES], 128);
    finalize(out, 0x02);
    return REC_LEN;
}

int synth_settings(uint8_t *out, size_t cap)
{
    if (cap < REC_LEN) return -1;
    memset(out, 0, REC_LEN);
    /* Named fields at the verified offsets (match jk_decode_settings). */
    put_u32(&out[26],  10);      /* balance trigger voltage, mV (0.010 V)      */
    put_u32(&out[78],  2000);    /* max balance current, mA (2.000 A)          */
    put_u32(&out[126], 1);       /* balancer enabled                           */
    put_u32(&out[114], 16);      /* cell count                                 */
    finalize(out, 0x01);
    return REC_LEN;
}

int synth_device_info(uint8_t *out, size_t cap, const char *name)
{
    if (cap < REC_LEN) return -1;
    memset(out, 0, REC_LEN);
    /* Verified string placement: model @6, hw @22, sw @30, device name @46. */
    memcpy(&out[6],  "JK-PB2A16S20P", 13);
    memcpy(&out[22], "19A", 3);
    memcpy(&out[30], "19.31", 5);
    if (name) strlcpy((char *)&out[46], name, 16);
    finalize(out, 0x03);
    return REC_LEN;
}
