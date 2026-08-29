# jk-ble-tunnel

A BLE-over-TCP tunnel that lets the **unmodified iOS JK BMS app** reach four
JK-PB2A16S20P units (fw 19.31, 16S 314 Ah) sealed inside a near-Faraday
garage battery enclosure the house cannot reach by radio.

```
  garage (Faraday)                        house
 ┌─────────────────┐                ┌──────────────────┐
 │ 4× JK BMS       │   BLE          │                  │
 │ BMS_0-00..3-03  ├────► Node A ───┼──TCP──► Node B   │◄──BLE── iOS JK app
 │                 │   (ESP32-S3    │       (ESP32-C3, │
 └─────────────────┘    central)    │        4 clone   │
                                    │        adv sets  │
                                    │        TUN_*)    │
                                    └──────────────────┘
```

- **Node A** (`node_a/`, ESP32-S3, garage): BLE central holding all four
  bank links; decodes cell frames to MQTT (cell volts + balance-wire
  resistance — the values RS485/CAN paths don't carry); serves the tunnel;
  remote ops + push-OTA over MQTT/HTTP.
- **Node B** (`node_b/`, ESP32-C3, house): four advertising-set clones
  (`TUN_0-00` … `TUN_3-03`, NVS-stable addresses) with a mirrored JK GATT
  table; the app's writes relay verbatim through the tunnel to the real
  unit, notifications flow back.
- **test_board/**: TCP-driven bench emulator (plays app-central or
  BMS-peripheral) — `tools/bench.py`.
- **scan_probe/**: minimal BLE observer firmware (any spare ESP32-C3) that
  prints every named advert over USB — the house-side diagnostic scanner.
- **tools/**: `ota_push.py` (push OTA to either node), host-native protocol
  tests (`host_test_jk_proto.c`).

## Documentation

- [`docs/SPEC.md`](docs/SPEC.md) — the design spec (rev 3).
- [`SESSION_NOTES.md`](SESSION_NOTES.md) — living status + the hard-won
  protocol/RF findings. **Read the top block first.** Highlights: the JK
  app's opener trilogy (0x97, 0x96, 0x6C — 0x6C is SET-RTC, not auth; there
  is no auth anywhere), module BLE sleep behavior, per-module ATT write-type
  differences (Telink vs the trio), and the C3 controller's advertising
  activity budget.

## Build

ESP-IDF v5.2.3. Copy `components/secret/secret.h.example` →
`components/secret/secret.h` and fill in (git-ignored). That one file is the
entire site config: WiFi, MQTT broker, BMS PIN, **and your BMS fleet**
(`FLEET_BMS_TABLE` — each unit's advertised name, id, and public address;
the example explains how to discover them with the built-in MQTT scan dump).
No source edits needed to adopt a different fleet.

```
idf.py -C node_a set-target esp32s3 build
idf.py -C node_b set-target esp32c3 build
```

First flash over USB; afterwards `tools/ota_push.py a|b --host <ip>`.

## Deliberate stances

- **No settings writes yet**: `JK_ENABLE_WRITES=0` is a hard gate.
- **No security layer** (tunnel auth, pairing, MQTT ACLs) by owner decision:
  remote off-grid site, radio/LAN range ⊆ physical access.
- **Never route BMS firmware through the tunnel.**
- The client is deliberately *gentle*: no GATT reads ever, paced reconnects,
  nothing sent to a streaming unit (the BMS piezo-acks every command frame).
