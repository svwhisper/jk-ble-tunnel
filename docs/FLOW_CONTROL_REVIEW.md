# End-to-end flow-control review — 7 September 2026

## Verdict

**Pause incremental firmware remediation. The control plane needs a coherent
transaction/session model before another phone-facing patch.** This does not
mean replacing FreeRTOS, NimBLE or the two-node architecture. It means removing
competing policy drivers and making the existing queues carry explicit ownership,
completion and cancellation semantics.

The current system combines a nominally transparent phone tunnel with an active
JK client on A and a synthetic-response proxy on B. These three participants can
all advance the same handshake independently. Local safeguards now protect some
boundaries, but the end-to-end exchange has no single authoritative lifecycle.
The result is not merely excessive latency: accepted work can disappear, stale
work can execute later, and unrelated observations can count as success.

This review is analysis only. No firmware, operating configuration, BMS settings,
radio state, deployments or recovery images were changed. The scheduled 22:30
resumption was paused at the owner's request to resume now. New diagnostic
harnesses exercise production C with existing host adapters and no hardware I/O.

## Scope and evidence

Reviewed source at `20d8ca1` on `resilience-stages-20260905`, the cumulative delta
from `2f34ad8`, README, SPEC, current/session-history documentation and remediation
checkpoints, both nodes' control/data paths, shared protocol/validation/reassembly,
Wi-Fi and scheduled maintenance interactions, and the saved phone-test capture.
Also inspected the pinned ESP-IDF MQTT publish implementation locally.

Recorded live state: **A5c1 / B10c**. B10c is HOLD after failed phone acceptance;
A5c1's targeted retry correction was observed working, not proof of full app
acceptance. Tracked A/components match `0b8b526`; B matches `2c77dca`.
No fresh live-device probes were needed for this source review.

- B10b boundary observer is **excluded**, preserved on
  `hold/b10b-source-20260907`.
- A6a selective pending-clear draft is **excluded**, preserved on
  `resilience-stage6a-local-20260905`. Its proposed retention fix must not simply
  be reinstated: retaining commands without session/expiry rules can retain
  dangerous stale work.
- Eleven undesirable behaviors were reproduced under ASAN/UBSAN using four
  small review harnesses. These assert that defects exist; passing them is NOT
  a fixed-system acceptance result. Existing test adapters do not simulate a
  complete radio, TCP connection, phone parser or all FreeRTOS scheduling.
- Capture: saved B10c `phone-acceptance-capture.jsonl` beside its rollback image.
  Node uptimes were correlated by host receipt time, not compared directly.
  USB chunks must be reassembled before reading lines. UDP is lossy; absence
  from a capture is not exhaustive proof of absence on the device.

References below are repository-relative files with line numbers at this source
checkpoint. The accompanying harnesses are in `docs/review-evidence/20260907/`.

## 1. Actual control and data paths

### Control ownership

| Participant | Actual responsibility | Conflict/gap |
|---|---|---|
| Phone | Sends97/96/settings commands and judges protocol responses | Its timeout is not represented end to end |
| B NimBLE callbacks | Adopt sessions, mark CCCD, enqueue CLIENT/WRITE, create replay debt | CLIENT admission is unchecked; no wire session identity |
| B tunnel task | TCP receive/send, cache updates, live forwarding, replay scheduling | Blocking TCP can also stop replay/grace processing |
| A tunnel task | Enqueue phone demand/commands; publish app-write diagnostics | TCP arrival is not arbiter admission; diagnostics can block it |
| A arbiter | Per-bank FIFO/busy bit, connect injection, app guard, readback | No outstanding command-id check, no priority queue, no full lifecycle |
| A supervisor | Sample link state, bootstrap, several reconnect drivers, idle/verify | Periodic observations generate commands and can miss link generations |
| A decoder | Decode/cache/publish; additionally queue96 and FFE2 trilogy | Data consumer is also an unsynchronised handshake owner |
| A BLE task and NimBLE host | Execute requests, discovery, responses, timeouts and raw forwarding | Discovery is locked; notify/scan paths still share mutable state without that lock |

Control path:

`phone → B queue → TCP → A arbiter inbox → bank FIFO → BLE queue → ATT/BMS`

Data path:

`BMS → A raw queue → TCP → B session-start filter → phone notifications`

The same BMS input also goes through A reassembly into the decoder queue and
the tunnel queue. Complete frames refresh B's cache; B can inject those cached
frames into the phone stream independently of the live raw stream.

### Queue and completion inventory

| Queue | Capacity | Current overload/lifetime behavior |
|---|---:|---|
| B outgoing CLIENT/WRITE/resync |8 | WRITE reject only at this first boundary; CLIENT/resync failure ignored; queue survives tunnel loss |
| A arbiter inbox |24 |20ms bounded send, then log/drop for every message type, including ownership and writes |
| A per-bank pending FIFO |8 | Log/drop on full; whole-ring clear at link-up; pop/requeue-to-tail can reorder on BLE-queue saturation |
| A BLE requests |12 |20ms admission attempt; no app epoch/overall expiry |
| A BLE responses |12 |200ms wait then drop; losing a response can leave bank busy indefinitely |
| A raw + complete tunnel records |8 | Unchecked zero-wait drops; no loss marker/session identity |
| A decoded records |8 | Independent unchecked drops; may disagree with phone/cache/evidence path |
| A outbound control/state |24 | Log/drop; refresh repairs IDENT/LINK eventually, not arbitrary results |

Queue capacities are bounded, which protects memory. **Bounded memory is not
bounded completion:** losing an ownership event or terminal response is not
equivalent to dropping replaceable telemetry.

## 2. Prioritised findings

Severity below is defect impact. Implementation risk is listed separately in
the proposed sequence. Most central defects are inherited from the baseline;
the later safeguards do not eliminate them.

### F1 — High: link-up bootstrap deletes accepted commands

`node_a/main/supervisor.c:202` queues CLEAR,97,96 on observing LINK_UP.
`node_a/main/arbiter.c:504` clears the entire ring, not just obsolete internal
polls. App and MQTT writes waiting for the connection are silently deleted.
The three messages are separate bounded admissions, not an atomic sequence.
The connect response can dispatch a pending command before CLEAR is handled;
remaining commands can then be discarded. Depending on task order, the same
phone sequence can therefore work or fail.

**Reproduced:** queued app and MQTT writes both disappear under actual ARB_CLEAR.
Inherited defect; the held6a draft recognised part of it. Correct repair requires
session-aware cancellation and ordered bootstrap, not merely “keep all writes”.

### F2 — High: transaction completion does not identify the transaction

- `arbiter.c:322` accepts any response for a bank and clears `busy`, ignoring
  `cmd_id`; it does not store an authoritative outstanding id/deadline.
- `ble_owner.c:327` completes any active poll on any complete record. The
  `want_record` field at71 is unused. A cell frame can “answer” a97 device-info
  request, releasing the next command before the actual device-info exchange.
- `ble_owner.c:764` identifies a write callback by current connection handle
  and current transaction kind, not the submitted operation's generation/id.
  A late callback after timeout can acknowledge the next write.
- `ble_owner.c:875` has no general “transaction already active” rejection before
  replacing `l->txn` for ordinary writes/polls. It assumes the upstream busy
  bit is correct, although the paths above can invalidate that assumption.
- `ble_owner.c:946` reports RESP_OK for FFE2 even when its submission returns an
  error; write-request FFE2 also lacks an acknowledged-completion callback.

**Reproduced:** stale-id response dispatches a second request; CELL completes
DEVICE_INFO poll; late ATT completion acknowledges replacement command43;
FFE2 ENOTCONN is reported OK. All are inherited behavior, not caused by10c.

Request admission, ATT submission, ATT acknowledgement, matching JK response
and phone acceptance must remain distinct results. Unsolicited telemetry is
useful data, not a universal transaction acknowledgement.

### F3 — High: phone ownership does not survive the queue boundaries

`B tunnel_cli.c:28,44,52,117` carries no session id, command id or expiry.
CLIENT updates share the8-slot queue with WRITE and their enqueue result is
ignored. Resync appends TABLE_REQ plus four CLIENT snapshots behind old queued
messages; it neither reserves capacity nor waits for a snapshot acknowledgement.

`A arbiter.c:357` does not revoke queued app writes on real departure.
`A tunnel_srv.c:230` does not clear app demand on tunnel death. If B cannot
reconnect to deliver false, A can retain phantom demand and keep retrying.
Conversely a lost true can leave A unaware of the phone even though writes arrive.
`TUN_RAW` and complete records carry no real-link or phone-session generation,
so B10a/10c's local epoch cannot reject old data already queued upstream.

**Reproduced:** a queued app write dispatches after phone disconnect. This is
important for settings as well as openers. B10c correctly fixes one *local cached
burst* ownership hole; it does not establish end-to-end cancellation.

### F4 — High: competing bootstrap drivers, stale session flags and missed edges

Startup requests come from:

- app attach (`arbiter.c:365`), plus implicit connect-before-command (`:124`);
- supervisor link-up CLEAR/97/96 (`supervisor.c:202`);
- supervisor app driver every5s (`:254`), demand driver every20s (`:269`),
  unreachable probe every60s (`:311`) and boot verify driver every5s (`:154`);
- decoder96 after device-info when `!have_cells` (`decoder.c:99`);
- decoder FFE2 97/96/6C trilogy after cells, or silent-link fallback
  (`decoder.c:34`, `supervisor.c:306`).

The first valid cell frame can generate three extra commands even while the
phone is already conducting its own handshake. Internal trilogy traffic calls
`arbiter_app_write`, falsely labelling its source SRC_APP. This also prevents
clean cancellation and accurate accounting.

`s_stream_armed` is a cross-task volatile flag, marked before three unchecked
admissions succeed. `have_cells` is lifetime cache availability, never reset on
link replacement; it is used as evidence that the *new* stream is armed.
The supervisor samples `rt.link` once per tick, so UP→DOWN→UP between ticks
misses bootstrap/reset entirely. Stale decoded frames have no link generation.

**Reproduced:** one app demand queues two independent connect-driving polls in
one tick; a complete UP→DOWN→UP between ticks produces no new bootstrap.
These drivers are mostly inherited. The5a ready gate now correctly waits for
subscription ACK, but correct readiness still feeds this inconsistent policy.

### F5 — High: deadlines and retry policy do not compose

| Timer/gate | Actual scope | Problem |
|---|---|---|
| Phone attempts observed around6–7s | Phone protocol acceptance | Not a documented universal phone timeout, but the relevant observed budget |
| Scan5s, controller connect up to5s | A radio establishment | Combined worst case already exceeds9s before GATT discovery |
| CONNECT9s | Starts at BLE execution | Excludes earlier queues; timeout during scan/connect does not cancel those phases |
| App guard10s | From CLIENT(true) | Longer than observed phone attempts; not cleared on successful readiness |
| Poll/write3s | A execution | Poll can complete on wrong record; late ATT callback can affect next request |
| Arbiter backoff2→30s | Per-bank response failures | Local radio contention is treated as remote link failure |
| Supervisor holds60→600s / productive300s | Some internal drivers | Other app/verify/queued work bypasses them; messages saying “hold” do not mean no attempts |
| B replay2s grace /5s expiry | Debt creation time | No whole-handshake budget; cancellation keyed only to devinfo attempts |
| TCP dead15s / B grace8s | Loop checks | Blocking I/O can prevent either check from running |

`start_connect` (`ble_owner.c:838`) returns LINK_DOWN for a busy global scan/
connect resource. The arbiter then backs off a healthy bank. There is no fair
global radio scheduler prioritising the currently selected phone bank. An
explicit queued TXN_CONNECT bypasses the `connect_after_us` gate that only
applies to implicitly generated connects (`arbiter.c:123`). Old-bank queued work
can thus compete with the newly selected bank.

The app guard is not a stream-ready deadline. Once a link has succeeded, its
old guard remains armed; a later radio drop after that deadline can immediately
flush pending work. The guard clears **all** queued sources, only reports app
ones, and leaves app_connected true, so later supervisor ticks can retry again.

The captured scan/discovery failures are real observations, but do not prove
RF conditions are the sole cause. Local scheduling/contention and budget
interactions must be excluded before another RF or timing adjustment.

### F6 — High: TCP liveness and resync are not resilient under stalls

`A tunnel_srv.c:169,180` and `B tunnel_cli.c:130,155` use blocking read_n/send.
`select` proves only some bytes are available, not a complete header/body.
A partial message can block the only I/O loop indefinitely at application
level. `send < 0` treats a short positive send as full success, losing framing.
No application send/receive deadlines bound these calls.

B also blocks in DNS/connect before checking its8s grace. It marks `s_up=true`
before resync finishes. A accepts a replacement client only after serve_client
returns: its “replace half-open client” comment is not the implementation.
Both sides drain outbound queues without a per-loop budget before servicing
input/control. B's replay scheduler shares this same potentially stalled task.

Required behavior: bounded I/O, retained partial-send offsets, incremental
framing, fair control/data service, a tunnel generation and a completed resync
barrier before queued application work is eligible. A healthy TCP connection
does not prove the two nodes agree about session state.

### F7 — High: replay completion conflates different response obligations

`B nb_state.c:228` cancels the *whole* pending mask when `dev_seen_us` advances.
`ble_periph.c:441` advances that stamp on a devinfo-looking chunk **before**
mbuf allocation or successful submission; replay also advances it. This can
cancel settings/cell debt after a96 because unrelated03 arrived, including
only its first fragment. Debt is claimed/removed before delivery; failure does
not retain the undelivered obligation. Missing cache entries at opener time
are not remembered as owed (`ble_periph.c:112`).

**Reproduced:** device-info attempt cancels SETTINGS|CELLINFO without delivery
of either.10c preserves this policy deliberately; its epoch fix does not make
that policy correct. Track each requested response/phase and a truthful delivery
milestone separately. Do not equate “saw03” with “completed97/96 handshake”.

### F8 — High: byte-stream integrity is only protected at the initial boundary

A's8-item raw/complete queue silently drops arbitrary fragments
(`ble_owner.c:335,365`). Complete-frame decode and phone raw delivery can diverge.
B's10a filter starts at a valid300-byte record but, once aligned, resumes raw
forwarding indefinitely (`ble_periph.c:395`). Loss, tunnel reconnection and
real-link replacement do not necessarily change B's phone epoch or reset it.

Cached replay can still be injected between fragments of a live record. A
single task serialises **calls**, not complete application records;10b is held
and absent. `notify_session` continues after failed chunks; after startup the
caller ignores its false result and remains aligned. There is no explicit
notification capacity/backpressure policy or loss-triggered resynchronisation.

Startup filtering also deliberately discards pre-boundary AT/C8/unknown bytes.
That fixed the demonstrated headerless-suffix issue, but compatibility with
every handshake that needs an early acknowledgement is not established.
Preserve necessary auxiliary traffic through a defined protocol arbiter, not
an unexamined “all raw” versus “all parsed” replacement.

### F9 — High: automation write admission/readback is not a safe lifecycle

`arbiter.c:189` checks app ownership only when admitting an MQTT request. A
queued write can execute after the phone takes ownership. **Reproduced.**
Readback starts before successful queue admission or actual transmission
(`:225–245`); a later command can overwrite the one active readback slot after
the3s debounce but before the15s deadline. Matching old settings can report OK
before transmission; timeout reports “written_unverified” even if the write was
dropped or never dispatched. No frame generation/time proves post-write origin.
Readback nudges can also run during app ownership.

Normal write completions are not routed back as TUN_WRITE_RESULT: the only
production call is the app link-guard flush (`arbiter.c:393`). B's advertised
consecutive-failure policy is just a comment (`ble_periph.c:490`); the counter
is not maintained. FIFO correlation is not reliable without explicit ids and
complete outcomes. An untagged delayed LINK_DOWN can terminate a replacement
phone session.

No live settings tests are warranted until admission, cancellation, dispatch,
ATT outcome and fresh readback each have distinct testable states. Automatic
retry of potentially side-effectful settings is not an assumed safe fallback.

### F10 — High: MQTT/diagnostics can block the BLE and tunnel control paths

`mqtt_task.c:31` calls synchronous esp_mqtt_client_publish from the decoder,
arbiter, supervisor, A tunnel task and some NimBLE callbacks. The pinned
`mqtt_client.c:2065` acquires the MQTT API lock and performs transport writes.
Checking EVT_MQTT_UP does not make this nonblocking. QoS0 avoids retained
outbox growth for some telemetry but still performs synchronous I/O. Faults
are published retained/QoS1 on every decoded cell frame, not only changes.

The supervisor's broker-down detector forcibly disconnects otherwise-up Wi-Fi
(`supervisor.c:350`), making a broker outage disrupt the independent phone
tunnel and OTA. Its observation does not distinguish a broker failure from a
Wi-Fi zombie. This contradicts the isolation promised by the spec.

Only supervisor tasks subscribe to TWDT; a blocked tunnel/arbiter can remain
stuck while its supervisor keeps feeding. Raising feeder priority prevented a
previous starvation reset, but is not monitoring the workers' progress.

### F11 — High/medium: partial locking leaves lifecycle races outside tested paths

Discovery/write completion/timeout paths now take the link-pool mutex, but
`BLE_GAP_EVENT_NOTIFY_RX` and `scan_event` do not (`ble_owner.c:742,784`). They
read/write link slots, txn_active, txn and scan/connect pointers also mutated
by the BLE task. Host callbacks are serial with each other, not with the BLE
task on another core. Notification arrival can race timeout/request execution;
locking only the latter does not remove the data race.

B advertising state is independently mutated by tunnel, NimBLE and supervisor
tasks without a lock/owner (`adv_mgr.c:118–165`). B's controller-truth audit
reads a handle, checks it, then unconditionally clears that identity
(`ble_periph.c:341`); a replacement session between those steps can be erased.
Use compare-and-clear against a captured epoch/handle, or route all changes
through the state owner. SUBSCRIBE should also check the attribute handle:
currently any subscribed characteristic changes the FFE1 notify flag (`:586`).

These are source-level races; this review did not claim a new TSAN reproduction
of every interleaving. Existing TSAN tests cover selected discovery/state-cache
paths, not all production callers.

### F12 — Medium/high: cache availability is not current session truth

Device-info selection counts nonzero bytes including volatile uptime and private
fields (`nb_state.c:29,88`). A new identity with a lower score can lose to the
old one; same-score serial/private changes do not necessarily persist. Existing
diagnostic tests already reproduce these issues. B reboot restores03 but not
settings/cells; A's TABLE resync still has only a placeholder for cache priming
(`tunnel_srv.c:122`). A5c1 continuing while B reboots therefore does not re-run
A's boot verification or refill all B caches.

Cell cache is eligible for **ten minutes** (`nb_state.h:22`), then stamped with
a new counter during replay; the phone has no added age indicator. That is not
fresh telemetry. Its timestamp is B's receipt time, not acquisition time, so
old queued frames can become “fresh” on reconnect. Settings have no equivalent
age gate. A full900-byte cached reply still failed phone initialization in the
latest capture; filling all cache slots alone is not an established cure.

### F13 — Medium: quiet-idle, reachability and maintenance policies disagree

- The idle fence protects against stale phone/link epochs, but does not account
  for pending MQTT/readback work. Expired idle checks can keep enqueueing
  disconnects until GAP confirms release; this was **reproduced**. Execution
  reports disconnect OK before termination succeeds/completes.
- Genuine phone departure always queues a97 placeholder reread. It can wake an
  unheld bank after a cancelled short visit and contend with the next bank.
  The source itself documents96, not97, as the settings-refresh command.
- Reachable-idle means “eligible to attempt,” not measured reachability.
  Failed scans can hide an identity, while recovery probing itself requires
  app demand; after demand is gone, that can form a recovery dead end until
  an external event/boot. Keep configured eligibility distinct from live health.
- The app-guard deadline is not disarmed after a successful link; last_seen
  conflates readiness and frames; lifetime have_cells drives per-session arming.
- `supervisor.c:342` computes `changed` after updating s_last_link at246, so
  the intended immediate MQTT change publication does not fire there. Its30s
  refresh can make retained app/link preflight flags stale.
- BLE-off neither cancels in-flight scan/connect nor gates every queued write;
  some helpers treat valid handle0 as absent (`ble_owner.c:110`). A pending
  connection can complete after off. This is the still-open6d issue.
- Nightly reboots remain intentionally unconditional, A01:00/B01:05 with2h
  guard. B reboot discards warm settings/cells. Do not silently alter the
  owner's schedule, but include these real cold states in acceptance.
- Measurement writes remain disabled/unported. Restore functions must not be
  described as operational crash-safe recovery; enabling that flag is not an
  implementation. This is outside the immediate handshake fix.

## 3. What the recent changes improved—and what they did not

| Change | Retain the intended guarantee | Remaining interaction/acceptance limitation |
|---|---|---|
| Input validation/OTA stages1–2 | Reject malformed input; prove exact running ELF and OTA VALID | Neither proves application behavior |
| A reassembly3 | Consume all bytes after a completed record; copied queue payloads | Can increase real delivered load on unchanged8-item queues; queue loss remains |
| Atomic runtime4 | No stale whole-snapshot writeback | Atomic fields do not make queued actions atomic |
| Discovery5a1–3 | Error cleanup, generation guards, discovered CCCDs, ACK-before-ready | Correctly delays “ready”; old bootstrap/queues still assume simpler timing |
| Verification5b1/5b2 | Real frame evidence; verification cannot evict active phone | Any valid record is not a streaming-status/phone test;60s leases overlap banks |
| A6c | Redundant CLIENT(false) does not invent new work | Genuine departure/CLIENT(true) resync still enqueue work |
| A6b1 | Old idle teardown cannot apply to new phone/link epoch | Does not fence commands, pending MQTT demand or duplicate release intents |
| A5c1 | Application owns retries; no adoption of hidden NimBLE retries | App budget/global radio scheduling still unresolved; do not relax orphan guard |
| B10a | Reject initial headerless suffix; align one valid record per local session | No continuous integrity/loss/tunnel-generation protection; early auxiliary bytes withheld |
| B10c | One claimed replay burst cannot adopt a replacement local session | Completion policy, stale upstream queues and cached-only acceptance remain unresolved |
| B10b (held) | Proposed boundary-safe replay | Not active, not accepted; not a general solution to the upstream handshake |

The relevant arbiter/supervisor/decoder defects largely predate this remediation.
The additions increased correctness at individual boundaries without reconciling
the complete timing/ownership model. It would be wrong either to blame every
failure on the latest patch or to declare every old policy safe because it once
passed a short phone test.

## 4. Interpretation of the latest failures

The saved10c capture supports at least two distinct classes:

1. Initial TUN1 received live03 and sent96 but did not show status. The first
   observed MQTT cell summary was6.44s after departure. This implicates stream
   startup/response sequencing, not merely a missing local cached03.
2. Failed TUN2/TUN3/final TUN1 got prompt cached replies but no observed live
   startup before the phone timeout. A logged service failure0x23e or missed
   scans. Warm retries succeeded once real traffic was available.

For the final TUN1,900 cached bytes/nine successful host submissions still did
not complete initialization. Submission success is not phone acceptance.
These captures do not prove queue-clear/callback races caused each occurrence;
new local reproductions establish those flaws independently. The next evidence
must connect *one command/session* through the entire chain, not add another
unlabelled timeout or infer causality from neighboring log messages.

## 5. Recommended coherent model

Keep the two nodes and existing task framework. Make these invariants explicit:

1. **One per-bank control owner on A.** Arbiter owns demand, connection generation,
   handshake phase, pending requests and deadlines. Supervisor posts desired
   maintenance/verification demand, not repeated opcode commands. Decoder emits
   typed frame evidence with link generation; it does not initiate a trilogy.
2. **One fair radio-establishment scheduler.** A local busy resource means WAIT,
   not remote failure/backoff. Prioritise active phone demand over verification
   and post-session refresh; coalesce duplicate connection intents. Do not
   preempt an already-issued controller procedure unsafely.
3. **A real session/transaction envelope.** Distinguish tunnel generation,
   phone session, real-link generation and command id; propagate them through
   queues/results. An idle epoch is not a substitute for all four. Every admitted
   command reaches one terminal state. Never replay an uncertain settings write
   across a tunnel break automatically.
4. **Explicit readiness stages.** Disconnected → waiting-radio → connecting →
   discovering → subscribed → starting-stream → streaming. “Have cached cells,”
   “CCCD ACKed” and “phone received status” are different observations. Expected
   response predicates and absolute deadlines belong to the relevant phase.
5. **One B output scheduler.** Cached and live records share framing/order rules;
   response debt is per type/phase, not a global03 timestamp. Session/tunnel/loss
   events invalidate partial output. Preserve required auxiliary protocol bytes.
6. **Truthful data freshness.** Cached identity may aid discovery, but stale
   cells must not be disguised as live by a fresh counter. Cache provenance and
   age belong to the contract, including B-only and nightly restarts.
7. **Separate recoverable state from commands.** CLIENT/LINK snapshots coalesce
   and reconcile reliably. Telemetry can replace older telemetry. Commands and
   terminal results require explicit acceptance/rejection, not log-and-drop.
8. **Bounded peripheral work.** Network publication and diagnostics cannot block
   BLE callbacks, control scheduling or deadlines. Workers expose progress;
   recovery targets the failed subsystem, not a healthy Wi-Fi association.

### Important design choice before implementing handshake changes

Choose and validate an explicit mode instead of mixing two accidentally:

- **Transparent active-phone mode:** forward phone commands with strict ordering;
  internal bootstrap only when no phone owns the bank. This is simpler, but
  cold-radio establishment may exceed the phone's timeout.
- **Protocol-aware startup proxy:** deliberately handle selected handshake steps
  locally while A establishes the link, then perform one controlled transition
  to live traffic. This can help cold starts, but needs proof of phone acceptance,
  per-response state and freshness. Today's synthetic03/01/02 burst is not that
  proof and must not silently stand in for it.

Recommendation: use transparent phone ownership as the baseline model, then
justify the smallest necessary startup-proxy exception with captured exchanges
and tests. Do not simply turn replay off on live nodes or remove internal
openers without establishing how cold sessions and silent modules will work.
Preserve quiet-idle/no background keepalive; do not solve latency by holding
all four BMS links indefinitely.

## 6. Revised implementation and validation sequence (proposal only)

The original isolated-stage discipline is right. Its order should now follow
end-to-end dependencies, with a written invariant per deployment.

| Step | Work | Risk / rollout boundary |
|---|---|---|
| R0 | Freeze state contract; add deterministic event/queue/clock integration harness and minimal command/session trace design | Local tests/docs only; no OTA |
| R1 | A exact transaction ids, expected responses, late-callback fencing and reliable terminal completion | High; A-only isolated stage |
| R2a/R2b | B and A bounded TCP I/O, partial sends, fair service, absolute grace/dead deadlines | High; separate node stages on unchanged wire protocol |
| R3 | End-to-end session/demand resync and cancellation; explicit admission outcomes | High; compatibility-negotiated wire changes if needed, each node independently backout-safe |
| R4 | Consolidate A bootstrap/demand/radio scheduling; replace whole-ring CLEAR only with lifecycle-aware cancellation | High; isolated A stage, one state-machine invariant at a time |
| R5 | B cached/live handshake and frame-loss recovery with per-response obligations | High; isolated B stage after local protocol acceptance model |
| R6a/R6b | Move MQTT publication off critical tasks; separately replace broker-triggered Wi-Fi reset policy | High; independent outage tests/stages |
| R7 | Correct identity/cache provenance and freshness, then write readback lifecycle | Separate changes; settings verification requires its own safety clearance |
| R8 | Reconcile docs, dead policy knobs, maintenance diagnostics and targeted worker health checks | Bundle only genuinely low-risk maintenance |

Do not deploy the entire target model at once. Some steps may be split further
after harness evidence; no step is authorised for implementation by this review.
Do not silently promote held10c/10b/6a or treat current live images as a fully
accepted pair. Preserve immediate and earlier accepted fallback artifacts.

### Required acceptance matrix

Local deterministic tests must cover combinations, not just helper functions:

- Phone leaves at every phase: queued CLIENT, scan, connect, discovery, CCCD,
  first97 response,96 response and midway through cached/live records.
- Bank sequence1→2→3→1 both before first paint and after stable streaming;
  radio-busy contention must not become a remote-bank failure.
- Duplicate/missing/delayed CLIENT and responses; same-handle reuse; stale
  callback; missed supervisor tick; UP→DOWN→UP; notification/decode delays.
- All queues at capacity: no lost ownership/completion, no command reorder,
  no permanent busy bit, no stale settings execution. Force failed admission.
- TCP header/body fragmentation, positive short send, peer stops reading,
  blackhole, reconnect before/after grace, B crash mid-phone-session.
- Cache empty,03-only, all cached, expired cells, identity replacement;
  A-only reboot, B-only reboot and nightly A-then-B order.
- Live unsolicited02 during outstanding97; ATT ACK versus matching JK reply;
  allocation/notify failure in every chunk; auxiliary C8/AT before alignment.
- MQTT down/slow while phone traffic continues; no Wi-Fi kick or unbounded
  memory. Workers/deadlines must make measurable progress.
- No phone/no pending authorised work after grace means no reconnect demand.
  Verification may observe but never evict a live phone or leak demand afterward.
- Readback for dropped/not-sent/unknown-outcome writes and concurrent app attach:
  no premature OK, no overwritten correlation id, no unrequested retry.

After local tests, use one isolated image change, verify exact ELF/OTA state,
then a bounded owner phone window with phase-labelled traces. Cold and warm
success are separate gates. No live fault injection, settings/calibration change,
or rollback drill is implied. The existing bench waiver remains a constraint,
not evidence that simulation covers real radio/app behavior.

## 7. Process corrections

The “as-built” SPEC and README still contain incompatible claims: always-held
links versus quiet-idle, disabled versus enabled writes, FFE2 versus FFE1,
strict matched-response gating versus any-record completion, full resync/cache
priming versus placeholders, WRITE_FAIL_LIMIT versus no implementation, and
all-worker watchdog coverage versus supervisor-only registration. Historical
comments also claim causes subsequently revised or disproven.

Keep history as evidence, but create one current behavioral contract and label
hypotheses explicitly. Do not use old comments as proof a protocol assumption
is true. Large passing assertion counts and short “all good” phone sessions
establish their specific tested properties—not the complete pipeline.

**Decision requested after this review:** agree the ownership/readiness contract
and R0 harness scope before authorising another implementation stage. Current
nodes and backout points remain untouched.
