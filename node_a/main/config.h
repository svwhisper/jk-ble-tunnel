/*
 * config.h — Node A configuration surface (spec §12).
 *
 * Compile-time defaults live here. Secrets (WiFi/MQTT/PIN) come from the shared
 * secret.h (git-ignored; copy components/secret/secret.h.example). Per-unit
 * target selection is the CFG_BMS_* table below — edit for your fleet.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "jk_ble_defs.h"
#include "secret.h"          /* WIFI_SSID/PASS, MQTT_*, JK_LOGIN_PIN */

/* ---- Topology / identity ------------------------------------------------ */
#define CFG_NUM_UNITS            JK_MAX_UNITS   /* 4 */

/* Per-unit target: BLE name to match + a stable local bms_id (0..3).
 * Address may be left 0 to select purely by advertised name. VERIFY names
 * against your units at harvest. */
typedef struct { const char *name; uint8_t bms_id; } cfg_bms_target_t;
static const cfg_bms_target_t CFG_BMS[CFG_NUM_UNITS] = {
    { "JK-B2A20S20P", 0 },
    { "JK-B2A20S20P_2", 1 },
    { "JK-B2A20S20P_3", 2 },
    { "JK-B2A20S20P_4", 3 },
};

/* ---- Link pool / timing (spec §4) --------------------------------------- */
#define CFG_LINK_POOL_SIZE       3       /* concurrent central links (~3 budget) */
#define CFG_IDLE_DISCONNECT_MS   60000   /* free BMS client slot after app leaves */
#define CFG_APP_LINK_TIMEOUT_MS  10000   /* app-write wait for link-up before fail */
#define CFG_REACHABILITY_PROBE_S 60      /* supervisor probe floor for unreachable */
#define CFG_RECONNECT_CAP_MS     30000   /* exponential backoff cap               */
#define CFG_CONN_ITVL_MIN_MS     30      /* central connection interval range      */
#define CFG_CONN_ITVL_MAX_MS     50
#define CFG_POLL_PERIOD_MS       5000    /* internal round-robin poll cadence      */

/* ---- Tunnel (spec §6) --------------------------------------------------- */
#define CFG_TUNNEL_PORT          3760
#define CFG_TUNNEL_PING_MS       5000
#define CFG_TUNNEL_DEAD_MS       15000

/* ---- Balance write path (spec §10) -------------------------------------- */
/* Per-key range clamps. Reject (never silently clamp) out-of-range. Values
 * are placeholders — set to the safe envelope for your cells (EVE MB31). */
typedef struct { const char *key; double min; double max; } cfg_range_t;
static const cfg_range_t CFG_BALANCE_RANGE[] = {
    { "balance_trigger_voltage", 3.30, 3.60 },
    { "balance_current",         0.10, 1.00 },
    { "balancing_enabled",       0.0,  1.0  },
};
#define CFG_BALANCE_RANGE_N (sizeof(CFG_BALANCE_RANGE)/sizeof(CFG_BALANCE_RANGE[0]))

#define CFG_ARB_MODE_QUEUE       0   /* 0=block automation writes while app active,
                                        1=queue to apply on app disconnect (§10)  */
#define CFG_BALANCE_RATE_PER_CYCLE 1 /* default: one balance write per charge cycle */

/* ---- Measurement mode (spec §9) ----------------------------------------- */
#define CFG_MEAS_SETTLE_MS       20000
#define CFG_MEAS_TIMEOUT_MS      120000  /* supervisor hard restore; settle < this */
#define CFG_MEAS_BALANCE_FLOOR_A 0.30
#define CFG_MEAS_CELLCOUNT_TOGGLE 0      /* O-4: default OFF until firmware-verified */

/* Compile-time guard for the §9 assertion (settle < timeout). */
_Static_assert(CFG_MEAS_SETTLE_MS < CFG_MEAS_TIMEOUT_MS,
               "measurement settle must be < MEAS_TIMEOUT_MS (spec §9)");

/* ---- MQTT (spec §7) ----------------------------------------------------- */
#define CFG_MQTT_BASE            "jkbms"
#define CFG_MQTT_BRIDGE_STATUS   CFG_MQTT_BASE "/bridge/status"
#define CFG_NTP_SERVER           "pool.ntp.org"

/* WiFi TX power cap, units of 0.25 dBm (34 = 8.5 dBm). Needed on a marginal USB
 * supply: at full power the PA browns out on transmit and 802.11 auth fails
 * (reason 2). Raise toward 78 (~20 dBm) once on a solid supply / for range. */
#define CFG_WIFI_MAX_TX_QDBM     34

#endif /* CONFIG_H */
