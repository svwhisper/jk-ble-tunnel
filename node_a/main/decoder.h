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
#endif
