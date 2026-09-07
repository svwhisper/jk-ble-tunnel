/*
 * na_types.h — shared Node A types: arbiter requests/responses, event bits,
 * per-unit runtime state. Kept separate so every task includes one header.
 */
#ifndef NA_TYPES_H
#define NA_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "jk_proto.h"
#include "tunnel_proto.h"

/* Who originated a transaction (spec §4 q_bms_request.source). */
typedef enum {
    SRC_APP      = 0,  /* relayed app GATT write via tunnel  */
    SRC_MQTT     = 1,  /* automation cmd via MQTT            */
    SRC_INTERNAL = 2,  /* poll / measurement / harvest       */
} req_source_t;

/* Kind of transaction the ble_owner must perform. */
typedef enum {
    TXN_POLL,        /* write a read-cmd, collect the response frame        */
    TXN_RAW_WRITE,   /* relay raw bytes to 0xFFE1 (app write)               */
    TXN_BALANCE_WRITE,/* validated settings write (gated, §10)              */
    TXN_CONNECT,     /* establish link                                      */
    TXN_DISCONNECT,  /* tear down link                                      */
} txn_kind_t;

#define REQ_PAYLOAD_MAX TUNNEL_MAX_WRITE_DATA

/* Local queue identity only: never serialized on TCP/MQTT or persisted.
 * Zero is reserved; the arbiter does not reuse ids within a boot. */
typedef uint64_t bms_cmd_id_t;

/* q_bms_request item (arbiter -> ble_owner). */
typedef struct {
    uint8_t      bms_id;
    bms_cmd_id_t cmd_id;     /* exact local operation identity              */
    txn_kind_t   kind;
    req_source_t source;
    uint8_t      opcode;     /* for TXN_POLL: JK_CMD_*                       */
    uint8_t      idx;        /* characteristic index (app writes)           */
    bool         with_response;
    bool         response_needed;
    uint32_t     timeout_ms;
    uint8_t      payload[REQ_PAYLOAD_MAX];
    uint8_t      payload_len;
    /* Internal idle-release fence; never serialized onto TCP/MQTT/NVS.
     * Explicit bounces leave idle_only=false and retain their own policy. */
    bool         idle_only;
    uint64_t     idle_epoch;
} bms_request_t;

/* Result status carried on q_bms_response. */
typedef enum {
    RESP_OK, RESP_TIMEOUT, RESP_LINK_DOWN, RESP_GATT_ERR, RESP_REJECTED,
} resp_status_t;

/* q_bms_response item (ble_owner -> arbiter). The legacy frame fields are
 * currently NULL/0: decoded data travels through copied notification queues,
 * never as a borrowed pointer into the mutable reassembly buffer. */
typedef struct {
    uint8_t       bms_id;
    bms_cmd_id_t  cmd_id;
    resp_status_t status;
    const uint8_t *frame;
    uint16_t      frame_len;
    jk_record_t   record;
} bms_response_t;

/* Per-unit runtime state (owned by arbiter/supervisor, snapshot-read). */
typedef struct {
    tunnel_link_state_t link;        /* reachability tri-state (spec §4)     */
    bool     app_connected;          /* app holds this identity              */
    bool     link_held;              /* real central link currently up       */
    uint32_t backoff_ms;             /* current reconnect backoff            */
    int64_t  last_seen_us;           /* legacy link-up / good-frame activity */
    int64_t  last_frame_us;          /* complete checksum-valid frame ONLY   */
    int64_t  app_left_us;            /* for idle-disconnect timer            */
    bool     meas_in_progress;
    uint64_t idle_epoch;             /* changes on app or held-link edges   */
    int64_t  link_up_us;             /* actual ready-link edge, not a tick  */
} bms_runtime_t;

/* Fan-out notification item (ble_owner -> tunnel_srv and -> decoder).
 * raw=false: a complete, checksum-valid reassembled frame (decoder + Node B
 * read-cache). raw=true: a verbatim BMS notify chunk, forwarded to a connected
 * app byte-for-byte (TUN_RAW) so the clone is transparent — the real stream
 * includes AT heartbeats and AA5590EB C8 command-acks the app waits for, which
 * reassembly strips. Copied into each consumer's queue — never shared. */
typedef struct {
    uint8_t  bms_id;
    uint8_t  idx;              /* characteristic index (JK: the 0xFFE1 slot) */
    bool     raw;              /* verbatim chunk (TUN_RAW) vs complete frame */
    uint16_t len;
    uint8_t  data[JK_FRAME_MAX];
} notify_item_t;

/* Event group bits (spec §4 evt_link). */
#define EVT_MQTT_UP     (1 << 0)
#define EVT_TUNNEL_UP   (1 << 1)
#define EVT_APP_ACTIVE  (1 << 2)     /* any identity app-connected           */
#define EVT_BMS_UP(n)   (1 << (8 + (n)))  /* per-unit link-up, n in 0..3     */

extern EventGroupHandle_t g_evt;

#endif /* NA_TYPES_H */
