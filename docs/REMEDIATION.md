# Resilience remediation — staged acceptance

Baseline: `2f34ad805bc31c81da2bafd045f9da89c8cc4dc3`; working branch
`resilience-stages-20260905`. No stage may silently inherit unaccepted firmware
changes from another stage. Each stage gets a commit, saved binaries, tests,
and an explicit acceptance decision before the next live deployment.

## Current checkpoint — resumed 2026-09-06

Owner resumed work after the overnight pause. Last accepted pair remains
**A Stage 5b1 / B Stage 2**; recovery
images and exact identities are in
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-stage5b1-evidence/MANIFEST.md`.
Stage 5b1 accepted 08:34 after owner TUN2 phone pass and all-bank idle/app=false,
conn/disc 10/10 at A uptime 335–365 s, OTA/BLE up. Stage 5a3 predecessor retained.
Stage 5b2 app-safe verification release is next and is not included in this image.
Stage 5a3 accepted at 08:14 after owner cold-session phone pass and fresh idle
telemetry: all four links reachable-idle/app=false, conn/disc 19/19, OTA/BLE up
at A uptime 1148–1163 s. Earlier intermittent TUN2 symptoms remain unresolved
observations, not claimed fixed. Stage 5a2 recovery predecessor retained.
Stage 5a1 deployed to A on 2026-09-06 07:28; exact ELF
`9b68ac3f631792d56d658cd4ae62e073277fc77446198737799a95f3765a5aca`
and OTA VALID verified at uptime 7396 ms. HTTP 200, ota_0, 1,210,608 bytes in
21.05 s. Owner phone test passed; subsequent fresh telemetry showed all links
idle/app=false and conn/disc balanced at 8/8 (uptime 185 s), OTA/BLE up,
internal free heap 99,443 bytes (minimum 95,055). **Stage 5a1 accepted.**
B unchanged; no Stage 6
firmware deployed. Reviewed image/backout manifest:
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-stage5a1-reviewed/MANIFEST.md`.

Local Stage 5a1 draft handles discovery start/callback errors, missing FFE0/FFE1,
immediate subscription-start rejection and discovery deadline expiry. It rejects
stale generation callbacks and premature requests; failed links retain their
slot until teardown, with rate-limited terminate retries. Successful descriptor
assumptions/CCCD-ack policy remain unchanged for a separate Stage 5a2.
Native tests of production callbacks/sweep/request gates pass under ASAN/UBSAN,
alongside all previously active tests. A firmware build passed with sdkconfig
byte-identical to baseline. Review against pinned ESP-IDF 5.2.3 NimBLE confirmed
callbacks run without its host lock, GATT caching is disabled, procedure-start
failures return without synchronous callbacks, and GATT failure callbacks precede
connection deletion/GAP DISCONNECT. Fixed an ENOTCONN reuse window: keep the
failed slot until GAP DISCONNECT, not merely until the controller reports absent.
Handle-0 lookup excludes unconnected scan slots; CONNECT requires readiness.
Moved existing GAP telemetry publication outside the new link-pool lock (not
off the host task; publication isolation remains Stage 8a). Expanded tests cover
1,000 simultaneous completion/deadline races under ASAN/UBSAN and TSan, and
GATT-error-before-disconnect ordering. All active tests and final A build pass
(1,210,608 bytes, unchanged sdkconfig). Live acceptance pending; preflight
verified A Stage 4/VALID but B initially did not answer at .234. Owner asked
to retry; both expected accepted ELF identities/VALID then confirmed (B uptime
59.6 s) before A-only deployment. Live phone/idle gates completed as above.
Bench waiver remains valid; no live fault injection or battery-setting changes.

Stage 5a2: report LINK_UP/CONNECT success only after
the FFE1 CCCD write receives a successful ATT acknowledgement for that handle.
The original early-success behaviour was reproduced in the production callback
test. A missing/failed ACK follows the existing discovery deadline/teardown;
duplicate and stale callbacks cannot revive a failed session. Optional FFE2
subscription remains best-effort; CCCD address assumptions remain for Stage 5a3.
All prior tests plus 260 rejected callback statuses, malformed/duplicate/stale/
missing ACK cases and 2,000 completion/ACK/deadline races per ASAN/UBSAN and TSan
run passed. A build passed (1,210,784 bytes), unchanged sdkconfig. Stage 5a1
accepted image is the backout point; no Stage 6a, wire or persistent-data change.
Committed `923b721`, A deployed to ota_1 (2026-09-06 07:36, HTTP 200,
1,210,784 bytes in 18.66 s); exact ELF
`035805ce35589fb88a007d9e83f77892eaab923477aa24d95c01b200cff0ecdd`
and OTA VALID verified at uptime 6192 ms. B unchanged. Owner phone test passed;
fresh telemetry confirmed bank 3 returned idle, all app flags false, conn/disc
8/8 at uptime 215–230 s, OTA/BLE up, internal heap 99,543 (minimum 94,571).
**Stage 5a2 accepted.** Image/backout manifest:
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-stage5a2-subscribe/MANIFEST.md`.

Stage 5a3 (accepted; chronological test history below): discover CCCD UUID 0x2902 within each
characteristic's own handle range; no value+1 guess. Ranges stop before the next
declaration (including unrelated UUIDs) or service end. Primary CCCD is required;
optional FFE2 is best-effort unless the link is lost or the returned layout is
malformed. Discovery phases, generation tokens and the existing deadline guard
late/missing callbacks. No settings, wire, NVS or connection-timing policy change.
Tests exercise non-adjacent CCCDs, both FFE1/FFE2 declaration orders, every valid
primary position in a sample range, unrelated attributes, malformed/overlapping
ranges, duplicate/missing/error/stale callbacks and optional fallback, alongside
the existing ACK and concurrent discovery/deadline sanitizer suites. A build
passed (1,212,016 bytes), unchanged sdkconfig. Stage 5a2 is the backout point.
Committed `3a6810d`; deployed to A ota_0 on 2026-09-06 07:55, HTTP 200 in
22.38 s. Exact ELF
`45eba4a502d4b734f9aad43e46de254552c89e9c00da32ac76fe25d10b582bfd`
and OTA VALID verified at uptime 6256 ms. B unchanged; phone and post-session
idle acceptance pending. Saved candidate/backout manifest:
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-stage5a3-descriptors/MANIFEST.md`.

Acceptance exception: owner reported TUN1/3 working but TUN2 connected without
a data page (one brief page flash). Retry subsequently updated normally,
before any reset or firmware change. Fresh bank-2 summary telemetry contained
continuous changing voltages; the primary discovery/subscription path therefore
completed and delivered decoded frames on the retry. This does not identify
the cause of the earlier phone failure or establish reliable cold-session
behaviour. Keep Stage 5a3 provisional; no next-stage deployment yet.

USB inspection on 2026-09-06 08:00 identified actual Node B at
`/dev/cu.usbmodem1101`, USB serial `70:AF:09:0D:47:50`, not yesterday's probe.
Opening the console with pyserial DTR/RTS false nevertheless triggered
USB_UART_CHIP_RESET (confirmed fresh OTA uptime 17,488 ms). No commands or
flash were sent; boot also reported PHY calibration saved after checksum
failure. B recovered on unchanged Stage 2 exact ELF/VALID, tunnel up and
advertising; heap 69,288, zero phone connections at uptime 34 s. Owner's
successful TUN2 retry preceded this diagnostic-induced reset. Do not reopen
this port assuming read-only means reset-free. Original transient remains
unexplained; acceptance requires another cold TUN2 session after idle.

Further owner observation: from TUN2's initially blank default Status page,
swiping to Settings produces a populated page; returning to Status produces
normal continuing updates without a reconnect. Treat page switching as a
workaround, not acceptance. This points toward initial exchange/replay handling,
but is not a captured proof of the exact lost/misordered frame. Code review
confirms two pre-existing candidate mechanisms: A's ARB_CLEAR clears all pending
requests including app writes (Stage 6a); B's nb_replay_action cancels all replay
bits after dev_seen_us advances, while that timestamp is recorded on the first
device-info chunk before notification success is checked (replay/delivery stages).
Neither changed in Stage 5a3; added discovery latency could expose their timing.
Do not conflate this hypothesis with a demonstrated descriptor regression or
silently bundle the candidate fixes into Stage 5a3. B stayed exact Stage 2/VALID
at uptime 103,908 ms after console close; no repeat USB reset observed.

2026-09-06 08:04: owner additionally reported slow acknowledgement when
turning balancing off and an error (suspected timeout) when turning it on
from the phone Control page. Bank identity and exact error remain unconfirmed;
asked owner to pause toggles. Do not infer rejection from timeout or retry the
write automatically. Passive 20 s observation yielded only retained settings,
not a fresh settings frame, so current enabled state is not verified. B
acknowledges receipt/enqueue before A/BMS completion; normal app-write outcomes
are not routed as correlated TUN_WRITE_RESULT replies, and B only acts on
LINK_DOWN. FFE2 raw-write submission also reports RESP_OK even on immediate
submission error. These are existing command/result-path flaws, not proof of
the observed timeout's exact cause; phone protocol reply delivery and queue
timing remain candidates. MQTT's separate 15 s write/readback workflow does
not handle these phone writes. No assistant-issued settings commands, USB
reopen, reboot or deployment during this investigation. Stage 5a3 remains
unaccepted; stage progression held and Stage 5a2 recovery image retained.

08:06 owner confirmed the control symptom was TUN2; error no longer occurs
and toggles now respond quickly. No intervening assistant firmware change,
reset or battery write. Record as recovered intermittent behaviour, not a
verified fix or proof of the final balancing state (owner did not specify
the final setting). No further toggle tests needed. Next acceptance check is
a read-only cold TUN2 session after disconnect/idle, opening Status directly
without the Settings-page workaround; Stage 5a3 remains provisional.

08:08 owner answered "all seems fine" to the requested 90 s idle/reconnect,
direct-Status, read-only TUN2 test: phone acceptance passed. Both exact running
ELF identities and OTA VALID rechecked (A Stage 5a3, B Stage 2). Fresh telemetry
at A uptime 817 s shows TUN2 app_connected=true/link up, others idle/app=false,
conn/disc 19/18, internal free heap 99,327 (minimum 94,311), OTA/BLE up. One
held link is consistent with the still-open phone session, not a demonstrated
idle leak. Final post-session idle gate remains before the next deployment;
earlier recovered blank-page/control symptoms remain unresolved observations,
not claimed fixed by descriptor discovery. No additional device changes.

08:14 final idle gate passed after owner reported two minutes disconnected:
all four links reachable-idle/app=false, conn/disc balanced 19/19, OTA/BLE up,
internal free heap 99,127–99,331 (minimum 94,311), uptime 1148–1163 s.
**Stage 5a3 accepted: A Stage 5a3 / B Stage 2.** Saved unchanged B image beside
A and verified its hash, establishing the next recovery pair. Retain Stage 5a2
as predecessor and keep the transient TUN2 startup/control observations open.
Next isolated stage is 5b boot verification evidence/app-safe release; no 5b
or Stage 6 firmware is included in this accepted image.

Stage 5b red-team split: 5b1 frame evidence first, 5b2 app-safe release later.
Local 5b1 adds last_frame_us, written only by complete checksum-valid reassembly,
and uses it for boot verification. Existing last_seen_us link-up/activity meaning
and all reconnect/idle policies remain unchanged. "ok" means a complete valid
frame arrived after this bank's turn began, not sustained streaming, successful
decode or phone delivery. No new wire/NVS fields or Node B changes. Existing
45 s bank deadline and once-per-boot polling remain; a silent bank may now
consume that deadline instead of falsely passing at link-up. Wait for the whole
round before phone testing while immediate verification release awaits 5b2.

Actual supervisor regression test failed on old link-up/no-frame success, then
passed with separate evidence. Tests cover boot/MQTT/BLE gates, stale/equal/
cross-bank times, cache updates without fresh radio frames, held-silent versus
never-connected deadlines and one publication per boot. Production notify tests
reject heartbeat/partial/bad-checksum evidence and accept a completed frame.
All active ASAN/UBSAN, state/discovery TSan and updater tests pass; A build is
1,212,144 bytes with unchanged sdkconfig. Committed d54a389; deployed A-only
2026-09-06 08:28 to ota_1 (HTTP 200, 22.98 s). Exact ELF
`586d734959a6e48a7b90f7dee8a2309a7f8c8d50c6518010b0f4d61471eec920`
and OTA VALID verified at uptime 6366 ms. Boot-round/phone/idle acceptance
pending; B Stage 2 unchanged, Stage 5a3 recovery pair preserved. Manifest:
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-stage5b1-evidence/MANIFEST.md`.
Fresh boot report at uptime 33 s returned all four "ok" using the new frame-only
criterion. This does not establish bank 0 hardware health or sustained values.
At uptime 49 s OTA/BLE up, internal free heap 99,519 (minimum 95,055),
conn/disc 6/5; phone and final post-session idle gates remain.
08:34 acceptance: owner TUN2 direct-Status/30 s live-values check passed with
no reported blank screen, timeout or new beeping. After grace, all four links
returned reachable-idle/app=false and conn/disc balanced at 10/10 for uptime
335–365 s; OTA/BLE up, internal free heap 99,335–99,551 (minimum 94,859).
**Stage 5b1 accepted: A Stage 5b1 / B Stage 2.** Copied/hash-verified unchanged B
beside accepted A as next recovery pair, with Stage 5a3 predecessor preserved.
No new deployment or device setting changes during acceptance.

Stage 5b2 local: boot verification now relinquishes demand without queuing its
own unchecked disconnect. Normal idle maintenance is the release owner: a phone
keeps the link, otherwise the existing 60 s grace applies. No additional requests,
state fields, allocation, wire/NVS changes or B firmware changes. This can retain
more links during early boot, within the existing four-link pool/controller limit.
General stale idle-request/session fencing remains Stage 6b; removing this
verification-specific path does not claim that separate race is fixed.

Actual supervisor test reproduced old timeout-disconnect with an app attached.
New tests cover completion/timeout with a phone, attach after completion, departure,
exact grace boundary, no-phone cleanup and unchanged boot evidence/result gates.
Full ASAN/UBSAN plus existing state/discovery TSan and 16 updater tests passed
(`/private/tmp/jk-host-tests.ydY1eB`). A build passed with unchanged sdkconfig.
Committed 70082cb; A-only OTA completed 2026-09-06 08:42 to ota_0,
HTTP 200, 1,212,096 bytes in 21.82 s. Exact ELF
`371b549a5c2f6b1a4f26628e4024341a47bceb947729ab768d637c0a7b8e3458`
and OTA VALID verified at uptime 6188 ms. B unchanged. Owner asked immediately
after verification to connect TUN2 and hold Status readings 90 s, then disconnect;
boot/app-overlap and final idle acceptance pending. Stage 5b1 pair preserved.
Manifest: `/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-stage5b2-release/MANIFEST.md`.

Stage 5b2 live exception: TUN2 attached during boot (app=true at uptime 34 s),
but owner reported connected with a non-updating display through the requested
test, then confirmed it started updating on its own, without page switch or
reconnect. No intervening firmware/reset/settings change. Bank 2 had a real
BLE timeout (0x208) at 27 s and reconnected at 38 s; verify reported all four
ok at 42 s. During the reported display stall, A continued publishing fresh
changing bank-2 summary values. Thus a ready BLE link/valid-frame boot report
does not establish successful phone initialisation or delivery.

Backout was announced after failed acceptance, but held before any upload when
owner reported recovery. **Stage 5b2 remains deployed but unaccepted**; Stage
5b1 remains the accepted recovery point, and no subsequent stage may deploy.
Owner disconnected after confirming self-recovery; post-session idle check in
progress. Earlier TUN2 transient behaviour predates 5b2, so neither new release
policy causation nor a particular queue/replay fault is proven. Delayed initial
exchange is a hypothesis, not a diagnosed fix. No further live control toggles,
USB console access or fault injection requested.

Stage 6a is excluded from this draft: commit `45b918f` remains on branch
`resilience-stage6a-local-20260905` with its saved local-only candidate. Revert
`43af289` removed it from the active branch while preserving its documentation;
do not accidentally restore/bundle it into a Stage 5 deployment.

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
- Stages 5–12: pending; acceptance gates deliberately prevent batch deployment.
  Stage 5a review confirms ignored discovery errors, unchecked procedure starts,
  assumed CCCD handles and premature LINK_UP before subscription success. Its
  isolated acceptance requires bench tests for missing service/characteristic,
  subscription failure, disconnect during discovery and successful retry.
  No USB test board is currently enumerated (2026-09-05 19:04); arrange an
  isolated bench target before live deployment. No Stage 5 code or firmware
  changes made, and no fault injection against live battery peripherals.
  Update 19:14: owner connected `/dev/cu.usbmodem101`, Espressif USB-SJ serial
  `70:AF:09:0E:82:58`. Boot output identifies ESP32-C3 v0.4 running `app_probe`
  version `8806d3a-dirty` (ELF prefix `639bf746b`), not the S3 `test_board`
  build. Serial opening showed USB_UART_CHIP_RESET and RF calibration save;
  the attempted `status` query was interpreted as survey commands (`s`),
  confirmed against app_probe source. Surveys are finite and do not connect;
  no session, role-change or flash command was sent. Stage 5a still needs an
  isolated central plus BMS-emulator pair; do not retarget live Node A or
  impersonate a real battery address to compensate for a missing spare board.
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
  Stage 6a candidate is preserved on branch `resilience-stage6a-local-20260905`
  and in its local-only artifact directory; it has been reverted from the
  active deployment branch so Stage 5 can ship independently.

## Owner-approved live testing without bench (2026-09-05)

The owner waived bench testing and confirmed physical access for reset/USB
recovery if OTA becomes unreachable. Continue local failure tests and one
isolated live change at a time, exact ELF/VALID verification and read-only
phone acceptance. This replaces the bench-required deployment gate, not the
no-live-fault-injection or battery-setting restrictions. No USB probe changes
are required. Hardware failure-path coverage remains explicitly unproven.
Split discovery further: 5a1 failed/timed-out discovery cleanup first; 5a2
subscription acknowledgement only; 5a3 descriptor lookup separately; 5b boot
verification evidence/app-safe release still separate.

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
