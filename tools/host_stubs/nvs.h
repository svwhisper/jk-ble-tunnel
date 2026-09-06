#ifndef HOST_NVS_H
#define HOST_NVS_H
#include <stddef.h>
#include <stdint.h>
typedef unsigned nvs_handle_t;
#define ESP_OK 0
#define NVS_READWRITE 1
int nvs_open(const char *, int, nvs_handle_t *);
int nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
int nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
int nvs_commit(nvs_handle_t);
#endif
