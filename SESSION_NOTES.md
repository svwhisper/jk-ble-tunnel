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
- **App <-> Node B path VALIDATED with a real phone (2026-08-28):** phone sees
  the clone `JK-B2A20S20P`, connects, Node B resolves it to identity 0
  (`app connected -> identity 0`, conns=1), and forwards notifications out to the
  phone. Fix required: identity is resolved by the connection's **our_ota_addr**
  (per-set random addr), not our_id_addr (ble_periph.c). The real JK **app**
  connects but then errors ("request device information failure") + disconnects
  because our emulator sends SYNTHETIC frames, not real JK protocol — the real
  app needs a real BMS behind Node A (or real offsets, O-1/O-2). Pipe itself is
  proven.
- **BENCH = 1 unit** (CFG_NUM_UNITS=1 both nodes) because with 4 configured but
  only 1 present, Node A wastes ~5 s/cycle scanning the 3 absent units, which
  starved the real one (arbiter "pending full" flood) and flapped the A<->B
  tunnel. Restore to JK_MAX_UNITS for production AND fix that scan-contention
  robustness first (open item). node_b tunnel_cli stack 6144->8192 + serve()
  buffers moved off-stack (was a stack-overflow crash when forwarding a NOTIFY
  to a connected app).
- Bench note: Mac is on the /23 (192.168.3.243) and reaches the broker by IP,
  but pfSense DNS (.2.1:53) doesn't answer the Mac — use the broker IP directly.
  Test-board role isn't persisted; hold it advertising with scratchpad/hold_bms.py
  (keeps the serial open so it isn't reset).

## DEPLOYED + O-1 COMPLETE (2026-08-28 afternoon)

Node A is DEPLOYED in the garage next to the four real JK units
(JK-PB2A16S20P, fw 19.31, 16S, 314 Ah EVE MB31). **Real decoded cell data is
live on MQTT for banks 1/2/3** — cells, wire resistances, pack V/I, SOC, temps —
verified against the .90 aggregator (current −0.133 A exact match).

What it took (all remote after deploy, via OTA + the new MQTT ops commands):
- Real advertised names ≠ spec guess: `BMS_0-00` / `BMS 1-01` / `BMS 2-02` /
  `BMS_3-03` (mixed underscore/space; unit 0 is a different HW: Telink OUI).
  CFG_BMS now carries the real names; CFG_NUM_UNITS=4.
- **Unit 0 PARKED** (name=NULL in CFG_BMS): connects then flaps; revisit after
  1-3 are settled. Owner will rename it (in the JK app) once settled.
- Stale bench harvest in NVS poisoned the §O-9 layout check (`layout_mismatch`
  for all real units) AND kept Node B advertising the bench name — fixed via
  new `nvsclear` command (wipe harv_* + reboot, fresh harvest).
- **Decode offsets PINNED from live captures** (jk_proto.c, BENCH-VERIFIED
  comments): JK02_32S layout, cell mask @70 drives cell_count (16S), packV@150,
  I@158, SOC@173, wire-warn@146, cycles u32@182, temps 144/162/164, settings
  trigger@26 / bal-current@78 / balancer-enable@126, device strings 22/30/46.
  `synth_frames.c` updated in lockstep; host test now includes a REAL captured
  frame as a regression (26/26 pass; run per header comment in
  tools/host_test_jk_proto.c).
- **Cell-stream trigger (fw 19.31): 0x97 then 0x96, in that order, per
  session.** Either alone yields 0x01/0x03 replies but never 0x02. Bootstrap
  pair fires on every link-up (supervisor); round-robin 0x96 = keep-alive;
  0x97 is nonce-filled (bytes 6..18) like the official app's.
- MQTT ops commands (all under jkbms/bridge/cmd/, non-empty payload, retained-
  cleared on receipt): `reboot`, `scan` (BLE scan dump -> bridge/scan),
  `rawcap <sec>` (raw notify hex -> <id>/raw; app writes always logged to
  <id>/appwrite), `nvsclear` (wipe harvest + reboot).
- Data nugget: bank 3 cell 9 wire resistance 0.106 Ω vs ~0.07 peers — outlier;
  relevant to the long-running cell-9 investigations.

## Stability crisis + resolution (2026-08-28 afternoon, commit 09bd959)

Node A died minutes after every boot (garage AND indoors). Root-caused via
USB console: task-WDT (reset_reason 6) — supervisor (the WDT feeder) was the
LOWEST-priority task and starved whenever BLE churn + streaming loaded the
single core. Full fix chain (all in 09bd959's message): supervisor prio 3->8;
arbiter failure-path rate-limit + dispatch sweep + exponential backoff;
connect timeout 9 s (> 5 s scan); **duty-cycled scans 30/100 ms** (continuous
scans starved WiFi entirely — "wifi:m f null" flood = zombie association);
MTU 256 (517 cost ~13 KB/link); tunnel payload/queue trims; QoS 0 telemetry.
Soak: 4.5 min indoors at marginal BMS range, heap flat 42 KB, min 31.5 KB,
zero resets. **Ops forensics now built in:** `jkbms/bridge/boot` (retained
reset reason) + `jkbms/bridge/health` (heap/min/rssi/up, 15 s) + gattdump cmd.

Real-unit GATT (captured via gattdump, bank 3): GAP(2a00 rw!)/1801/FFE0
(**FFE2 write-no-rsp + FFE1**)/**DIS 0x180A (9 chars)**/**Battery 0x180F**/TI
OAD f000ffc0. The clone lacks DIS/battery/FFE2/GAP-name — near-certainly why
the app pops "request device information failure". Mirror = next code task.

## GARAGE CONSOLE SESSION (2026-08-28 16:00-17:00) — THE CHIRP INVESTIGATION

Laptop + both nodes in the garage; USB consoles + mic-audio correlation +
LL event counters. Findings, in discovery order:

1. **Chirps == LL connect/disconnect events** (mic recording beep-detected and
   count-matched against conn/disc counters).
2. **The 3-strike terminate was a chirp machine** — policy now: NEVER
   terminate a healthy LL link; unarmed links are held silently and re-armed
   in place (supervisor re-sends 0x97 every 30 s, decoder sequences the 0x96).
3. **JK modules demand 16/0/600 conn params via L2CAP.** Reject -> the module
   HANGS UP (churn). Accept-then-renegotiate -> param war every ~20 s (it
   re-requests forever). Final policy: ACCEPT its params outright.
4. **JK names live only in the SCAN RESPONSE** -> passive scanning can't match
   names; and active scanning's SCAN_REQs CHIRP the units (proven: parked
   bank 3 chirped during scans that never connected to it). Fix: **PASSIVE
   scan + match by burned-in PUBLIC ADDRESS** (CFG_BMS now carries addr;
   public-only matching also makes the Node-B clone self-loop impossible —
   earlier a desk 'soak' held a proud stable link to our own clone).
5. **Supervision-timeout churn (LL reason 0x208) even at the module's own
   params**, migrating between units — resolved by the biggest finding:
6. **THE JK BLE MODULES THEMSELVES WEDGE under connect churn** and then beep
   CONSTANTLY on their own — bank 2 beeped nonstop while our BLE was fully
   OFF. Bank 2 stopped beeping without a power-cycle — whether it 'self-cleared'
   or stopped for some other reason is UNKNOWN (single observation; the
   self-clear mechanism is a guess, per owner). Bank 1 was
   power-cycled and also cleared — sufficient but possibly unnecessary.
   Operational guidance: stop all BLE activity and WAIT a few minutes first;
   power-cycle only if beeping persists. Much of the day's churn was probably
   modules in progressively degraded states.
7. **Unresolved defect:** ~45 KB transient allocation spike drove heap floor
   to **76 bytes** and crashed the node once; also a post-crash boot came up
   with BLE mysteriously enabled (near-OOM state corruption?). Root cause
   TBD; an S3 with PSRAM would absorb it regardless.

**Owner decision: port Node A to the ESP32-S3** (Lonely Binary Gold Edition,
currently the test board): dual-core kills the contention class, PSRAM kills
the heap class. The codebase already builds for esp32s3.

## NEXT SESSION PLAN
0. Power-cycle ALL FOUR BMSs first (clean, unwedged modules) — then judge.
1. Port node_a to esp32s3 (set-target, sdkconfig, partitions/OTA on 16 MB
   flash + PSRAM; malloc-heavy paths can prefer PSRAM).
2. With clean modules + all current policies (accept-params, never-terminate,
   passive-addr scan, counters), re-soak: the system may simply be stable now.
3. Chase the 45 KB allocation spike (heap tracing) if troughs persist.
4. Then: re-enable banks stepwise, Node B GATT-mirror crash fix + phone test,
   unit-0 unpark, O-2 writes.

## SUSPENDED 2026-08-28 ~15:40 — ALL UNITS PARKED, NODE A INSIDE

The owner CONFIRMED by power-off test that Node A's BLE reconnect churn was
chirping the BMS units continuously (drop/reconnect cycles FASTER than the
1 Hz link sampler — the link topics looked pinned while the chirps said
otherwise; trust the ears). Cause unresolved: either connection-management
firmware cycling links, or radio-level supervision drops (WiFi coex at hot
RSSI vs weak BMS signals through metal racks) with chirping auto-reconnects.
Node A now runs an ALL-PARKED image (CFG_BMS all NULL): WiFi/MQTT/OTA up,
zero BLE, physically inside the house.

## NEXT PHASE (bench-first — nothing runs against the batteries until proven)
1. Instrumented build: cumulative BLE connect-event counter in
   bridge/health (the number that can't lie), per-connect console log line,
   and a LISTEN-ONLY mode (scan/observe, never connect) for chirp-free
   diagnosis. Bench-verify on USB where marginal indoor range reproduces
   churn visibly.
2. Diagnose with it: the reconnect churn (chirps), the ~9 KB/min heap leak
   correlated with connect churn (min_heap dipped to 3.4 K before the last
   wedge), and the ~13 KB transient allocation spikes.
3. Fix Node B's crash-on-phone-connect in the GATT mirror code (DIS/battery/
   FFE2/GAP-name — commit ac98dda..819f51c era); B on USB gives the panic
   backtrace on the next controlled phone connect (clones of parked units
   won't advertise — un-park a bank on A briefly OR add a B-side test mode).
4. Bank 3 stream-arming failure (0x97/0x96 sequencing wasn't sufficient).
5. Un-park units incrementally, garage soak via bridge/health each step.
6. Later: unit-0 flap, O-2/O-5 writes, TX-power tuning.

Ops crib: push = tools/ota_push.py a|b --host <ip>; cmds = jkbms/bridge/cmd/
{reboot,scan,rawcap,nvsclear,gattdump}; health/boot topics are the vitals.

## Firmware OTA — push model, rollback-protected (added 2026-08-28)

Reverses the spec's original "no OTA" non-goal: Node A lives in the garage, out
of USB reach, so both nodes now self-update over WiFi.

- **Push, not pull.** The host POSTs the `.bin`; the device receives it. Chosen
  because the dev Mac isn't always up to *serve* a file (we hit exactly that in
  the WiFi chase), so a device-initiated fetch is fragile. Push only needs the
  Mac up while you're actively updating.
- **Receiver:** `components/ota/` — an always-listening `esp_http_server` on
  `CFG_OTA_PORT` **3765** (both nodes), `POST /ota`, body = raw app `.bin`
  streamed straight into `esp_ota_write` (no full-image RAM buffer). Any failure
  aborts the OTA handle; the running slot is untouched, so a dropped WiFi
  transfer can't brick a node. No auth (LAN range = physical access).
- **Rollback:** dual-slot OTA partition table (`ota_0`/`ota_1`, 1984 KB each on
  the 4 MB flash) + `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. A pushed image boots
  PENDING_VERIFY and is confirmed by `ota_mark_valid()` **only after WiFi is up**
  (end of `app_main`). A build that crashes or can't join WiFi never confirms →
  bootloader auto-reverts on next reset. Narrow downside: if the AP is down >20 s
  at the exact moment of a fresh push, a good image also reverts (safe — lands on
  the previous good image).
- **Host tool:** `tools/ota_push.py a|b` — validates the `.bin` (0xE9 magic),
  preflights DNS + port, pushes via `curl --fail-with-body`, then waits for the
  node to drop and re-open :3765 to confirm the new image booted. `--host <ip>`
  when pfSense DNS won't resolve the name from the Mac (common here).
- **⚠️ One-time USB flash required before deploy.** The partition-table change
  can't go over the air — the OTA-enabling image must be flashed over USB on the
  bench *before Node A leaves for the garage*. After that, updates are OTA.
- Both projects build clean on this layout (node_a 1.25 MB / 38 % slot free,
  node_b 1.08 MB / 47 % free).
- **HARDWARE-VALIDATED end-to-end 2026-08-28.** Both nodes USB-flashed with the
  OTA-enabled image (IPs held: A 192.168.3.241, B 192.168.3.234; both listen on
  :3765). A live `ota_push.py a` pushed 1.25 MB → Node A wrote it to **ota_1**,
  rebooted, rejoined WiFi, confirmed the image (rollback cancelled), and reopened
  :3765 in 7 s. Node A now runs from ota_1; next push ping-pongs to ota_0. Nodes
  stable, no boot loop. From here on, updates are `tools/ota_push.py a|b`
  (use `--host <ip>`; pfSense DNS won't resolve the node names from the Mac).

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
