#ifndef HOST_QUEUE_H
#define HOST_QUEUE_H
#include "FreeRTOS.h"
typedef void *QueueHandle_t;
typedef void *QueueSetHandle_t;
typedef void *QueueSetMemberHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size);
QueueSetHandle_t xQueueCreateSet(UBaseType_t count);
BaseType_t xQueueAddToSet(QueueHandle_t queue, QueueSetHandle_t set);
QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t set, TickType_t ticks);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
#endif
