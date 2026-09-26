# What decides whether the JK app shows Status — 26 September 2026

## Sources

The evidence is every captured phone session from 7 to 26 September: 33
sessions across 7 passive captures. Node B ran the same image (Stage 10c, ELF
`e60ff867…`) in all of them. Node A ran R1a, R1b, R1c, R4a and earlier stages.

Run `python3 tools/attach_sessions.py <capture.jsonl>…` to regenerate the
per-session table (captures come from `tools/capture_attach.py <out.jsonl>
[minutes]`). The captures and tables themselves are kept outside the repo, in
`~/Downloads/jk-ble-tunnel-local/20260926-cold-warm/`. All times are host receipt times in
seconds after B saw the phone connect.

## The rule

**Status appears reliably when the bank's live device-info frame (its real
answer to the app's 0x97) reaches the phone within about 6.5 s of attach.**

- **Failures:** in every failed session the phone disconnected itself 6.6–7.2 s
  after attaching, and no live device-info frame had arrived.
- **Passes:** every session that got a live device-info frame by 6.5 s passed.
- **B's cached burst never rescued a cold attach** (0 of 8), although it arrived
  on time (0.8–2.9 s) with correct counters and checksums in every one of those
  failures.

*Correction after the D1 test the same day:* on a warm link that is already
streaming live cell frames, the cached burst sometimes does satisfy the app:

- The 26 Sep 08:49:59 TUN 2 session: live device info came only at 12.1 s, but
  the owner saw Status at about 3 s.
- The 26 Sep 07:33:14 TUN 1 session was probably the same.
- In the 3 warm failures, the cached burst plus a live stream was not enough.

So the cached burst is at best a partial help on warm links, and no help on
cold ones.

## Where the time goes

| Attach type | Sessions | Failed | Cause of the failures |
|---|---|---|---|
| Cold (A had no link) | 21 | 8 (38%) | A did not have a usable link to the bank in time |
| Warm (A already linked) | 12 | 2 (17%) | The module ignored the app's 0x97. Live cell frames kept flowing, but no device-info reply came. |

A 13th warm session (19:24:43 on 10 September) lasted only 3.1 s, which looks
like the owner leaving early, so it is not counted.

The cold failures break down as follows:

- **Discovery scan heard nothing within 5 s:** 3 cases, all bank 3.
- **Scan found the bank too late:** 1 case, at 5.0 s.
- **The connection failed straight after connecting** (0x23E, or discovery
  error rc=7), followed by the 2 s backoff and a rescan: 3 cases.
- **A started late** because it was still busy with the previous bank: 1 case,
  which also contributed to others.

Across all captures, A's discovery scan found the bank with a median of 1.4 s,
took more than 3 s five times, and **missed entirely (5 s) 7 times out of 37**.
Bank 3 missed 5 times out of 10. The scan listens only 30 ms in every 100 ms,
a guard carried over from the C3 board to protect Wi-Fi. The connect that
follows already listened full-time.

## Today's attended test

The first four sessions are in the table below. All four passed.

| Session | Owner's estimate | Live device info (capture) | Notes |
|---|---|---|---|
| TUN 1 cold | 4.2 s | 2.1 s | |
| TUN 1 warm | ~3 s | 6.5 s | The cached burst came at 2.9 s. The owner's estimate may be early, or the cache helped here. This is the one ambiguous point. |
| TUN 2 cold | 7 s | 6.0 s | First connect failed (0x23E, rc=7), then a 2 s backoff and reconnect. |
| TUN 2 warm | ~3 s | 1.4 s | |

The owner's warm-up attempt (TUN 2, about 07:28:30) was slow: more than 20 s.
It began before the capture started, so only its second half was recorded.
During that half the link was up and cell frames were streaming to the phone,
but Status did not appear until the module dropped the link (0x208) and A
reconnected, which produced a fresh live device-info frame at 07:28:58.7. That
matches the warm "module ignores 0x97" failure mode.

## What was done

1. **D1:** A connects directly to the bank's public address instead of scanning
   first. This targets the largest cold-failure cause.
2. **D2:**
   - An immediate 0x23E / rc=7 failure is retried after 250 ms instead of 2 s
     while the phone is waiting.
   - On a warm link whose module ignores the app's 0x97, if no live device-info
     arrives within 1.5 s, A drops and re-establishes the link.
3. **D3:** D2's check is also satisfied by a device-info header forwarded to the
   phone, for bank 0, whose replies A rarely reassembles.
4. **B1:** the name mix-up was separate: NimBLE's advertising re-attempt
   re-applied TUN 3's name to other sets. It is now disabled on B.
5. **B left unchanged otherwise.** The cached startup burst stays as it is; its
   defects do not decide whether Status appears.

## Result: stage D1 deployed and accepted, 26 Sep 08:41

A now connects directly to the bank's public address (`1a576ab`). The owner
tested it the same morning, read-only, on banks 1–3; 22 attaches were captured
in `capture-d1.jsonl`, with the table in `sessions-d1.txt`:

| | Old firmware (7–26 Sep) | D1 |
|---|---|---|
| Cold (A link down at the app's 0x97) | 13 passes out of 21 | **10 out of 10**, live device info at 1.0–4.3 s |
| Bank 3 cold | 1 pass out of 5 | **4 out of 4** |
| Warm | 10 passes out of 12 | 11 out of 12 |

A reached the bank 0.0–0.2 s after starting to connect, where the old search
took a median of 1.4 s and sometimes never found it. The boot verify round
passed 4/4 in 52 s. Wi-Fi and MQTT health stayed normal.

Remaining failure modes, both known:

- **Warm bank ignores the app's 0x97.** 1 failure: bank 2 at 08:50:21, whose
  link had died (0x208) and reconnected 4 s earlier. A's own request to it also
  timed out.
- **Establishment failure straight after connecting** (0x23E / discovery rc=7),
  then a 2 s backoff: 2 sessions were slowed (4.3 s, 3.9 s), with no failures.
  The boot verify saw 3 in a row on bank 1.

Also observed: 3 times, a tap the owner intended for TUN 3 connected to TUN 2's
advertisement, each time straight after a bank 2 or 3 session. B served the
bank matching that advertisement, so the bridge mapping is consistent. The
cause is on the phone or app side and is not yet explained.

## Caveats

- **Small sample.** There are 33 sessions and only 10 failures, and A's
  firmware varied across the captures.
- **Timing rather than proof.** The ~6.5 s cutoff is inferred from when the
  phone disconnected, not from the app's code.
- **Host timestamps.** These are not radio times. UDP logs are lossy, so an
  absent A line does not prove an event didn't happen.
