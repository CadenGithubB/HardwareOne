# Shared mesh reliability investigation — 28 September 2026

Status: shared-source audit, proposed fixes and coordinator-owned hardware evidence. The audit agent did not access devices; the primary coordinator ran the baseline linked below.

## Source and existing evidence

The current shared source still contains the three weaknesses observed in the 27 September full mesh acceptance run. The 28 September [33-check baseline](mesh-baseline.json) successfully delivered one 8 KiB file in each direction with receiver readback, along with boundary text, rekey and ordered radio reopen. It preserved existing pairing. This new success complements the earlier intermittent transfer failure; it does not add retry/commit-acknowledgment logic or establish fresh-pairing reliability. The later BLE-role coordinator explicitly does not repair mesh pairing.

Compared the tracked source with both frozen JPEG full-app copies under `experiments/jpeg_portable/private/app-{p4,s3}/components/hardwareone/`. Their `System_ESPNow_Handlers_Crypto.cpp`, `System_ESPNow_Files.cpp` and `System_ESPNow_Sessions.cpp` are byte-identical to the tracked files. Both prepared `System_ESPNow.cpp` files are identical to each other; their investigation overlay changes radio identity caching, RX metadata and initialization, not the pairing/session/file semantics audited here.

Existing evidence:

- `../p4_mesh/results/2026-09-27/RESULTS.md`: fresh pairing failed; P4→S3 8 KiB file passed; S3→P4 got 40/41 chunks and was correctly rejected by its receiver while sender reported success.
- `../p4_mesh/PORTING.md`: diagnostic duplicate KEY_EX replies caused successive SESSION_OPEN IDs; the second replaced the first, so its CONFIRM had no matching in-flight session.
- `../p4_ble_roles/results/2026-09-27/FINAL.md`: short encrypted text, web and accessory coexistence passed. This does not test fresh pairing or files.
- `../p4_io/RESULTS.md`: two encrypted 401-byte messages with display/input and accessories active. No file or fresh-pairing rerun.

The coordinator also ran fresh discovery pairing on 28 September. The first
passive-notice wait timed out but later revealed a queued receiver notice.
An active-poll repeat then observed explicit S3 acceptance; only S3 persisted its
peer, while P4 remained unpaired for the full readiness bound. See
[active-poll evidence](mesh-fresh-active.json). This is a new failed fresh-pair
result, separate from the successful preserved-pair baseline; original peer
registries were subsequently restored and verified from the full backups; see
[restoration evidence](restoration.json).

## Priority 1: make handshake initiation idempotent

Actual path:

1. `System_ESPNow_Handlers_Crypto.cpp::v4hKeyExReply` persists the verified identity, sends KEY_EX_CONFIRM, then calls `espnowSessionOpenInitiate` whenever pending frames exist.
2. `espnowSessionOpenInitiate` calls `sessionAllocate` without checking whether this peer/mesh already has an ESTABLISHING, ACTIVE or REKEYING session.
3. `System_ESPNow_Sessions.cpp::sessionAllocate` reuses the existing slot, zeroes its keys, resets counters, and sets ESTABLISHING. The caller installs a new random session ID and ephemeral material.
4. A second verified KEY_EX_REPLY while the first OPEN is in flight can therefore overwrite it. The earlier CONFIRM is correctly rejected because its session ID no longer exists.

The ordinary encrypted-send path already checks for ESTABLISHING before initiating, but this protection is at a caller rather than the common operation. Other callers, including explicit `espnowsessionopen`, bypass it. Existing simultaneous-opener MAC ordering resolves opposite-side glare; it does not prevent duplicate same-side initiation.

Minimal shared change:

- Put an atomic per-peer/per-mesh single-flight claim at the common initiation boundary; an existing ESTABLISHING operation returns “already pending” without changing keys, nonce, session ID or deadline. An ACTIVE/REKEYING session is retained unless an explicit reset/rekey operation requests replacement.
- Use a short lifecycle lock or serialize claims through one owning task; a bare check followed by allocation is still racy across RX, command and TX callers. Do not hold a critical section across signing, driver/RPC calls or waits, and do not casually widen the existing AEAD TX lock across those operations.
- Release the claim on immediate failure and the existing bounded establishment timeout. Preserve signed transcript checks, anti-key-substitution policy, glare ordering and replay protection.
- Duplicate valid replies may resend KEY_EX_CONFIRM but must not reopen the session. A later extension that retries SESSION_OPEN should retain its original operation identity and let the responder replay the same cached confirmation, rather than regenerate keys for a duplicate OPEN.

Focused tests: two duplicate replies while pending; two simultaneous local initiators; opposite-side glare; active-session duplicate reply; immediate send/sign failure; timeout then a new attempt; incorrect identity/signature still rejected. Assert session ID/key continuity, at most one pending operation, and eventual queued-frame drain.

## Priority 2: reliable request/accept without changing consent

Actual path in `System_ESPNow.cpp`:

- `sendPairCtrl` sends one authenticated broadcast and discards its Boolean send result.
- `cmd_espnow_pairrequest` sends before publishing `gPendingPairOut`; a fast acceptance can arrive before the matching outgoing state exists. No retry follows loss.
- `cmd_espnow_accept` clears incoming state, sends ACCEPT once, then commits local pairing. The initiator can be left without its registry entry.
- `v4h_pair_accept` clears outgoing state before submitting its deferred job. Queue failure loses the request; `runDeferredPairComplete` emits completion text even if the CLI pairing helper reports failure.
- `v4h_pair_request` rejects already-paired peers. Blindly adding request retries would therefore reject a legitimate initiator after the target already accepted and its ACCEPT was lost.
- Pair-completion helpers omit the mesh argument, so `cmd_espnow_pairsecure` defaults to slot zero. Testing against a different slot is not evidence for this path.

Minimal compatible first change:

- Return and inspect the actual send result; publish pending state before transmission and roll back only the matching failed attempt.
- Add bounded request retries, preserving one consent transaction/deadline; retries must use fresh outer message IDs or explicitly handle dedup. Do not extend the user’s discovery window through repeated packets.
- Cache a recent explicit Accept decision for the same authenticated peer and transaction window. A matching retry resends ACCEPT instead of automatically pairing again or returning REJECT. Never call `pairsecure` repeatedly: it opens a one-shot identity-replacement window, which must remain an explicit user-authorized action.
- Keep completion pending until the deferred job was accepted and local registry persistence succeeded. Use a typed internal result instead of interpreting CLI text. Emit truthful failure/pending/completed states.
- Carry the authenticated mesh context into local pairing rather than implicitly choosing slot zero.

These local changes can preserve current payloads and interoperate with old peers, but exact transaction correlation and acknowledged remote completion need an optional versioned/negotiated extension. Current payloads identify target/MAC, not a dedicated consent nonce. Do not invent an unconditional new ACK meaning or accept a generic transport ACK as consent.

Focused tests: dropped REQUEST, dropped ACCEPT, immediate send error, duplicate request before/after Accept, full deferred queue, allocation/persistence failure, expired/closed discovery, wrong target, wrong mesh HMAC, unrelated/stale Accept, identity replacement still denied outside explicit authorization.

## Priority 3: honest file completion and bounded repair

Current code already validates full chunk counts, byte counts and CRC, confines writes to the receive inbox, stages/renames files, and emits encrypted FILE_CANCEL on rejection/failure. Preserve all of that.

Actual remaining issue:

- `sendFileToMac` sends each 200-byte FILE_DATA chunk without ACK_REQ. Its three attempts retry immediate enqueue failure, not asynchronous radio loss.
- It returns success once all chunks and FILE_END were handed to the send layer (`return !aborted && endSent`). A cancellation that arrives after this return cannot change the synchronous success result.
- `v4_try_handle_incoming` sends generic ACK immediately after AEAD verification, before FILE_END completeness, CRC or filesystem commit. `v4_send_ack` emits an unencrypted generic ACK. All file frames share one transfer ID and normal fragment index zero, so these ACKs do not identify a file stage/chunk.
- The RAM receiver destroys an incomplete slot at FILE_END. The streaming receiver aborts on gaps; it cannot currently repair an arbitrary missing chunk in place.

Smallest immediate correction: separate “submitted” from “receiver confirmed” in application status. Do not advertise storage success on the current return value. This can preserve the wire format, but it is not reliable file delivery by itself.

Actual reliable completion requires explicit, authenticated feedback:

- Reserved wire IDs already earmark FILE_ACK=113, FILE_PROGRESS=114 and FILE_NACK=116; generic mesh capability IDs 37/38 are also only earmarked, not implemented. A shared extension needs version/capability negotiation and documented legacy behavior.
- FILE_ACK must be session-encrypted and bind peer, transfer ID, stage, byte count and content checksum; only emit final success after validated storage/rename. Keep a bounded completion cache so a retried FILE_END gets the same result without rewriting.
- Receiver missing-chunk feedback must be bounded and correlated. For the RAM path, retain its existing chunk bitmap until bounded repair/timeout; retry only missing chunks, with a new AEAD frame sequence for every transmission. Maintain original chunk index and transfer ID.
- Streaming needs a separate design choice: bounded in-order acknowledged windows or staging-file random-access repair. Merely enabling RAM-style retries breaks its current gap-abort contract.
- Avoid waiting for completion on a task needed to finalize incoming files. Streaming filesystem jobs use `cmd_exec`; simultaneous opposite-direction transfers must not block both command executors while each waits for the other’s finalize job. An asynchronous transfer worker/state machine is safer than adding a long blocking wait to the existing command path.
- Mixed-version peers must remain usable, with truthful “legacy/unconfirmed” status or an explicit unsupported confirmation result. Do not reinterpret plain generic ACK as secure completion or silently weaken the existing pairing+AEAD requirements.

Tests: both directions at 1, 199, 200, 201, 8192 bytes and around the 128 KiB RAM/stream boundary; deterministic one-chunk loss, duplicate/reordered chunk, dropped START/END/final ACK, CRC mismatch, storage/rename failure, receiver cancellation, sender reset, timeout, and simultaneous transfers. A pass requires final receiver readback equality/hash and a sender result consistent with receiver commit.

## Safe hardware baseline using current restored images

First inventory through each board’s own authenticated USB console, using its existing account (credentials stay in an ignored mode-0600 file and are redacted by MeshConsole). Do not assume both boards use the old shared test account.

Exact read-only commands, on BOTH boards:

```text
whoami
status json
espnowstatus json
espnowmode json
espnowchannel
espnowmeshes listjson
espnowlist
espnowsubs json
espnowsessions json
espnowstats json
```

Use `loglink on` for the runtime console if it is not already on, so IDF warnings cannot split command completion output. Record and restore its prior value if changed. Serial open/reopen may reset these boards; keep one owner and avoid gratuitous reconnects.

The P4’s mesh MAC is its companion radio identity reported by `espnowstatus json`, NOT the P4 chip/USB MAC. Never reuse the old P4 board’s default MAC just because the serial port matches. Current verified USB port names from the preceding JPEG run were P4 `/dev/cu.usbmodem2201` and S3 `/dev/cu.usbmodem1101`; re-identify before opening.

Existing reusable coordinators:

- `../p4_mesh/test_mesh.py`: `MeshRunner.text`, `file`, `read_file`, `sessions`, `stats_snapshot`, `pair`; exact 1/201/202/203/400/401/1024-byte text plus 1025-byte rejection and bidirectional 8 KiB files. Unique fixture filenames; receiver completion and byte-for-byte readback. `--pair preserve` refuses missing peer state and never unpairs. `--configure` changes configuration and is NOT a harmless default.
- `../p4_ble_roles/board_control.py`: `RolesConsole`, JSON `command`, `mesh`, `ble`, `http`; login and `loglink` handling. Its default ports, MACs and shared credentials belong to the older board pair.
- `../p4_mesh/probe_recovery.py`: preserves pairing but hardcodes previous configuration; reuse methods rather than launch blindly.
- `../p4_mesh/test_persistence.py`: compares saved identity/config/peer state before and after proven USB reboot, then verifies a new session and encrypted traffic. Also assumes one credentials object; adapt per-board login first.

Conditional exact existing-runner command (only after same-login/shared mesh/current radio MAC prerequisites are verified):

```sh
"$IDF_PYTHON_ENV_PATH/bin/python" \
  experiments/p4_mesh/test_mesh.py \
  --credentials experiments/board_qualification/private/mesh-credentials.json \
  --run-root experiments/board_qualification/private/mesh-runs \
  --p4-port /dev/cu.usbmodem2201 --s3-port /dev/cu.usbmodem1101 \
  --p4-mac <radio-MAC-from-P4-status> --s3-mac <radio-MAC-from-S3-status> \
  --pair preserve
```

This command deliberately omits `--configure`, `--pair secure`, `--rekey` and `--reopen`. It still sends benign test messages, creates uniquely named file fixtures, and may initiate an ephemeral session; it is not read-only. Do not run it until prerequisites match. The same-login limitation means the current pair may need a small private coordinator using per-board login and the existing MeshRunner assertion methods.

If the new P4 is still mesh-unconfigured, there is no honest existing-session baseline. Record that condition first; create a temporary test mesh under the coordinator’s preserved settings plan rather than overwrite unrelated mesh slots, identities or accounts. Fresh pairing cannot be exercised against a peer that is already paired without deliberately changing state. A new unpaired P4 and this S3 may allow a fresh test while retaining the S3’s previous P4 peer; verify absence of the NEW reciprocal peer first.

After inventory, perform existing-session text/file checks before repairs, save sender result AND receiver evidence, and only then compare changed code. Preserve failed runs. Final verification must also show existing unrelated peers/accounts and initial settings were retained or restored. Root alone executes all hardware steps.
