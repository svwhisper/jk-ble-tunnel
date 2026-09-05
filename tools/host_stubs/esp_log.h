#ifndef HOST_ESP_LOG_H
#define HOST_ESP_LOG_H
#include <assert.h>
#include <stdio.h>
#define ESP_LOGW(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGI ESP_LOGW
#define ESP_LOGD ESP_LOGW
#define ESP_ERROR_CHECK(rc) assert((rc) == 0)
#endif
