# JK BMS BLE Tunnel — Implementation Spec (rev 4, as-built)

**Target:** ESP-IDF (FreeRTOS), two ESP32 nodes on one LAN
**Status:** implemented and deployed (2026-08-28/29); this revision corrects the design-era text to match the built system (link pool, chip choices, MTU, write-op selection, protocol findings). The document stays self-contained; `SESSION_NOTES.md` carries the operational findings and their evidence. Rev 2 incorporated a design review (single shared GATT table on Node B, tunnel write results + resync, crash-safe measurement mode, NVS-persisted harvest, MTU handling). Rev 3 closed the seams a second pass found: measurement × app arbitration, a tunnel grace window, concurrent-connection enforcement, an unreachable-probe floor, incremental harvest, write-FIFO hygiene.
**Reference implementation to port from:** `syssi/esphome-jk-bms` (`components/jk_bms_ble/`). Do not reimplement the JK frame decode from scratch; lift and adapt it. This spec tells you *what to build around it*.

---

## 1. Goal

Let an **unmodified JK BMS iOS app** connect to and configure JK BMS units that are physically out of Bluetooth range, and simultaneously give **house automation (any MQTT client)** read access to all BMS values plus write access to **balance parameters only**. Each BMS accepts exactly one BLE client at a time, so a single owner must arbitrate.

Two consumers, one radio link per BMS:
- **iOS app** — full function, must believe it is talking to the real BMS.
- **automation** — read-all, write-balance-only, via MQTT (a Mosquitto broker; the consumer is whatever the site runs — Node-RED, Home Assistant, scripts — this code only speaks MQTT).

**Four BMS units are in scope.** The house-side node presents all four to the app at once; the operator selects one by connecting to it in the app, and switches by disconnecting and connecting to another — using nothing but the app.

### Non-goals (owner decisions — do not add these back)

- **No security layer.** No tunnel authentication or encryption, no MQTT ACL requirements beyond broker user/pass, no BLE pairing/bonding on Node B. Rationale: remote off-grid residence — anyone within BLE or LAN range already has physical access to the equipment. Note the §10 whitelist is a **safety** measure (limits the blast radius of automation bugs), not a security control; it stays.
- **Node-firmware OTA is push-model and rollback-protected** (`components/ota/`,
  always-listening `POST /ota` on `CFG_OTA_PORT` 3765; host tool
  `tools/ota_push.py a|b --host <ip>`). Added 2026-08-28 when Node A moved to
  the garage out of USB reach (reversing the original no-OTA stance). No
  security on it, consistent with the item above. The one-time OTA-enabling
  flash (partition-table change) is over USB; everything after is OTA.
- **No BMS firmware updates through the tunnel** (§11 — safety rule, stays). Node
  OTA above is the two ESP32s' *own* application firmware only; it never touches
  BMS firmware and never routes through the tunnel.

## 2. Physical topology

- **Node A (garage):** BLE central. Sole owner of the links to the BMS units (deployed fleet: 4× **JK-PB2A16S20P**, fw 19.31, 16S). Also MQTT client and TCP tunnel server. Powered continuously near the battery. **This node is unavoidable** — it must be within BLE range of the batteries. **Chip: ESP32-S3 with PSRAM** (as built: S3, 16 MB flash, 8 MB octal PSRAM → ~8 MB heap). An ESP32-C3 was tried first and proved marginal: BLE churn + streaming produced ~45 KB transient allocation spikes against a ~100 KB free heap, and on the single core the watchdog-feeding supervisor starved under load. The S3's PSRAM and second core bury both failure classes. Exclude the ESP8266 (no BLE) and ESP32-H2 (no WiFi). Prefer an external-antenna board if the BMS units sit in a metal enclosure or more than ~2 m away — antenna matters more than variant for reliability out at the battery.
- **Node B (house):** BLE peripheral. Advertises **four** connectable identities (clones of the four BMS units), one per real unit. The app connects to whichever it wants. TCP tunnel client to A. **Must be a BLE 5.0 chip — ESP32-C3 / S3 / C6 — not the original ESP32-WROOM (BT 4.2).** Four simultaneous connectable advertising sets require BLE 5.0 multiple-advertising-sets support.
- Both on the same LAN. Added round-trip latency ≈ one BLE connection interval on each side + a few ms of LAN — tens of ms per GATT round trip. Acceptable for the app and for notifications.

## 3. Architecture overview

```
  BMS1 ┐
  BMS2 ┤ ═BLE═► Node A ══TCP(LAN)══► Node B ═BLE═► iOS app (sees 4 devices)
  BMS3 ┤          │
  BMS4 ┘          └══MQTT══► Mosquitto ══► automation (reads all 4, writes balance)
```

Node A holds a BLE central link to **all four units concurrently** (4-slot link pool, `NIMBLE_MAX_CONNECTIONS=4` on the S3) and multiplexes them. Every BMS interaction — from the app (relayed via B, tagged with which identity) or from automation (via MQTT) — is funnelled through a per-BMS command queue with a strict response-or-timeout gate, because the JK protocol is request/response framed and interleaving corrupts its state machine.

---

## 4. Node A — task design (FreeRTOS)

Use ESP-IDF with NimBLE (central role). Suggested tasks, queues, and sync primitives:

### Tasks

| Task | Priority | Core | Responsibility |
|---|---|---|---|
| `ble_owner` | high | 0 (radio) | Owns NimBLE central. Connects/disconnects BMS units, writes command frames (ATT write op chosen per the char's **discovered properties** — FFE1 vs FFE2 write types differ between the two radio-module variants in the fleet, §8), receives notifications, **reassembles frames** (by length + checksum). The ONLY task that touches BLE. Manages the 4-slot pool of concurrent central links. |
| `arbiter` | med | 1 | Serialises requests **per BMS** through per-unit FIFOs. Enforces priority and the response/timeout gate. Tracks which identity the app currently holds. |
| `mqtt` | med | 1 | Mosquitto client. Subscribes to command topics, publishes decoded state per BMS. Translates MQTT ↔ arbiter requests. Registers an LWT (§7). |
| `tunnel_srv` | med | 1 | TCP server for Node B. Relays app GATT operations ↔ arbiter (tagged by identity), streams notifications outbound. **Single client: a new connection replaces the old one** (handles half-open sockets after a B reboot). |
| `decoder` | low/med | 1 | Receives **completed, reassembled frames** from `ble_owner` via queue — decode never runs on the BLE callback stack. Parses into per-BMS structs; publishes to the state cache. Must tolerate unknown/unsolicited frame types (the app can trigger e.g. logbook `0xA1` responses). |
| `supervisor` | **high (8 — above every worker)** | 1 | Watchdog feeder, reconnect pacing, harvest coordination, health metrics, **measurement-mode restore guard (§9)**. Priority is load-bearing: as a low-priority task it starved under BLE + streaming load and the task WDT reboot-looped the node — the WDT feeder must outrank the work it supervises. |

Node A runs SNTP (NTP server configurable) so `last_seen` and log timestamps are wall-clock.

### Queues / sync

- `q_bms_request` — arbiter → ble_owner. Items: `{bms_id, cmd_id, opcode, payload, source, response_needed, timeout_ms}`.
- `q_bms_response` — ble_owner → arbiter. Items: `{bms_id, cmd_id, status, frame*}`.
- `q_notify` — ble_owner → (tunnel_srv, mqtt). Fan-out tagged with `bms_id`. Copy or ref-count buffers; never share a mutable buffer across tasks. Size the pool for ~300-byte frames at ~1 Hz × 4 units (e.g. 8 × 512 B is ample).
- `mtx_link_pool` — guards the central-connection pool state.
- `evt_link` — event group: per-BMS `BMS_UP[n]`, plus `APP_ACTIVE`, `MQTT_UP`, `TUNNEL_UP`.
- State cache: per-BMS decoded telemetry + settings, single-writer or mutex-guarded snapshot reads.

### Harvest & persistence (NVS)

- **First boot: harvest incrementally — never block commissioning on all four units being alive.** As each unit becomes reachable: connect, read the full GATT table + device-info frame (name, frame version, firmware version), disconnect, store to **NVS**, and bring that identity online at B. **Verify layout identity against the first harvested table** — the single-shared-table design on Node B (§5) depends on it. (Frame version, O-1, is about payload offsets, not the attribute table — a mixed JK04/JK02 fleet will typically still pass this check.)
- **Every later boot:** serve from NVS immediately — B gets its blueprint with no dependency on any unit being awake or reachable.
- **Re-harvest trigger:** on every real-link bring-up A reads the device-info frame anyway; if the firmware version differs from NVS, re-harvest that unit's table and re-verify layout identity.
- **Layout mismatch: warn and ACCEPT** (publish `layout_differs_accepted` on the unit's ack topic, keep serving it). The original exclude-on-mismatch policy was wrong in practice: B's clone table has been static since the GATT-mirror landed, so per-unit differences are harmless — and unit 0's radio module legitimately differs in characteristic *properties* (not UUIDs) from the other three, which the exclude rule kept punishing. The phone app works identically against all four.

### Connect-on-demand + link pool

Per-unit reachability is a tri-state, carried to B in `LINK` frames: **0 = unreachable** (connect attempts failing, backoff running), **1 = reachable-idle** (no link held; normal state), **2 = link-up**. At boot, units start **reachable-idle** — optimistic; the fail-loud connect path corrects it. **Recovery from unreachable is supervisor-owned and independent of MQTT:** the supervisor probes every unreachable unit at `REACHABILITY_PROBE_S` (default 60 s) indefinitely — otherwise an unreachable unit's identity never advertises again and the operator can't even try it from the app.

- The pool holds **all four units** — no slot contention, no round-robin of link ownership. A unit's BLE module sleeps its radio ~25 s after the last received command (its own notify stream doesn't count), so links to non-streaming units drop on supervision timeout and are re-raised by paced reconnects; a module that latches stay-awake streams indefinitely.
- When the app connects to clone #N (signalled by B over the tunnel), an **app-attach driver** raises the real link within ~5 s, bypassing any reconnect hold, and re-raises it if the module dozes mid-session. The app shows connecting, then populates.
- **App writes arriving before the link is up are queued** (bounded FIFO, flushed in order on link-up). If the link isn't up within `APP_LINK_TIMEOUT_MS` (default 10 s), mark the unit unreachable, send `LINK 0`, and B terminates that app connection (§5) — the operator sees a disconnect, not a zombie session.
- **When the app disconnects from a unit**, A performs a settings + cell-info re-read: the operator may have changed anything in the app, and retained MQTT settings / B's read cache must not go stale. Republish `state/settings` and push `READ_CACHE`.
- **No keep-alive polling, ever.** The BMS piezo-acks EVERY command frame it receives, so a streaming unit must hear nothing; the 0x02 cell stream itself is the health signal. Commands are sent only to arm/re-arm a session (the §8 opener) and for app traffic. Link vitals (RSSI) come from the local controller — zero on-air cost.
- Idle-disconnect is **effectively disabled** (`IDLE_DISCONNECT_MS` = 24 h): with a 4-slot pool holding every bank, freeing a slot after an app session just causes a pointless disconnect + reconnect chirp.
- Session-end reconnect pacing is 3-regime: a stable session (≥10 min) reconnects promptly; a short session that streamed waits a fixed 300 s; a short *silent* session escalates 60 s→10 min — never hammer a struggling module. App traffic bypasses all holds.
- **Connection parameters: accept the module's own L2CAP request outright** (it demands 20 ms interval / latency 0 / 6 s supervision timeout). Rejecting makes it hang up; renegotiating starts a parameter war. The design-era "request 30–50 ms" guidance is obsolete.

---

## 5. Node B — task design (single ESP32, four identities)

### One GATT table, four identities

A BLE peripheral has **one attribute table shared by every connection — per-connection GATT tables do not exist** in NimBLE or any stack. Registering four replica services would expose four copies of `0xFFE0` to every client, and the app would grab the first `0xFFE1` it finds — wrong unit.

Because all four JK units expose an identical layout (verified at harvest, §4), Node B registers the replica table **once**. Identity comes from advertising, not from the table:

- Four extended-advertising sets, each with that unit's advertised name and a **distinct static-random address**.
- On connection, resolve which set it landed on → identity → `bms_id`. All traffic on that connection handle routes to that unit.
- **Four per-identity read caches** back the single table — a read is answered from the cache of the identity the connection belongs to.
- The GAP Device Name characteristic is global to the stack — set it to something generic (`JK-Tunnel`); the app identifies units by the per-set advertised name, which is what its scan list shows.

| Task | Responsibility |
|---|---|
| `tunnel_cli` | TCP client to Node A. Receives the shared table, four identities, read-cache primes, and notifications (tagged `bms_id`); sends app writes and connect/disconnect events tagged with the identity they arrived on. Auto-reconnect; resync per §6. |
| `adv_mgr` | Maintains the four advertising sets as **legacy-format connectable** advertisements (even on the 5.0 controller — maximises app compatibility). A set advertises only while its unit is reachable (`LINK ≥ 1`) **and** the tunnel is up; resumes after its connection ends. |
| `ble_periph` | Registers the single replica table. Routes per-connection traffic by identity. Answers reads from that identity's cache. Forwards writes over the tunnel. **Keeps CCCD state locally per connection and filters notification forwarding with it** — there is no subscribe message on the tunnel (§6). |
| `supervisor` | Watchdog, cache-invalidation, RAM/connection-slot monitoring. |

### Selection model (nothing but the app)

- All reachable units are visible in the app's scan list by their four distinct JK names. **An unreachable unit's set stops advertising** — the scan list truthfully reflects what can actually be reached (config flag `ADVERTISE_WHEN_DOWN` to override; default off).
- The app connecting to identity #N **is** the selection. `ble_periph` maps the connection → identity → `bms_id` and sends `CLIENT{bms_id, connected:1}` to A, which brings up the real link to BMS #N.
- Switching = the operator disconnects in the app and connects to a different name. On disconnect, that set resumes advertising and A is told `CLIENT{bms_id, connected:0}`.
- If A reports the bring-up failed (`LINK 0` for that unit, or write results indicate a dead link — §6), **B terminates the app connection and stops advertising that set** until it's reachable again. Fail loud; never leave the operator staring at a hung session.
- Default policy: **one app connection at a time** across the four identities. The app is inherently single-BMS in view, so this matches usage; keeping the other three *advertising* (not connected) while one is connected is the required behaviour — validate advertise-while-connected holds for 1 connection + 3 adv sets on the target chip. **Enforcement:** because the other three stay connectable, a second concurrent connection (second phone, second app instance) is possible — BLE has no reject primitive, so B **accepts it and immediately terminates it**; the first session stays authoritative.

### iOS-specific requirements

- **Do NOT clone BMS MACs.** iOS CoreBluetooth identifies peripherals by a system UUID, never MAC. The app matches on **advertised name + service UUID**, both replicable. Four distinct names → the app distinguishes the four naturally. Distinct (random) addresses per set mean clean, independent iOS GATT caches — avoiding the attribute-handle-matching problem that breaks MAC-cloned relays.
- **iOS GATT cache is sticky.** If the replica table changes between builds, rotate the sets' random addresses on boot and/or use Service Changed. Document a "toggle Bluetooth on the phone" fallback.
- **Advertising-format compatibility (open item O-7):** confirm the JK iOS app surfaces the four sets. Prefer legacy connectable PDUs per set for safety.

---

## 6. Tunnel protocol (A ↔ B)

TCP, LAN-only, `TCP_NODELAY` on. Length-prefixed binary frames. Frames carry a `bms_id` where identity matters; `0xFF` = link-level / identity-independent:

```
[u8 type][u8 bms_id][u16 len little-endian][payload]
```

| type | dir | payload | notes |
|---|---|---|---|
| `TABLE` | A→B | char count, per-char {svcUUID, chrUUID, props} | the **shared** replica blueprint; `bms_id 0xFF` |
| `IDENT` | A→B | advertised name | one per unit; also implies the identity exists |
| `READ_CACHE` | A→B | `[u8 idx][data]` | primes/refreshes one identity's read cache |
| `NOTIFY` | A→B | `[u8 idx][data]` | subscription data, BMS→app; opaque byte chunk |
| `WRITE` | B→A | `[u8 idx][u8 withResponse][data]` | app write → arbiter, for this bms_id |
| `WRITE_RESULT` | A→B | `[u8 idx][u8 status]` | 0 ok, 1 timeout, 2 link-down, 3 GATT error. Writes are serialised per identity (one outstanding), so results correlate FIFO |
| `LINK` | A→B | `[u8 state]` | per-BMS tri-state: 0 unreachable / 1 reachable-idle / 2 link-up |
| `CLIENT` | B→A | `[u8 connected]` | app connected/left this identity → drives connect-on-demand + arbitration |
| `TABLE_REQ` | B→A | — | B asks for blueprint + state after (re)connect |
| `PING` | both | — | keepalive; `bms_id 0xFF` |

**(Re)connect resync sequence:** B connects → sends `TABLE_REQ` → A replies `TABLE`, `IDENT` ×4, `LINK` ×4, `READ_CACHE` primes → **B replays `CLIENT` for any identity that currently holds an app connection** (a tunnel blip mid-session must not strand A's arbitration state).

**Keepalive & grace window:** `PING` every 5 s each way; declare the peer dead after 15 s of silence (a socket error counts immediately). On tunnel loss B does **not** drop app connections at once: it holds them for `TUNNEL_GRACE_MS` (default 8 s) while reconnecting, so a WiFi hiccup is invisible to the operator — this is what makes the `CLIENT` replay in the resync sequence meaningful. App writes arriving during the grace window are queued (small bounded FIFO) and flushed after resync completes; notifications missed during the outage are simply lost — the stream resumes. Beyond the grace window, B drops app connections and pauses all advertising until reconnected (§11). A tears down app-driven state on peer death and accepts the next connection (which replaces any half-open old one).

**Reads are answered locally by B from the identity's cache.** The reason is *not* ATT timing (ATT allows ~30 s per transaction) — it's that **NimBLE's GATT access callback is synchronous on the host task**: blocking it on a LAN round trip stalls the entire BLE stack. Safe for the JK app, which reads rarely and is notification-driven. B keeps each cache current from `READ_CACHE` pushes (sent at prime time, after the post-app-session settings re-read, and after write readbacks) and from `NOTIFY` traffic for the notifying characteristic. `idx` is the characteristic's position in `TABLE` order, on both sides. (The `0xFFE1` cache entry ends up holding the last stream chunk — semantically meaningless, and harmless: the app never reads it.)

**Writes:** the same synchronous-callback constraint means B must complete the ATT write (both with- and without-response) immediately, then forward over the tunnel. ATT-level errors therefore cannot reach the app — acceptable, because the JK app confirms operations at the frame level via notifications, not via ATT status. `WRITE_RESULT` exists for observability and failure policy: on `link-down`, or `WRITE_FAIL_LIMIT` consecutive failures, B terminates that app connection (§5) instead of silently eating writes. **Hygiene:** the per-identity result-correlation FIFO and the consecutive-failure counter are cleared on every `CLIENT` transition and on tunnel resync — stale results from a previous session must never mis-correlate to, or kill, a fresh connection.

**MTU / fragmentation:** A requests ATT MTU **256** from each BMS — the reference implementation's 517 costs ~13 KB of heap per central link and buys nothing (the PB2A16S20P notifies in ≤128 B chunks). B accepts whatever the iOS side negotiates (typically 185–527). JK frames (~300 B) span multiple notification chunks; `NOTIFY` payloads are opaque byte chunks, and **B re-chunks to ≤ (app MTU − 3)** when forwarding. Chunk boundaries don't matter — the app reassembles a byte stream by header + length.

---

## 7. MQTT contract (A ↔ automation)

Base topic per unit: `jkbms/<bms_id>/`  (bms_id ∈ the four units)

> **Scope note:** cell voltages and pack summary duplicate the existing aggregator API (`192.168.3.90/api`) and the `bms-history` archive on `.5`, which remain canonical for voltage history. The data that is genuinely BLE-only — **wire resistance, the offset-114 warning bitmask, balance settings, and the balance write path** — is the point of this contract. Keep publishing `state/cells`/`state/summary` (they're nearly free), but automation should not grow a second source of truth from them.

### Bridge-level (Node A)

| Topic | Payload | Retained |
|---|---|---|
| `jkbms/bridge/status` | JSON `{online, boot_time, ...}`; **registered as MQTT LWT** so a dead Node A reads offline, not stale | yes |

A publishes a fresh `bridge/status` on every boot — automation can use it to detect restarts (queued deferred writes are lost across reboots, §10).

### Published by Node A per unit (retained where noted)

| Topic | Payload | Retained |
|---|---|---|
| `state/cells` | JSON: array of `{n, v, r}` (voltage, wire resistance Ω; see §9) | no |
| `state/summary` | JSON: pack V, current, power, SOC, temps, cycle count, balancing bool, charging/discharging bool | no |
| `state/settings` | JSON: current balance parameter values (from settings frame) | yes |
| `state/faults` | JSON: decoded error bitmask + wire-resistance warning bitmask (offset 114) | yes |
| `state/link` | JSON: `{reachability, app_connected, last_seen}` | yes |
| `state/meas` | JSON: last clean resistance measurement + conditions (SOC, balance current, balancing state) | yes |

### Subscribed by Node A

| Topic | Payload | Action |
|---|---|---|
| `cmd/balance/set` | JSON: named balance params (see §10), optional `id` | validated write, balance-only |
| `cmd/measure` | JSON: `{trigger:true}`, optional `id` | run measurement-mode sequence (§9) |
| `cmd/refresh` | optional `id` | force settings + cell-info read. If the app holds that unit: cell info can be answered from the passive app-session stream, but a settings read cannot (the passive stream only carries what the app requests) — the settings half is acked `deferred_app_active` and satisfied by the post-session re-read (§4) |

Results acked on `jkbms/<bms_id>/ack` with `{cmd, id, status, detail, readback}` — `id` echoed verbatim for correlation when multiple commands are in flight.

---

## 8. JK BLE protocol notes (as measured on the JK-PB2A16S20P fleet, fw 19.31)

- Service UUID `0xFFE0`, characteristics `0xFFE1` (notify + write — the BMS→client stream and our poll channel) and `0xFFE2` (the official app's command channel).
- **Characteristic write types vary by radio module, within one BMS model.** Three of the four PB2A16S20P units carry one module family (OUI C8:47:80): FFE1 accepts Write Request, FFE2 accepts Write Command. The fourth carries a Telink-OUI module (Nordic Secure-DFU service 0xFE59 on board) with the types **inverted**: FFE1 = write-no-rsp only, FFE2 = write-with-rsp only. ATT silently drops a mismatched write op, so the client must select the op from each characteristic's discovered properties — never hardcode it.
- Commands (20-byte frames `AA 55 90 EB` + cmd + len + value + tail + 8-bit-sum checksum): `0x97` device info, `0x96` cell info, `0x6C` **set-RTC** (u32 LE seconds since 2020-01-01 00:00 AEDT in the official app's frames; the tail bytes are recycled app buffer garbage), `0xA1` logbook.
- **Session opener (fw 19.31):** the 0x02 cell stream starts only after `0x97` then `0x96` land in that order in one session; the official app follows with `0x6C` — its full opener trilogy goes down FFE2. There is **no auth anywhere on this path**; the app's PIN never appears on the wire.
- **The BMS piezo-acks every command frame received** — design every client to go silent once streaming.
- Frame versions in the reference taxonomy: `JK04 (0x01)`, `JK02_24S (0x02)`, `JK02_32S (0x03)`. The PB2A16S20P fw 19.31 cell-info frames decode with the JK02_32S-style layout — but the shipped offsets were **pinned from live captures** of this fleet (cell mask @70, pack V @150, I @158, SOC @173, wire-warn @146; see `jk_proto.c`) and cross-checked against an independent RS485 aggregator, not assumed from the reference.
- Min response ~300 bytes; frames span multiple notifications — reassemble by length + checksum before decode. Tolerate interleaved junk: the Telink-module unit emits an `AT\r\n` heartbeat (~7/s, even while streaming) and short pack-voltage ticker frames on FFE2 notify; the reassembler discards both.
- Cell wire resistances and the **per-wire warning bitmask at byte offset 114** (one bit per cell/wire) are in the cell-info (`0x96`) frame. "Wire resistance" is bit 0 of the main error bitmask.
- Balance parameters come from a **separate settings read**, not cell-info. The reference component exposes `balance_trigger_voltage` (writable) and `balancing` (state bit) — port both the settings parse and the write-frame builder.

> **Implementer action (still open with the write path):** extract the exact settings-frame offsets and write-frame format from the reference source and verify against captures before enabling writes. Read/stream sessions carry no auth at all (measured); whether writes require the PIN on this firmware is unverified (O-5). Do not guess register layouts — this is a 60 kWh protection device.

---

## 9. Wire-resistance handling (critical — affects read and measure paths)

1. **Zero means "measurement failed," not "excellent."** The BMS zeroes all resistances before repopulating successful ones and does not display abnormally-high values. So:
   - Alarm on `r == 0` **and** on the offset-114 warning bitmask — never on `r > threshold` alone.
   - Store `0` as **null** in the time series.

2. **It's a bus-bar monitor too.** Computed indirectly through bus bars and cells, so a poor cell-terminal/bus-bar joint shows here — high-value telemetry given long high-current runs into a busbar. Good joints sit well under ~0.08 Ω; original crimped harnesses can read 0.25–0.30 Ω.

3. **Read and write are coupled.** The BMS derives lead resistance by comparing cell voltage with vs. without the set balance current. At high SOC the voltage to push balance current inflates apparent resistance; near full charge deltaV collapses to ~0, yielding zeros. **The balance-current parameter automation writes is the same parameter that decides whether the resistance metric is valid.**

### Measurement-mode sequence (`cmd/measure`)

**Crash safety first:** if Node A dies between steps 2 and 5, the BMS is stranded at a 0.3 A balance floor and balancing silently degrades. So the sequence is wrapped:

- Before step 2, persist `{measure_in_progress, bms_id, saved_settings}` to **NVS**. Clear it after a successful restore.
- **At boot, if the record exists:** restore the saved settings first thing, publish an alert (`measure_restored_after_reboot`) on the unit's `ack` topic, then clear the record.
- The supervisor enforces `MEAS_TIMEOUT_MS` (default 120 s): expiry always restores the saved settings, whatever state the sequence is in. Config load asserts settle interval < `MEAS_TIMEOUT_MS`.
- The `cmd/measure` ack includes the restore status; `restore_failed` is a loud error, never silent.
- **App arbitration:** `cmd/measure` is refused with `deferred_app_active` while the app holds that unit. If the app connects mid-measurement, the measurement **aborts and restores the saved settings before the app session proceeds** (the ~5 s link bring-up absorbs this) — otherwise step 5's restore would later clobber whatever the operator changed in the app.

1. Record current balance settings.
2. Set balance current low (reference floor ~0.3 A); ensure balancing enabled.
3. OPTIONAL re-init (reference trick: toggle cell count e.g. 16→17→16) — gate behind a config flag; verify on target firmware before enabling.
4. Wait a settle interval; capture a clean cell-info frame.
5. Restore original settings.
6. Publish resistances **with** the SOC / balance-current / balancing-state they were captured under (`state/meas`).

Trend only samples captured under comparable conditions. Log SOC + balance current + balancing state with every sample regardless.

---

## 10. Balance-parameter write path (the sensitive part)

**Scope: balance parameters ONLY.** No charge/discharge MOSFET, no protection thresholds, no current limits, no cell count (except transiently inside measurement mode if O-4 permits).

### Whitelist (reject anything else with `status: rejected_out_of_scope`)
- `balance_trigger_voltage` (V)
- `balance_current` / balance current limit (A)
- `balancing_enabled` (bool)
- (add only balance-related settings confirmed writable on target firmware)

### Validation pipeline (every write)
1. **Whitelist check.**
2. **Range clamp** — per-key `[min,max]`; reject out-of-range (don't silently clamp), log.
3. **Auth handshake** — replay login before the write (PINs seen in the wild: 1234 / 123456; real one held in Node A config, §12).
4. **Serialise through the per-BMS arbiter** — never interleave with an app exchange on that unit.
5. **Write, then read back** — re-read settings, confirm; publish `readback`, **update retained `state/settings`, and push `READ_CACHE` to B** so the app never reads a stale value. On mismatch: `status: readback_failed`, no blind retry.
6. **Rate limit** — default: one balance write per charge cycle unless overridden; coalesce rapid writes.

### Arbitration policy (default)
- **App connected to that unit → automation writes BLOCKED**, acked `status: deferred_app_active`, optionally queued (config flag) to apply on app disconnect. Human-in-app takes precedence.
- **The deferred queue is RAM-only and is lost on reboot** — deliberately: a stale queued write applying after a restart is worse than a dropped one. automation detects restarts via `bridge/status` and re-issues if still wanted.
- App reads/telemetry continue regardless.
- Two loops on one variable: if automation writes balance settings while DVCC also curtails, note both act — reconciliation is out of scope, but log both in the ack.

### False-alarm awareness
Some firmware emits false balance-wire-resistance warnings. `state/faults` exposes the raw bitmask so automation applies its own debounce rather than the firmware forcing an alarm.

---

## 11. Failure modes & watchdog

- **BMS unreachable:** tri-state per §4 — backoff reconnect on A; B **stops advertising that identity** (default; `ADVERTISE_WHEN_DOWN` flag to override) so the scan list reflects reality; if the app was connected, B terminates that connection. Publish `state/link`.
- **Tunnel down:** B holds app connections for `TUNNEL_GRACE_MS` while reconnecting (§6 — short blips are invisible to the operator); beyond that it drops them and **pauses all advertising** until the tunnel is back — an advertised clone that can't reach its BMS is a lie. Both sides auto-reconnect; full resync per §6. A keeps the MQTT path alive throughout.
- **MQTT down:** A keeps serving the app; bounded queue / drop-oldest for state. Never block the arbiter on MQTT.
- **Arbiter timeout guard:** every request has a hard timeout that frees the link and logs. No request holds a link indefinitely.
- **Task watchdog (TWDT):** all long-running tasks subscribed; panic-reboot on stall. The measurement-mode NVS guard (§9) makes a mid-measurement panic safe.
- **Never route a BMS firmware update through this tunnel** — a dropped frame mid-flash can brick the unit. Firmware ops require a direct in-range BLE connection. State in operator docs.

## 12. Config

Site-specific values (WiFi, MQTT broker, BMS PIN, tunnel host, **and the fleet table** — per-unit advertised name, `bms_id`, public address, count) live in one git-ignored file, `components/secret/secret.h` (`FLEET_BMS_TABLE`; documented in `secret.h.example`). **Unit selection is by burned-in public address with passive scanning** — JK names appear only in scan responses, and active scanning's SCAN_REQs chirp the units' piezos; address-only matching also forecloses ever mistaking Node B's clones (static-random addresses) for real units.

**Node A:** `IDLE_DISCONNECT_MS`; `APP_LINK_TIMEOUT_MS`; `REACHABILITY_PROBE_S`; reconnect-pacing constants (hold base/cap, stable-session threshold); per-key balance `[min,max]`; rate limit; arbitration mode (block/queue); measurement settle time + `MEAS_TIMEOUT_MS` (settle < timeout, asserted at config load); cell-count-toggle flag (default off); tunnel PING interval/timeout. (No connection-interval config — the module's own L2CAP-requested parameters are accepted outright, §4.)
**Node B:** WiFi; Node A host/port; four advertised identities (default: harvested names); advertising format (legacy connectable, default); `ADVERTISE_WHEN_DOWN` (default off); `TUNNEL_GRACE_MS`; random-address rotation flag; `WRITE_FAIL_LIMIT`; max connection slots.

## 13. Build / toolchain

- ESP-IDF **v5.2.3** (pinned). NimBLE. **Node B on a BLE 5.0 target** (as built: ESP32-C3) for four advertising sets. **Node A: ESP32-S3 with PSRAM** (§2 — a plain C3 proved marginal under load).
- Two firmware images (`node_a`, `node_b`) sharing a common `jk_proto` component adapted from `syssi/esphome-jk-bms`, with the decode offsets re-pinned from live PB2A16S20P captures — single source of truth for frame logic, host-native regression test in `tools/`.
- Node B registers **one** replica GATT table (§5) — no per-identity GATT servers. Limits that matter: `NIMBLE_MAX_CONNECTIONS=4` on A; on B, the ext-adv instance count (≥4) **and the controller activity budget** — each CONNECTABLE adv set costs **2** activities, so four clone sets need `BT_CTRL_BLE_MAX_ACT` ≥ 8 (as built: 10; the default 6 fit exactly three sets and the fourth failed `ext_adv_configure` with HCI 0x07 Memory Capacity Exceeded).
- B's four static-random clone addresses are minted once and persisted in NVS — phones dedupe scan lists by address, so per-boot random addresses turn every B reboot into four "new" devices plus stale ghosts.

## 14. Test plan

1. **Decode parity:** captured frames through `jk_proto` vs ESPHome output must match, incl. offset-114 bitmask — for every frame version present in the fleet.
2. **Four-identity visibility:** app scan lists all four by name; each connects and shows correct live data for the right unit (routing check for the shared-table design — deliberately verify unit N's data never appears under identity M). A second concurrent connection from another phone is accepted-then-terminated; the first session is unaffected.
3. **Switch:** disconnect in app, connect to another; previous set resumes advertising; new unit's real link comes up within ~5 s (app-attach driver); no leaked BLE slots.
4. **Read path:** an MQTT client sees per-unit cells/summary/settings/faults; failed resistances are null not 0.
5. **Write path:** `cmd/balance/set` on a bench BMS changes value; readback confirms and updates `state/settings` + B's cache; out-of-scope + out-of-range rejected.
6. **Arbitration:** app on a unit → automation write deferred/blocked per policy; app writes unaffected. App changes a setting then disconnects → post-session re-read updates retained settings.
7. **Measurement mode:** captured resistances plausible; settings restored; conditions published.
8. **Measurement crash-restore + abort:** power-cycle Node A mid-measurement → boot restores saved balance settings and publishes the alert. Connect the app to that unit mid-measurement → measurement aborts and settings are restored before the app session proceeds.
9. **Tunnel drop mid-session:** (a) blip shorter than `TUNNEL_GRACE_MS` → the app session survives; resync (`TABLE_REQ` → state → `CLIENT` replay) reconciles A and grace-queued writes flush. (b) Longer outage → B drops the app + pauses advertising; on reconnect, full resync and normal operation resumes.
10. **MTU re-fragmentation:** with a test client negotiating a small MTU, verify a full ~300 B cell-info frame reassembles correctly through B's re-chunking.
11. **Cold start from NVS:** boot A + B with all BMS unreachable → B builds from the persisted blueprint, advertises nothing; power a BMS on → its identity appears (the supervisor probe alone must bring it back; there is no polling).
12. **Resilience:** kill tunnel / MQTT / each real link independently; confirm degraded behaviour + clean recovery; `bridge/status` LWT flips on A death.
13. **Concurrency ceiling:** app on unit 2 while units 1/3/4 stream to MQTT; confirm link-pool behaviour and no starvation.
14. **Soak:** 72 h with periodic app connects/switches, continuous MQTT streaming, forced disconnects; no TWDT resets, no slot leaks.

## 15. Open items

- **O-1** *(resolved)* All four units are JK-PB2A16S20P, fw 19.31, 16S; their cell-info frames decode with the JK02_32S-style layout, offsets pinned from live captures of this fleet and cross-checked against the independent RS485 aggregator (§8).
- **O-2** *(open)* Extract exact settings-frame layout and balance-write frame format; verify against captures before enabling writes (`JK_ENABLE_WRITES` is a hard compile-time gate, currently 0).
- **O-3** *(resolved)* Four identities on one Node B, app-driven selection, no control surface — this spec, verified in production.
- **O-4** *(open)* Whether measurement mode may toggle cell count on this firmware. Default OFF until verified.
- **O-5** *(open)* Whether balance writes require the PIN on this firmware revision — read/stream sessions carry no auth at all (measured; the PIN never appears on the wire).
- **O-6** *(open)* Verify no false-alarm firmware quirk before wiring any hard alarm to the resistance warning.
- **O-7** *(resolved)* The JK iOS app surfaces all four sets (legacy connectable PDUs) and advertise-while-connected holds — verified in daily use.
- **O-8** *(resolved)* On the C3 controller each connectable adv set costs 2 activities: `BT_CTRL_BLE_MAX_ACT=10` as built (§13). RAM headroom fine on both nodes.
- **O-9** *(resolved)* All four units expose the identical FFE0 service/characteristic UUID layout; the Telink-module unit differs only in characteristic write-type **properties**, handled per-link (§8). The iOS app writes its command channel (FFE2) without response on the majority module family.
