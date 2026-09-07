# Flow-control contract — R0, 2026-09-07

Status: target contract and local implementation guide, **not an as-built or
phone-acceptance claim**. The owner authorized unattended local work after the
review. No OTA, BMS operation or live fault injection is part of this work.
Baseline source: `20d8ca1`; recorded deployment: A5c1 / B10c HOLD.

## Red-team decision

Keep the two nodes, FreeRTOS tasks and NimBLE. Do not introduce a second generic
state-machine framework, unbounded queues, automatic settings retries or a
permanently held fleet. Use A's arbiter as the single policy owner per bank.
Keep BLE procedure ownership in the BLE worker/host under one mutex. Supervisor
supplies demand; decoder supplies evidence. Neither should independently drive
the phone handshake in the target design.

Transparent active-phone ownership is the baseline: internal maintenance must
not inject an independent handshake into it. Any startup proxy on B is an
explicit, narrowly defined exception with per-response obligations. Its exact
cold-start protocol cannot yet be fixed from the captures: cached-only replies
have failed even when all three records were submitted. Do not remove existing
replay/bootstrap on hardware on the strength of this design alone.

## Invariants and owners

| ID | Invariant | Owner / implementation stage |
|---|---|---|
| T1 | At most one admitted BLE operation per bank; only its exact non-reused command id can release its arbiter gate | A arbiter, R1 |
| T2 | Callback identity is immutable across timeout, connection-handle reuse and replacement transaction | A BLE owner, R1 |
| T3 | Poll completion requires the expected valid record; unrelated telemetry still reaches normal consumers | A BLE owner, R1 |
| T4 | Every admitted request has one retained terminal outcome; a full queue cannot silently lose completion or reorder pending work | A queues/arbiter, R1 and R3 |
| S1 | Phone, tunnel and real-link generations are distinct; stale work/data cannot enter a replacement session | Both nodes, R3 |
| S2 | A departure revokes unsent work owned by that phone session; already-submitted settings have an unknown or observed outcome, never an assumed cancellation | A arbiter, R3/R7 |
| S3 | State snapshots reconcile before commands become eligible after tunnel reconnect; state and commands have different overload rules | Both nodes, R2/R3 |
| D1 | One intent per bank drives establishment; local radio BUSY means waiting, not remote failure; active-phone demand outranks background verification | A arbiter/radio scheduler, R4 |
| D2 | No demand or authorized work after grace means no reconnect; idle release is one outstanding intent and cannot invalidate new work | A arbiter, R4 |
| B1 | Live/cached output is ordered at complete record boundaries; loss invalidates partial output, without discarding required auxiliary protocol traffic | B output owner, R5 |
| B2 | Settings, cells and device information are separate obligations; allocation/submission failure is not successful delivery | B output owner, R5 |
| C1 | Cache age/provenance is not refreshed by replay or delayed transport; cached cells are not described as current readings | Both nodes, R7 |
| L1 | Bounded I/O and fair service let deadlines run even if peers stall; logging/MQTT cannot block critical control paths | R2/R6 |

## Lifetimes, readiness and results

Command ids are internal to A today. Widening them must not change TCP, MQTT or
NVS layouts. Reserve zero; do not silently wrap/reuse an exhausted id. A boot
discards all local request/response queues and callbacks together. Wire session
ids need an independently versioned/negotiated R3 design; do not serialize the
in-memory structs or silently change B's protocol.

Readiness is a progression, not a single success bit:

`no demand → waiting for radio → connecting → discovering → subscribed → starting stream → streaming`

Cached availability, CCCD acknowledgement, fresh cells and phone acceptance are
different evidence. Each link generation starts without stream evidence. Demand
may disappear at any step. An old completion can update diagnostics but cannot
advance the new generation. Do not derive edges from a sampled boolean alone.

Results distinguish rejection before admission, accepted/pending, submitted,
ATT acknowledgement (when the selected characteristic supports it), matched JK
response, cancellation before submission, timeout/unknown outcome and terminal
failure. Existing RESP_OK is transport-local; it must not imply BMS application
or phone success. The matched-record rule is weaker than a wire request id:
unsolicited records of the same type cannot be uniquely correlated by JK, and
this limitation must remain explicit.

Absolute deadlines include queue residence once commands are admitted. A local
timeout must not simply free the arbiter gate while the underlying BLE operation
can still act on the next transaction. Cancel/fence or wait for terminal worker
evidence. No automatic resubmission of settings with uncertain outcome.

## Deterministic test strategy

`tools/host_test_flow.c` drives the real arbiter loop and locked state cache
through finite FIFO adapters, controlled clock ticks and explicitly scheduled
BLE responses. Tests can fill queues, delay/duplicate responses and interleave
banks. BLE/radio execution is a scripted endpoint in this harness, **not** a
simulated phone or controller. Production BLE callbacks remain covered by the
separate discovery/transaction harness. Link those layers with broader lifecycle
tests as stages add actual session fields; do not create a parallel toy protocol
and call its passing tests firmware coverage.

R0 records two known failures (stale completion and queue-full reordering) using
explicit legacy expectations. Each fix changes its expectation to the desired
invariant and joins the normal host suite. All other review reproducers remain
historical baseline evidence: some deliberately assert broken behavior and must
not be mistaken for post-fix regression tests.

Upcoming tests must add: completion queue saturation; poll/frame interleavings;
late ATT callbacks after timeout/handle reuse; session departure at every phase;
TCP fragmentation, short send and blackhole; cold/warm caches; bank switching
before first paint; MQTT stalls. Local success does not replace attended phone
acceptance or prove a particular historical failure's cause.

## Trace contract (design only)

Emit bounded events at admission, dispatch, BLE submission, matching completion,
timeout, cancellation and session transition. Fields: boot/tunnel generation,
bank, phone generation, real-link generation, command id, source, operation,
phase, monotonic timestamp and outcome. Omit raw settings/credentials. Across
nodes correlate generations and host receipt time; never compare raw uptimes.
Counters must distinguish rejected, stale, dropped-telemetry and terminal-work
loss. Do not add synchronous MQTT publication on a critical path to obtain this.

## Staging and backout

R1 is split into smaller independently testable checkpoints: R1a arbiter exact
identity; R1b pending FIFO admission; R1c BLE transaction/callback semantics;
R1d retained completion under saturation. These do not authorize collapsing R3
session cancellation or R4 bootstrap into the same image. Keep high-risk changes
separate even if they share a finding number. Candidate branches/commits are
local only; there is no overnight deployment candidate acceptance.

No compatibility-sensitive startup choice is forced while the owner sleeps.
Record a blocked hardware-dependent gate, then continue independent local work.
For each checkpoint retain the commit, test/build evidence and predecessor;
rollback images and deployed-source branches are never overwritten. Revert or
select the predecessor only after preserving later work; never hard-reset a
dirty checkout. The first attended rollout must choose one isolated change and
explicitly address the currently unaccepted B10c baseline.
