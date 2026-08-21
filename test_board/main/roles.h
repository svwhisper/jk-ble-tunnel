/*
 * roles.h — the two bench emulator roles. One is active per boot session; the
 * control server selects it with the first `role` command.
 */
#ifndef ROLES_H
#define ROLES_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { ROLE_NONE, ROLE_APP, ROLE_BMS } role_t;

/* iOS-app emulator (BLE central), tests Node B. */
void emu_app_start(void);
void emu_app_scan(void);
bool emu_app_connect(const char *name);
void emu_app_subscribe(void);
void emu_app_read(void);
void emu_app_write(const uint8_t *data, int len);
void emu_app_disconnect(void);

/* JK-BMS emulator (BLE peripheral), tests Node A. */
void emu_bms_start(const char *adv_name);
void emu_bms_push(const char *what);   /* "cell" | "settings" | "devinfo" */
void emu_bms_autopush(int period_ms);

#endif /* ROLES_H */
