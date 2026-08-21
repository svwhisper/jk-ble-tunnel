/*
 * nb_state.h — Node B shared state: the single replica blueprint, the four
 * per-identity read caches, per-identity link + advertising + connection
 * status. One mutex guards it; BLE callbacks and the tunnel task both touch it.
 */
#ifndef NB_STATE_H
#define NB_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "tunnel_proto.h"
#include "jk_ble_defs.h"

#define NB_MAX_CHARS   8
#define NB_CACHE_MAX   320   /* per (identity,idx) cached read value */

typedef struct {
    uint16_t len;
    uint8_t  data[NB_CACHE_MAX];
} nb_cache_t;

typedef struct {
    /* identity */
    char     name[32];
    uint8_t  addr[6];         /* static-random address for this adv set */
    bool     have_name;

    /* status driven by A over the tunnel */
    tunnel_link_state_t link;  /* 0/1/2 (spec §4) */

    /* BLE connection */
    bool     connected;
    uint16_t conn_handle;
    bool     notify_enabled;   /* CCCD state for 0xFFE1 (local, spec §6) */
    uint8_t  write_fail_count;

    /* per-idx read cache */
    nb_cache_t cache[NB_MAX_CHARS];
} nb_identity_t;

typedef struct {
    uint8_t            char_count;
    tunnel_char_desc_t chars[NB_MAX_CHARS];
    bool               valid;
} nb_blueprint_t;

void nb_state_init(void);

void nb_get_blueprint(nb_blueprint_t *out);
void nb_set_blueprint(const nb_blueprint_t *bp);

/* identity accessors (locked copy / apply) */
void nb_get_identity(uint8_t bms_id, nb_identity_t *out);
void nb_set_ident_name(uint8_t bms_id, const char *name);
void nb_set_link(uint8_t bms_id, tunnel_link_state_t s);
void nb_set_cache(uint8_t bms_id, uint8_t idx, const uint8_t *data, uint16_t len);
void nb_get_cache(uint8_t bms_id, uint8_t idx, nb_cache_t *out);

/* connection bookkeeping */
void nb_set_conn(uint8_t bms_id, bool connected, uint16_t handle);
void nb_set_notify(uint8_t bms_id, bool enabled);
int  nb_identity_for_conn(uint16_t handle);   /* -> bms_id or -1 */
int  nb_active_conn_count(void);

#endif /* NB_STATE_H */
