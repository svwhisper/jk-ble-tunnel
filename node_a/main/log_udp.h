/* UDP log mirror (garage node has no reachable console). Call once after
 * WiFi is up; before that, sendto just fails silently — harmless. */
#ifndef LOG_UDP_H
#define LOG_UDP_H
void log_udp_start(void);
#endif
