/*
 * net_util.h — WiFi STA + SNTP, shared by both nodes. Credentials are passed
 * in (they live in the shared secret.h), never compiled in here.
 */
#ifndef NET_UTIL_H
#define NET_UTIL_H

#include <stdbool.h>

/* hostname is registered with the DHCP server (DHCP option 12) so the device is
 * reachable by name; pass NULL to keep the default. */
void net_wifi_start(const char *ssid, const char *pass, const char *hostname);
bool net_wifi_wait(int timeout_ms);     /* block until IP or timeout */
bool net_wifi_up(void);                 /* have an IP right now */
void net_wifi_ip_str(char *buf, int n); /* "192.168.3.241" or "-" */
void net_sntp_start(const char *server);

/* Cap WiFi TX power (units of 0.25 dBm; e.g. 34 = 8.5 dBm). Call after
 * net_wifi_start. On a marginal USB supply, full TX power (~20 dBm) browns out
 * the PA during transmit and 802.11 auth never completes (reason 2) — capping
 * it fixes that. Deployment on a solid supply can raise or skip this. */
void net_wifi_set_txpower(int8_t max_qdbm);

#endif /* NET_UTIL_H */
