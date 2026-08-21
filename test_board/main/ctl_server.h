/*
 * ctl_server.h — line-oriented TCP control channel for the bench (spec §2/§14:
 * "a dedicated ESP32 test board you connect to via TCP, in lieu of an iOS
 * device"). A laptop drives the board with nc/telnet or tools/bench.py.
 *
 * Commands (one per line, CRLF or LF):
 *   role app                     become the iOS-app emulator (BLE central)
 *   role bms <name>              become a JK-BMS emulator (BLE peripheral)
 *   status                       print role + BLE state
 *   -- app role --
 *   scan                         list advertised names seen
 *   connect <name>               connect to a Node B identity by name
 *   sub                          subscribe to 0xFFE1 notifications
 *   read                         read 0xFFE1 (served from Node B cache)
 *   write <hexbytes>             write raw bytes to 0xFFE1
 *   disconnect
 *   -- bms role --
 *   push cell|settings|devinfo   emit one synthetic frame as a notification
 *   autopush <ms>                stream cell-info every <ms> (0 = off)
 *
 * Events are emitted as lines beginning "EVT ": connected, disconnected,
 * notify <hex>, read <hex>, adv <name>, write <hex>, etc.
 */
#ifndef CTL_SERVER_H
#define CTL_SERVER_H

#define CTL_PORT 4000

void ctl_server_start(void);
void ctl_emit(const char *fmt, ...);      /* send an event line to the client */

#endif /* CTL_SERVER_H */
