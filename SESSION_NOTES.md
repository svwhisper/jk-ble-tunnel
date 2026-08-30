# SESSION_NOTES — JK BLE Tunnel

Living design/status doc. Keep current alongside code changes.

## 2026-08-30 NIGHTCAP: BANK 0 ALIVE + FFE1 IS THE UNIVERSAL WRITE CHANNEL
Owner power-cycled unit 0 ("BMS 0 was wedged") — it has streamed
continuously since (>10 min vs the historic ~100 s stall): the deaf-inbound
stall was plausibly a long-hung bridge MCU, not endemic. CHANNEL DISCOVERY:
unit 0's Telink ACKs FFE2 writes at ATT level and silently DROPS them (cell
count via FFE2: delivered, ignored); the app's FFE1 writes recalibrated it.
TXN_BALANCE_WRITE now rides FFE1 (the app/esphome channel, prop-aware op) —
regression-verified on bank 3 (trigger 0.011/0.010 round trip) and bank 0
(cell_count 15 VERIFIED APPLIED — first confirmed settings write to unit 0
ever; recal ran, faults 0). Fleet 4/4 MQTT-scriptable. Bank-0 wire-R: cells
10-13 high cluster (0.113-0.124) REPRODUCED across power-on, app recal and
MQTT recal = real physics/AFE, not stale calibration. WATCH: does unit 0
stall again over the coming days? cmd/bounce + power-cycle are the known
remedies. Bank-0 baseline in docs/ alongside the others.

**Unit-0 write quirk (owner-observed via app, 2026-08-30 night):** first
settings write gets "write error" in the app (confirmation timeout), retry
succeeds — same shape as our MQTT written_unverified-then-ok. A-side shows
ALL app writes ATT-acked promptly, no txn errors: transport exonerated.
The settings echo lagged/skipped the first apply (no cell_count:15 frame in
the app window despite the recal running). Unit-0 firmware personality:
either busy-during-rescan starving the confirm, or refuse-then-accept.
CANDIDATE IMPROVEMENT: arbiter auto-retry ONCE on written_unverified (all
whitelisted registers are absolute-value = idempotent, so a retry is safe);
would make MQTT writes to unit 0 one-shot like the trio.

## 2026-08-30 FINAL: WIRE-R RECAL SOLVED — count-write WITH BALANCER ON
The trigger IS the cell-count write — but ONLY with the balancer ENABLED:
`cell_count 15 -> 5 s -> cell_count 16` (balancer untouched, ON). Owner
proved it on bank 2 (app), then validated end-to-end over MQTT on bank 3
(both acks ok, count 15 VERIFIED APPLIED — first time ever). The
balancer-OFF step in the forum folklore was the poison: with balancer off
the unit refuses the count write (bank 2's "failures") or botches a wire
(bank 1's wire-16 + alarms). Wires re-measure SEQUENTIALLY over ~1 min.
Results: bank 2 13/16 changed (odd-cell highs dropped 4-8 mOhm — the
odd/even split was partly stale artifact), bank 3 11/16 (cell-11 outlier
0.105->0.099). All banks freshly calibrated 2026-08-30, faults 0
everywhere. MQTT: jkbms/<id>/cmd/balance/set {"cell_count":15|16}
(whitelist-clamped 15..16). NEVER while iBMS charge control is live.

## (superseded) 2026-08-30 EOD v2: WIRE-R RECAL — TRIGGER STILL UNIDENTIFIED
Falsified by experiment on bank 2: (a) the forum off/start-volt/on dance
(zero change); (b) "active balancer calibrates on engagement" — balancer
CONFIRMED running 2.5 min (summary balancing:true x152) via trigger 0.003 +
start 3.00, ZERO change; (c) cell-count write as trigger — count 15/17 was
NEVER observed applied on ANY unit (settings always 16); bank 2 refuses it
outright (writes delivered, value unchanged). Bank 1 recalibrated twice
during app sessions containing count-write attempts — actual trigger
unidentified (candidates: opcode 0xA7, alarm-state interaction, unit-
specific looseness). cell_count now MQTT-writable (0x1C, clamped 15-16) —
NEVER while iBMS charge control is live (transient error would drop
Victron power). Bank 2/3 wire-R values are stale but STABLE. Next recal
sighting on bank 1: capture with A-UDP running to catch write results.

## 2026-08-30 EOD (superseded above): WIRE-R RECALIBRATION "DECODED" (app-driven experiment)
Trigger = a CELL-COUNT WRITE (reg 0x1C, u32 LE) — even same-value. All 16
wires re-measure within seconds (+1-2 mOhm vs the stale cached values).
The forum "balancer off -> start-volt 3.00 -> on" dance alone does NOTHING
(verified: bit-identical values after a clean MQTT run of it). HAZARD: a
refused count write (17 on this 16S pack) botched wire 16's measurement —
unit raised wire_warn_bitmask bit 15 + error bit 0; re-running the trigger
(owner set 15 then 16 in the app) re-measured and cleared both alarms.
Check faults==0 after any recal. balance_start_voltage now MQTT-writable
(0x22, 2.90-3.50 V); cell count 0x1C deliberately NOT whitelisted
(protection-relevant — app only). New unknown app opcode seen once: 0xA7.
Baselines: docs/baseline_resistance_2026-08-30.json.

## READ FIRST — 2026-08-30 PM: APP-CONNECT RELIABILITY FIXED (2 root causes)

The "flaky initial app connect" resolved into two separate bugs, both
found by measurement (not tuning) and both fixed + verified:

1. **Node A multi-connect race** (`ble_owner.c scan_event`): DISC events
   arrive in bursts; `s_connecting` stayed set until the CONNECT event, so a
   duplicate DISC failed `ble_gap_connect` (busy) and FREED the in-flight
   link. The orphan completed into a freed slot; each arbiter retry stacked
   another connection. Bank 0 (Telink — advertises even while connected)
   held **4 phantom connections** within 3 s of boot (conn:7/disc:2, 2 real
   links) and went silent; on the trio the same race = the historical
   "UNREACHABLE-while-advertising" ghosts. Fix: resolve scan phase before
   connecting, separate `s_conn_inflight`, adopt only the expected
   completion (terminate orphans). Verified: clean boots, exact accounting.
2. **Node B warm replay dropped by the CCCD gate**: the app writes its 0x97
   opener BEFORE subscribing (measured with `app_probe/`), so the instant
   replay was discarded → sleeping banks timed out ("request device
   information failure"). Fix: replay owed while CCCD is off is marked
   pending and delivered by `ble_periph_replay_tick()` from the tunnel
   task's 100 ms loop. `warm_devinfo` is NVS-persisted (write-once; the
   compare skips the rolling counter byte 5 + trailing checksum); cell
   replay age-gated 10 min (never show old volts as live).

**Verified (sleeping bank 1, app-like order):** devinfo replay 197 ms after
subscribe, cell replay 1.9 s, live stream 2.6 s, full session, no crash.

**Lessons paid for in blood today:**
- `nb_identity_t` is ~3.3 KB (ten embedded caches). Copying it in a NimBLE
  callback = stack protection fault on `nimble_host` (B crashed twice).
  Use `nb_notify_ready()`/`nb_replay_ready()`; keep big buffers static.
- B's resync must send TUN_CLIENT **false** too: a B crash mid-app-session
  left A re-raising a bank for a phantom app.
- `app_probe/` (retired C3 over USB) = fake-app endpoint tester: keys 0-3
  run a session, 'o' toggles write-first/subscribe-first, hex-logs every
  notify with ms stamps. Never sends 0x6C (junk RTC).

**LATER 2026-08-30 PM — the dark-TUN class (the real "never displays"):**
Two ways an identity went dark until reboot: (1) a connection held after its
central died with no disconnect event; (2) a connection whose CONNECT event
never reached ble_periph at all (ATT served, nothing logged) — the ext-adv
instance auto-stops on connect and adv_mgr's stale `advertising=true` meant
nothing restarted it. Fixes: ADV_COMPLETE handled (controller truth + conn
adoption by INSTANCE when CONNECT is missing), per-tick `adv_mgr_audit()` +
`ble_periph_audit_conns()` (any stranded set/conn heals within 10 s), loud
logs on unmapped connects, and the connect handler's own ~3.3 KB
nb_identity_t stack copy removed (suspected author of the vanished event).
Settings frames (0x01) joined the warm replay on 0x96. Verified: kill-test
heals in 12 s; sleeping bank serves devinfo 244 ms / settings 0.7 s / cells
0.8 s after subscribe, live at 2.2 s.

**LATER STILL — the phone's opener is on FFE1 (the real fix):** the app
writes 97/96/6C to **FFE1** on the clones (live-proved; the 08-29 "opener
via FFE2" note is wrong for app→clone sessions). Warm replay was hooked
into FFE2 only, so the PHONE never received a replay — every probe pass
was a false comfort (app_probe wrote FFE2). `maybe_replay_opener()` now
serves both chars; app_probe writes FFE1. Quick re-taps that catch the
module mid-0x208 were exactly the "Request device information failure".

**FINAL 2026-08-30 PM — replay must impersonate the module perfectly:**
two last app-killers after the FFE1 fix, both from phone reports: (1) the
app ignores notification chunks >128 B (the bridge's native max) — replay
now chunks at 128; (2) replays interleaved with live TUN_RAW chunks corrupt
the app's contiguous-stream reassembly ("device is not supported" on quick
re-taps of a still-streaming bank) — replays now fire only into a quiet
pipe (no raw for 500 ms; live in-stream answers cover the opener otherwise)
and expire after 5 s. Probe-verified both modes incl. forced mid-stream
re-tap. Rule of thumb earned today: anything B sends on FFE1 must be
byte-for-byte and timing-plausibly THE MODULE — the app tolerates nothing
else.

**AND the devinfo is TWO PAGES** (post-gauntlet discovery): page A =
model/hw/sw/serial, page B = versions zeroed + passcode/mfg-date. Cache
held whichever arrived last; a page-B-only replay = "device is not
supported" / ignored — the phone likely NEVER accepted a replay before
this. Now cached per page (NVS wa*/wb*), replayed A-then-B. Node A also
gained a **UDP log mirror** (broadcast :3766, `nc -ulk 3766`) — the garage
console is finally observable; next fresh-link 0x216 thrash (bank 1,
twice seen, unexplained) will be caught in it.

**GRIND FINALE — the deaf-live-link class:** mortal modules stream while
their inbound is already dead (97 forwarded -> 'txn timeout, link held'),
and A's raw forwarding can lag a re-attach — so neither "link up" nor
"unit emitted devinfo" implies the app heard anything. B's replay logic is
now: link down -> deliver instantly; link up -> 2 s grace, cancel only on
a devinfo chunk actually forwarded to the app, else deliver anyway. A no
longer marks a dozy bank UNREACHABLE on app link-up timeout (that pulled
the TUN off the air). Jig result: 4/4 rapid-cycle bank-1 sessions got
devinfo at 1-3 s. Diagnostics kept: B logs opener/link/deliver; A logs
stream over UDP :3766.

**THE ANSWER (after the whole day): the app accepts the opening handshake
only as a WHOLE.** A lone replayed devinfo — rich, fresh-countered,
128-chunked, instantly delivered — is always ignored; the app proceeds
when devinfo arrives WITH settings + cell frames around it, like a live
armed unit. 97-replay now sends the full stamped burst (dev+settings+
cells). Phone: bank 1 went 3/3 'quick data'. Corollaries kept en route:
richest-devinfo caching, fresh counter stamping (byte 5 + re-checksum),
link-gated delivery with 2 s deaf-link grace, cancel on app-forwarded
devinfo, all delivery single-tasked on the tunnel tick. The app's PIN
prompt is Settings-only ('verify password') — not part of connecting.

**BANK-0 RECAL ATTEMPTS (2026-08-30 evening, both failed — window race):**
`cmd/bounce` (new: jkbms/<id>/cmd/bounce -> TXN_DISCONNECT; next queued cmd
re-raises the link) works — link provably drops/re-raises. But the queued
cell_count write dispatches seconds AFTER the link-up bootstrap traffic and
lands in the deaf zone (txn timeouts, strikes 3-7). The unit DOES accept
FFE1 polls at link-up instant (last_seen updates), so the window is real
and ~seconds wide. TO WIN: a first-after-connect fast lane for queued
app/MQTT writes (ahead of the opener), plus FFE1-vs-FFE2 write-path
experiments on this module. Note: verifying any recal needs bank 0's cell
stream back — same investigation as the stall.

**STILL OPEN — bank 0 stream stall:** with ONE clean connection unit 0
answers link-up polls (last_seen = link-up moment) then goes deaf: later
0x96 refreshes elicit NOTHING (tested twice on a clean handle). Its BLE
link stays up throughout. Separate investigation needed (rawcap on bank 0;
suspect the Telink UART bridge dozes independently of its radio). App
sessions to TUN-0 will at least get devinfo once NVS has cached it.

## PRIOR READ FIRST — 2026-08-30: BALANCE WRITES LIVE (O-2/O-5 resolved)

Settings writes are ENABLED and live-verified end-to-end over MQTT on a
sealed garage bank, no app involved. `JK_ENABLE_WRITES=1`.

**Register map** (decoded from the app's OWN write frames via the clones):
`0x06` balance_trigger_voltage (u32 LE mV, a **delta**), `0x13`
balance_current (u32 LE mA), `0x1F` balancing_enabled (u32 LE 0/1). Frame =
`AA5590EB|reg|04|u32|8 junk|sum8` → **FFE2**. **No auth on the wire** (O-5):
no login frame in captured write sessions; the app password prompt is local
UI only. Checksum validated over whatever tail is sent (zero tail + self-sum
works).

**Pipeline** (`arbiter.c handle_balance_set`): one-key-per-command →
whitelist → range clamp → app-active defer → 3 s debounce → build → FFE2
write (property-aware op, bank 0's inverted module included) → readback.
**Readback**: FFE2 write has no ATT ack and the BMS pushes settings frames
sparsely, so the arbiter nudges a `0x96` cell-info poll every ~4 s (ONLY
0x96 elicits a fresh 0x01 settings frame on fw 19.31 — 0x97 does NOT) and
compares the next decoded frame to the target. Only a MATCH is definitive
(`ok`+readback); a pre-write frame with the old value is IGNORED not failed
(races the ~5 s idle-link connect — this bit us live); deadline 15 s →
`written_unverified` (never a false failure).

MQTT: `jkbms/<id>/cmd/balance/set` `{"balance_trigger_voltage":0.010}` (one
key). Acks on `jkbms/<id>/ack`: `ok` / `out_of_range` /
`rejected_out_of_scope` / `one_key_per_command` / `deferred_app_active` /
`rate_limited` / `written_unverified`. All verified live 2026-08-30.

Corrected en route: whitelist ranges (trigger is a DELTA 0.003–0.100 V not
absolute; current up to 2 A); the "one write per charge cycle" rate limit
had NO reset → permanent lock after the first write, replaced with a 3 s
debounce. Measurement-mode writes split to their own gate
`JK_ENABLE_MEASURE_WRITES=0` (still unported) so balance writes don't
half-activate the measurement sequence. Host test 35/35 (builder vs app
frames). **All three registers live-verified on bank 1 2026-08-30**:
trigger 0.012/0.014 V, current 1.5/2.0 A, enable false/true — each `ok`
with matching readback, all reverted to baseline (0.010 V / 2 A / enabled).
NOT tested live: app-active defer (identical code path). NEXT: if wanted,
measurement-mode port (O-4).

## READ FIRST — 2026-08-29 EOD: ALL FOUR BANKS STREAM (unit 0 SOLVED)

### UNIT 0 SOLVED (2026-08-29 evening) — it was OUR ATT write ops all along
Un-parked and streaming full 16-cell data within minutes of the fix. The
Telink-OUI module (A4:C1:38:00:86:05, Nordic Secure-DFU service 0xFE59 on
board) exposes the SAME FFE0/FFE1/FFE2 chars but with **INVERTED write
types vs the C8:47:80 trio**: FFE1 = notify+write-NO-rsp (0x14), FFE2 =
write+notify (0x18). ATT silently drops a Write Request to a char without
the write property (and a Write Command without write-no-rsp) — so EVERY
command any era of this client sent to unit 0 was discarded on arrival.
"Connects but never streams" was never the unit; it was us.
Fix (ble_owner.c): record char properties at discovery; pick the ATT op
each char permits (trio behavior unchanged — props that allow the proven
op keep it). Also subscribe FFE2's CCCD when it carries notify, and a
supervisor **silent-link opener fallback**: a held link with no frame 4 s
after link-up gets the FFE2 trilogy blind (decoder_send_opener; the
frame-gated trigger alone deadlocked on this module — no frames until
opener, no opener until frames).
Module personality (harmless, all captured): "AT\r\n" heartbeat ~7/s on
FFE1 notify (idle AND while streaming — reasm discards it), and little
12-byte pack-voltage ticker frames on FFE2 notify (u16 LE mV + zeros).
Same JK02_32S frame layout; cells/wire-R decode with the trio's pinned
offsets. TOPOLOGY (owner, 2026-08-29): the JK units are NOT in the DC path
— they sense cell voltages + balance wires only, so JK-reported current/
power/SOC/cycles are meaningless on EVERY bank (Lynx/iBMS own those).
Bank 0's cycles:0 / SOC 100 / I≈0 are expected, not decode errors. The
meaningful BLE-unique payload remains cell volts + wire resistance.

### TUN_3 outage + fix (2026-08-29 evening, after un-parking unit 0)
Adding the 4th clone blew Node B's C3 controller activity budget: a
CONNECTABLE adv set costs 2 activities, BT_CTRL_BLE_MAX_ACT was 6 = exactly
three sets, and the refresh configures ids 0→3, so TUN_3 lost the last slot
(`ext_adv_configure[3] rc=519` = HCI 0x07 Memory Capacity Exceeded, every
30 s on B's console). Fix: MAX_ACT 6→10 (sdkconfig + defaults). Also made
B's four static-random clone addresses NVS-persistent ("advmgr"/"addrs"):
phones dedupe scans by address, so per-boot random addrs made every B
reboot mint four "new" devices + stale ghosts (the duplicate-TUN-3
sighting). Verified on-air: all four TUNs beaconing (owner + scan probe).

### NEW TOOL: scan_probe/ (house-side BLE observer)
The retired C3 ex-Node-A board, revived over USB — prints every named
advert (`ADV <addr> rssi= evt= name=`) on the USB-Serial-JTAG console.
Flash: `idf.py -C scan_probe -p /dev/cu.usbmodemXXX flash`; read with
`stty -f /dev/cu.usbmodemXXX 115200 raw -hupcl; cat ...`. Active scanning
is safe in the house (clones don't beep; garage is Faraday-walled). Node
B's own console is ALSO readable the same way when it's on the Mac's USB —
that's what caught rc=519. Mac-native BLE scanning is NOT available to
Claude (TCC SIGABRTs unauthorized processes; bleak dies silently).

### NEXT SESSION (owner, 2026-08-29): enable settings writes (O-2/O-5)
Flip JK_ENABLE_WRITES and build the write path properly. Groundwork now in
place: per-char ATT write-op selection covers unit 0's inverted props, and
the app's write frames have been captured via the clones. Scope/safety
still to design (the §10 balance-only whitelist stance, readback-verify,
which registers). Remember unit-0's FFE2 is write-WITH-response.

## Prior READ FIRST — 2026-08-29 EOD: APP-VIA-TUNNEL WORKS ON ALL BANKS

**STATUS: BLE ON (persisted; owner re-armed 13:05).** Victron currently
ignores iBMS+JKs; the owner will re-enable iBMS charge control now that
experimentation has settled. **USE CASE (owner, stated 2026-08-29): BLE is
NOT for continuous monitoring or control — that is iBMS→CAN. The tunnel is
for ~weekly app sessions (tweak params, view balance-wire resistance).**
Continuous MQTT cell streaming is a nice-to-have only; the NR charge-stop
guard gets whatever coverage falls out.

**Verified live with the owner: the app works through TUN 1, 2 AND 3, and
all three TUNs are visible CONTINUOUSLY (on-demand model, deployed to A+B
end of day — owner confirmed "tun1 connected with data immediately").**
How it works: B advertises any identity that is not UNREACHABLE (mortal
banks sit at REACHABLE_IDLE between snapshots); when an app attaches to a
clone, A's app-attach driver brings the real link up within ~5 s (bypassing
all holds) and re-raises it if the module sleeps mid-session. A reports
parked units and BLE-off as UNREACHABLE so dead clones stay dark (the TUN-0
ghost fix moved to the A side).

### THE DECODES (owner's phone captured through the clones)
- **0x6C = SET RTC.** u32 LE seconds since 2020-01-01 00:00 **AEDT**
  (offset = unix − 1577797125, measured from the app's own frames), 9
  don't-care tail bytes (the app sends recycled buffer garbage — identical
  strings recur across commands/sessions), 8-bit-sum checksum. NOT a token.
  No auth anywhere on this path; the PIN never appears.
- The app opener = 97 + 96 + 6C, **all via FFE2 write-no-rsp**, value bytes
  junk except the 6C clock. 0x67 also seen mid-session (payload zeros) —
  purpose UNIDENTIFIED.
- **Bug found + fixed:** our old replayed 6C wrote a STALE timestamp into
  every bank's RTC at every link-up. Now built fresh from SNTP (skipped
  until time is valid) — every link-up NTP-syncs the BMS clock instead.

### THE SLEEP/LATCH MODEL (final; replaces BOTH earlier reframes)
- Modules sleep their BLE ~25 s after the last received command; their own
  notify stream does NOT count as activity. Sleep = radio off → our 0x208.
  That is ALL the "churn"/"dormancy"/"crash-loop" ever was.
- A **stay-awake latch** exists but is **FLAKY per-session with per-unit
  tendencies**: bank 3 latches readily (held hours), bank 2 sometimes
  (immortal for DAYS pre-14:09, then mortal, then 12+ min unassisted at
  15:11), bank 1 never today (22–46 s always — even the APP's own opener
  failed to hold it once, 14:26). Identical fw 19.31, identical settings
  (owner compared pages: no BLE settings exist; Smart Sleep identical and
  unused). It is transient aux-CPU state. "Crown bank" = whoever latched
  last. Falsified en route: keepalive reads (removal changed nothing), conn
  latency (moot — module renegotiates to native 20 ms/0/6 s), RSSI
  (ANTI-correlates), per-unit 6C tokens (no tokens exist), settings deltas.
  esphome-jk-bms issue #732 = the same endemic behavior.
- Our opener now mimics the app: 97/96/6C all via FFE2, fresh clock,
  sequenced after the session's first cell frame (the FFE1 polls at link-up
  still elicit those first frames).

### NODE-A LEADS (real, next session)
Duplicate llevent "connect" events (2–3 for one link, no disconnect between)
correlate with bank 1 going UNREACHABLE-while-advertising at ~14:50 — a node
A reboot cleared it instantly (owner suggested it; confirmed). Suspect ghost
conn state in the NimBLE controller. Related oddity: one 3 s reconnect
bypassed the 300 s hold (15:11:13).

### Client profile (deployed)
NO GATT reads ever (bridge/ka = passive vitals {links,rssi{bank:dBm},up}).
3-regime reconnect pacing on session end: stable ≥10 min → prompt;
short-but-STREAMED → fixed 300 s hold; short-and-silent → 60 s→10 min
ladder. App/NR traffic bypasses holds. Latched banks stream continuously;
mortal banks give ~22 s snapshots every ~5m20s (a few beeps per cycle in the
garage — tune CFG_RECONNECT_HOLD_PROD_S if annoying). iBMS clean ALL day
(bitmask 0, no crash); node A internals solid (imin ~96.6 K, ota:1).

### Open threads
(1) Why/when the latch takes (rawcap matrix; app-direct vs clone timing).
(2) Bank-1 ghost-connect NimBLE lead + the hold-bypass reconnect.
(3) ~~Unit 0 never streams~~ SOLVED — see top block. Remaining unit-0 tails:
    verify TUN-0 app session end-to-end; does this module sleep/latch at all
    (6+ min unbroken on first contact + AT heartbeat suggest it never dozes)?
(4) Owner re-enables iBMS charge control.
(5) ~~GitHub upload~~ DONE: private repo svwhisper/jk-ble-tunnel
    (2026-08-29; docs/SPEC.md = canonical spec now). (6) O-2/O-5 writes —
    SCHEDULED next session (owner,
    2026-08-29): "tomorrow, we will get settings write enabled".
(Resolved 08-29: duplicate TUN-3 advert after ~6 A-reboots — gone after a
Node B restart + app restart; recurrence would point at B's adv-set table.)

### The incident (11:42–12:20)
Bank 3's JK latched **"CPUAux error"** (per-bank errors_bitmask 4) at 11:44,
minutes after a fresh BLE re-bootstrap from our node. The iBMS relayed it as
"B:3 Internal BMS fault", itself crashed + rebooted (11:42–11:45), came up
alarm-latched with CAN CCL/DCL=0 → **the inverter dropped house AC**. Owner
restored power by removing the iBMS from Victron control; bank 3's fault
cleared only by power-cycling that unit (fuse pull). All four banks verified
healthy throughout via the .5 archive (`~/bms-history/bms.db`, ts in UTC,
per-bank `errors_bitmask` in pack_readings). iBMS reboot tally: 3 on our two
garage-BLE days vs 1 in the prior ten — correlation, though the 08-18 reboot
proves it can also fall over on its own.

### THE REFRAME (invalidates the "dormancy" framing)
The JK-PB's **aux CPU IS the BLE/RS485 bridge MCU**. Working model now: our
client pattern (hours-held links + 15 s keepalive GATT reads + bootstrap
storms) **crash-loops the aux CPU** — the ~30 s 0x208 "dormancy" churn was
watchdog resets, the per-bank stable/churny gradient is per-unit tolerance,
bank 3's latched fault was an escalation of the same, and the iBMS crashes
are plausibly collateral (garbled RS485 from a crashing bridge hitting a
fragile parser). The phone app never does any of this — which is why it
never hurt anything. Disproven along the way: 6C-at-linkup timing, 6C via
FFE2 (kept anyway — it's the app's own path), "app session latches the
module", "keepalive read is life support" (the proc-pool starvation WAS real
and fixed, but reads don't keep modules alive — they may be a crash driver).

### Real bugs found + fixed today (ALL deployed to A; B has the OTA fixes)
1. Resync tail-drop: announce = 9 frames into an 8-deep silent-drop queue on
   the tunnel task → queue 24 + WARN on drop + 30 s IDENT+LINK refresh; B
   advertises LINK_UP-only (killed the "TUN 0+2 only" ghost). (878653c)
2. NimBLE GATT proc pool 4→16; keepalive never stacks (ka_pending), every rc
   checked; per-cycle vitals on `jkbms/bridge/ka` incl. per-bank link RSSI.
3. OTA overhaul: `ota_start` moved BEFORE NimBLE (it lost the boot resource
   race, bound the port, failed task-create, LEAKED the listener → phantom
   accepts-but-never-serves + EADDRINUSE forever); supervisor retries;
   bounded recv timeouts (half-open upload wedged httpd for a boot);
   esp_timer reboot (xTaskCreate reboots failed silently under RAM
   exhaustion); `ota` flag in health. Verified end-to-end over the air.
4. Internal-RAM starvation (the soil under #3): health `imin` hit **23
   bytes** with BLE armed. ALWAYSINTERNAL 8192→512, reserve 64→128 K, NimBLE
   heap → PSRAM. Now ifree ≈100 K, imin ≈95 K. Health carries ifree/imin.
5. BLE switch NVS-persisted, auto-arm removed. (4c53642, 98461c8, 459ca59)

### NEXT SESSION — design the GENTLE CLIENT before any re-arm
1. Research first: esphome-jk-bms runs JK-PB over BLE 24/7 at scale — pin
   down its session profile (no GATT reads? conn params? reconnect cadence?)
   as the known-survivable reference. We already know: it writes 0x96 on
   FFE1 write-no-rsp and simply re-kicks when the stream stalls.
2. Candidate profile: NO keepalive reads; connect, bootstrap 97/96(/6C),
   stream until the aux CPU resets, reconnect with LONG backoff (minutes,
   not seconds); no re-arm storms.
3. Canary rollout: ONE bank (not bank 3), watching `bridge/ka` RSSI,
   llevent, and the iBMS per-bank errors_bitmask (the archive poller is the
   tripwire). Escalate to more banks only after a clean multi-hour soak.
   **Owner go/no-go gates every step.**
4. Still open behind that: unit 0 (Telink) never streams; O-2/O-5 writes
   (JK_ENABLE_WRITES stays 0); GitHub upload.

### Ops crib (current)
- OTA: `tools/ota_push.py a|b --host <ip>` (A=192.168.3.239 garage,
  B=192.168.3.234 house). Self-healing now; `ota:1` in health = serving.
  Preflight can false-fail on WiFi power-save latency — just retry.
- BLE: `jkbms/bridge/cmd/ble on|off` — persists across power cycles.
- Vitals: `bridge/health` {heap,min_heap,ifree,imin,rssi,up,ota,ble,conn,
  disc}; `bridge/ka` {ok,skip,err,rssi{bank:dBm}}; `bridge/llevent`.
- Archive on .5: `~/bms-history/bms.db` (UTC!); iBMS read-only at
  `192.168.3.90/api` (top-level `bms_num` is NOT a bank count — use
  modules_online/offline; per-bank faults in pack_readings.errors_bitmask).

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

## [SUPERSEDED by READ FIRST above] MISSION COMPLETE + DORMANCY HUNT PENDING (2026-08-29 morning)

**Delivered & in production:** JK app works FROM THE HOUSE on TUN 1 & 2
clones with live garage-bank data (spec's core promise). 3-bank fleet
(1/2/3) streams to MQTT. S3 Node A on garage power: **BLE auto-arms 30 s
after boot** (guaranteed OTA window every reboot — lockout-proof, owner
design; cmd/ble any time overrides). Node B in the house, TUN-prefixed
clones. Idle-disconnect disabled (was pointless 0x216 churn). Latest
commit 194845e.

**Protocol (final):** bootstrap = 0x97, 0x96, then **0x6C** (the app's
third command, captured through the transparent clone — replayed verbatim,
20-byte frame hardcoded in supervisor.c). 6C = stream-enable: redeemed
"flaky" bank 1 and "never-arms" bank 3 instantly. The BMS piezo-acks
EVERY command frame — a streaming unit must hear NOTHING (silent-client).

## [SUPERSEDED — "dormancy" reframed as aux-CPU crash loops; see READ FIRST] NEXT SESSION — THE DORMANCY HUNT
1. **0x208 cycle persists on some boots despite 6C** (~1 drop/30 s, one or
   two banks; TUN_3 flickers in house scans when bank 3 is the victim).
   Suspects: 6C delivery timing/ordering on link-up (raw write races the
   96?), 6C payload session-sensitivity, or per-boot module state.
   Tools: jkbms/bridge/llevent (per-bank reasons), rawcap, appwrite.
2. Consider: capture a SECOND app session's 6C (does the payload differ?).
3. Unit 0 (Telink module): connects, never harvests/streams even with 6C
   + relaxed layout check — own case, likely different protocol dialect.
4. Later: O-2 writes, TX-power tuning, move OTA base URL story to GitHub.

Ops crib: push = tools/ota_push.py a|b --host <ip> (A=.239 garage, B=house
lease); cmds jkbms/bridge/cmd/{ble,reboot,scan,rawcap,gattdump,nvsclear};
vitals = bridge/{health,boot,llevent}. A auto-arms BLE 30 s post-boot.

## S3 PORT — DONE (2026-08-28 evening, autonomous session)

**Node A now runs on the ESP32-S3 Gold Edition** (16 MB flash, 8 MB octal
PSRAM, dual core). Port took one config pass — the codebase was already
target-clean:
- `set-target esp32s3`; 16 MB dual-slot OTA layout (2x 3 MB slots);
  SPIRAM=y OCT mode + SPIRAM_USE_MALLOC → **heap is now 8.4 MB** (the whole
  C3 heap-crisis class — 45 KB spikes, 76-byte floors — is buried).
- Console on UART0 (the cabled port is the CH340 bridge; the C3s used native
  USB-Serial-JTAG). NIMBLE_MAX_CONNECTIONS=4.
- **Old C3 Node A RETIRED**: otadata + both app headers blanked over USB —
  bootloader halts; no hostname/MQTT/tunnel collision. (The C3 board is
  reusable: reflash bootloader+table+app to revive.)
- Validated: octal PSRAM init, WiFi, MQTT birth+health, **OTA round-trip by
  hostname** (jk-node-a.localdomain → new lease .239), **A↔B tunnel up**
  (Node B needed one reset to drop its cached DNS answer for the dead C3's
  .241 — lwIP caches by TTL; remember this after any A re-addressing).
- **Owner correction (important):** the desk-era "marginal indoor BMS
  connections" NEVER existed — those were Node B clone self-loops. The
  Faraday cage is absolute: the house cannot reach the BMSs, period.
- Unattended scan-path soak left running (BLE on, banks 1+2 targeted but
  unreachable, clones excluded by public-addr match → chirp-proof).

## NEXT SESSION PLAN
0. Overnight S3 soak PASSED: 15.1 h, heap floor unmoved by one byte, zero
   LL events, zero reboots. Attended garage session:
1. **NO BMS power-cycles — owner constraint (fuse pull next to open battery
   terminals; hazardous; last resort only, owner's call).** Place S3-A in the
   garage (~1 m+ from AP), `ble on`, listen: expect one chirp per bank then
   silence; counters prove it. A misbehaving unit gets PARKED remotely.
2. If stable: re-enable bank 3 + unit 0 stepwise (same counters), then the
   Node B GATT-mirror crash fix + phone test, then O-2 writes.
3. If churn returns even on the S3 with clean modules: the remaining variable
   set is small (module firmware behavior vs our hold-forever policy) — the
   never-terminate policy means any churn is now module-initiated and the
   0x208/0x213 reason codes will say so.

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
