# ESP-NOW Mesh System: Architecture, Mesh Usage, and Robustness Roadmap

*HardwareOne firmware — engineering report for the firmware maintainer*

---

## 1. Executive Summary

ESP-NOW in this firmware is a **single-hop star network** with a sophisticated libsodium-based security stack layered on top. Each device holds a long-term Ed25519 identity; pairs of devices establish trust via a passphrase-authenticated KEY_EX handshake, then derive per-direction ChaCha20-Poly1305 AEAD keys through a signed X25519 session handshake. On that confidential channel ride two unrelated things: **encrypted chat delivery** (`espnowsessionsend`, delivery-only, never executed) and **remote command execution** (`espnowremote`, an authenticated request/reply RPC). The command surface is broad — roughly 80 registered commands across status, crypto/session, pairing, mesh/routing, metadata/discovery, remote-exec, files, and telemetry.

### What works well

- **The crypto stack is sound.** Plaintext remote commands are correctly and loudly rejected (`System_ESPNow.cpp:4861`); the bond channel is a deliberately distinct admin/RCE path; forward secrecy and a 64-frame replay window are real.
- **1:1 master→backup failover** genuinely works (promote on master silence, auto-demote on return).
- **The web UI implements the correct async pattern** — baseline-seq snapshot → POST → poll `since=cursor` → reassemble by `reqId`/`piece`/`of` — and is the reference model for any programmatic driver.
- **Multi-mesh slots, per-mesh passphrases, and single-hop topology discovery** are all real and functional.

### The one structural gap

There is **no first-class path for battery/power telemetry to cross the mesh, and no blocking "run a command and get its output" primitive.** Battery is absent from the worker-status struct, the sensor enum, *and* the entire V4 wire opcode list (proven: `System_ESPNow_Wire.h:92-171`, `System_ESPNow_Sensors.h:16-29`, `System_ESPNow.cpp:320-323`). The only way to read a peer's battery is to run the literal `batterystatus` CLI string over the generic remote-command pipeline — which itself returns only `{"ok":true,"reqId":N}` on *wire delivery*, never the command's output. The real result lands asynchronously in a polled per-peer ring, correlatable only by `reqId`. A driver that types a command and reads the immediate return **always** concludes "no output." This is exactly why a simple "get gold's battery over the mesh" task floundered.

### Headline recommendations

| # | Recommendation | Effort / Risk |
|---|---|---|
| 1 | `espnowask` / `espnowremote --wait <ms>` — bounded synchronous remote-exec that returns the reply inline | M / med |
| 2 | Thread the CMD_RESP success byte + a result-class into `espnowmessages` so auth/not-encrypted/cmd-not-found/busy are distinguishable | M / low |
| 3 | Busy-NACK instead of the current silent single-in-flight CMD drop | S / low |
| 4 | Pack battery% into the free HEARTBEAT reserved byte; surface it in `meshstatus`/`devices` | S / low |
| 5 | `espnowbattery <peer\|all>` — cache-first, fan-out fallback | M / med |
| QW | `help <command>` resolution, slot-naming arg errors, self-documenting `sessionsend`, `espnowdeviceinfo` arg fix, reqId echoed in human replies | all S / low |

---

## 2. Architecture

### 2.1 Transport & framing

Every V4 frame carries a 32-byte `EspNowV4Header` (`System_ESPNow_Wire.h:200`): `magic 0x3148`, `ver=4`, `type` (opcode), 16-bit flags, **`msgId` at bytes 8–11 — the correlation id echoed in every reply**, origin MAC, `ttl`, fragmentation fields, and a `crc16`. Correlation across the entire request/reply/stream protocol is *entirely* via `header.msgId`.

Opcodes are enumerated in `EspNowV4Type` (`System_ESPNow_Wire.h:92`): `CMD=50`, `CMD_RESP=51`, `STREAM=90`, `HEARTBEAT=30`, `SENSOR_BROADCAST=150`, `BOND_SENSOR_DATA=179`, `SENSOR_STATUS=151`, `BOND_HEARTBEAT=170`, plus metadata, file, remote-FS, and KEY_EX/SESSION opcodes. **There is no battery/power/voltage/charge/fuel opcode anywhere** in the enum.

"Mesh" sending is `v4_broadcast` (`System_ESPNow.cpp:1765/1796`): a loop over active `gMeshPeers` slots calling `v4_send_frame` to each directly. It is a **star fan-out, not a relay**. The `ttl` byte is *written* on TX (`:1604`, `:1985`) but **never read on RX** — a grep for `->ttl` returns only those two writes. No node ever forwards a frame.

### 2.2 Crypto / identity / sessions

A layered libsodium stack (`System_ESPNow_Crypto.h:28` wraps sodium/mbedtls: Ed25519, X25519, ChaCha20-Poly1305 IETF AEAD, PBKDF2, HMAC, Blake2b KDF).

| Layer | Component | File:line | Role |
|---|---|---|---|
| Long-term identity | `EspNowIdentity` | `System_ESPNow_Identity.h:33` | One Ed25519 keypair/device, AES-encrypted at rest in `/system/espnow/identity.json` |
| Per-peer trust | `PeerIdentity` | `System_ESPNow_Identity.h:95` | Peer's Ed25519 pubkey + meshId, persisted plaintext per MAC; written by KEY_EX |
| Mesh keys | `MeshDerivedKeys` | `System_ESPNow_MeshKeys.h:40` | PBKDF2-stretched passphrase → `bootstrapKey` (KEY_EX HMAC) + `groupKey` (BROADCAST_AUTH) |
| Session state | `SessionState` | `System_ESPNow_Sessions.h:57` | RAM-only per-(peer,mesh): sessionId, dir A/B, TX/RX AEAD keys, 64-frame replay window, 16-byte `bondToken` |

**KEY_EX** (`System_ESPNow_Handlers_Crypto.cpp:209`): HELLO/REPLY carry pubkey + HMAC over `(fingerprint‖mac‖pub)` keyed by the mesh `bootstrapKey`; on HMAC pass the peer pubkey is persisted. **SESSION** (`Handlers_Crypto.cpp:672`): X25519 ECDH signed by the long-term Ed25519 keys; `sessionDeriveAeadKeys` (`Sessions.cpp:108`) yields `keyAtoB`/`keyBtoA` + a `bondToken` for every session. **AEAD** (`sessionWrapFrame`/`sessionUnwrapFrame`, `Sessions.cpp:329`): 12-byte nonce `(sessionId‖dir‖frameSeq)`, AAD = first 30 header bytes; unwrap verifies tag then runs the replay-window check. **REKEY** by frame-count (10000) or age (1h), old RX key retained 5s.

Two auth backends, deliberately distinct:
- **`isValidUser(user, pass)`** (`System_ESPNow.cpp:4944` → `System_User.cpp:975`): re-validated against local `users.json` on **every** CMD. There is **no session-scoped auth** for this path.
- **`validateBondSessionToken`** (`System_ESPNow.cpp:766`): compares `@BOND:<token>` against the live bond peer's derived `bondToken`; runs as the reserved `kBondAdminUser` (`bond-admin`, admin-gated) for the life of the bond. This is a different trust model from the user store and grants admin — keep it out of default-encrypt rollouts.

All sessions are **RAM-only and vanish on reboot**; trust (peer pubkeys on disk) survives, but the AEAD channel must be fully re-handshaked. The heartbeat pre-warm (`:2780`) re-establishes it automatically for known peers, but there is a window where encrypted sends fail/queue.

### 2.3 Mesh roles, routing, TTL, topology, heartbeats, failover, multi-mesh

`espnowmode mesh` flips one bit (`gSettings.espnowmesh`). Routing is always fan-out-to-direct-peers. Reaching a named peer like `gold` works **only** if `gold` is in this node's local paired table — `resolveDeviceNameOrMac` (`System_ESPNow.cpp:6813`) scans `gEspNow->devices[]` only. There is no next-hop table, no forwarding.

**Roles** `{WORKER=0, MASTER=1, BACKUP_MASTER=2}` (`System_ESPNow.h:9`) are broadcast in the heartbeat but gate only three behaviors:
1. Master unicasts an HB to the backup MAC (`:7628`).
2. Backup self-promotes on master silence (`:7653`).
3. Non-masters stream sensors to remote; masters aggregate (`shouldStreamSensorToRemote`).

No command routing consults role for gateway selection — **a node self-reporting "worker" can still aggregate, time-sync, and gateway.** Role is advisory; infer the real gateway from behavior, not the broadcast field.

**Failover** is genuine but strictly 1 master ↔ 1 backup, runtime-only (`setMeshRole` does not persist, so reboot restores configured role). Promotion needs at least one prior authenticated master HB (`gLastMasterHeartbeat>0`, `:7655`) — a cold-start dead master never triggers failover. Master-liveness counts only frames where `ctx.isAuthenticated && src==meshMasterMAC` (`:2799-2807`), an anti-spoof fix. Two backups would both promote (split brain).

**Topology** (`:7361`) is single-hop: each node reports its DIRECT peers only; chain inference is printed text for a human (`:10910`). It is observability, not a routable graph.

**Multi-mesh** (`:11226`): up to 4 independent meshes (`Settings::MeshIdentity meshes[4]`, `System_Settings.h:690`), per-mesh passphrase + 16-bit fingerprint scoping the group key and topo filtering.

**TTL theater:** `gSettings.meshTTL` and `meshAdaptiveTTL` appear only in display/set code; every send passes a hardcoded literal. `espnowmeshttl 5` changes a displayed number and nothing else. `meshmetrics` self-documents this (`:9947`): the forward/path/drop counters "had zero increment sites."

**Health tables:** `MeshPeerHealth` (`System_ESPNow.h:187`) holds per-MAC RX telemetry (lastHeartbeat, rssi, counts) and drives `isMeshPeerAlive` (`MESH_PEER_TIMEOUT_MS=30000`). Written **lockless from the RX task** (`System_MeshPeers.h:30`) — treat single reads as eventually-consistent.

### 2.4 The remote-command execution pipeline

`espnowremote <target> <user> <pass> <cmd>` is a **fully asynchronous request/reply RPC.** The flow:

```
SENDER (Device A)                          RECEIVER (Device B)
cmd_espnow_remote :12699
  build "user:pass:cmd"  :12743
  msgId = generateMessageId() :12747
  sendAeadSync(CMD=50, msgId) :12754  ───► v4h_cmd :2722  (snapshot to deferredCmd*)
  return {"ok":true,"reqId":msgId} :12767     v4_handle_cmd :4835  (espnow_task)
  (NOT the command output)                      reject if !sessionEncrypted :4861
                                                 isValidUser :4944  OR @BOND :4877
                                                 createStreamSession(msgId) :4963
                                                 STREAM_BEGIN :5000
                                                 submitCommandAsync(...) :5030 → cmd_exec
                                  ◄─── STREAM frames (op 90, header.msgId) during exec
                                  ◄─── v4CmdResultCallback :4770
                                         STREAM_END :4782
                                         CMD_RESP (op 51) {success, result} :4809
v4h_cmd_resp :2741
  deferredCmdRespReqId = header.msgId :2753
espnow_task drain :7794
  storeReceivedMessageChunked(..., MSG_CMD_RESULT, reqId) :7802
       ↓
  PeerMessageHistory.messages[] ring  (reqId, piece, of, seqNum)
CALLER POLLS: espnowmessages json [sinceSeq] :14025
  correlate reqId == msgId from :12767
```

The synchronous return (`sendAeadSync`, `System_ESPNow_Tx.cpp:275`) reports **wire delivery only** — not auth, not execution. The reply (`V4PayloadCmdResp { uint8_t success; char result[201] }`, `System_ESPNow_Wire.h:353`) lands in the per-peer ring via `storeReceivedMessageChunked` / `espnowStoreInRing` (`:14600/14541/14503`), split into ≤200B pieces sharing `reqId`. Retrieval is by polling `espnowmessages json` (`:14025`), which emits `{seq,reqId,piece,of,mac,name,msg,enc,ts,type,sent,sendState}` and correlating on `reqId`.

**Fan-out** (`espnowroomcmd`/`espnowtagcmd`, `:10398/10448`) sends the same CMD frame to every matching peer but mints a **separate msgId per device** (`:10428/10489`) that is **never surfaced** — the caller gets only a dispatch summary.

### 2.5 Device metadata & discovery

Per-device model: name, friendlyName, room, zone, tags, stationary, derived meshRole, STA MAC. Local metadata lives in `gSettings` (NVS); the mesh-wide directory is `gMeshPeerMeta[]` (`MeshPeerMeta`, `System_ESPNow.h:213`), populated only when a METADATA_RESP/PUSH arrives. The wire struct `V4PayloadMetadata` (`System_ESPNow_Wire.h:331`) carries deviceName/friendlyName/room/zone/tags/stationary — **no meshRole, no MAC, no capability/sensor masks.**

Discovery is **request-then-poll**: `espnowrequestmeta <peer>` (`:12503`) fires a METADATA_REQ and returns "request sent"; the reply lands later via `v4h_metadata_resp_push` (`:3667`) → `processMetadata` (`:6498`) into `gMeshPeerMeta`. Then `espnowdevices`/`espnowrooms`/`espnowfind` (`:10204/10274/10356`) read that cache, joined with `MeshPeerHealth` for liveness. **Capabilities** (sensorMask, featureMask, chipModel) live *only* in the bond-channel `CapabilitySummary` (`System_ESPNow.h:326`), never in the metadata path — discovery tells you *what a peer is*, never *what it can do*.

### 2.6 Worker / sensor telemetry

Two narrow channels, **neither carrying battery:**

- **Worker status** (`espnowworker`, `WorkerStatusConfig` at `:317`) has four toggles — heap, rssi, thermal, imu — but is a **dead shell**: its only consumer is its own show command. The WORKER_STATUS opcode-83 transmitter was deleted 2026-05-21 (`:2364`). Real periodic worker telemetry is the fixed V4 HEARTBEAT (`role, peerCount, rssi, uptimeSec, freeHeap, deviceName`, `Wire.h:224`).
- **Sensor streaming** (`espnowsensorstream`, `RemoteSensorType` at `System_ESPNow_Sensors.h:16`) streams opt-in JSON for 11 sensor types (thermal, tof, imu, gps, input, fmradio, camera, microphone, rtc, presence, apds). `camera`/`microphone`/`apds` have **null builders** (`Sensors.cpp:139-151`) — their status flips but no data ever lands (silent no-op). Workers push; the master caches into `gRemoteSensorCache` (read by `GET /api/sensors/remote`, `WebPage_Sensors.cpp:577`). In plain mesh a master cannot *pull* — only bond mode has `BOND_STREAM_CTRL` to request streaming.

**Battery** is in none of these. The local source is rich (`BatteryState`: voltage, percentage, status, isCharging, usbPresent, cratePctPerHr — ADC or MAX17048 backend, `System_Battery.cpp:481`), reachable across the mesh **only** by running `batterystatus` as a remote CLI command.

### 2.7 Remote files (browse / fetch / sendfile)

`espnowbrowse`/`espnowfetch` take `<target> <user> <pass> <path>` and ride the same async CMD pipeline; results land in `espnowmessages json` keyed by `reqId` (fetch also writes the file locally). `espnowsendfile` is a synchronous local transmit. Big-file streaming has **no resume**: a single `STREAM_APPEND_FAIL` in `v4h_file_data` (`:3759/3781`) aborts the whole transfer with `FILE_CANCEL_INCOMPLETE` — one sustained loss kills a 343 KB/1717-chunk transfer (HW-validated note). FRAG_REQ/REPLY opcodes (2–4) are reserved but unimplemented.

---

## 3. How to Use It in a Mesh (Today)

### 3.1 The correct patterns

The remote-exec reply is **never** in the synchronous return. The canonical loop (cleanly shown in the web UI's `browseRemoteFiles`, `WebPage_ESPNow.h:1483`) is:

1. **Baseline** — snapshot the current max seq *before* sending (`GET /api/espnow/messages?since=0`, take `max(seq)`). A per-operation local cursor avoids racing concurrent ops.
2. **Send** — issue the command; the POST resolves with `"Remote command sent..."` (the ack, *not* the result).
3. **Poll** — `since=cursor`, advancing the cursor each tick, filtering by target MAC.
4. **Match / reassemble** — collect all records sharing `reqId`, stitch by `piece`/`of`; stop on completion or timeout.

The OLED/G2 frontends do **not** do this — they fire synchronously and let a 1 s timer rebuild the view from `espnowGetConversation`. That model has no completion or timeout signal and must **not** be used as the reference for a programmatic driver.

### 3.2 Worked example — gold's battery from red

**Preconditions first** (the part the floundering task never satisfied):

```text
espnowsetpassphrase primary <MESH_PASSPHRASE>   # both devices share it → encryptionEnabled (sender gate :12707)
espnowpairsecure 44:1B:F6:DD:67:48 gold         # secure pair → resolveDeviceNameOrMac('gold') works + session carries CMD
espnowprobe gold 3000                           # (optional) confirm alive + session ready
```

**The correct command sequence:**

```text
espnowremote gold <ADMIN_USER> <ADMIN_PASS> batterystatus
  → returns {"schema":1,"ok":true,"reqId":<N>}        # wire-send only; capture N

espnowmessages json <sinceSeq> 44:1B:F6:DD:67:48
  → poll until a row with reqId==<N> and type==MSG_CMD_RESULT appears
  → its msg field is gold's battery output (e.g. "82%, 4.14V, charging")
```

The CMD_RESP arrives asynchronously (`:4809` → stored at `:7802` as `MSG_CMD_RESULT`). If `of>1`, concatenate pieces in `piece` order across records sharing `reqId`.

**Contrast — the canonical web-UI request→poll loop** does the same thing structurally: baseline `?since=0` → `POST /api/cli` with the `espnowremote ...` string → `setInterval` poll `/api/espnow/messages?since=cursor&mac=<MAC>` → reassemble by `reqId`/`piece`/`of` (the always-on `pollEspNowMessages` at `WebPage_ESPNow.h:3607` is the reference) → render and `clearInterval` on completion or after `maxPolls`.

### 3.3 Command surface reference

`{sync/async, needs-auth, reply-location}` for the operationally important commands. "needs-auth" here means the `<user>/<pass>` payload credential, distinct from the table's `requiresAdmin` flag.

| Command | Sync / Async | Needs `<user> <pass>` | Reply location |
|---|---|---|---|
| **Remote exec** | | | |
| `espnowremote <target> <u> <p> <cmd>` | async | yes | `espnowmessages json` (by reqId) |
| `espnowroomcmd <room> <u> <p> <cmd>` | async fan-out | yes | `espnowmessages json` (per-peer reqId, **not surfaced**) |
| `espnowtagcmd <tag> <u> <p> <cmd>` | async fan-out | yes | `espnowmessages json` (per-peer reqId, **not surfaced**) |
| **Files** | | | |
| `espnowbrowse <target> <u> <p> [path]` | async | yes | `espnowmessages json` (by reqId) |
| `espnowfetch <target> <u> <p> <path>` | async | yes | `espnowmessages json` + local file |
| `espnowsendfile` | sync (local tx) | no | none |
| **Messaging (no exec)** | | | |
| `espnowsessionsend <peer> <msg>` | async send | no | **NONE — chat only, never executed** |
| `espnowsend <peer> <msg>` | async send | no | none |
| `espnowbroadcast <msg>` | async send | no | none (returns count) |
| **Discovery / metadata** | | | |
| `espnowrequestmeta <peer>` | async | no | `espnowdevices` / `espnowdeviceinfo` cache |
| `espnowdevices` / `espnowrooms` / `espnowfind` | sync | no | inline |
| `espnowdeviceinfo [json]` | sync | no | inline (**self only — ignores peer arg**) |
| **Crypto / session** | | | |
| `espnowprobe <peer> [ms]` | sync (bounded) | no | inline (alive + mesh + fw) |
| `espnowkeyex` / `espnowsessionopen` / `espnowrekey` | async | no | `espnowsessions` |
| `espnowsessions` | sync | no | inline |
| **Mesh / health** | | | |
| `espnowmeshstatus` / `espnowmeshmetrics` | sync | no | inline |
| `espnowmeshtopo` | async | no | **`espnowtoporesults`** (not `espnowmessages`) |
| `espnowmessages [json] [sinceSeq] [mac]` | sync | no | **inline — IS the reply sink** |
| **Telemetry** | | | |
| `espnowsensorstream <sensor> <on\|off>` | sync toggle | no | master `espnowsensorstatus` |
| `espnowworker` | sync | no | inline (**dead config — no transmitter**) |

**Async reply sinks are not uniform.** remote/browse/fetch/roomcmd/tagcmd → `espnowmessages json`; `meshtopo` → `espnowtoporesults`; `requestmeta` → the devices cache. Polling the wrong sink yields nothing forever.

---

## 4. Why a Simple Task Floundered (Diagnosis)

### 4.1 Root causes

1. **Missing `<password>` silently swallowed the command.** `espnowremote` requires `hasMinArgs(4)`; it parses `user=arg(1)`, `pass=arg(2)`, `command=remaining(2)`. So `espnowremote <peer> root batterystatus` becomes `user=root, pass=batterystatus, command=""` → 3 tokens → generic `Usage:` and **nothing sent** (`System_ESPNow.cpp:12713-12718`). Forgetting the password is indistinguishable from wrong order.

2. **`espnowsessionsend` is delivery-only.** It ships `ESPNOW_V4_TYPE_TEXT` (`:9648`), which `v4h_text` enqueues as an `MSG_TEXT` chat bubble (`:2667`) — **never** the executor. Sending `batterystatus` made gold store the literal string and reply nothing. This was run ~10 times expecting execution.

3. **`espnowdeviceinfo` ignores its argument.** It reads only `gSettings.*` + self MAC (`:10128-10174`); `espnowdeviceinfo gold` returned **red's** identity (name=red, MAC ...40), misread as facts about gold.

4. **`espnowmessages json` was never polled after a *correct* remote command** — because none was ever issued. All async results surface only there, keyed by `reqId`; the one successful poll followed `requestmeta` (metadata, no battery field), not a real exec.

5. **`help espnowremote` failed.** Help is module-scoped (`System_CLI.cpp:552-562`); a command name falls through to "Unknown help topic. Available modules:". The 4-arg signature lives only in the table usage string, shown on a failed invocation, not discoverable up front.

6. **Encryption + credentials were never satisfied.** The sender requires `encryptionEnabled` (`:12707`); the receiver rejects non-session-encrypted CMDs (`:4861`) then runs `isValidUser` (`:4944`). With no passphrase and no creds, even a 4-token attempt would have failed receiver auth — and the visible arg-count error masked these deeper blockers.

### 4.2 Command-by-command: expected vs actual

| Command issued | Expected | Actual |
|---|---|---|
| `espnowprobe gold 3000` | Confirm reachability | **Correct** — synchronous KEY_EX probe reported alive+mesh+fw. Proves reachability, establishes no command channel. |
| `espnowremote batterystatus gold` | Run batterystatus on gold | Wrong order + too few args. Parsed `target=batterystatus, user=gold`, 2 tokens → `Usage:`, nothing sent. |
| `help espnowremote` | Show the signature | "Unknown help topic. Available modules:" — help is module-scoped. |
| `espnowmessages [json]` | See gold's battery reply | Returned only stored chat text; no CMD_RESP ever existed to match. |
| `espnowsessions` | Confirm a command channel | Correctly showed one ACTIVE session — but a transport session is **not** remote-exec authorization. |
| `espnowsessionsend <MAC> batterystatus` (×~10) | Execute and get output | Sent `ESPNOW_V4_TYPE_TEXT`, stored on gold as a chat bubble. No executor, no reply, ever. |
| `espnowrequestmeta gold` | Pull gold's metadata | **Correct** async pull — name/room/tags arrived. But metadata carries no battery. |
| `espnowdeviceinfo gold` | Show gold's metadata | Returned **red's own** metadata; arg ignored. Misread as evidence about gold. |
| `espnowremote <MAC> root batterystatus` | Run as user root | Still 3 tokens: `pass=batterystatus, command=""` → `Usage:` again. Password slot kept eating the command. |

### 4.3 Systemic lessons

- **Two adjacently-named verbs are semantically opposite and the naming hides it:** `sessionSEND` (delivery-only chat) vs `REMOTE` (auth'd exec). Both "succeed"; only one executes.
- **A command that accepts a target-looking token but answers about self** (`deviceinfo`) is a silent wrong-answer an automated driver cannot detect.
- **Help is module-scoped**, so the natural `help <command>` returns an error — the place a driver looks first.
- **The result path is async and out-of-band** with no completion event; nothing reminds the driver to capture `reqId` and poll.
- **Hard preconditions (encryption + creds) surface only as a generic arg-count error**, masking the deeper blockers.
- **Positional middle-of-line credentials** make "forgot the password" indistinguishable from "wrong order."

---

## 5. Robustness & Capability Roadmap

### 5.1 Quick Wins (directly fix the battery case)

These are S/low, ship-first changes. Each prevents a specific step of the floundered task.

| Quick win | Seam | What it fixes |
|---|---|---|
| **`help <command>` resolves via `findCommand()`** — insert a lookup before the "Unknown help topic" fallback | `System_CLI.cpp:559` (helper at `System_Command.cpp:86`) | Shows the `<target> <user> <pass> <cmd>` signature on the *first* natural move |
| **Graduated, slot-naming arg-count errors** — echo what was parsed (`arg 3 "batterystatus" was read as the PASSWORD`) + a runnable example with the resolved peer MAC | `System_ESPNow.cpp:12713 / 10401 / 10451` | Kills the password-swallowing trap that blocked every attempt |
| **Self-documenting `sessionsend` success string** — lead with "Delivered as CHAT TEXT (NOT executed). To run a command: espnowremote ..." | `System_ESPNow.cpp:9661` | Ends the ~10 wasted sessionsend executions |
| **`espnowdeviceinfo` honors or rejects its peer arg** — minimum: reject non-json positional with "shows THIS device only; for a peer use espnowdevices / espnowrequestmeta" | `System_ESPNow.cpp:10130` | Removes the silent wrong-answer (red's identity read as gold's) |
| **Echo `reqId` on the human (non-JSON) reply** for remote/browse/fetch — append "(reqId N — poll: espnowmessages json 0 \<mac\>)" | `System_ESPNow.cpp:12770 / 12618 / 12624` | Gives serial/OLED operators the correlation key + the exact next step |

### 5.2 Ranked roadmap

Effort: S/M/L. Risk: low/med. Each hooks into a verified seam.

| Rank | Item | Effort / Risk | Seam | Depends on |
|---|---|---|---|---|
| **1** | **`espnowask` / `espnowremote --wait <ms>`** — bounded synchronous remote-exec returning the reply inline. Reuses the proven probe poll-loop (`vTaskDelay(20)` tick, bounded cap) so it never starves tasks. `--wait`+`--json` returns `{reqId, peer, name, ok=cmdSuccess, complete, output}`; no-wait renames `ok`→`sent` to kill the wire-vs-command ambiguity. | M / med | `:12754` (send) + `:9414` (probe loop) + `:14725` (`getPeerMessages`) | — |
| **2** | **Thread the CMD_RESP success byte + result-class into `espnowmessages`** so auth / not-encrypted / cmd-not-found / busy are distinguishable. The byte is already captured at `:7809` (`deferredCmdRespSuccess`) but **dropped** before `storeReceivedMessageChunked`. Add `success` + `resultClass` to `ReceivedTextMessage`, thread through, emit `"ok"`/`"class"` in the JSON. | M / low | `:7802` (drop site) + `:14070` (JSON) + `:2752` (capture) + `Wire.h:354` | rank 1 (shared path) |
| **3** | **Busy-NACK** — `v4h_cmd` at `:2723` has a no-else guard: a second concurrent CMD to one hop is silently dropped, its reqId never resolves. Set a deferred `cmdBusyRejectMsgId` and emit a `success=0` "receiver busy, retry" CMD_RESP from the espnow_task drain (NOT inline — RX must not block), reusing the error shape at `:4966`. | S / low | `:2723` | rank 2 (carry `busy` class) |
| **4** | **Battery% into the HEARTBEAT reserved byte** — exactly one unused `reserved` byte at `Wire.h:228` (32-byte `static_assert` stays intact). TX sets `hb.reserved = roundf(getBatteryPercentage())` (0xFF=unknown); RX stashes into a new `MeshPeerHealth.batteryPct` (lockless single-byte write is atomic). Surface `batteryPct`/`batteryAgeSec` in `meshstatus` + `devices` JSON/text. Zero new frames, zero creds — the whole mesh's battery in one call. | S / low | `Wire.h:228` + `:7609` (TX) + `:2762` (RX) + `:7228` / `:10199` (JSON) | — |
| **5** | **`espnowbattery <peer\|all> <user> <pass>`** — single peer = sugar over rank-1 bounded-wait with `batterystatus json`. `all` answers first from the rank-4 cache (no airtime), fans CMDs only to stale peers. Forces the fan-out fix: hoist the inline `generateMessageId()` at `:10428/10489` into a named local and return a `{mac,name,reqId,queued}` manifest so roomcmd/tagcmd replies finally correlate with a known expected count. | M / med | `:10398-10445` + table `:14127` | ranks 1, 4 |
| **6** | **Sender-side pending-reqId table + timeout reaping** — closes the third legibility leg: an unreachable/dropped peer produces **no row at all**. A small `PendingRemoteCmd[]` populated at `:12747`, reaped in the espnow_task drain (beside `:7795`), emits a synthetic `MSG_CMD_RESULT` "timed out waiting for reqId R from peer P" into the same ring, same reqId. Generalizes rank-1's timeout to fan-out and fire-and-forget. | M / med | `:7795` (reap) + `:12747` (populate) + `:2753` (clear) | rank 2 (pending/unreachable class) |
| **7** | **Pre-dispatch reachability gate** — extract the probe body into `espnowReachabilityCheck()`, opt-in via `espnowremote --probe`, returns `{ok:false,error:unreachable,reason:...}` instead of a phantom reqId. Largely subsumed by ranks 1 & 6; the reusable helper also pays off for `espnowbattery all`. | M / low | `:12747` + probe loop `:9405-9464` | rank 1 |
| **8** | **Dedicated POWER beacon opcode** — now reserved as `POWER_STATUS=152` in the sensors range (150–169) since the 2026-07 opcode renumber (the old "83 is poisoned" concern is obsolete; the renumber eliminated that hole). Carries voltage/charging/USB/heap, broadcast on the worker-status interval, **reviving the dead `espnowworker` config** as its gate. Richer-but-heavier follow-on to rank 4; adds a frame to the air. | M / med | `Wire.h:156` + dispatch `:4127` + dead config `:317 / 10588` | rank 4 |
| **9** | **Saturation/backpressure as JSON + truncated-reply tagging** — `espnowSaturationReport` already computes fps/queue-depth/drops/ACK-RTT but `cmd_espnow_saturation` only prints and returns "OK". Add a `json` mode and tag a reqId's stored result `truncated` when `streamDroppedCount` rose during its lifetime. Lets a high-fan-out driver throttle before saturating. | M / low | `:9135` + drop counter `:3717` / drain `:7768` | rank 2 |
| **10** | **Align `espnowfetch` / `espnowbrowse` path-quoting** — fetch always requires a quoted path (`:12643`); browse accepts unquoted (`:12567`). Cheapest safe fix is doc-only (make fetch's error say "path MUST be quoted, e.g. ..."). | S / low | `:12643` vs `:12567` | — |
| **11** | **Session-scoped / `espnowauth` credential-free remote-exec** — authenticate once per peer (a `@SESSION:` sentinel mapped to a per-peer `espnowAuthUser` on `PeerIdentity`, or a per-session `authValid` flag) so a driver stops resending `user:pass` on every call. Gated behind `wasSessionEncrypted` (plaintext rejection at `:4861` untouched), maps to a **non-admin** identity, deliberately OFF the bond admin channel. Real ergonomics win but L/med, touches identity+session+usersync, and **not needed for the battery outcome**. | L / med | `Sessions.h:93` + `:4944` + `:766` (template, not reused) | rank 1 |
| **12** | **End-to-end file-transfer resume via FILE_NACK** — today one sustained loss aborts a 1717-chunk transfer. A receiver→sender FILE_NACK with the missing-chunk bitmap, gated to the buffered (non-streaming) path first, converts "1 loss = whole transfer lost" into selective retransmit. Orthogonal to the battery/health outcome; separate workstream. | L / med | `:3776-3784` + `:3924` + new opcode (`FILE_NACK` reserved at 116, `Wire.h:146`) | — |

### 5.3 Explicitly rejected ideas

| Rejected | Reason |
|---|---|
| **Accepting plaintext remote commands** (any shortcut around the `:4861` gate) | The plaintext rejection is correct security: a plaintext frame exposes the password/token on-air and is forgeable/replayable (token-replay RCE). No proposal touches this. |
| **Folding ordinary `user:pass` auth into the bond admin channel** | Bond is a distinct auth/RCE path that runs as `kBondAdminUser` (admin). Widening it to every mesh peer would expand the admin blast radius. The session-scoped-auth proposal (rank 11) deliberately maps to a non-admin identity and stays off bond. |
| **Shipping `#1` and `#2` as separate features** | `#2` is the JSON return-shape of the same bounded-wait wrapper; separately they'd be two passes over the same poll-loop and reassembly code. Merged into rank 1. |
| **Three separate `espnowdeviceinfo` fixes** (`#6/#13/#25`) | Same bug (ignores arg, returns self). Merged into one quick win; reject-arg is minimal, honor-via-cache is the follow-up. |
| **Trusting the broadcast mesh role to identify the gateway** | Role is cosmetic — a "worker" can aggregate, time-sync, and gateway. Any driver must infer the gateway from behavior, not the role field. |
| **Tuning TTL to extend reach** | `meshTTL`/`adaptiveTTL` are inert config theater — never fed to any send. Multi-hop forwarding does not exist; robustness of multi-hop today is *zero* (absent, not fragile). |

### 5.4 Themes

1. **Wire-delivery is mistaken for command success at every layer** — the deepest systemic flaw. `ok:true` means delivery-to-wire; the real success byte is captured at `:7809` then dropped. Fixing this one semantic (ranks 1+2) unblocks the battery outcome.
2. **The result path is async, out-of-band, and self-correcting nowhere** — a driver who reads the immediate return always concludes "no output." A bounded-wait wrapper is the single highest-leverage change.
3. **Silent wrong-answers and silent drops are the recurring failure mode** — `deviceinfo` answers self, `sessionsend` stores commands as chat, a second CMD vanishes with no NACK, an unreachable peer produces no row. Each must fail loudly and legibly.
4. **Discoverability is broken at the entry point** — `help <command>` doesn't resolve, the 4-arg signature is reachable only by triggering an error, and arg-count errors hide which slot was swallowed. Cheap S/low fixes here would have prevented the entire failure before any wire traffic.
5. **Push-first telemetry beats creds-gated pull for ambient health** — battery/voltage/heap are the most-asked fields, the HEARTBEAT already broadcasts a free byte, and `MeshPeerHealth` is the natural sink. Ranks 4/8 remove the remote-CLI escape hatch for the common case.
6. **Fan-out is fundamentally un-correlatable today** — per-peer reqIds are minted inline and thrown away. A `{mac,name,reqId,queued}` manifest is the precondition for any mesh-wide roll-up.
7. **The security posture is sound and must stay sound** — plaintext rejection at `:4861` and bond's distinct admin channel are correct. Every ergonomics proposal respects this.

---

## 6. Appendix — Full Per-Slice Gaps & Traps

| Slice | Issue | Evidence (file:line) | Impact |
|---|---|---|---|
| Remote-exec pipeline | Reply is never returned by `espnowremote`; lands async in a polled ring keyed only by `reqId` | `:12767` (return) → `:7802` (store) → `:14060` (emit) | Driver must capture reqId, poll, collect by reqId, reassemble by piece/of; no blocking primitive, no completion event |
| Remote-exec pipeline | Receiver CMD handling is single-in-flight; second CMD silently dropped; only 4 StreamSession slots | `:2723` (no else) ; `:989` ; `:4968` | Two commands to one node in quick succession → second lost → caller's poll never finds that reqId |
| Remote-exec pipeline | `roomcmd`/`tagcmd` give no correlation: per-peer msgId never surfaced, only a summary returned | `:10428` / `:10489` ; `:10442` / `:10503` | Fan-out replies attributable only best-effort by MAC; no count/timeout contract |
| Remote-exec pipeline | `sendAeadSync` success = wire-delivery (or queue-acceptance), NOT command success | `System_ESPNow_Tx.cpp:299` ; `:12758/12767` | `ok:true` says nothing about auth/exec/receipt; the real signal is in `espnowmessages` |
| Remote-exec pipeline | Tiny per-peer rings (overwrite oldest), 8-record page cap; large results can evict their own pieces | `:14531-14533` ; `:14043` (MAXM=8) ; `:14546` | Output > ring×200B → permanently un-reassemblable; driver must poll aggressively |
| Crypto/session | `espnowsessionsend` delivers TEXT that is never executed — chat only | `:9648` (TEXT) ; `:2667` (enqueue) ; `:14112` (help caveat) | A command string sent via sessionsend lands as chat, returns no output silently |
| Crypto/session | Remote commands require BOTH credential AND session encryption; plaintext refused before creds parsed | `:4861` ; `:4415` (flag set) | Cannot fire-and-forget at a cold peer; needs KEY_EX + ACTIVE session (or encrypt-or-queue) |
| Crypto/session | No session-scoped auth for user/pass path — re-validated every CMD | `:4944` ; `System_User.cpp:975` | Embed creds in every frame, or use bond's per-session token |
| Crypto/session | Bond auth is a different trust model and grants admin | `:4892-4901` ; `:766` ; `System_User.h:78` | Bond = admin RCE for the one configured peer while session ACTIVE; keep out of default-encrypt rollouts |
| Crypto/session | All sessions RAM-only; vanish on reboot | `System_ESPNow_Sessions.h:11-13` ; `:9671` | After peer reboot, every SESSION_FRAME drops until SESSION_OPEN re-runs (heartbeat pre-warm covers known peers, with a gap) |
| Crypto/session | `espnowsessions` shows dir/state but not bondToken validity / which peer is the bond peer | `Sessions.h:60/92` ; `:9716` | Can confirm ACTIVE session but not whether `@BOND` auth will work |
| Mesh/routing | Multi-hop forwarding does not exist; `ttl` written on TX, never read on RX | `:1604` / `:1985` (only writes) ; `:9947` | Reachability = "is peer in MY devices[]"; an A–B–C line cannot pass A↔C; multi-hop robustness is zero |
| Mesh/routing | Configured + adaptive TTL are inert | `:7621` / `:2222` (literals) ; `gSettings.meshTTL` only in display/set | `espnowmeshttl 5` changes a number and nothing else |
| Mesh/routing | Mesh role is cosmetic — a "worker" can act as gateway | role gates only `:7628` / `:7653` / `shouldStreamSensorToRemote` | Don't trust the broadcast role field; infer the gateway from behavior |
| Mesh/routing | Topology discovery single-hop; chains are a manual human exercise | `:7371` ; `:3024-3050` ; `:10910` | `toporesults` gives star-adjacency lists, not a routable graph |
| Mesh/routing | Failover strictly 1↔1, runtime-only; cold-start dead master never promotes | `:335-337` ; `:7655` ; `:2799-2807` ; `:7288` | No N-way/quorum; two backups → split brain |
| Mesh/routing | Health/meta tables written lockless from RX, read everywhere unsynchronized | `System_MeshPeers.h:30-31` ; `:7140` | Snapshots momentarily inconsistent; treat single reads as eventually-consistent |
| Worker/sensor telemetry | **No first-class battery/power path on the mesh** — absent from worker struct, sensor enum, AND opcode list | `:320-323` ; `Sensors.h:16-29` ; `Wire.h:92-171` ; `Wire.h:224/283` | Battery reachable only via `batterystatus` over remote-CMD; no push, cache, freshness, or `/api/sensors/remote` surface |
| Worker/sensor telemetry | `espnowworker fields` config is a dead shell — toggling changes nothing on the wire | `:10588-10678` (only consumer) ; `:2364` (transmitter deleted) | Misleading surface; only worker telemetry emitted is the fixed HEARTBEAT |
| Worker/sensor telemetry | camera/microphone/apds have null builders — "stream" flips status but emits no data | `Sensors.cpp:139-151` ; `:600` | Silent no-op that looks like a working stream |
| Worker/sensor telemetry | Remote sensor data is request/cache, not live-subscribe; plain mesh can't pull on demand | `Sensors.cpp:674-695` ; TTL 30 s/60 s ; `:2969` (bond-only BOND_STREAM_CTRL) | Master is a passive consumer in mesh mode |
| Metadata/discovery | `espnowdeviceinfo` always shows local metadata, silently ignores its peer arg | `:10128-10174` | `espnowdeviceinfo gold` returns the local node's identity — a silent wrong-answer |
| Metadata/discovery | Remote metadata is request-then-poll with no completion signal | `:12528-12532` ; `:3686-3688` ; `:8178-8202` | No synchronous "describe peer X"; if peer has no session the REQ silently fails |
| Metadata/discovery | Directory exposes what a peer IS, never what it can DO; capabilities not in metadata | `Wire.h:331-339` ; `:6540-6552` ; `:3478` | Capability-based routing needs a separate bond + CapabilitySummary exchange |
| Metadata/discovery | Liveness coarse (online + lastSeenSec); RSSI captured but not surfaced; battery not modeled | `:10192-10201` ; `System_ESPNow.h:196` | No signal-quality/battery in the discovery surface |
| Metadata/discovery | `resolveDeviceNameOrMac` resolves PAIRED devices only — directory entries not addressable | `:6813-6835` | A peer discovered via `espnowdevices` but never paired → "Device not found"; listed ≠ addressable |
| Command surface / UX | Help is module-based; `help <commandname>` fails | `System_CLI.cpp:552-562` ; `System_Utils.cpp:2465` | Natural `help <cmd>` returns "Unknown help topic" + a list of modules |
| Command surface / UX | Per-command Usage emitted only on an error with a magic prefix | `System_Command.cpp:245-247` | Drivers learn syntax by trial-and-error; non-prefixed errors never append usage |
| Command surface / UX | Auth-arg position inconsistent across the remote family; absent on sessionsend/send | `:12715` / `:12563` / `:10403` ; `:9626` | The #1 syntax footgun: target-first vs selector-first vs no-auth, all invisible |
| Command surface / UX | `sessionsend` (chat) easily mistaken for `remote` (exec) — opposite, adjacently named | `:9621` (TEXT) vs `:12699` (CMD) | Driver wanting exec calls sessionsend, gets OK, polls forever |
| Command surface / UX | Async reply sinks not uniform — three retrieval channels, no machine-readable mapping | `:14025` vs `:10866` vs `:14173` | Polling the wrong sink yields nothing; only `espnowmessages` has reqId correlation |
| Command surface / UX | No machine-readable async/sync or reply-location metadata in `CommandEntry` | `:14094` (struct) | `requiresAdmin` is the only structured flag; async-ness is free-text prose |
| Frontends | Async reply is NOT in the POST response — only "Remote command sent..." | `WebPage_ESPNow.h:1426/1520` ; `System_Utils.cpp:4101` ; `:12770` | A driver treating the POST body as output sees success with empty result |
| Frontends | Correct usage needs a baseline-seq snapshot before sending, then poll-since + mac filter | `WebPage_ESPNow.h:1500-1531/1565` | Skipping the baseline surfaces stale replies as the current answer |
| Frontends | No single completion signal — heuristic per-command done-markers; wrong marker → false completion | `WebPage_ESPNow.h:1584-1585` ; `:1720` ; `:3585` | A generic driver can't know when a streamed reply is done without reqId/of framing |
| Frontends | Replies arrive as chunked pieces (reqId/piece/of) needing client reassembly | `WebPage_ESPNow.h:3535-3590` | A naive poller renders fragments and may never recognize completion |
| Frontends | OLED/G2 don't correlate reply to request — fire sync, rebuild from shared store on a 1 s timer | `OLED_ESPNow.cpp:1783/193/1426` ; `G2_Page_ESPNow.cpp:843` | No completion/timeout/result correlation; must NOT be the model for a programmatic driver |
