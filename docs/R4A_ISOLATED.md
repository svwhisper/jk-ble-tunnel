# R4a isolated hardware candidate

Parent: live R1c5c9129675620a75cc189cfeadf57b72fd9c9907c.
Ported from558844a, excluding its owned-command integration because R1c has
no owned-mode machinery. Only arbiter.c, ble_owner.c and na_types.h production
files change; no B/shared/wire/configuration changes.

Red-team boundary: a local occupied scan/connect slot is not proof of remote
failure. Return local RESP_CONNECT_WAIT, retain pending demand and apply the
existing100ms anti-spin gate without increasing remote failure backoff.
An explicit CONNECT occupies its original pending slot until a non-WAIT result;
exact command IDs prevent a late result from removing a replacement after CLEAR.
Real scan failures and quiet-idle policy remain unchanged. This is not a complete
scheduler/fairness fix and does not prove the cause of the captured third refusal.

Validation uses actual R1c production arbiter and BLE code with native adapters:
477 arbiter scenarios, including100 repeated WAITs for explicit/implicit demand
with a full queue, stale response, CLEAR and genuine failure escalation; three
resource-owner/release scenarios under ASAN/UBSAN and TSan; original full suite.
The new test initially omitted its mock mutex initialization; that fixture-only
failure was saved and corrected before validation. Native tests cannot replace
controller/phone acceptance.

Build with pinned ESP-IDF5.2.3 and the unchanged R1c sdkconfig SHA256
5478142f539686a596989cd700b6be4f995c78ad53a9685ae97cb676afd31485.
Reuse the existing ignored site secret; never commit it. Freeze image, ELF,
source commit, test/build logs and hashes before OTA. No other local stage may
be included in this candidate. Fresh phone-disconnected/recovery readiness and
node identity checks precede an attended Node A-only deployment.

Immediate backout: saved live R1c image at
/Users/dw/Downloads/jk-ble-tunnel-rollback/20260912-r1c-att-lease/node_a.bin,
SHA25614e0381ad0931fd6be2a28da596ce4fc4714e19695b1b92768eaab6dc44f5f24,
ELF9da78b6985fca2db3c9635b940aeb13bcd0d12ee9f406a46a16e86028ec8cb1e.
Retain earlier R1b/R1a backouts. No repeated rollback drill is required.
