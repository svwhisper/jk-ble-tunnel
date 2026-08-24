/*
 * net_util.h — WiFi STA + SNTP, shared by both nodes. Credentials are passed
 * in (they live in the shared secret.h), never compiled in here.
 */
#ifndef NET_UTIL_H
#define NET_UTIL_H

#include <stdbool.h>

void net_wifi_start(const char *ssid, const char *pass);
bool net_wifi_wait(int timeout_ms);     /* block until IP or timeout */
void net_sntp_start(const char *server);

#endif /* NET_UTIL_H */
