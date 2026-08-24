# JK BMS BLE Tunnel

Two ESP32 nodes that let an **unmodified JK BMS iOS app** reach four JK BMS
units that are out of Bluetooth range, and give **Node-RED** read-all +
balance-only write access over MQTT. Implements
[`../jk-ble-tunnel-spec.md`](../jk-ble-tunnel-spec.md) (rev 3).

```
  BMS1..4  ═BLE═►  Node A (garage)  ══TCP(LAN)══►  Node B (house)  ═BLE═►  iOS app
                       │
                       └══MQTT══►  Mosquitto  ══►  Node-RED
```

- **Node A** — BLE central, owns the real BMS links, tunnel server, MQTT client.
  Any WiFi+BLE ESP32 (WROOM/WROVER fine; external antenna recommended).
- **Node B** — BLE 5.0 peripheral (C3/S3/C6). Advertises **four identities**
  from a **single shared GATT table**; the app picks one by connecting.
- **test_board** — a bench ESP32 driven over TCP that emulates either the iOS
  app (BLE central → tests Node B) or a JK unit (BLE peripheral → tests Node A),
  since neither the phone nor the batteries are on the bench yet.

## Status — read before flashing

This is a first implementation written ahead of hardware. Two honesty
boundaries, both deliberate:

1. **The JK payload layout is not finalised.** Frame reassembly + checksum are
   real; every decode **offset** in [`components/jk_proto/jk_proto.c`](components/jk_proto/jk_proto.c)
   is a `VERIFY` best-guess to be confirmed against `syssi/esphome-jk-bms` and a
   bench capture (spec O-1/O-2). The synthetic bench frames in
   [`test_board/main/synth_frames.c`](test_board/main/synth_frames.c) use the
   same offsets so decode round-trips on the bench.

2. **The write path is compile-gated OFF.** `JK_ENABLE_WRITES=0`. Balance
   writes, the login handshake, and the measurement-mode floor-lowering all
   return "not supported" until the frame formats are ported and bench-verified.
   Turning the flag on without the port is a hard `#error`. Everything *around*
   the write path (validation pipeline, arbitration, readback plumbing, the NVS
   measurement crash-guard) is implemented and testable now.

Lines tagged `NIMBLE-PASS` need a compile/link check against the pinned ESP-IDF
NimBLE headers (exact arg structs) — do that first on real hardware.

No security layer and no node OTA, by owner decision (remote off-grid site;
BLE/LAN range ⊆ physical access) — see the spec's Non-goals.

## Build

Each node is its own ESP-IDF project. Pin an IDF version (5.x).

```bash
# one-time, ONCE for all three projects: create the shared secrets header.
# Every device joins the same AP, so WiFi lives here a single time; the file
# also holds Node A's MQTT/PIN and Node B's Node-A host (git-ignored).
cp components/secret/secret.h.example components/secret/secret.h   # then edit

# Node A (any WiFi+BLE ESP32)
cd node_a && idf.py set-target esp32 && idf.py build flash monitor

# Node B (BLE 5.0 target)
cd node_b && idf.py set-target esp32c3 && idf.py build flash monitor

# Bench board (any BLE ESP32)
cd test_board && idf.py set-target esp32 && idf.py build flash monitor
```

Also edit the per-unit target names in [`node_a/main/config.h`](node_a/main/config.h)
(`CFG_BMS`) and the balance ranges (`CFG_BALANCE_RANGE`) for your cells.

## Bench without hardware

```bash
# terminal 1: emulate a BMS so Node A has something to talk to
tools/bench.py <board-ip>
> role bms JK-B2A20S20P
> autopush 2000            # stream synthetic cell-info every 2 s

# ...point Node A at that name, watch MQTT jkbms/0/state/* populate.

# to test Node B instead, reflash the board and:
> role app
> connect JK-B2A20S20P     # by the name Node B advertises
> sub
> read
> write aa5590eb9600...    # raw bytes relayed to Node A
```

See the spec's §14 test plan for the full matrix.

## Layout

| Path | What |
|---|---|
| `components/common` | tunnel wire format + JK BLE constants (shared) |
| `components/jk_proto` | frame reassembly/checksum/decode; gated write builders |
| `components/net_util` | WiFi STA + SNTP |
| `node_a/main` | ble_owner, arbiter, decoder, tunnel_srv, mqtt, measure, supervisor |
| `node_b/main` | tunnel_cli, adv_mgr, ble_periph (single shared table), supervisor |
| `test_board/main` | ctl_server + emu_app + emu_bms + synth_frames |
| `tools/bench.py` | drives the test board's control channel |

See [SESSION_NOTES.md](SESSION_NOTES.md) for design decisions and the open-item
tracker.
