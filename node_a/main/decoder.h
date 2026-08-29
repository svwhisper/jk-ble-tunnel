/*
 * decoder.h — parses reassembled frames into per-BMS structs and updates the
 * state cache, then nudges MQTT to publish (spec §4 decoder task; kept off the
 * BLE callback stack by running on its own task fed by g_q_decode).
 */
#ifndef DECODER_H
#define DECODER_H
#include <stdint.h>
void decoder_start(void);
/* New BLE session for this unit (supervisor calls it at every link-up): allows
 * the decoder to send the one-shot 0x6C stream-enable again when the session's
 * first cell frame lands. */
void decoder_session_reset(uint8_t bms_id);
/* One-shot per session: the app-style FFE2 opener trilogy (97/96/6C with a
 * fresh RTC). Fired by the decoder on the first cell frame, and by the
 * supervisor a few seconds into a SILENT link-up (unit 0's module never
 * volunteers a frame, so the frame-gated trigger alone deadlocks there).
 * No-op until SNTP has real time or if already sent this session. */
void decoder_send_opener(uint8_t bms_id);
#endif
