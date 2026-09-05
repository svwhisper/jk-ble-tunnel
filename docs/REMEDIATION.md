# Resilience remediation — staged acceptance

Baseline: `2f34ad805bc31c81da2bafd045f9da89c8cc4dc3`; working branch
`resilience-stages-20260905`. No stage may silently inherit unaccepted firmware
changes from another stage. Each stage gets a commit, saved binaries, tests,
and an explicit acceptance decision before the next live deployment.

## Categories and order

Complexity/risk are implementation/change risk, not severity of the existing bug.
High-risk rows are separate stages, never a single combined deployment.
For categories with independent high-risk fixes, use separate sub-stages:
5a discovery / 5b boot verification; 6a pending flush / 6b session fencing /
6c redundant resync polling / 6d BLE-off cancellation; 8a publication isolation /
8b broker-outage recovery policy; 10a notification delivery / 10b replay ordering.
Do not combine these sub-stages in one live test merely because they share a row.

| Stage | Category / audited issues | Complexity / risk | Acceptance gate |
|---|---|---|---|
| 0 | Source and firmware recovery points | Low / low | Source bundle and image hashes preserved |
| 1 | OTA: false-positive update confirmation, B startup/retry, handler registration and validation gating | Medium / high (recovery path) | Host failure-path tests; both firmware builds; B then A one at a time; exact image and boot state verified; owner phone smoke test |
| 2 | Input/memory safety: JSON root/type/finite/integer validation and optional id; oversized app writes/id bounds; short settings and tunnel records; retained/fragmented command rejection | Medium / low–medium, bounded bundle | Sanitizer boundary/malformed-input tests, both builds, unchanged valid app reads/writes; no live battery-setting tests without safety gate |
| 3 | Frame reassembly: unconsumed bytes after completed frame | Medium / medium, isolated | Consecutive records at every chunk boundary, bad checksum/resync tests, live frame rates |
| 4 | Runtime state: non-atomic read/modify/write | High / high, isolated | Concurrent field-update tests, app-demand/idle transitions and soak |
| 5 | BLE discovery and boot verification: failed discovery cleanup, false frame evidence, app-safe release | High / high, isolated | Failure injection on bench; successful discovery, silent peripheral, app attach during verify; quiet-idle preserved |
| 6 | Command lifecycle: link-up flush drops valid work, stale work across sessions, false→false resync polling, BLE-off cancellation | High / high, isolated | Queue/order/session-generation tests; reconnect/drop tests; no stale or unintended writes |
| 7 | TCP transport: blocking connect/read/write, short sends, dead/grace deadlines | High / high, isolated | Host socket fault injection (slow peer, partial header/body, blackhole/backpressure), deadline and reconnect soak |
| 8 | MQTT isolation: synchronous publication stalls critical tasks; broker outage restarts healthy WiFi | High / high, isolated | Bounded memory/queue and broker-outage tests; BLE/tunnel remain responsive |
| 9 | Cache correctness: replacement identity persistence, stale settings, missing read-cache priming | Medium / medium, isolated | Replacement/reboot/expiry tests; owner app warm-start; no NVS schema migration |
| 10 | Notification delivery: queue loss and ignored BLE errors; replay mid-record | High / high, isolated | Saturation tests and byte-level replay/live ordering; phone sessions under load |
| 11 | Write confirmation: overwritten/early readback, dispatch/drop correlation, missing WRITE_RESULT | High / high, isolated | Simulated dispatch/readback failures first; live benign setting round trip only with explicit safety clearance |
| 12 | Maintenance bundle: disabled measurement recovery path; stale docs/test commands/post-app opcode | Medium / medium | Host tests and startup/recovery checks; reconcile docs with accepted behaviour |

## Rollback and live test rules

- Baseline files are outside the worktree at
  `/Users/dw/Downloads/jk-ble-tunnel-rollback/20260905-baseline/`.
  They include source.bundle, both existing app binaries/sdkconfigs and the old
  updater. These are preserved build outputs, **not device flash readbacks**;
  their equivalence to the previously running images is not proven.
- Baseline app SHA256: A
  `cb37042b60a63e40b1d64bfc75ad57862eb60b49ee1c2e2775c8415097d7cb86`, B
  `4ef8a8f96f481b949f5b4619594074f20adfe570e18f63d6d57d2e618701d5bd`.
- Preserve accepted stage images before any subsequent build. Use explicit
  `--bin /absolute/path/to/saved/node_X.bin` for backout, never a mutable build
  path. Verify hashes and target (A: ESP32-S3 `.239`; B: ESP32-C3 `.234`).
- Source backout uses `git revert <stage-commit>` after checking the worktree;
  no hard reset, destructive checkout, force push, flash erase or NVS erase.
- No partition-table, bootloader, credential or persistent-data format changes.
  A/B wire compatibility must remain valid during rolling updates/backout.
- Flash only one node at a time. Keep the other on its accepted image until
  the first is checked. An accepted image may overwrite the automatic rollback
  slot on the next update: saved host binaries are the durable recovery path.
- Backout was exercised successfully on B: Stage 2 → saved Stage 1 → Stage 2,
  with exact ELF and VALID verified after each reboot. Owner requested no
  repeated backout drills for subsequent deployments; preserve/checkpoint each
  accepted image and use backout only when required.
- If OTA is unavailable, stop and use owner-assisted USB application-only
  recovery with the saved matching binary and existing partition layout.
  Do not claim automatic rollback can repair every hang; it needs a reset.
- An HTTP listener proves reachability, not firmware identity or BLE health.
  Require exact ELF identity and valid boot state where the endpoint exists;
  a legacy image needs independent boot/app checks and an explicit legacy mode.
- Preserve quiet-idle, passive scanning and the owner's nightly reboot policy.
  Do not use bank 0's known hardware failure as a firmware acceptance test.
- Owner confirmed availability for phone testing/USB recovery on 2026-09-05.
  Calibration/cell-count/bounce/power-cycle tests remain prohibited until owner
  confirms iBMS charge control is disabled. Do not fault-inject the live broker
  or live battery commands as a substitute for a bench test.

## Status

- Stage 0: complete; baseline saved and branch created.
- Stage 1: local implementation committed as `8329df0`; 16 updater tests and
  35 sanitizer-enabled protocol checks passed, both firmware targets built.
  Node B deployed successfully (HTTP 200, ota_1); exact expected ELF
  `79888f98507eb6dda5e7f20cefcef86cef087c5d67b381ee22af2a2224e0af8c`
  and OTA VALID verified; owner confirmed identities, connection and live
  values work. Node A deployed after one preflight failure (no upload on that
  attempt), then HTTP 200 to ota_0; expected ELF
  `bf1f41bc74e0832f4ddcf0d291e4ba5e9abe73a9742322a6097f14f282e53f88`
  and OTA VALID verified. Owner repeated phone check after A's boot: live values
  normal, no new problems. **Stage 1 accepted on both nodes** and saved as the
  verified backout point for Stage 2.
- Stage 1 artifacts and backout commands:
  `/Users/dw/Downloads/jk-ble-tunnel-rollback/20260905-stage1-ota/MANIFEST.md`.
- Stage 2: local implementation/tests complete: 380 assertions plus 1,052,672
  exact-buffer validator calls under ASAN/UBSAN, existing 35 protocol checks,
  16 updater tests, and both firmware builds passed. Stage 1 saved images are
  its backout point. B deployed and verified after the successful backout
  drill; owner confirmed B phone test passed. A then deployed to ota_1
  (HTTP 200, 1,208,816 bytes in 21.48 s); exact ELF
  `7663cb35b61980a46b4aac3b91fdd42325d3aac2cb840df985dff4e11ecde248`
  and OTA VALID confirmed at uptime 6122 ms. Owner confirmed post-A read-only
  phone test passed: **Stage 2 accepted on both nodes.** Its saved images are
  the recovery point for Stage 3.
- Stage 3: isolated reassembly fix passed local tests. Regression reproduced on
  Stage 2 code (continuous 128-byte input loses the middle record). The API now
  reports consumed bytes; A drains each chunk fully and copies completed frames
  before continuing. Raw forwarding, queues, connection timing and wire layout
  unchanged. B does not call this API and will remain on its accepted Stage 2
  image; both targets are built to check shared-component compatibility.
  Both firmware builds passed with unchanged sdkconfigs. 71,769 byte-exact
  reassembly assertions passed under ASAN/UBSAN (all fixed chunk sizes and
  two-chunk splits, noise, checksum failure, retained partial magic, reset),
  alongside all Stage 2 tests. Committed as `fb57f1b`; A deployed successfully
  to ota_0 (HTTP 200, 1,208,880 bytes in 22.32 s), exact ELF
  `919dc075bccc6ae54f72c14aa1180994f252b44461640015cdee4ca037b94860`
  and OTA VALID verified at uptime 6119 ms. Owner phone check passed; 86 fresh
  bank-3 summaries and three health messages observed in 45 seconds, OTA up,
  internal heap 99,659 bytes (minimum 95,163). **Stage 3 accepted: A Stage 3,
  B unchanged on accepted Stage 2.** Saved pair and
  A-only backout command are in
  `/Users/dw/Downloads/jk-ble-tunnel-rollback/20260905-stage3-reassembly/MANIFEST.md`.
- Stage 4: local implementation replaces whole-runtime snapshot writeback with
  field-specific locked operations, atomically pairs app state/departure time,
  rejects stale timestamp cleanup and prevents reachability promotion from
  overwriting a held link. Timestamps cannot move backwards under interleaving.
  Old code's lost app_connected was reproduced deterministically. Production
  state_cache.c passes deterministic checks and six threads × 25,000 iterations
  using native mutex adapters under ASAN/UBSAN and separately ThreadSanitizer;
  all prior tests pass. A firmware build passed with unchanged sdkconfig;
  committed as `882f2fb`. A deployed to ota_1 (HTTP 200, 1,209,232 bytes in
  22.13 s); exact ELF
  `120f05d8659889a09a91af6061cb84e499285a5e2fdf082fb383d5305ba75154`
  and OTA VALID verified at uptime 6286 ms. B unchanged. Owner's fresh
  post-deployment phone test passed. Fresh MQTT then showed all four links
  reachable-idle with app_connected=false; bank 3 was held during the grace
  period, then returned idle. At uptime 215 s, connection/disconnection counts
  were balanced at 6/6, OTA/BLE up, internal heap 99,547 (minimum 95,167).
  **Stage 4 accepted: A Stage 4 / B Stage 2.** This is a short live soak plus
  sanitizer stress testing, not a claim of long-duration field reliability.
  Saved images and backout instructions:
  `/Users/dw/Downloads/jk-ble-tunnel-rollback/20260905-stage4-runtime/MANIFEST.md`.
  This does NOT change the link-up-as-frame-evidence policy (5b) or fence an
  already-queued idle disconnect against a new app session (6b). Those require
  their own stages; a state-cache lock cannot make queue actions atomic.
- Stage 5: deferred at owner's request (bench unavailable); no deployment.
  Stage 5a review confirms ignored discovery errors, unchecked procedure starts,
  assumed CCCD handles and premature LINK_UP before subscription success. Its
  isolated acceptance requires bench tests for missing service/characteristic,
  subscription failure, disconnect during discovery and successful retry.
  No USB test board is currently enumerated (2026-09-05 19:04); arrange an
  isolated bench target before live deployment. No Stage 5 code or firmware
  changes made, and no fault injection against live battery peripherals.
- Stage 6a: prepared locally while Stage 5 awaits the bench. The production
  ARB_CLEAR handler previously erased all queued requests; a native test of
  the actual task loop reproduced the loss. Cleanup now removes only
  SRC_INTERNAL/TXN_POLL entries, retaining other requests byte-for-byte and
  in order. A bounded eight-entry rotation needs no allocation and leaves
  active-transaction, command counter, backoff and deadline fields unchanged.
  122,640 native cases exercise every ring occupancy/head/internal-poll placement,
  all kind/source pairs, busy and non-busy state, idempotence, subsequent
  fill/drain, and bank isolation under ASAN/UBSAN. All external sends are
  trapped; no test requests can reach devices. Firmware build passed with
  unchanged sdkconfig. **Not deployed or live-accepted.**
  Remaining gate: isolated cold-link/bootstrap and queued-command tests.
  Retaining writes can expose the existing cross-session stale-work problem
  (6b), so session safety must be resolved/reviewed before a live rollout;
  a passing ring test is not proof a queued write is still authorized.
  Bootstrap posting is still separate from response dispatch and is not made
  atomic here. Readback/WRITE_RESULT behaviour remains Stage 11. Stages 6b–12
  remain pending. Live nodes stay on the accepted A Stage 4/B Stage 2 pair.

## Stage 2 input contract

- MQTT balance writes: an object with exactly one setting plus optional string
  `id` (at most 31 bytes). Only finite JSON numbers are accepted; only
  `balancing_enabled` also accepts booleans and numeric 0/1. `cell_count` must
  be integral. Existing whitelist/range/safety policy is unchanged.
- MQTT commands must arrive in one complete, non-retained event, with topic
  shorter than 64 bytes and payload shorter than 192 bytes. Fragmented,
  oversized, embedded-NUL and empty commands are rejected before routing.
  This suppresses broker replay; MQTT v3 live delivery of a newly retained
  publish may have retain=false, so this is not a publisher-side retain ban.
- App writes of 1–32 bytes pass unchanged; zero/oversized writes are rejected,
  not truncated. Node B reports ATT errors for invalid length/queue-full when
  the ATT operation supports a response. Write-no-response cannot report one.
- Tunnel message IDs, exact control/TABLE sizes, index/state flags and buffer
  bounds are validated before payload access. The byte format is unchanged.
- Native checks: `bash tools/run_host_tests.sh`; uses installed IDF cJSON,
  enables writes in the protocol tests, sends nothing to nodes/BMSs. Installed
  cJSON emits macOS sprintf deprecation warnings; project test code passes.
