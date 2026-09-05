#ifndef COMMAND_VALIDATION_H
#define COMMAND_VALIDATION_H

#include <stdbool.h>
#include "cJSON.h"

/* Pure validation; root owns *setting. No queues, I/O or settings writes.
 * Return NULL on success, otherwise an MQTT acknowledgement status. Range
 * policy remains in config.h and is applied by the arbiter after this check. */
const char *balance_command_validate(const cJSON *root,
                                    const cJSON **setting, double *value);

/* MQTT commands must fit in one complete event; never execute a truncated or
 * retained command. Retained state remains supported on the publish side. */
bool command_event_valid(bool retained, int offset, int total_len,
                         int data_len, int topic_len);

#endif
