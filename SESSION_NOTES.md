# SESSION_NOTES — JK BLE Tunnel

Living design/status doc. Keep current alongside code changes.

## Where things stand (2026-08-24)

Implementation of `jk-ble-tunnel-spec.md` rev 3. **All three projects build clean
(zero warnings, zero errors)** on ESP-IDF v5.2.3. Targets: **Node A + Node B =
`esp32c3`**; **test board = `esp32s3`** (Lonely Binary S3 Gold Edition — dual-core,
native USB, roomy for the bench emulator). The NimBLE API signatures are
therefore compiler-verified, and the BLE activation paths are wired for real:
Node B ext-adv (configure/set_addr/set_data/start/stop) and Node A scan-by-name
→ connect. What remains is **runtime** verification on hardware, plus the
decode-offset bench pass (`VERIFY` markers) — not a compile pass.

Toolchain notes: C3 is single-core, so the four Node-A tasks that were pinned to
core 1 now use `tskNO_AFFINITY`. `esp_task_wdt` is part of `esp_system` in 5.2
(not its own component). The IDF Python env needed `idf-component-manager~=1.5` +
`ruamel.yaml<0.18` (the auto-installed latest set was too new for the 5.2 checker).

## Bench status (2026-08-28)

Infrastructure PROVEN on real hardware + real LAN (Southerness):
- Both C3 nodes join WiFi (TX power capped at 8.5 dBm), get DHCP, stable
  (single boot, no task-watchdog, logs quiet).
- Node A: MQTT broker connected (mqtt.localdomain resolves).
- A<->B TCP tunnel established: Node B resolves jk-node-a.localdomain via
  pfSense DNS and connects to Node A's tunnel server (`tunnel up`, tunnel=1).
- **Node A <-> BMS central path VALIDATED** against the test-board BMS emulator:
  scan-by-name -> connect -> discover 0xFFE0/0xFFE1 -> subscribe -> poll(0x96)
  -> 300-byte frame (chunked notifies) -> jk_proto reassemble -> decode -> MQTT.
  Broker (192.168.2.5) shows jkbms/0/state/{cells,summary,faults,link}, correct
  §9 r:null + cell-9 imbalance + reachability tri-state. (cells 17-32 junk with
  the 16-cell synth frame = placeholder-offset overlap, O-1.)
- STILL not exercised: app <-> Node B path (phone or app-emulator board), real
  JK decode offsets (O-1), and the gated write path (O-2/O-5).
- Bench note: Mac is on the /23 (192.168.3.243) and reaches the broker by IP,
  but pfSense DNS (.2.1:53) doesn't answer the Mac — use the broker IP directly.
  Test-board role isn't persisted; hold it advertising with scratchpad/hold_bms.py
  (keeps the serial open so it isn't reset).

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

`test_board` is **serial-driven** (`tools/bench.py <port>`, or `idf.py monitor`),
one role per boot:
- `role bms <name>` → Node A gets a target; `autopush <ms>` streams cell-info
  (only notifies once a central connects + subscribes).
- `role app` → Node B gets a client; `connect/sub/read/write/disconnect`.

To exercise the whole A↔B↔app path at once you need two boards (or one board as
BMS + a phone). One board covers either half.

**The bench board has no WiFi** (removed 2026-08-28). It is USB-tethered and its
control channel runs over the console UART; it only ever talks to the nodes over
Bluetooth, so WiFi was pointless.

**WiFi failure ROOT CAUSE = TX power / marginal USB supply (SOLVED 2026-08-28).**
Every ESP32 failed 802.11 auth (reason 2 AUTH_EXPIRE, pre-handshake) on EVERY
AP — the UniFi U6-Mesh *and* an iPhone hotspot — across 3 boards. Not the AP,
not creds, not our code. At full TX power (~20 dBm) the PA browns out the board
on transmit (the `phy_init checksum failure` each boot is the tell), corrupting
RF cal + the auth frames so auth never completes; low-power RX was always fine
(hence "sees APs great, can't auth"). **Fix: cap WiFi TX power** —
`net_wifi_set_txpower(CFG_WIFI_MAX_TX_QDBM)`; 34 (8.5 dBm) connects instantly.
Confirmed: bare STA and Node A both get IPs on the hotspot at 8.5 dBm.
Common factor was all boards fed from the MacBook's USB. WHOLE Fast-Roaming /
U6-Mesh investigation was a red herring (FT was the only per-WLAN config diff,
but disabling it changed nothing — the real cause was TX power all along).
Deployment on a solid supply can likely raise TX power (tune CFG_WIFI_MAX_TX_QDBM
toward 78 for range); keep it capped on the bench (MacBook-USB-powered).

**Open after the WiFi fix:** Node A trips the task-watchdog (~20 s) on the bench
because with no BMS units present, failed BLE connects back up the arbiter →
ble_owner queues and the supervisor blocks on `xQueueSend(…, portMAX_DELAY)`.
Fix = bounded/timeout sends (drop-or-log when full) so no task blocks forever.

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

1. ~~Compile each project on real IDF~~ ✅ done (esp32c3, v5.2.3, clean). BLE
   paths wired; runtime behavior still to be verified on hardware.
2. Capture real frames; fix `jk_proto.c` offsets; confirm decode parity (§14.1).
3. Fill `READ_CACHE` priming in `tunnel_srv.c send_blueprint` from the decoded
   cell/settings snapshots (left as a marker until offsets are real).
4. Complete the post-app-session **settings** re-read (currently a device-info
   poll placeholder in `arbiter.c on_app_conn`).
5. Node A harvest: capture device name/version into the NVS harvest entry
   (currently only the GATT table is filled by `ble_owner`).
