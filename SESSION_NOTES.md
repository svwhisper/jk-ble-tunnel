# SESSION_NOTES — JK BLE Tunnel

Living design/status doc. Keep current alongside code changes.

## Where things stand (2026-08-21)

First implementation of `jk-ble-tunnel-spec.md` rev 3, written before hardware
is available (bench next week). Compiles-in-intent against ESP-IDF 5.x + NimBLE;
**not yet compiled or flashed** — expect a NimBLE API-signature pass (`NIMBLE-PASS`
markers) and a decode-offset bench pass (`VERIFY` markers) first.

## Design decisions carried from the spec

- **Node B = one shared GATT table, four identities by advertising address.**
  A peripheral has a single attribute table; identity is resolved on connect via
  the per-set static-random address (`adv_mgr_identity_for_addr`), and each
  identity has its own read cache. See `ble_periph.c` header comment.
- **Reads served from cache, writes acked immediately** — because NimBLE's
  access callback is synchronous on the host task, not because of ATT timing.
  `WRITE_RESULT` is for observability + the drop-app-connection failure policy.
- **Measurement mode is crash-safe** via an NVS record written before the
  balance floor is lowered; restored at boot (`measure_check_boot`) or on the
  supervisor `MEAS_TIMEOUT_MS` guard. Independently testable even with writes
  disabled (with writes off, the floor is never lowered, so restore is a no-op).
- **Reachability tri-state** (unreachable/reachable-idle/link-up) drives Node B
  advertising; a down unit stops advertising (truthful scan list). Recovery from
  `unreachable` rides a supervisor probe floor (`REACHABILITY_PROBE_S`),
  independent of Node-RED polling.
- **Tunnel grace window** (`CFG_TUNNEL_GRACE_MS`) on Node B holds app
  connections through a short blip; resync replays `TABLE_REQ` → state →
  `CLIENT` for connected identities.
- **Write-FIFO / failure-counter hygiene** reset on every `CLIENT` transition.
- **Addressing is hostname + DHCP, no static IPs.** Each ESP32 sets its DHCP
  hostname (`net_util` via `esp_netif_set_hostname` on STA_START); `tunnel_cli`
  resolves Node A with `getaddrinfo` (DNS / `.local` / literal IP). Both nodes
  enable `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES`. Node B→A bare-name resolution
  needs pfSense "Register DHCP leases in the DNS Resolver" (or use a `.local`
  responder / literal IP). MQTT broker is a hostname with **no auth** (anonymous).

## Honesty boundaries (do not paper over)

- `JK_ENABLE_WRITES=0`. Do not enable until `jk_build_balance_write` /
  `jk_build_login` are ported (O-2/O-5) and bench-verified. It is a hard
  `#error` otherwise, on purpose.
- All `VERIFY` offsets in `jk_proto.c` are guesses kept in lockstep with
  `synth_frames.c`. Replace both together from a real capture (O-1).

## Bench rig

`test_board` is TCP-driven (`tools/bench.py`), one role per boot:
- `role bms <name>` → Node A gets a target; `autopush <ms>` streams cell-info.
- `role app` → Node B gets a client; `connect/sub/read/write/disconnect`.

To exercise the whole A↔B↔app path at once you need two boards (or one board as
BMS + a phone). One board covers either half.

## Open items (from spec §15, tracked here)

- O-1 confirm frame version per unit; pin decode offsets.
- O-2 extract settings/balance-write frame format + login; then flip
  `JK_ENABLE_WRITES` and wire the readback in `arbiter.c handle_balance_set`.
- O-4 measurement cell-count toggle (default off, `CFG_MEAS_CELLCOUNT_TOGGLE`).
- O-5 confirm login PIN + whether balance writes require it.
- O-7 confirm the JK iOS app surfaces all four sets; validate advertise-while-
  connected for 1 conn + 3 sets on the chosen C3/S3/C6.
- O-8 confirm ext-adv instance count + RAM under menuconfig on target.
- O-9 assert identical GATT layout across the four units at harvest
  (`supervisor.c harvest_tick` does the check; verify the mismatch path).

## TODO before/at bench

1. Compile each project on real IDF; clear every `NIMBLE-PASS`.
2. Capture real frames; fix `jk_proto.c` offsets; confirm decode parity (§14.1).
3. Fill `READ_CACHE` priming in `tunnel_srv.c send_blueprint` from the decoded
   cell/settings snapshots (left as a marker until offsets are real).
4. Complete the post-app-session **settings** re-read (currently a device-info
   poll placeholder in `arbiter.c on_app_conn`).
5. Node A harvest: capture device name/version into the NVS harvest entry
   (currently only the GATT table is filled by `ble_owner`).
