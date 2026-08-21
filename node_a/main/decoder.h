/*
 * decoder.h — parses reassembled frames into per-BMS structs and updates the
 * state cache, then nudges MQTT to publish (spec §4 decoder task; kept off the
 * BLE callback stack by running on its own task fed by g_q_decode).
 */
#ifndef DECODER_H
#define DECODER_H
void decoder_start(void);
#endif
