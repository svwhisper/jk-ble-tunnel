/*
 * ota.h — push-model, rollback-protected over-the-air firmware update, shared by
 * both nodes. The host pushes a new .bin with an HTTP POST; the device streams
 * it into the inactive OTA slot, validates, switches boot, and reboots.
 *
 * Host side: tools/ota_push.py (invokes curl). Contract:
 *   POST http://<node>:<CFG_OTA_PORT>/ota   body = raw app .bin (octet-stream)
 *   200 -> "OK ..."  (device reboots ~1 s later)   4xx/5xx -> reason, no reboot
 *
 * Rationale for push (not pull): the dev Mac isn't always up to serve a file,
 * so a device-initiated fetch is fragile; push only needs the Mac up while you
 * are actively updating. No auth by design (LAN range = physical access).
 */
#ifndef OTA_H
#define OTA_H

#include <stdint.h>

/* Confirm the running image so the bootloader won't roll it back. Call ONCE,
 * only after the device is known-healthy (we call it after WiFi is up). A
 * freshly-pushed image boots in PENDING_VERIFY; if it never reaches this call
 * (e.g. it crashes or can't join WiFi) the bootloader reverts on next reset.
 * A no-op when the running image isn't pending (e.g. a fresh USB flash). */
void ota_mark_valid(void);

/* Start the push-OTA HTTP receiver on `port` (POST /ota). Idempotent-ish:
 * call once after the network is up. */
void ota_start(uint16_t port);

#endif /* OTA_H */
