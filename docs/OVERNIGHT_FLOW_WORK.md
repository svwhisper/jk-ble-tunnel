# Unattended local remediation — 7 September 2026

**No live-node changes.** Recorded A5c1 / B10c HOLD remain installed. All
recovery images and held branches remain intact. Work is on the separate local
branch `flow-control-local-20260907`, starting from deployed-source checkpoint
`20d8ca1`. These stages are not phone accepted and must not be bundled into an
unreviewed OTA image. B10c's failed acceptance remains unresolved.

## R0 — contract and deterministic harness

Checkpoint `946bd3c`: archived the completed review and its historical
reproductions, added `FLOW_CONTROL_CONTRACT.md` and a finite-queue/clock test of
the real arbiter task and state cache. Eleven scenarios passed, including two
explicit expected failures (stale responses and queue-full reordering).
Log: `/private/tmp/jk-r0-flow.log`.

## R1a — exact arbiter completion identity

Checkpoint `97c9ba8`, predecessor `946bd3c`.

Red-team boundary: accepting any response can falsely make a busy bank idle.
Fix this local identity invariant without touching bootstrap, replay, retry
timers, wire protocol, radio settings or session cancellation. A stores the
currently dispatched id and ignores results that do not match an active
operation. Ids are now 64-bit, zero-reserved, local-only and not reused within
a boot; exhaustion blocks dispatch rather than aliasing an old response.
The worker's response helper preserves the widened id. A successful queue
admission is the point at which the id is consumed.

Tests: fourteen deterministic production-arbiter scenarios passed, including
every existing result status for a delayed duplicate, implicit connect/backoff,
interleaved banks, old 16-bit boundary, 32-bit boundary, true id exhaustion and
duplicate result while idle. Full existing native suite passed with ASAN/UBSAN
and its separate TSAN runs. The FIFO defect is still explicitly expected here.
Logs: `/private/tmp/jk-r1a-flow.log`, `/private/tmp/jk-r1a-host.log`.
Pinned ESP-IDF A build passed, sdkconfig SHA256 unchanged
(`5478142f539686a596989cd700b6be4f995c78ad53a9685ae97cb676afd31485`).
Image SHA256 `7324bfd13169e3cb59256eb3e5b4455c6bdc8222bdf78581e25bc58ebf984f7e`.
Build log `/private/tmp/jk-r1a-build.log`; immutable local validation artifacts
are preserved in `/Users/dw/Downloads/jk-ble-tunnel-local/20260907-r1a/`.
This directory is **not** a deployment approval or accepted rollback image.

This does **not** repair wrong-type JK completion, late ATT callback identity,
lost completion under response-queue saturation or phone-session cancellation.
Historical defect reproducers that assert old behavior are not post-fix tests.

## R1b — retain FIFO head until admission

Predecessor `97c9ba8`. The pending ring now removes its head only after the BLE
queue accepts a copy. Saturation no longer rotates commands or consumes an id.
No queue sizes, timers, bootstrap clearing or overload reporting were changed.

Full native suite passes, including 270 deterministic arbiter/queue scenarios:
100 blocked retry ticks retain identical state and 32 full-ring cycles preserve
order through repeated saturation/drain/resume. Neither flow test now expects
a legacy defect. A firmware build passes with the same sdkconfig hash. Image
SHA256 `c92c8eae50ab6563d7b5a565eabdc720b6abb41d1e32800122bdf3fe6cbba218`.
Validation artifacts: `/Users/dw/Downloads/jk-ble-tunnel-local/20260907-r1b/`.
Logs: `/private/tmp/jk-r1b-host.log`, `/private/tmp/jk-r1b-build.log`.
Still local only; whole-ring bootstrap CLEAR and full-ring admission reporting
remain unresolved and must not be conflated with this ordering fix.

## Backout / next attended gate

Local backout starts with the preceding isolated checkpoint, preserving later
commits on this branch. The original `resilience-stages-20260905` branch remains
at `20d8ca1`; no force/reset is required. Deployed rollback binaries under
`/Users/dw/Downloads/jk-ble-tunnel-rollback/` are independent of mutable build
outputs and were not overwritten. Do not deploy a mutable `node_a/build` image
by accident. Select one stage and preserve its exact binary/manifest only after
the attended baseline/acceptance plan is agreed.
