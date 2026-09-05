#ifndef HOST_FREERTOS_H
#define HOST_FREERTOS_H
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskNO_AFFINITY (-1)
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                 uint32_t stack, void *arg, UBaseType_t priority,
                                 void *handle, BaseType_t core);
#endif
