/*
 * measure.h — wire-resistance measurement mode (spec §9), crash-safe.
 *
 * The saved balance settings are persisted to NVS *before* the balance floor
 * is lowered, and restored at boot or on a hard timeout, so a crash never
 * strands the BMS at 0.3 A. The write steps are gated behind JK_ENABLE_WRITES;
 * the NVS guard and boot-restore are always active and independently testable.
 */
#ifndef MEASURE_H
#define MEASURE_H

#include <stdint.h>
#include <stdbool.h>

void measure_init(void);                 /* create task */
void measure_check_boot(void);           /* restore an interrupted run (spec §9) */
void measure_start(uint8_t bms_id, const char *cid);
bool measure_active(uint8_t *bms_id_out); /* for the supervisor timeout guard */
void measure_force_restore(uint8_t bms_id); /* supervisor MEAS_TIMEOUT_MS path */

#endif /* MEASURE_H */
