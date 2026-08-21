#include <string.h>
#include "synth_frames.h"

/* Must match jk_proto.c (VERIFY constants) until real captures replace both. */
#define REC_LEN         300
#define OFF_TYPE        4
#define OFF_CELL_V      6
#define OFF_CELL_R      80
#define OFF_WIRE_WARN   114
#define OFF_PACK_V      118
#define OFF_PACK_I      126
#define OFF_ERR         134
#define OFF_SOC         141
#define OFF_CYCLES      150

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
    put_u32(&out[OFF_WIRE_WARN], (seq & 1) ? 0x00000004u : 0);  /* cell3 warn */
    put_u32(&out[OFF_PACK_V], 52800 + (seq % 20));              /* mV         */
    put_u32(&out[OFF_PACK_I], (uint32_t)(int32_t)(1500));        /* +1.5 A     */
    put_u32(&out[OFF_ERR], 0);
    out[OFF_SOC] = 74;
    put_u16(&out[OFF_CYCLES], 128);
    finalize(out, 0x02);
    return REC_LEN;
}

int synth_settings(uint8_t *out, size_t cap)
{
    if (cap < REC_LEN) return -1;
    memset(out, 0, REC_LEN);
    /* Placeholder settings payload; named-field offsets are O-2. */
    finalize(out, 0x01);
    return REC_LEN;
}

int synth_device_info(uint8_t *out, size_t cap, const char *name)
{
    if (cap < REC_LEN) return -1;
    memset(out, 0, REC_LEN);
    if (name) strlcpy((char *)&out[6], name, 16);   /* rough placement (O-1) */
    finalize(out, 0x03);
    return REC_LEN;
}
