# Resilience remediation — staged acceptance

Baseline: `2f34ad805bc31c81da2bafd045f9da89c8cc4dc3`; working branch
`resilience-stages-20260905`. No stage may silently inherit unaccepted firmware
changes from another stage. Each stage gets a commit, saved binaries, tests,
and an explicit acceptance decision before the next live deployment.

## B replay session ownership — isolated Stage 10c, 2026-09-07

Owner authorized proceeding after the early-switch failure. Local reproduction
found an independent session bug: a reconnect during the first cached record
aborted that record, but fresh snapshots for the next two records sent600 old
bytes into the replacement connection. This is not proof that this precise race
caused the latest phone error; radio loss and cached-only initialization remain
open. Red-team choice: repair ownership without changing cached content, replay
timing/order, transport queues, radio policy or protocol. Do not adopt held10b's
mid-record-boundary observer in this stage. Its complete history is preserved
on `hold/b10b-source-20260907`; commit7df2163 first restored the active B files
to10a, keeping A5c1 unchanged.

Replay eligibility, debt removal and destination snapshot now happen under one
existing state mutex. All records/chunks in that claim use that same epoch and
connection; a reconnect/CCCD change stops remaining old submissions without
consuming new-session debt. Opener cache checks create a single debt mask,
committed only against the captured session epoch. Writes before CCCD remain
supported. No mutex spans NimBLE calls, no notification retries or larger
buffers, and already-authorized host submissions cannot be recalled. Existing
2s UP grace/5s expiry, cache bytes/counter stamping, NVS, A application and
boot/idle/nightly policies are unchanged. Outgoing command queue ownership and
mid-record replay interleaving remain separate work, not silently bundled.

Full native suite passes, including ASAN/UBSAN/TSan,39,078 B notification cases,
every replay chunk boundary over six MTUs, cache-read session changes,
960 original-policy equivalence cases,10,000 concurrent session/claim cycles,
12,880 cache-selection cases and16 updater tests. Original cache-selection
defects remain reproduced, not repaired. Logs `/private/tmp/jk-stage10c-host-tests.log`
and `/private/tmp/jk-stage10c-build.log`; test binaries `/private/tmp/jk-host-tests.BJico2`.
B build passed:1,114,368B (45% partition headroom), ELF
`e60ff867328f0ea58eb1bd9272efa92498597e05d0ace026e3ac35f03f01403c`, SHA256
`5cf52028d84e151b65cddcf3c92e70035263781d4729180d4eed80dbc8872a78`.
17:56 preflight: exact A5c1/B10a both OTA VALID; all four MQTT app flags false.
Immediate backout is the hash-verified saved B10a image; earlier accepted pair
also retained. B-only OTA and read-only phone acceptance pending.

## A reconnect ownership — isolated Stage 5c1, 2026-09-07

Owner authorized proceeding after the restored10a comparison. Red-team choice:
disable NimBLE's internal connection reattempt on A, leaving A's existing
scheduler retry, scan duty cycle, backoff, discovery/CCCD ACK gates, slot
ownership and orphan guard intact. Do not adopt an unsolicited retry by
weakening stale-callback protection. Pinned ESP-IDF code on0x3e tears down GATT
and automatically reconnects with a saved callback; the observed successful
reattempt then arrived without s_conn_inflight and was terminated as an orphan.
With reattempt disabled, the ordinary GAP disconnect path remains compiled
and A's next scheduled request owns the complete scan/connect/discovery cycle.

This is a single A configuration change on the currently running6b1 source,
not acceptance of6b1 or a new B deployment. B stays on restored10a;10b remains
held. No application callback behavior, timer, BMS command, idle/boot/nightly
policy, NVS or wire change. Add a compile-time effective-NimBLE-config guard so
an old local sdkconfig cannot silently override the checked-in default.
Update both sdkconfig.defaults and this Mac's ignored sdkconfig; a defaults
change alone would not change an existing build.

Tests reproduce the unsolicited retry's orphan rejection, then exercise
actual production GATT cancellation/GAP disconnect/request/scan/connect/
discovery/subscribe-ACK handling and stale-cookie rejection for handles0/7.
Initial harness attempt used all-zero dummy addresses (disabled targets);
corrected to nonzero synthetic addresses only in that test, no real site data.
Effective-config guard tested with retry enabled (must fail compile) and
disabled (must pass). Full sanitizer/TSan suite,46,263 B notify/12,880 cache/
16 updater tests pass; log `/private/tmp/jk-a-retry-host-tests-final2.log`,
executables `/private/tmp/jk-host-tests.5Fq66k`. These are simulated stack
events plus source/build inspection, not bench radio-fault injection.

A build passed `/private/tmp/jk-a-retry-build.log`. Effective configuration
diff from pre-build snapshot has exactly reattempt true->false and removal
of its now-inapplicable max count3; no other key changed. New map contains
no ble_gap_master_connect_reattempt/ble_gap_reattempt_count symbols. Tracked
A/component delta from6b1 is only the defaults setting and compile guard;
held B10b source is not linked into A. Candidate1,211,088B, ELF
`7927af2873d6bb93a2dfcfa7f2e291b5884d740d676036f0070cf2583676172a`, SHA256
`d86c415b8897c26cc26b5bfbe36a6b96399b14beefeb2d2a1274bee00d8ce2e9`.
A6b1 immediate backout and accepted6c fallback hashes rechecked. This will
not guarantee faster radio establishment, fix failed scans, or make cached-only
phone initialization reliable. Stage5c1 OTA/boot/phone acceptance pending.

17:36 deployment: source0b8b526 saved at
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260907-stage5c1-retry-owner` and
uploaded A ota_1 (HTTP200,1,211,088B,22.532548s). Exact candidate ELF/OTA VALID
verified6194ms. Paired capture
`/private/tmp/jk-stage5c1-capture.cRYKRe/capture.jsonl` opened at A18254ms;
B10a exact/VALID unchanged. Boot-frame verification and phone test pending.

17:37 boot check: all four banks reported fresh-frame verification ok at39s.
The capture contains a natural bank1 connection-establishment failure during
discovery (rc7), followed by ordinary GAP disconnect0x23e at26765ms and a
scheduler-owned retry; no "Reattempt connection" or "orphan connect" logged.
This exercises the intended config-off cleanup path on hardware without
fault injection. Other transaction timeouts and bank2 radio0x208 at49615ms
remain; do not claim the radio or cached-only phone problem solved. B10a
unchanged/no USB reset, conns0/tunnel1. Phone switching/live-value test pending.

17:40 owner reports normal bank switching all good, then deliberately switching
before the first screen appeared produced "request device information failure"
on TUN1. **Normal-switch test PASS; early-switch reliability remains FAIL/open**,
not full application acceptance. Keep5c1 in place: its targeted retry path
worked and no internal retry/orphan recurred, but do not attribute the remaining
failure solely to switching or declare it unrelated to every firmware detail.

Final TUN1 session B33687907..33694983ms lasted7.076s; CCCD33688651,0x97
33688861/linkUP. Replay07 at33690952 (2.091s after opener), devinfo/settings/
cell300B each,900B/nine submissions rc0, valid pre-stamp headers/checksums,
no allocation failure. No live startup alignment/live devinfo or phone0x96
in that session. Earlier brief TUN1 and TUN2 visits ended62/212ms after their
openers, before reply grace. Exact timing of phone error display is not logged.
A bank1 radio0x208 at160512ms (receipt17:38:45.301) preceded phone departure
(receipt17:38:45.468); no idle teardown/internal retry/orphan caused that
radio-loss event. A's earlier valid-frame/MQTT evidence is not proof the phone
received a complete live record. Cached-only initialization again did not
carry the app through this session, consistent with earlier pre5c1 failures;
host-submission success does not establish delivery or app acceptance.

Do not rollback the independently verified retry-owner change or reintroduce
held B10b on this evidence alone. Next bounded investigation: session cleanup
and queued opener/replay ownership across rapid disconnect/reconnect, plus
cached-only handshake fidelity; reproduce locally before another firmware
change. A radio0x208 remains separate. Current A5c1/B10a stay unchanged,
all backouts preserved, phone retries paused; no new implementation/OTA.

## B notification diagnostics — owner authorized 2026-09-06 10:12

### Stage 10b — isolated B replay boundary gate, 2026-09-07

Owner said proceed after accepting10a. Red-team scope: prevent later replay
interleaving without permanently buffering/filtering live traffic, relying on
silence, assuming raw chunks begin at headers, or changing reply content,
timers, cache/NVS policy, A firmware or BMS commands. A production-function
test reproduced300 cached bytes inserted100 bytes into a live cell record.

Reuse the existing bounded scanner as an observer after startup. Live bytes
and notification chunking stay unchanged. Replay is eligible only before any
live bytes have been submitted in this app/CCCD epoch, or when the last fully
submitted live input ends exactly at a checksum-valid300-byte JK02 record.
Any suffix (AT/C8/unknown/partial header), corrupt record or failed submission
closes replay. Wait for a subsequent valid end; do not take/clear/renew the
debt while waiting. Existing2s grace,5s expiry and live-devinfo cancellation
continue to run. The epoch helper also resets an old observer when a new
session's replay tick runs before its first raw input. No extra allocation
or buffer beyond10a; established input is observed, not delayed/reassembled
for delivery. First-frame startup behavior remains10a.

Tradeoff: if every valid record shares its final raw chunk with auxiliary
suffix bytes, or no valid ending arrives, replay can expire without delivery.
Do not force it into an uncertain stream or split live chunks to manufacture
an opportunity. This requires isolated phone testing and10a backout on
regression. An incomplete/failed previous submission can still leave the phone
parser needing recovery; this gate is not an acknowledgement or delivery-loss
fix. Replay debt/session ownership across separate state operations, cached
content/counters, A queued-session tags and A radio failures remain separate.

Tests preserve10a suites and add all299 live splits at six MTUs for every
replay debt type, repeated waiting ticks, byte-exact raw/auxiliary forwarding,
checksum/counter/order checks after deferred replay, corrupt input, same-chunk
record ends and following partial headers, failed first/established sends,
replay during buffered startup and epoch resets. Actual nb_state tests check
that repeated decisions do not extend the original grace/expiry. Production
reproduction before fix passed; full final tests/build and deployment record
follow below. Only B10b is in scope; accepted10a immediate backout retained.

08:11 validation: full46,263 notification/12,880 cache/16 updater and prior
A sanitizer/TSan tests passed; log `/private/tmp/jk-stage10b-host-tests-final.log`,
executables `/private/tmp/jk-host-tests.VtM6Kk`. B build passed/config unchanged,
static observer/startup buffers still1,248B total. Preflight exact A6b1/B10a
VALID, B conns0/tunnel1/heap67916 and raw USB uptime617939->632503ms/no reset.
Candidate1,114,496B, ELF
`0d92d0147250f637b0c83595df58e0895f3542c3aa2155e8f2bd65496310033f`, SHA256
`b5512883765a0432225692c2572ea2fee43d8a5824da1d645c3d2272e83b675c`.
Save image/backout manifest at
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260907-stage10b-replay-boundary/MANIFEST.md`.
Accepted10a backout hash rechecked. Deployment/phone acceptance pending;
A6b1 remains unchanged/unaccepted, no cache/NVS/BMS-setting changes.

08:13 deployment: B10b source d815252 uploaded to ota_1 (HTTP200,
1,114,496B,14.609167s). Exact candidate ELF/OTA VALID verified at6666ms.
Paired capture opened `/private/tmp/jk-stage10b-capture.vpnacL/capture.jsonl`;
A6b1 still exact/VALID/unchanged. B phone acceptance pending, accepted10a
backout retained; no next-stage deployment until that result is recorded.

08:15 owner reports all banks worked but initial TUN1/TUN2 connections were
much slower: **10b acceptance HOLD / performance test failed**. Do not infer
causation from sequence alone. First TUN1 connection102171ms, cached devinfo
submitted103180ms (65ms after0x97), first aligned live devinfo107955ms,
phone0x96 at108245ms. TUN2 connect120855ms, cache121772ms (56ms after0x97),
aligned live123787ms,0x96 at124116ms. Thus initial cached devinfo was not
held by the new boundary gate. Neither initial session logged a settings/cell
replay after0x96; exact gate-wait versus cancellation/expiry is not logged.
A concurrently had bank1 and bank2 transaction timeouts, plus bank1 reconnect
and later radio loss. Time of the phone's actual Status paint is unknown.
The new gate may affect later fallback, but its responsibility for the
reported delay is unproven. Restore accepted10a for comparison, not a new fix.
Accepted10a image hash rechecked. Last capture still shows phone connected
to2; backout awaits owner disconnect. B10b remains running for now, A unchanged.

08:17 backout completed after owner clarified the app was killed. Fresh
pre-upload capture already showed conns0/tunnel1, so no active phone session
was interrupted. Restored accepted B10a to ota_0 (HTTP200,1,114,288B,
14.448000s), exact ELF6a70c8bd996c916bdde3243b1af3bfc5cd4975d5c235a082bc60037af587d919
and OTA VALID verified6650ms. Post-boot console14353ms conns0/tunnel1/heap67960.
Existing10b capture spans this intentional OTA reboot; do not count it as
an unexplained reset. A6b1 unchanged. B10b is NOT accepted and is no longer
running; source d815252 remains committed for diagnosis, not deployment.
Repository HEAD still contains held10b code: do not blindly rebuild/redeploy
it. Accepted10a image/manifest remains the running B recovery checkpoint.
Post-backout phone-speed comparison pending; no NVS/BMS-setting changes.

08:21 restored10a comparison: owner reports TUN2 pass, TUN1 timeout then retry
pass, TUN3 timeout. **Rollback did not eliminate the intermittent startup
failure**;10b remains held, but is not its sole cause. Exact A6b1/B10a VALID
rechecked (A26433232ms/B253744ms). Capture
`/private/tmp/jk-stage10a-comparison.Mn2bX2/capture.jsonl`.

First TUN1 B session135950..142991ms: cached devinfo submitted136977ms,
47ms after0x97,300B/3 calls/errors0; no live alignment or app0x96 before exit.
A bank1 first discovery failed rc7 (pinned NimBLE BLE_HS_ENOTCONN) following
"Reattempt connection; reason0x3e". The stack then retried automatically,
and A logged "orphan connect handle=2" and terminated it. A's own later retry
completed subscription just after the first phone session ended. The owner's
second TUN1 attempt154440ms then had live alignment155414ms, live devinfo and
0x96 at155976ms, and worked.

TUN3 B session164401..173052ms: cached devinfo submitted166780ms,30ms after
0x97,300B/3 calls/errors0; no live alignment or0x96 during session. A scans
26344036..26349062 and26351456..26356477 failed; the next attempt started
26360478 and reached opener/stream26367832, about15s after phone departure
by capture receipt times. Cached-only replies did not bridge these waits;
host submission success still does not establish over-air/app acceptance.

Pinned source explains the bank1 retry mismatch: sdkconfig enables NimBLE
connection reattempt (max3); ble_gap_master_connect_reattempt tears down GATT
and reconnects with the saved callback, while A's GAP CONNECT adoption only
accepts s_conn_inflight. An internal retry has no matching application-owned
inflight attempt and is correctly rejected by the stale/orphan safety guard.
Do not simply weaken that guard: it protects against stale slot reuse.
Next isolated review target is one owner for A reconnects (evaluate disabling
the stack's hidden retry versus explicitly tracking it), with production-path
tests and separate A deployment/backout. B cached-only initialization remains
a second open issue. No implementation/config/OTA change in this comparison;
pause phone retries, keep B10a running and10b source/deployment held.

### Stage 10a — isolated B session-start boundary guard, 2026-09-07

Owner authorized proceeding with remediation; B-only change built on diag3.
A6b1 stays unchanged and unaccepted; this does not advance 6b2/6a or fix the
separately reproduced cache selection/persistence defects.

Red-team decision: do NOT permanently replace the raw stream with decoded
records, and do not infer alignment from a raw chunk's first byte. At each
connection/CCCD epoch, B discards the uncertain live prefix and buffers until
one complete checksum-valid 300-byte JK02 record (01/02/03) is available. It
submits that record using the existing MTU/128-byte cap, then passes every
subsequent byte verbatim, including AT/C8 and unknown auxiliary traffic.
The startup prefix deliberately includes any pre-alignment auxiliary bytes:
their framing is not sufficiently established to distinguish them reliably
from an old-frame tail. This is a startup-only compatibility tradeoff to test
on the phone, not a claim of universal auxiliary-frame parsing. No timeouts,
delays, fresh BMS commands or writes are added. The first live record incurs
one-record buffering latency. The per-identity buffers occupy 1,248 static
bytes total; notification state reads now use a small locked snapshot instead
of copying the multi-kilobyte identity/cache structure.

The scanner preserves fragmented headers and slides across failed candidates
so an embedded valid start is not thrown away with a corrupt candidate. Cache
replay remains independent: it cannot unlock the live gate or interrupt the
first buffered record's submission (same tunnel-task owner). Reconnect and
off/on subscription edges use a 64-bit epoch, including same-handle reuse.
Each notification chunk and the early devinfo-attempt stamp are fenced to the
captured session. GAP callbacks never touch the parser or send notifications.
No lock spans a NimBLE call; a submission already authorized by the last
check cannot be recalled. A partially failed first-frame submission does not
open the gate. Subsequent notification-error continuation and early-attempt
replay cancellation policy remain unchanged, not delivery acknowledgements.

Limits explicitly deferred: replay can still interrupt later live records;
old complete data may already be queued by A without a session tag; delivery
loss after alignment is not resynchronized by this startup-only change;
and pending replay ownership across separate debt/cache operations is not
made transactional here. Checksum validity is not authentication or proof
of the cause of the phone's intermittent error. Keep this change isolated
and back out B if initialization or auxiliary/control behavior regresses.

Tests: production notification path checks every 299 attach/CCCD split,
all six MTUs at every first-record split, all record types and input chunk
sizes 1..320 with concatenated auxiliary prefixes/suffixes, corrupt/overlapping
headers, repeated false prefixes, replay while live bytes are buffered,
same-handle epoch changes between/during submissions, allocation/submission
failures, and per-bank parser isolation. Actual nb_state checks epoch edges,
duplicate callbacks, handle reuse and stale replay-cancellation stamps.
Existing established-stream/cache/updater and A sanitizer/TSan tests retained.
Final full suite passed:38,940 notification cases,12,880 cache equivalence
cases,16 updater cases and existing A sanitizer/TSan suites. Log
`/private/tmp/jk-b-start-full-tests-final.log`, executables
`/private/tmp/jk-host-tests.danCS4`; B build passed with sdkconfig unchanged.
Preflight07:58: both exact expected images OTA VALID; fresh B USB console
heap69292/conns0/tunnel1, uptime24724621->24741594ms without reset.
Candidate1,114,288B, ELF
`6a70c8bd996c916bdde3243b1af3bfc5cd4975d5c235a082bc60037af587d919`, SHA256
`2dd3a8820e48417f4a629b23832ab0571d65ac359b992a7af471277fec3a635f`.
Saved candidate/backout manifest:
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260907-stage10a-session-start/MANIFEST.md`.
Bdiag3 and accepted B2 hashes rechecked before deployment. OTA/phone acceptance
pending; no A, NVS/cache policy or BMS-setting change.

08:00 deployment: Stage10a source6b0c555 uploaded to B ota_0, HTTP200 in
14.591796s; exact candidate ELF/OTA VALID2 verified at uptime6623ms. Saved
manifest updated; post-boot paired capture opened at
`/private/tmp/jk-stage10a-capture.RTB0jM/capture.jsonl`. Phone acceptance pending;
A remains6b1, unchanged/unaccepted. Backout artifacts remain intact.

08:03 owner reports "works perfectly" after the requested switching/Status
test: **B Stage10a phone acceptance PASS**. Capture shows nine app sessions
in order3,1,3,2,3,2,1,3,2, with live startup alignment observed on every healthy
identity (six sessions aligned; three brief sessions ended earlier). Logged
notification summaries show zero submission errors/no allocation failure.
B stayed on the exact candidate/VALID, uptime42135->209882ms; no reset after
the intentional OTA reboot. Final phone disconnect185085ms, conns0/tunnel1
and heap recovered from60888 to67928 by204352ms. Capture still running at this
checkpoint. This accepts the isolated read-only phone test, not a long soak
or control-setting test, and does not prove all intermittent faults cured.
A remains6b1/unaccepted: the same capture contains bank1 radio0x208 after37s
streaming, bank3 transaction timeout and discovery-deadline failure. Do not
label the whole system fault-free or silently accept A based on the B test.
Next boundary work remains later-live/replay interleaving; cache selection/
persistence and A queue/command session ownership remain separate stages.
No further OTA or BMS-setting changes; prior recovery images preserved.

Owner approved targeted diagnostics after the captured TUN1 initialization
failure. This is a separate temporary B-only observability build on accepted
B Stage 2, not acceptance of A6b1 or progression to 6b2/6a. A stays unchanged.
Red-team scope: preserve notification bytes/chunks, replay debt/order/counter
stamping, failure continuation, early dev_seen marking, all timers and NVS.
Pinned NimBLE ble_gatts_notify_custom returns host-submission status and owns
the mbuf even on failure; its synchronous notify_tx event adds no independent
over-air/phone acknowledgement. Log return codes once per replay call or live
device-info header, allocation offset, accepted byte count, first error/offset,
MTU/connection, cache length/header/checksum/counter (before stamping), replay
cancellation, and GAP MTU/disconnect context. No payload/passcode logging.
Routine successful cell notifications stay quiet; non-device-info live errors
are rate limited to one log per second. Logs can perturb timing: a successful
retry on this build is not proof the intermittent fault was repaired.

Local validation: 34,996 production notify/replay invariant cases under
ASAN/UBSAN (all lengths0..320, six MTUs, allocation failures at every chunk,
submission failure with unchanged continuation, byte order, CCCD filtering,
early dev_seen, malformed/empty caches, restamping and diagnostic rate limit).
Full existing host/TSan suites and16 updater tests passed; output
`/private/tmp/jk-b-diag-host-tests-final.log`, executables
`/private/tmp/jk-host-tests.hPDcue`. Initial test-only recording-buffer and
retained clock assumptions were corrected before the passing run. Firmware
build passed, B sdkconfig unchanged. New B production delta is ble_periph.c
only; later shared jk_proto reassembly changes are not linked in B (map checked).
Preflight: A6b1/B2 exact ELF/VALID, B conns0/tunnel1/heap69324, raw USB uptime
8276056->8288660ms (no reset). Preserve B2 binary/hash before B-only OTA.
Deployment and live diagnosis pending. No BMS setting writes or A deployment.
10:20 update: diagnostic source5e7796e deployed to B ota_1, HTTP200 in13.837998s,
1,112,304 bytes; exact ELF
`84ee75839e86d1291de54f2d6d82fd317b255f68aa5435d5f95cac8a579ef7dc`
and OTA VALID verified at uptime6585ms. Manifest/image/backout instructions:
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-b-notify-diag/MANIFEST.md`.
Paired five-minute A UDP/B reset-free USB/MQTT capture started at
`/private/tmp/jk-b-notify-diag-capture.Z4go1B/capture.jsonl`.
Phone acceptance and diagnostic outcome pending. A remains unchanged6b1;
no progression to6b2/6a and no claim of fixing initialization.

Captured comparison, owner report10:24: TUN1/2/3 worked, then returning to
TUN1 produced device-information failure. Final failed B session165228..172147ms
(6.919s): CCCD165965, opener0x97 at166085/linkUP; no live devinfo header logged
in this session, replay07 at168172 (2.087s after opener). Each300-byte record
03/01/02 used three128/128/44-byte notifications at MTU517; all900 bytes
submitted with rc0, no mbuf failures, valid pre-stamp headers/checksums. Counters
186/181/207 stamped67/68/69. No0x96 followed. Last preceding live notification
submission166345, 1.827s before replay: no logged concurrent live burst during
the replay, but a partial earlier frame/app reassembly state is not measured.
First successful TUN1 session108038 had cached devinfo alone at109044 followed
by live devinfo110440 and app0x96 at110674; this does NOT prove cached-only
initialization works. Other successful sessions likewise had fresh live info.
A bank1 was already connected during the final failure (not a scan delay):
txn46 timed out1673151, then radio disconnect0x208 at1676303; last decoded cell
summary arrived10:23:39.644, timeout10:23:42.658, phone departure10:23:45.456,
radio-disconnect log10:23:45.730. No idle teardown caused the failed attempt.
rc0 proves host submission only, not controller delivery or app acceptance.
Cache semantic content, counter handling, chunk delivery/pacing and app parser
state remain unproven; do not call this an allocation failure or fixed replay.
Capture ended normally (A11230B/B82125B/MQTT56544B), B conns0/tunnel1/heap69276.

Owner said continue. Diagnostic revision2 prepared locally12:22: compare only
documented public devinfo model[6..21], HW[22..29], SW[30..37] nonzero counts
and combined fingerprint on cached/live header chunks. No payload strings or
fingerprinting of byte38+ (potential secrets), no cache choice/behavior change.
Red-team: reject adding retry, pacing or counter changes without causal
evidence; small scalar locals only, same tunnel-task ownership, existing log
frequency. Test public-field signature independence from all byte38+ values,
short chunks and byte-exact prior behavior. Tests/build and fresh preflight
pending before a separately saved B-only diagnostic OTA. A6b1 remains held.
12:24 revision2 validation passed:35,285 B cases plus full previous suites/16
updater tests, `/private/tmp/jk-host-tests.GzLrUm`; B build passed/config unchanged.
Fresh A6b1/Bdiag1 identities/VALID verified; B conns0/tunnel1/heap67512,
raw USB uptime7441016->7453518ms confirms no reset. Candidate1,112,736B,
ELF9517f3cfd0bf62bdb606b9bdc96f59a5bb82350348e494a230972e8e47ac2fd2,
SHA25692ec52e708715c75902f66dd75b5df71737ca66fc7f2c28e2be8934b3dfaec00.
Preserve revision1 and accepted B2 independently; next update B only.
12:26 update: revision2 source89695f7 deployed B ota_0, HTTP200 in14.945131s;
exact candidate ELF/OTA VALID verified at6336ms. A6b1 unchanged. Saved image,
hashes and B2/diag1 backout paths in
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-b-notify-diag2/MANIFEST.md`.
Fresh paired capture `/private/tmp/jk-b-notify-diag2-capture.fSXlCA/capture.jsonl`;
phone comparison pending. No functional fix or stage acceptance inferred.

12:30 diagnostic2 result: owner randomly switched healthy identities1..3 and
eventually got device-information failure on2. Captured final identity2 session
128169..134924ms (6.755s); CCCD128922, opener0x97 at129042/linkIDLE, replay07
at129083 (914ms after connection,41ms after opener). All three300-byte cached
frames had valid headers/checksums; all900 bytes/nine notifications submitted
rc0/no allocation failure at MTU517. Devinfo public field occupancy14/3/5 and
fingerprintA4DC1393 match successful live2 (and this fleet's other identities).
This excludes missing model/HW/SW fields in the inspected cache; fingerprint
agreement is not validation of the full300-byte payload or phone acceptance.
No live2 devinfo or app0x96 appears during the failed final session. A had lost
bank2 radio at9193543 (0x208), before that attach; scan9201949 failed9206974,
later retry9215128 connected around9220, after phone departure. No idle
termination caused this failure. On earlier bank1 session122976..128111,
cached03/01/02 was followed by app0x96 at126099 without a logged live devinfo
header in that session, but other live traffic surrounded replay. Therefore
do not claim replay never works or that any particular counter value is invalid.
Earlier B audit114359 cleaned vanished identity2 handle5, followed by a new
identity2 connection/MTU handle7; this is separate from the final captured
session, not demonstrated as its cause. Logs still cannot prove over-air byte
delivery, complete app reassembly, full cached semantics or required pacing.
Next investigation target: cached-handshake delivery/session fidelity, not
allocation, absent public versions or idle teardown. Stop unguided retries;
no third diagnostic OTA or functional fix applied on this observation.
A6b1 remains unaccepted; Bdiag2 remains diagnostic-only, backouts unchanged.

12:38 continued local review: production-cache synthetic tests reproduce two
independent flaws: score38..159 includes uptime (e.g.131071->131072 decreases
nonzero bytes and rejects otherwise identical newer devinfo); equal-score
serial changes replace RAM but skip NVS because the persistence comparison
only covers public model/HW/SW6..37. Neither is established as the phone fault.
The field boundaries agree with the upstream JK BLE decoder:
https://github.com/syssi/esphome-jk-bms/blob/main/components/jk_bms_ble/jk_bms_ble.cpp
(device info: uptime38..41, power-on count42..45, name46..61). Its parser
is not evidence of the official iOS app's complete acceptance requirements.
Reject speculative pacing/retry/counter changes for now; cache-selection
changes also interact with persistent state and require their own backout plan.

Diagnostic3 observes the actual complete-frame selection: old/new scores and
counters, keep/discard, public-fields equality and stable-body equality excluding
counter/checksum/uptime/boot count. Equality flags only: no private bytes or
private-field hashes. Logs run outside the state mutex, buffers/selection/NVS
policy unchanged. 12,880 byte-for-byte selector equivalence cases cover all
identities, empty/full old caches, lengths0..321 and record classes0..4;
assert logging outside the mutex. Full existing suites plus35,285 notify cases
and16 updater tests passed at `/private/tmp/jk-host-tests.qw8bsg`;
`/private/tmp/jk-b-diag3-host-tests-final.log`. B build passed/config unchanged.
Candidate1,113,280B, SHA256ac6415e08b7daf1b8d839596257769776b9f86e26b5b437357b90d65a27b1ca3,
ELF783512e2347699935c60e1c1cbd930b107415cd8d5a0df5b814dd819216fb02a.
Preflight/deployment pending. A6b1 unchanged; retain Bdiag2 and acceptedB2.
Diagnostic2 capture completed A11257B/B46798B/MQTT42976B, final B conns0/tunnel1.
12:40 update: diagnostic3 sourceb36b64b deployed B ota_1, HTTP200 in14.800600s;
exact expected ELF/OTA VALID verified6413ms. Image/backout manifest:
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-b-notify-diag3/MANIFEST.md`.
New paired capture `/private/tmp/jk-b-notify-diag3-capture.Kuspws/capture.jsonl`.
A6b1 unchanged/unaccepted; phone/cache-selection comparison pending.

12:46 result: owner reports "device is not supported" on1 after switching.
Last recorded TUN1 session94596..97296ms (2.700s) subscribed95435, opener0x97
95615/linkUP and departed97296, before the2s replay grace. No cached replay
was sent during that session. Complete devinfo selections for1 all keep=0,
score61->61/public_same=1/stable_same=1: cache-selection flaws did not discard
the fresh frames in this trace. Three live notify attempts97243..97248 shortly
preceded departure; none had a devinfo header at byte0 according to current
diagnostics. The next byte0 devinfo header97430 was filtered after departure.
Do NOT equate absent byte0-header logs with proof of missing headers: raw
chunks can contain concatenated prefixes and headers at other offsets.
Earlier brief TUN1 session90629..92559 did send a valid cached devinfo; latest
session is the presumed reported failure based on owner sequence, not an
independently timestamped phone error. No idle teardown coincided with it.

New local reproduction (no firmware change): A's on_notify app_connected gate
and B's forward_notify CCCD gate independently admit a headerless old-frame
suffix when enabled mid-frame. Tests of actual production functions exercise
all299 split positions for each gate. A continues to decode/cache complete
valid frames, so healthy MQTT evidence does not establish complete phone input.
Full suite passed, `/private/tmp/jk-session-boundary-reproduction.log`, artifacts
`/private/tmp/jk-host-tests.QX6K4z`; A reproduction in ASAN/UBSAN and TSan runs,
B notify suite now35,584 cases, existing cache12880/updater16 pass.
These are tests of a known unsafe boundary, not a fix or proof of the exact
phone error's byte stream. Next isolated Stage10 boundary remedy must handle
both attach and CCCD transitions, reused connections, fragmented/concatenated
headers, live/replay ordering and AT/C8 auxiliary bytes; merely changing A's
idle fence or adding notification delays will not establish these guarantees.
No additional OTA/backout/cache policy or BMS changes. Keep A6b1 unaccepted;
Bdiag3 diagnostic-only and prior recovery images preserved. Pause phone retries.

## Current checkpoint — resumed 2026-09-06

Latest checkpoint: **A Stage 6c / B Stage 2 accepted with the documented
intermittent-display exception**, 2026-09-06 09:30. Owner's fresh TUN2 phone
test passed; all four links then returned reachable-idle/app=false, conn/disc
5/5 at A uptime335–350s, OTA/BLE up. Both exact identities/VALID rechecked.
Saved pair and hashes:
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-stage6c-app-resync/MANIFEST.md`.
This accepts the isolated 6c change and its tested operating checkpoint; it
does not claim the inherited intermittent display fault fixed or retroactively
erase the failed 5b2 test. Stage 5b1/B2 remains the earlier accepted fallback.
Live update 09:56: **A Stage 6b1 deployed and exact ELF/VALID verified; phone/
idle acceptance pending**. B remains Stage 2, and 6c/B2 is the backout pair.
10:04 acceptance hold: TUN2 updated, but switching to TUN3 produced "request
device information failure". Keep 6b1 unaccepted; no further deployment or
backout yet while collecting one paired read-only retry.
10:11 captured comparison: owner reports TUN3 and TUN2 then worked, but TUN1
produced the same device-information failure. Five-minute paired capture
completed at `/private/tmp/jk-tun3-capture.LpOhPM/capture.jsonl`
(A UDP 8,839 B; B USB 27,639 B; MQTT 44,562 B). No further retries requested.
For failed identity 1, B connected at uptime7518104ms, enabled CCCD7518808,
received opener0x97 at7518988, and attempted cached replay bits07 at7519079:
975ms after connection, followed by nine NimBLE notification submissions.
No subsequent0x96 opener appears; phone disconnected at7524629 (6.525s).
A's initial bank1 scan575921 failed at580942 (RESP_LINK_DOWN); retry582971
reached MTU exchange584186/CCCD write584485, after the phone had left.
Thus real-bank cold-connect latency exceeded this phone attempt. No idle
disconnect fired during the failed initialization; later normal bank1 idle
termination occurred at644992. Successful bank3 retry had real fresh summaries.
The prompt cached replay did not complete phone initialization, but logs prove
attempts, not successful notification delivery or app acceptance. B currently
ignores notification return codes and marks device-info seen before allocation/
submission success; these are existing code flaws, not yet a proven cause of
this occurrence. No real bank1 stream was established during the failed opener,
so live-frame/replay interleaving is not supported for this captured instance.
Next useful diagnostic is B notification-result/cache metadata evidence, not
more unguided phone retries. Keep 6b1 unaccepted and 6b2/6a deployment held;
no firmware, battery-setting, reset or backout action during this comparison.
Stage 5b1 accepted 08:34 after owner TUN2 phone pass and all-bank idle/app=false,
conn/disc 10/10 at A uptime 335–365 s, OTA/BLE up. Stage 5a3 predecessor retained.
Live baseline: A Stage 6c explicitly inherits provisional Stage 5b2 and its
open intermittent phone-display finding. The 09:21 delegated checkpoint
decision below permits this isolated rollout; it does not promote 5b2 to
full acceptance. B remains unchanged on Stage 2.
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
Subsequent fresh telemetry at uptime 215–230 s confirmed all four links idle/
app=false, conn/disc 7/7, OTA/BLE up, internal free heap 99,295–99,503 (minimum
94,523). Idle release gate passed, but delayed initial phone display remains
an acceptance failure; do not promote 5b2 merely because it later recovered.

### Read-only reconnect diagnosis, 2026-09-06 08:49–08:54

At the owner's request, captured B USB and A's existing UDP log broadcast.
B was opened with raw `O_RDONLY|O_NOCTTY|O_NONBLOCK`, without termios, ioctl,
control-line operations or writes. OTA uptime advanced 2,904,873 to 2,929,950 ms,
confirming no reset on this opening. This differs from the earlier pyserial
opening that reset B. Both bounded four-minute readers subsequently exited.
A's UDP mirror was already enabled; the laptop listener was newly started.
No firmware, configuration or battery-setting changes were made.

- TUN2 connected at B uptime 3,021,923 ms; CCCD enabled at 3,022,584;
  opener 0x97 at 3,022,704; cached-reply delivery attempted at 3,022,774
  (851 ms after connection). Notification attempts continued. A second
  opener 0x96 at 3,024,415 led to replay bits 0x06 at 3,024,488 while
  live notification attempts were also occurring. The owner reported slow
  initial Status display, then switched to TUN1 (correcting an initial TUN2
  label), where the page appeared but updates stalled and later resumed.
- A's bank-1 log showed command 0x33 timeout at uptime 487,370 ms,
  disconnect reason 0x208 at 510,065, and "streamed 26s then died" plus
  app-link-up timeout at 510,312–510,314. It scanned/reconnected and resumed
  GATT setup at 512,540–514,064. This is concrete source-side interruption
  during the test, not proof that it explains the whole phone-display delay.
- B's later ten-second snapshots consistently reported `conns=1 tunnel=1`,
  with continuing notification attempts. They do not prove identity-specific
  delivery, complete frames, or acceptance by the phone. A large initial USB
  output was truncated in the displayed tool result; later tails were retained.
  UDP logging is best-effort, so missing lines are not evidence of missing GATT
  operations. Node clocks differ; do not compare their raw uptime values.

Red-team conclusion: do not diagnose every stall as one fault. A real bank-1
radio interruption is observed, whereas the earlier bank-2 display failure
occurred with fresh changing decoded values on A. Existing B delivery flaws
remain candidates: `ble_periph_forward_notify` marks device-info answered
before allocation/notify success, ignores notify return codes, and drops the
remaining bytes on allocation failure. `nb_replay_action` can then cancel all
reply bits based on that first device-info chunk. `serve` invokes replay after
each tunnel message, not each complete JK record, allowing cached records to
interrupt a fragmented live record. Code establishes these failure mechanisms;
the present logs do not establish which occurred during the owner's stall.
Keep delivery/backpressure, replay ordering and A command lifecycle fixes in
separate reviewed/tested stages. No speculative fix or backout was deployed.

Further fresh MQTT capture was blocked by the approval service reporting a
usage-limit failure; do not bypass the rejected operation through another
network/serial route. Existing captures were drained and closed normally.
Next diagnostic gate: approved bounded, simultaneous A UDP/B reset-free USB/
per-bank MQTT capture with explicit phone identity and event timing. If existing
logs still cannot distinguish receipt from notification failure, review a
separate bounded diagnostic-instrumentation change before any deployment.

09:02 owner authorized capture restart. A five-minute simultaneous capture is
saved under `/private/tmp/jk-reconnect-capture.MrS9fl/capture.jsonl`, with Mac
receipt timestamps and unfiltered source chunks (reassemble chunks before
interpreting split lines). B exact Stage 2/VALID uptime advanced 3,723,690 to
3,729,303 ms across raw USB opening; no reset. USB/MQTT and then A UDP traffic
arrived. Owner reported TUN1 "connected and updating". B logs show disconnect
at 3,766,649 ms, reconnect at 3,772,100 (only 5.451 s later), CCCD at 3,773,243,
0x97 at 3,773,393, cached replay at 3,775,416; 0x96 at 3,777,084 and replay
at 3,779,167. Both opener replays followed the existing ~2 s live-answer grace.
A connection/disconnection counters remained 10/9 and bank-1 summaries stayed
fresh: the real BMS link stayed held through this short phone disconnect.
This is a successful **warm reconnect**, not a cold-link test or resolution of
the earlier intermittent fault. Stage 5b2 remains unaccepted; captures continue.

09:06 owner reported the next TUN1 reconnect was updating. This time the phone
gap was 72.061 s (B disconnect 3,871,828 ms; reconnect 3,943,889), and A had
actually released the BMS link: MQTT disconnect 0x216 at 09:05:39.686 and
new connect at 09:05:51.922. B CCCD enabled at 3,944,639; 0x97 arrived at
3,944,819 with link=reachable-idle and replay bits 0x07 were attempted at
3,944,823, 934 ms after phone connection. The 0x96 opener followed at
3,945,630 with link=up. A logged one transaction timeout at 09:05:55.984
(link held), then FFE2 bootstrap at 09:05:57.201; fresh bank-1 summaries and
conn/disc 11/10 followed without another radio disconnect in the observed
window. This is a successful **idle-to-connected** comparison despite a startup
transaction timeout, not proof that all opener replies were delivered or that
the earlier failure is fixed. Do not infer replay cancellation solely from a
missing second replay log. No deployment, reset or battery-setting changes.
Stage 5b2 remains unaccepted pending resolution of the intermittent failures.

Stage 6a is excluded from this draft: commit `45b918f` remains on branch
`resilience-stage6a-local-20260905` with its saved local-only candidate. Revert
`43af289` removed it from the active branch while preserving its documentation;
do not accidentally restore/bundle it into a Stage 5 deployment.

## Categories and order

### Stage 6b split and isolated 6b1 candidate (2026-09-06)

Acceptance exception 10:04: owner reports TUN2 connected/updating, then TUN3
"request device information failure". The four-minute MQTT capture ended at
A uptime230s while TUN2 was still active; it does not contain the failed TUN3
exchange. Later pre-retry health at486s showed all links idle/app=false,
conn/disc6/6, bank3 last_seen103s: no successful new bank3 radio connection
since boot, but this does not distinguish a failed scan from absent demand.
No causal attribution to6b1, its boot retry, or B's known replay defects yet.

Started five-minute paired A UDP/B reset-free USB/MQTT capture at owner-facing
diagnostic step; timestamped raw chunks saved at
`/private/tmp/jk-tun3-capture.LpOhPM/capture.jsonl`. A exact6b1/VALID uptime474551ms;
B exact2/VALID uptime advanced7,416,981 to7,422,106ms across raw USB opening
(no termios/ioctl/control-line calls, no reset). B initially conns0/tunnel1.
Requested one direct-Status TUN3 retry, no settings changes. Updated the local
temporary capture helper to accept a fresh output directory and suppress
bytecode files. No firmware, settings, NVS or rollback changes; 6c preserved.

Deployment 09:56: source `3020e22`, A ota_0, HTTP 200 in 22.860347 s,
1,212,192 bytes. Exact ELF
`9aaad66956678f65cf0880a5e08009d1c5443598a3168005f04ef023a1c13d79`
and OTA VALID verified at uptime 6368 ms. Preflight exact accepted A6c/B2
identities/VALID and all-bank idle/app=false, conn/disc5/5 at1720–1750s passed.
B unchanged. Boot/phone/final idle gates pending; no repeat rollback drill,
USB access, battery-setting writes or live fault injection. Manifest/backout:
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-stage6b1-idle-fence/MANIFEST.md`.

Boot observation 09:57: all four frame-evidence checks ok at44s, OTA/BLE up.
Bank3 connected and locally terminated (0x216) at38s, reconnected42s; cause not
captured, recovery observed, not attributed to the new guard. All app flags
false and links up at64s; internal free heap98,735/min94,227, conn/disc5/1.
Requested direct-Status TUN2 read-only90s session across idle-release timing,
then disconnect. Phone/final idle acceptance remains pending; do not advance
to6b2 or6a yet. Bank0 frame evidence is not hardware acceptance.

Owner requested proceeding after 6c acceptance. Red-team split: **6b1 idle
disconnect fencing** first; **6b2 app-command/session fencing** later. Queue
retention (6a) remains excluded and unsafe to deploy until the latter review.
No stale app/MQTT command or result-correlation fix is claimed by 6b1.

Local 6b1 gives each bank a RAM-only 64-bit idle epoch, updated under the
existing state mutex on actual app and held-link edges. The supervisor tags
idle requests with its snapshot epoch; all queue copies preserve it. The BLE
owner rechecks epoch, held/no-app state, kind and source immediately before
authorizing execution. Stale requests return RESP_REJECTED to release arbiter
busy bookkeeping without sending LINK_DOWN to a later phone session. Explicit
bounces remain unfenced and unchanged; they are not used in live tests.

Generation checking alone was insufficient for a fast real-link replacement:
both edges can occur between supervisor ticks, leaving its old link-up timer.
The state cache now records the actual ready-edge timestamp; idle grace uses
the later of it and app departure. Thus an old departure/tick cannot immediately
expire a newly ready link. The strict >60 s boundary, demand-only policy and
once-per-boot verification remain. New fields are internal A structs only;
queue creation uses sizeof, request constructors initialize the new flag, and
no TCP/MQTT/NVS layout or B firmware changes are required.

Safety boundary: the final state check is the authorization point. It cannot
retract an already-authorized/asynchronous BLE operation; the app state is what
A's arbiter has processed, not proof of instantaneous phone-side state. Lost/
delayed CLIENT messages, active teardown versus a later attach, queued writes,
MQTT ownership and stale response correlation remain separate review items.
The new path retains existing link-pool -> state-mutex lock order and does not
hold the state mutex across NimBLE calls, avoiding a new lock inversion or
long critical section. This is not a complete session-safety claim.

Old actual BLE handler reproduced stale idle termination after app acquisition.
Final full host suite passes: 2,402 final-handler stale/app/link/source/kind/
64-bit epoch cases under both sanitizer builds; actual arbiter queue metadata
preservation/rejection handling; 16,025 app-transition cases; actual supervisor
snapshot/cleanup, ABA and between-tick replacement/deadline tests; six-thread
state stress with exact epoch accounting and TSan, plus all prior protocol,
discovery and 16 updater tests. Final executables:
`/private/tmp/jk-host-tests.W3FLsg`. A build passed with unchanged sdkconfig,
1,212,192 bytes. Candidate source is committed separately with saved image/hash
and accepted 6c backout. Live acceptance pending; no live fault injection or
battery-setting changes. The inherited intermittent display issue stays open.

### Checkpoint decision — 2026-09-06 09:21

Owner delegated the baseline decision after requesting continued remediation.
Proceed with **5b2 as an explicitly provisional baseline**, keeping the
intermittent display issue open. Similar symptoms predated 5b2; successful warm
and idle-to-connected TUN1 tests plus balanced final idle counters do not prove
the problem fixed, but there is no established causal link to 5b2's release
change. This is a documented exception to the earlier progression hold, not
retroactive full acceptance. Stage 5b1/B2 remains the accepted fallback.

Authorize only isolated Stage 6c A rollout next; no 6a/6b or B change. Preflight
confirmed exact live A5b2/B2 ELF and OTA VALID, all four links reachable-idle/
app=false, conn/disc11/11 at A uptime2037–2067s, OTA/BLE up, internal free heap
99,295–99,503/min94,383. Candidate and both fallback hashes rechecked. No
repeat rollback drill, live fault injection or battery-setting tests. Fresh
post-OTA phone and idle gates remain required; a new regression stops rollout.

### Resumed local work: Stage 6c before queue retention (2026-09-06)

Acceptance update 09:30: owner answered "all good" to the requested direct
TUN2 Status/read-only 30 s test. Exact A6c/B2 ELF/VALID rechecked at A uptime
289,083 ms/B5,348,741 ms. Post-session TUN2 app flag was false, then normal
idle termination 0x216 at A uptime309s; all-bank idle/app=false and conn/disc5/5
confirmed at335–350s, OTA/BLE up, internal free heap99,315–99,507/min94,707.
**Stage 6c accepted with inherited known-issue exception.** Saved/hash-verified
unchanged B Stage 2 alongside A as the next recovery pair. Earlier 5b1 fallback
and immediate 5b2 predecessor remain preserved. No further OTA, USB access or
battery-setting changes; 6a/6b still excluded, and no claim of a long soak or
live network fault-injection coverage. Command-session safety review is next.

Deployment update 09:25: A-only OTA completed to ota_1, HTTP 200 in 22.362 s,
1,212,112 bytes. Exact ELF
`cb027b91f62e5747a1110310e7e045cf6609dbb6b931d3d35f068fa7976645b0`
and OTA VALID verified at uptime 6131 ms; rechecked at 74,329 ms. B remains
exact Stage 2/VALID, uptime 5,134,098 ms; no B reset or update. Boot verification
reported all four frame-evidence checks ok at 38 s (not bank-0 hardware or
phone-delivery acceptance). Fresh telemetry showed app=false during boot,
OTA/BLE up, internal free heap 99,251/min95,023 at 34 s. Final quiet-idle and
fresh read-only phone gates remain. Saved candidate/backout manifest:
`/Users/dw/Downloads/jk-ble-tunnel-rollback/20260906-stage6c-app-resync/MANIFEST.md`.

Local preparation history (superseded by the deployment update above):

Owner requested proceeding with remediation after the captured successful warm
and idle-to-connected TUN1 comparisons. This is not a claim that the intermittent
display fault is fixed. Live A remains 5b2/B2; accepted fallback remains A5b1/B2.

Red-team review keeps 6a queue retention excluded: app commands need protection
across both pending/dispatch queues and the final BLE execution boundary, and
queued MQTT writes must not violate later app priority. Merely filtering the
pending ring cannot establish that safety. Session fencing remains separate.
Take independent **6c false-to-false resync suppression** first; no 6a/6b code
is included. This reordering changes neither the wire format nor replay policy.

Local production change is one guard in `on_app_conn`: if already disconnected,
ignore another CLIENT=false. The arbiter task is the sole production writer of
app_connected, so its locked snapshot check cannot race another app transition.
Real true-to-false departure still records its original timestamp and queues
the existing one post-app poll; true and repeated-true behavior is unchanged.
The wrong post-app opcode, dropped transition messages, pending work/session
fencing, queued stale disconnects and write confirmation are NOT fixed here.

A native test runs the actual arbiter task handler and real locked state cache
with queue adapters that capture requests and cannot access any device. Old
code reproduced the bug: initial CLIENT=false emitted one poll and set the
departure timestamp. Fixed code passes 16,025 boot/attach/departure/resync cases,
all banks, held/unheld links, repeated false before/after supervisor timestamp
cleanup, bank isolation and unchanged true resync. Full host suite passed with
ASAN/UBSAN and existing threaded TSan suites; 16 OTA updater tests also passed.
Executables: `/private/tmp/jk-host-tests.1raXFk`. A firmware build passed with
sdkconfig unchanged from baseline; 1,212,112 bytes. No B build is needed for
an A-only internal handler change, and B will not be deployed.

**LOCAL ONLY, NOT DEPLOYED OR ACCEPTED.** The candidate explicitly inherits
live but unaccepted 5b2; do not silently promote that ancestor or call this an
accepted recovery pair. Saved candidate manifest records both the immediate
5b2 predecessor and the accepted 5b1 fallback. No new OTA, reset, NVS or battery
setting changes. Before rollout, make an explicit checkpoint decision on the
known intermittent fault, verify both image identities, and arrange a fresh
read-only phone/idle acceptance test. Do not fault-inject a live tunnel/broker.

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
