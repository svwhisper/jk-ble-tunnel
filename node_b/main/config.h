/*
 * config.h — Node B configuration (spec §12). Node B on a BLE 5.0 target
 * (C3/S3/C6) for four advertising sets. Secrets in config_secret.h.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "jk_ble_defs.h"
#include "config_secret.h"   /* WIFI_*, NODE_A_HOST, NODE_A_PORT */

#define CFG_NUM_UNITS            JK_MAX_UNITS   /* 4 identities */

/* Advertising (spec §5): legacy-format connectable PDUs per set for app compat.
 * One app connection at a time; the other sets stay advertising. */
#define CFG_ADV_LEGACY           1
#define CFG_ADVERTISE_WHEN_DOWN  0     /* default off: down unit stops advertising */
#define CFG_MAX_APP_CONNS        1     /* accept-then-terminate a 2nd (spec §5)     */
#define CFG_ROTATE_RANDOM_ADDR   0     /* rotate on boot if replica table changed   */

/* Tunnel (spec §6). */
#define CFG_TUNNEL_PING_MS       5000
#define CFG_TUNNEL_DEAD_MS       15000
#define CFG_TUNNEL_GRACE_MS      8000  /* hold app conns through a short blip       */
#define CFG_TUNNEL_RECONNECT_MS  1000

/* Terminate an app connection after this many consecutive write failures. */
#define CFG_WRITE_FAIL_LIMIT     3

#endif /* CONFIG_H */
