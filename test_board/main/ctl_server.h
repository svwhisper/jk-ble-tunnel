/*
 * ctl_server.h — line-oriented control channel for the bench test node, over
 * the board's USB **serial** port (it's USB-tethered and needs no WiFi; it
 * reaches the real nodes over Bluetooth). Drive it with tools/bench.py <port>,
 * `idf.py monitor`, or any 115200 serial terminal.
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

void ctl_server_start(void);
void ctl_emit(const char *fmt, ...);      /* send an event line to the client */

#endif /* CTL_SERVER_H */
