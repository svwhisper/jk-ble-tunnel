/*
 * net_util.h — WiFi STA + SNTP, shared by both nodes. Credentials are passed
 * in (they live in the shared secret.h), never compiled in here.
 */
#ifndef NET_UTIL_H
#define NET_UTIL_H

#include <stdbool.h>
#include <stdint.h>

/* hostname is registered with the DHCP server (DHCP option 12) so the device is
 * reachable by name; pass NULL to keep the default. */
void net_wifi_start(const char *ssid, const char *pass, const char *hostname);
bool net_wifi_wait(int timeout_ms);     /* block until IP or timeout */
bool net_wifi_up(void);                 /* have an IP right now */
/* How long WiFi has been down, in ms (0 when up). The C3 shares ONE radio
 * between WiFi and BLE: callers use this to quiesce BLE scanning while WiFi
 * re-associates, or the two starve each other forever (garage death spiral,
 * 2026-08-28: WiFi dropped mid-operation and the BLE connect loop kept the
 * radio so busy the 802.11 handshake never completed again). */
int64_t net_wifi_down_ms(void);
int net_wifi_rssi(void);               /* dBm to the AP; 0 = not associated */
void net_wifi_ip_str(char *buf, int n); /* "192.168.3.241" or "-" */
void net_sntp_start(const char *server);
/* Nightly maintenance reboot at hour:min local (POSIX TZ string, DST-aware).
 * Skips while busy() is true and within the first 2 h of uptime. */
void nightly_reboot_start(const char *tz, int hour, int min, bool (*busy)(void));

/* Cap WiFi TX power (units of 0.25 dBm; e.g. 34 = 8.5 dBm). Call after
 * net_wifi_start. On a marginal USB supply, full TX power (~20 dBm) browns out
 * the PA during transmit and 802.11 auth never completes (reason 2) — capping
 * it fixes that. Deployment on a solid supply can raise or skip this. */
void net_wifi_set_txpower(int8_t max_qdbm);

#endif /* NET_UTIL_H */
