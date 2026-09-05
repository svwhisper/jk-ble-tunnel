#ifndef HOST_ESP_LOG_H
#define HOST_ESP_LOG_H
/* Keep arguments type-checked without emitting firmware logs in native tests. */
#include <stdio.h>
#define ESP_LOGW(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGI ESP_LOGW
#define ESP_LOGD ESP_LOGW
#endif
