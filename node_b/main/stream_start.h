/* JK02 boundary search: startup gate and passive established-stream observer.
 * Single tunnel-task owner; no allocation, clocks, BLE calls or shared state. */
#ifndef STREAM_START_H
#define STREAM_START_H
#include <stdbool.h>
#include <stdint.h>
#define STREAM_START_RECORD_LEN 300
typedef struct {
    uint8_t data[STREAM_START_RECORD_LEN];
    uint16_t len;
} stream_start_t;
/* Consumes through the first complete checksum-valid 01/02/03 record, or all
 * input. On true, data[] holds that record; caller must reset before reuse.
 * Unknown pre-boundary bytes (including auxiliary traffic) are discarded. */
bool stream_start_push(stream_start_t *s, const uint8_t *data, uint16_t len,
                       uint16_t *consumed);
#endif
