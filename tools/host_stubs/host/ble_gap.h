#ifndef HOST_BLE_GAP_H
#define HOST_BLE_GAP_H
#include "ble_hs.h"
#define BLE_ADDR_PUBLIC 0
#define BLE_OWN_ADDR_PUBLIC 0
#define BLE_ERR_REM_USER_CONN_TERM 0x13
enum { BLE_GAP_EVENT_CONNECT, BLE_GAP_EVENT_DISCONNECT, BLE_GAP_EVENT_DISC,
       BLE_GAP_EVENT_DISC_COMPLETE, BLE_GAP_EVENT_CONN_UPDATE_REQ,
       BLE_GAP_EVENT_L2CAP_UPDATE_REQ, BLE_GAP_EVENT_CONN_UPDATE, BLE_GAP_EVENT_NOTIFY_RX,
       BLE_GAP_EVENT_ADV_COMPLETE, BLE_GAP_EVENT_SUBSCRIBE, BLE_GAP_EVENT_MTU };
struct ble_gap_upd_params { uint16_t itvl_min, itvl_max, latency, supervision_timeout, min_ce_len, max_ce_len; };
struct ble_gap_conn_params { uint16_t scan_itvl, scan_window, itvl_min, itvl_max, latency, supervision_timeout, min_ce_len, max_ce_len; };
struct ble_gap_disc_params { uint8_t passive; uint16_t itvl, window; };
struct ble_gap_conn_desc {
    uint16_t conn_handle, conn_itvl, conn_latency, supervision_timeout;
    ble_addr_t our_ota_addr, our_id_addr;
};
struct ble_gap_event {
    uint8_t type;
    union {
        struct { int status; uint16_t conn_handle; } connect;
        struct { int reason; struct ble_gap_conn_desc conn; } disconnect;
        struct { ble_addr_t addr; int8_t rssi; uint8_t *data; uint8_t length_data; } disc;
        struct { struct ble_gap_upd_params *self_params; } conn_update_req;
        struct { uint16_t conn_handle; int status; } conn_update;
        struct { uint16_t conn_handle; struct os_mbuf *om; } notify_rx;
        struct { uint8_t instance; int reason; uint16_t conn_handle; } adv_complete;
        struct { uint16_t conn_handle; uint8_t cur_notify; } subscribe;
        struct { uint16_t conn_handle, value; } mtu;
    };
};
typedef int ble_gap_event_fn(struct ble_gap_event *, void *);
int ble_gap_terminate(uint16_t, uint8_t);
int ble_gap_disc_cancel(void);
int ble_gap_disc(uint8_t, int32_t, const struct ble_gap_disc_params *, ble_gap_event_fn *, void *);
int ble_gap_connect(uint8_t, const ble_addr_t *, int32_t, const struct ble_gap_conn_params *, ble_gap_event_fn *, void *);
int ble_gap_conn_rssi(uint16_t, int8_t *);
int ble_gap_conn_find(uint16_t, struct ble_gap_conn_desc *);
int ble_gap_update_params(uint16_t, const struct ble_gap_upd_params *);
#endif
