/*
 * config.h — Node A configuration surface (spec §12).
 *
 * Compile-time defaults live here. Secrets (WiFi/MQTT/PIN) come from the shared
 * secret.h (git-ignored; copy components/secret/secret.h.example). Per-unit
 * target selection is the CFG_BMS_* table below — edit for your fleet.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>          /* NULL, used by the CFG_BMS parked entries */
#include "jk_ble_defs.h"
#include "secret.h"          /* WIFI_SSID/PASS, MQTT_*, JK_LOGIN_PIN */

/* ---- Topology / identity ------------------------------------------------ */
/* The fleet itself (names, ids, public addresses, count) lives in the site
 * file — components/secret/secret.h, FLEET_BMS_TABLE — so adopting this code
 * needs no source edits (see secret.h.example for the how-to).
 * Engineering rationale that must survive (2026-08-28): selection is BY
 * PUBLIC ADDRESS with PASSIVE scanning. JK names live only in the SCAN
 * RESPONSE, and active scanning's SCAN_REQ packets CHIRP the units — proven
 * when parked bank 3 chirped during scans that never connect to it. Public-
 * only matching also forecloses the Node-B clone self-loop class (clones use
 * static-random addresses). NULL name OR zero addr = parked. addr bytes are
 * little-endian as NimBLE presents them (reversed from display order). */
#define CFG_NUM_UNITS            FLEET_NUM_UNITS
typedef struct { const char *name; uint8_t bms_id; uint8_t addr[6]; } cfg_bms_target_t;
static const cfg_bms_target_t CFG_BMS[CFG_NUM_UNITS] = { FLEET_BMS_TABLE };

/* ---- Link pool / timing (spec §4) --------------------------------------- */
#define CFG_LINK_POOL_SIZE       4       /* hold ALL four banks (S3 + 6C era) */
/* Effectively disabled: with a 4-slot pool holding every bank, freeing a
 * slot after an app session just causes a pointless 0x216 disconnect +
 * reconnect chirp (observed after every house app session, 2026-08-29). */
#define CFG_IDLE_DISCONNECT_MS   86400000
#define CFG_APP_LINK_TIMEOUT_MS  10000   /* app-write wait for link-up before fail */
#define CFG_REACHABILITY_PROBE_S 60      /* supervisor probe floor for unreachable */
#define CFG_RECONNECT_CAP_MS     30000   /* exponential backoff cap               */
/* Gentle-client reconnect pacing (2026-08-29 CPUAux reframe): a session that
 * died young signals a struggling aux CPU — hammering reconnects at it (LL
 * connect chirp + bootstrap command-ack beeps per cycle, ~30 s apart) is the
 * abuse profile that latched bank 3's CPUAux fault. After a session shorter
 * than STABLE_SESSION_S the supervisor holds that unit's internal reconnect
 * drivers off for an escalating 60→120→240→…→HOLD_CAP_S; one session past
 * STABLE_SESSION_S resets the ladder. App/NR-sourced traffic still connects
 * on demand during a hold (explicit intent overrides the pacing). */
#define CFG_RECONNECT_HOLD_BASE_S 60
#define CFG_RECONNECT_HOLD_CAP_S  600
#define CFG_STABLE_SESSION_S      600
/* Soak verdict (2026-08-29 afternoon): short sessions are ENDEMIC to these
 * modules under ESP32 centrals — esphome-jk-bms issue #732 shows 7 s drops
 * on a PB2A16S20P, and reads/latency/params were each eliminated here (bank
 * 2 holds for hours at the WORST RSSI; banks 1/3 die 11–140 s in by sudden
 * LL silence while their RS485 side stays up). So a short session that
 * STREAMED isn't abuse-signal — the module served us until its BLE stack
 * died. Those reconnect on a fixed moderate hold (coverage for the
 * charge-stop guard); only sessions that never produced a frame escalate on
 * the ladder above. ~200 cycles/day/bank worst case vs the ~3000/day of the
 * churn era that latched bank 3. */
#define CFG_RECONNECT_HOLD_PROD_S 300
/* Connection parameters — THE coex fix (2026-08-28 architecture review).
 * NimBLE defaults (30 ms interval, 2.56 s supervision) demand the shared
 * radio every ~10 ms across 3 links; WiFi bursts then starve a link past its
 * tiny supervision timeout, the CONTROLLER drops it, and reconnects cascade
 * ("stack cycling between BMSs", chirping units). JK streams ~3x128 B/s —
 * a 300 ms interval carries that with 10x less radio pressure, and an 8 s
 * supervision timeout rides out any realistic coex gap. */
/* v2 (bench 2026-08-28): 300 ms starved the JK's tiny UART-BLE bridge buffer
 * (data gaps -> poll strikes -> terminate -> chirp). The compromise BLE was
 * built for: FAST interval + generous SLAVE LATENCY — the peripheral gets
 * 30-40 ms service while it has data and skips up to 9 events when idle
 * (~400 ms effective), so the radio budget stays coex-friendly. */
/* v3 (2026-08-29 gentle-client soak): latency 9 → 0. With the keepalive reads
 * removed, banks 1+3 STILL died 0x208 mid-stream (bank 2 solid) — and slave
 * latency is the one remaining divergence from every client that survives
 * these modules 24/7 (phone app, esphome-jk-bms: latency 0). Hypothesis: the
 * JK bridge MCU mishandles event-skipping — sleep-clock drift across 9
 * skipped events misses the RX window, module goes silent, supervision kills
 * the link; per-bank tolerance gradient = per-module crystal quality. Coex
 * stays covered by the 8 s supervision (the real 2026-08-28 fix), not by
 * latency. */
#define CFG_CONN_ITVL_MIN_MS     30      /* central connection interval range      */
#define CFG_CONN_ITVL_MAX_MS     40
#define CFG_CONN_LATENCY         0       /* NO event-skipping (see v3 note)        */
#define CFG_CONN_SUPERVISION_MS  8000    /* controller drop threshold              */

/* BLE master switch: the operator's last cmd/ble on|off is persisted in NVS
 * and restored at boot; a never-set node boots OFF. (Replaced the 30 s
 * auto-arm 2026-08-29 with owner consent: OTA is start-early + self-healing
 * now, so it no longer needs a radio-quiet arming window — and after the
 * CPUAux incident a power blip must NOT re-arm BLE on its own.) */
#define CFG_POLL_PERIOD_MS       5000    /* internal round-robin poll cadence      */

/* ---- Tunnel (spec §6) --------------------------------------------------- */
#define CFG_TUNNEL_PORT          3760
#define CFG_TUNNEL_PING_MS       5000
#define CFG_TUNNEL_DEAD_MS       15000

/* ---- Push-OTA receiver -------------------------------------------------- */
/* Nonstandard, unused port; the host updater (tools/ota_push.py) POSTs the new
 * .bin here. Must match ota_push.py's default --port. Always listening. */
#define CFG_OTA_PORT             3765

/* ---- Balance write path (spec §10) -------------------------------------- */
/* Per-key range clamps. Reject (never silently clamp) out-of-range. Values
 * are placeholders — set to the safe envelope for your cells (EVE MB31). */
typedef struct { const char *key; double min; double max; } cfg_range_t;
/* Ranges in the whitelist's human units. CORRECTED 2026-08-30 from live app
 * captures: balance_trigger_voltage is a DELTA (the app showed 0.010 V), not
 * an absolute cell voltage; and the PB2A16S20P balances up to 2 A. */
static const cfg_range_t CFG_BALANCE_RANGE[] = {
    { "balance_trigger_voltage", 0.003, 0.100 },  /* volts, delta */
    { "balance_current",         0.10,  2.00  },  /* amps         */
    { "balancing_enabled",       0.0,   1.0   },  /* bool         */
    { "balance_start_voltage",   2.90,  3.50  },  /* volts, ABSOLUTE — envelope
                                                   * covers the recalibration
                                                   * procedure (3.0) and the
                                                   * normal setpoint (3.42) */
    { "cell_count",              15.0,  16.0  },  /* recal trigger ONLY — writing it
                                                   * (even same-value) re-measures all
                                                   * wire resistances. NEVER while iBMS
                                                   * charge control is live: the
                                                   * transient error would drop
                                                   * Victron power. */
};
#define CFG_BALANCE_RANGE_N (sizeof(CFG_BALANCE_RANGE)/sizeof(CFG_BALANCE_RANGE[0]))

#define CFG_ARB_MODE_QUEUE       0   /* 0=block automation writes while app active,
                                        1=queue to apply on app disconnect (§10)  */
#define CFG_BALANCE_MIN_INTERVAL_MS 3000 /* debounce: min gap between writes/unit */

/* ---- Measurement mode (spec §9) ----------------------------------------- */
#define CFG_MEAS_SETTLE_MS       20000
#define CFG_MEAS_TIMEOUT_MS      120000  /* supervisor hard restore; settle < this */
#define CFG_MEAS_BALANCE_FLOOR_A 0.30
#define CFG_MEAS_CELLCOUNT_TOGGLE 0      /* O-4: default OFF until firmware-verified */
/* Measurement mode's own write steps (floor-lower + restore) are a SEPARATE,
 * still-unported capability (O-2 measurement half: step 2 floor write and the
 * NVS-raw restore are not built). Balance writes (JK_ENABLE_WRITES) are live,
 * but cmd/measure stays honestly disabled until this is 1. */
#define JK_ENABLE_MEASURE_WRITES 0

/* Compile-time guard for the §9 assertion (settle < timeout). */
_Static_assert(CFG_MEAS_SETTLE_MS < CFG_MEAS_TIMEOUT_MS,
               "measurement settle must be < MEAS_TIMEOUT_MS (spec §9)");

/* ---- MQTT (spec §7) ----------------------------------------------------- */
#define CFG_MQTT_BASE            "jkbms"
#define CFG_MQTT_BRIDGE_STATUS   CFG_MQTT_BASE "/bridge/status"
#define CFG_NTP_SERVER           "pool.ntp.org"

/* BLE quiesce while WiFi re-associates (shared C3 radio): when WiFi has been
 * down longer than this, stop initiating BLE scans/connects so the 802.11
 * handshake can complete; BLE resumes once WiFi is back. */
#define CFG_WIFI_QUIESCE_MS      10000

/* WiFi TX power cap, units of 0.25 dBm (34 = 8.5 dBm). Needed on a marginal USB
 * supply: at full power the PA browns out on transmit and 802.11 auth fails
 * (reason 2). Raise toward 78 (~20 dBm) once on a solid supply / for range. */
#define CFG_WIFI_MAX_TX_QDBM     34

#endif /* CONFIG_H */
