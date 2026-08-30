/*
 * log_udp.c — mirror the ESP log stream over UDP broadcast (owner request
 * 2026-08-30: node A's console is physically unreachable in the garage, and
 * A-side mysteries — the fresh-link 0x216 thrash — were undiagnosable).
 *
 * Fire-and-forget: one datagram per log call to the /23 broadcast address,
 * MSG_DONTWAIT, every error ignored. Nothing here can block or wedge a
 * logging task; if nobody listens the packets just die on the wire.
 * Listen on the Mac with:  nc -ulk 3766
 */
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "esp_log.h"
#include "log_udp.h"

#define LOG_UDP_PORT 3766

static int                s_sock = -1;
static struct sockaddr_in s_dst;
static vprintf_like_t     s_prev;

static int log_udp_vprintf(const char *fmt, va_list ap)
{
    /* Serial console first — behavior unchanged if UDP misbehaves. */
    va_list ap2; va_copy(ap2, ap);
    int r = s_prev ? s_prev(fmt, ap2) : vprintf(fmt, ap2);
    va_end(ap2);

    if (s_sock >= 0) {
        char buf[256];
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        if (n > 0) {
            if (n > (int)sizeof(buf)) n = sizeof(buf);
            sendto(s_sock, buf, n, MSG_DONTWAIT,
                   (struct sockaddr *)&s_dst, sizeof(s_dst));   /* best effort */
        }
    }
    return r;
}

void log_udp_start(void)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) return;
    int one = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    memset(&s_dst, 0, sizeof(s_dst));
    s_dst.sin_family = AF_INET;
    s_dst.sin_port = htons(LOG_UDP_PORT);
    s_dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);   /* flat /23 LAN */
    s_prev = esp_log_set_vprintf(log_udp_vprintf);
    ESP_LOGI("log_udp", "mirroring logs to UDP :%d (broadcast)", LOG_UDP_PORT);
}
