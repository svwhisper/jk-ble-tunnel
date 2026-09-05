#ifndef HOST_SEMPHR_H
#define HOST_SEMPHR_H
/* Test adapter: production state_cache.c uses its real locking boundaries. */
#include <assert.h>
#include <pthread.h>
#include "FreeRTOS.h"
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    return &mutex;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks)
{
    (void)ticks;
    int rc = pthread_mutex_lock(mutex);
    assert(rc == 0);
    return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    int rc = pthread_mutex_unlock(mutex);
    assert(rc == 0);
    return pdTRUE;
}
#endif
