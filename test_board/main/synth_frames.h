/*
 * synth_frames.h — synthetic JK frames for the bench (no real BMS available).
 *
 * Produces checksum-valid JK02 cell-info / settings / device-info records with
 * plausible values so Node A's decode + MQTT paths can be exercised, and so the
 * app-emulator sees a realistic notification stream from Node B.
 *
 * The byte offsets here are kept in lockstep with jk_proto.c's `VERIFY`
 * constants ON PURPOSE: replace both together once real captures land (O-1).
 */
#ifndef SYNTH_FRAMES_H
#define SYNTH_FRAMES_H

#include <stdint.h>
#include <stddef.h>

/* Fill `out` (>= 300 bytes) with a cell-info record. `seq` varies the values a
 * little each call so the bench sees movement. Returns frame length. */
int synth_cell_info(uint8_t *out, size_t cap, uint32_t seq);
int synth_settings(uint8_t *out, size_t cap);
int synth_device_info(uint8_t *out, size_t cap, const char *name);

#endif /* SYNTH_FRAMES_H */
