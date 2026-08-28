/*
 * nvs_store.h — durable state (spec §4 harvest persistence, §9 crash guard).
 *
 * Two things survive reboots:
 *   1. The harvested GATT blueprint + per-unit identity (name, versions), so
 *      Node B can be served from NVS with no dependency on any unit being
 *      awake after a power cycle.
 *   2. A "measurement in progress" record with the saved balance settings, so
 *      a crash mid-measurement restores the BMS instead of stranding it at the
 *      0.3 A floor (spec §9).
 */
#ifndef NVS_STORE_H
#define NVS_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "jk_proto.h"
#include "tunnel_proto.h"

#define HARVEST_MAX_CHARS 8

typedef struct {
    bool               valid;
    jk_frame_ver_t     ver;
    char               name[32];
    char               fw_version[16];
    uint8_t            char_count;
    tunnel_char_desc_t chars[HARVEST_MAX_CHARS];
} harvest_entry_t;

void nvs_store_init(void);

/* Harvest table per unit. */
bool nvs_get_harvest(uint8_t bms_id, harvest_entry_t *out);
void nvs_put_harvest(uint8_t bms_id, const harvest_entry_t *in);
void nvs_clear_harvest_all(void);   /* wipe all harv_* (stale layout reference) */

/* Measurement crash guard (spec §9). saved_settings is the raw settings frame
 * captured before altering balance current. */
typedef struct {
    bool     active;
    uint8_t  bms_id;
    uint16_t saved_len;
    uint8_t  saved[JK_SETTINGS_RAW_MAX];
} meas_record_t;

bool nvs_get_meas(meas_record_t *out);   /* true if a record is pending      */
void nvs_put_meas(const meas_record_t *in);
void nvs_clear_meas(void);

#endif /* NVS_STORE_H */
