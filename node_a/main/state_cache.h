/*
 * state_cache.h — per-BMS decoded telemetry + settings snapshot (spec §4).
 *
 * Single mutex guards all units; readers take a consistent snapshot by copy.
 * Decoder owns telemetry; arbiter, BLE host and supervisor update runtime
 * fields through targeted operations under the same mutex.
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

/* No whole-runtime setter: committing an old snapshot loses other writers'
 * changes. Each operation below updates only its fields under one lock. */
void state_set_app_connected(uint8_t bms_id, bool connected, int64_t now_us);
void state_set_link_state(uint8_t bms_id, tunnel_link_state_t link, bool held,
                          int64_t seen_us);
/* Only the complete checksum-valid reassembly path calls this; link-up,
 * cached decoder state, raw chunks and ATT acknowledgements are not evidence. */
void state_note_frame(uint8_t bms_id, int64_t seen_us);
/* Conditional supervisor/arbiter updates recheck current state under lock.
 * Return true only if the requested reachability update was applied. */
bool state_promote_unreachable(uint8_t bms_id);
bool state_mark_idle_if_unheld(uint8_t bms_id);
void state_clear_app_left_if(uint8_t bms_id, int64_t expected_us);
void state_get_runtime(uint8_t bms_id, bms_runtime_t *out);

/* Snapshot copy for publishers. Returns false if bms_id out of range. */
bool state_snapshot(uint8_t bms_id, bms_state_t *out);

#endif /* STATE_CACHE_H */
