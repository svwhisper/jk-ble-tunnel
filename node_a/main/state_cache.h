/*
 * state_cache.h — per-BMS decoded telemetry + settings snapshot (spec §4).
 *
 * Single mutex guards all units; readers take a consistent snapshot by copy.
 * Writers are the decoder task. Cheap because updates are ~1 Hz per unit.
 */
#ifndef STATE_CACHE_H
#define STATE_CACHE_H

#include "jk_proto.h"
#include "na_types.h"

typedef struct {
    bool            have_cells;
    bool            have_settings;
    jk_cell_info_t  cells;
    jk_settings_t   settings;
    bms_runtime_t   rt;      /* reachability/app/link bookkeeping */
} bms_state_t;

void state_cache_init(void);

/* Decoder writes. */
void state_set_cells(uint8_t bms_id, const jk_cell_info_t *ci);
void state_set_settings(uint8_t bms_id, const jk_settings_t *s);

/* Runtime bookkeeping (arbiter/supervisor). */
void state_set_runtime(uint8_t bms_id, const bms_runtime_t *rt);
void state_get_runtime(uint8_t bms_id, bms_runtime_t *out);

/* Snapshot copy for publishers. Returns false if bms_id out of range. */
bool state_snapshot(uint8_t bms_id, bms_state_t *out);

#endif /* STATE_CACHE_H */
