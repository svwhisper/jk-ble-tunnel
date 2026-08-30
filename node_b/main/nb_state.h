/*
 * nb_state.h — Node B shared state: the single replica blueprint, the four
 * per-identity read caches, per-identity link + advertising + connection
 * status. One mutex guards it; BLE callbacks and the tunnel task both touch it.
 */
#ifndef NB_STATE_H
#define NB_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tunnel_proto.h"
#include "jk_ble_defs.h"

#define NB_MAX_CHARS   8
#define NB_CACHE_MAX   320   /* per (identity,idx) cached read value */

/* Oldest cell-info frame we will replay to the app as a warm start. Stale
 * voltages presented as live are worse than a blank screen; 10 min covers a
 * mortal bank's snapshot cadence (~5m20s) with margin. Device-info has no
 * age limit — it is static content. */
#define NB_CELL_REPLAY_MAX_AGE_US (10LL * 60 * 1000000)

/* pending_replay bitmask */
#define NB_REPLAY_DEVINFO  0x01
#define NB_REPLAY_CELLINFO 0x02
#define NB_REPLAY_SETTINGS 0x04

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

    /* Warm-start frames (spec §-fix 2026-08-30): the last COMPLETE device-info
     * (0x03) and cell-info (0x02) frames seen from A, kept so B can answer the
     * app's opener (0x97/0x96) instantly from cache when the real link is
     * asleep — otherwise the app times out ("request device information
     * failure") waiting for A to wake and scan-find a sleeping module. */
    nb_cache_t warm_devinfo;   /* last 0x03 frame (NVS-persisted — static)  */
    nb_cache_t warm_cellinfo;  /* last 0x02 frame (RAM only, age-gated)     */
    nb_cache_t warm_settings;  /* last 0x01 frame (RAM only, no gate: the
                                * only writer is the owner via MQTT, and the
                                * write's own readback refreshes the cache) */
    int64_t    warm_cell_us;   /* when warm_cellinfo was captured           */

    /* Replay owed to the app but blocked by a disabled CCCD at write time
     * (the app writes its 0x97 opener BEFORE subscribing — measured with
     * app_probe 2026-08-30). Delivered by ble_periph_replay_tick() from the
     * tunnel task once notifications come up — NEVER from the subscribe
     * callback (that wedged the live stream, 2026-08-30). */
    uint8_t    pending_replay;   /* NB_REPLAY_* bits */
    int64_t    pending_since_us; /* set when bits first owed; replays expire
                                  * after the app's opener window — a held
                                  * replay must never inject a stale frame
                                  * into a live session much later. */
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

/* Warm-start frame cache (device-info 0x03 / cell-info 0x02). `rec` selects.
 * set: a devinfo frame is also persisted to NVS (once — the comparison skips
 * the frame counter), so it survives a Node B reboot.
 * get: a cell-info frame older than NB_CELL_REPLAY_MAX_AGE_US comes back
 * len=0 — the age gate applies to every replay path. */
void nb_set_warm(uint8_t bms_id, uint8_t rec, const uint8_t *frame, uint16_t len);
void nb_get_warm(uint8_t bms_id, uint8_t rec, nb_cache_t *out);

/* Deferred replay owed to a connected app whose CCCD was off at write time. */
void    nb_mark_replay(uint8_t bms_id, uint8_t bits);
uint8_t nb_take_replay(uint8_t bms_id);   /* returns and clears the bits */

/* Narrow flag reads. nb_identity_t is ~3.3 KB (ten embedded caches) — a
 * whole-struct copy in a NimBLE callback blew the nimble_host stack
 * (2026-08-30 panic). Use these where only the flags are needed. */
bool nb_notify_ready(uint8_t bms_id);     /* connected && notify_enabled */
bool nb_replay_ready(uint8_t bms_id);     /* ...&& pending_replay != 0   */
int  nb_conn_handle(uint8_t bms_id);      /* handle, or -1 if not connected */
bool nb_get_name(uint8_t bms_id, char *out, size_t out_len); /* false if unset */

/* Live-pipe tracker: replays must NEVER interleave with live TUN_RAW chunks
 * (the app reassembles one contiguous stream; interleaving corrupted its
 * devinfo — "device is not supported", 2026-08-30). */
void nb_note_raw(uint8_t bms_id);                    /* a raw chunk was relayed */
bool nb_raw_quiet(uint8_t bms_id, int64_t quiet_us); /* no raw for quiet_us?    */

/* connection bookkeeping */
void nb_set_conn(uint8_t bms_id, bool connected, uint16_t handle);
void nb_set_notify(uint8_t bms_id, bool enabled);
int  nb_identity_for_conn(uint16_t handle);   /* -> bms_id or -1 */
int  nb_active_conn_count(void);

#endif /* NB_STATE_H */
