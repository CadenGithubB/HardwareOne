<!-- Generated audit: espnow/bond CLI help-text vs behavior. 35 findings, code-verified. -->

# ESP-NOW / Bond CLI Help-vs-Behavior Audit

**Summary.** This audit found **35 distinct help/usage-vs-behavior mismatches** across the espnow/bond command surface (after merging near-duplicates). The dominant pattern is **stale/inert references**: a large cluster of settings and commands whose help implies a live feature that no longer exists or was never wired up — worker-status reporting (`espnowworker`, `espnowworkerstatusinterval`), mesh TTL/adaptive-TTL (`espnowmeshttl`, `espnowmeshadaptivettl`), mesh routing metrics (`espnowmeshmetrics`), topology auto-refresh/interval timers, and four buffer/chunk-size setters that no allocator or transmitter reads. A second pattern is **self-vs-peer / arg-signature confusion**: table-row usage strings that document `<mac>` when the handler also accepts a paired device name, a "json" flag and a "json"/role gate omitted from usage, plus a `gamepad` token the parser actually rejects in favor of `input`. A third is **wrong reply-sink / count semantics**: `espnowrequestmeta` names `espnowdeviceinfo` (which only ever shows self), and `espnowbroadcast` renders a single TX success bool as a per-device "count." Finally there are several **role-gating overclaims** ("master only" / "(master)" / "worker only") that the code does not actually enforce or that point the wrong way. Most findings are documentation-only fixes; the inert-config cluster additionally flags real dead-code/feature debt.

---

## High severity

**`espnowmeshmetrics` — advertises forward/path/drop counters that no longer exist**
- Says: "Show mesh routing metrics (forwards, path stats, drops)."
- Actually: The handler prints no forward/path/drop counters — those `RouterMetrics` fields were removed (zero increment sites, multi-hop forwarding was never instrumented). It emits only live mesh config (Mode, Active peers, Current TTL, Adaptive TTL) plus an explicit note that those counters are not instrumented.
- Evidence: `System_ESPNow.cpp:14133` (table entry); handler comment `System_ESPNow.cpp:9949-9953`; runtime note `9966-9968`; body emits config only at `9958-9965`.
- Fix: "Show live mesh routing config (mode, active peers, TTL, adaptive TTL). Per-forward/path/drop counters are not instrumented — use `espnowsaturation` / `espnowstats` for traffic metrics."

**`espnowworker` — configures a worker-status report that is never transmitted**
- Says: "Configure worker status reporting: `espnowworker [show|on|off|interval <ms>|fields <list>]`" — implying `on`/`interval`/`fields` configure a status report sent over the wire.
- Actually: A dead config shell. `gWorkerStatusConfig` is only written and read back for display inside `cmd_espnow_worker`; no transmitter reads it. The sender `v4_send_worker_status` was removed 2026-05-21 and opcode 83 removed 2026-06, so nothing is ever placed on the wire.
- Evidence: removal comment `System_ESPNow.cpp:2364`; `System_ESPNow_Wire.h:123`; `gWorkerStatusConfig` referenced only at `System_ESPNow.cpp:325` (def) and `10604-10667` (all inside the handler).
- Fix: Mark inert in both description and usage, e.g. "Configure worker status reporting (INERT: WORKER_STATUS transmitter removed 2026-05-21; config is stored but never sent — `on`/`interval`/`fields` have no wire effect)." Better: remove the command and its config struct.

---

## Medium severity

**`espnowkeyex` / `espnowsessionopen` / `espnowrekey` / `espnowrequestevents` — usage says `<mac>` but a device name is accepted**
- Says: Table-row usage documents a raw MAC as the only first arg (`espnowkeyex <mac> [<mesh>]`, `espnowsessionopen <mac> [<mesh>]`, `espnowrekey <mac>`, `espnowrequestevents <mac> <bitmask>`). For `espnowkeyex`, the in-handler usage *also* says `<mac>`.
- Actually: All four handlers accept a paired device NAME or a raw MAC via `resolveDeviceNameOrMac()` (case-insensitive name match before `parseMacAddress()`). The latter three already show `<name_or_mac>` in their in-handler usage — only the table row is the stale copy. `espnowkeyex` is the worst case: no corrected surface exists anywhere, so the user never sees the name path.
- Evidence: table rows `System_ESPNow.cpp:14113` / `14115` / `14118` / `14120`; in-handler usages `9288-9289` (stale MAC-only), `9332`, `9477`, `9566` (already `<name_or_mac>`); resolver `6818`; handler guards `9299`, `9336`, `9484`, `9578`; examples use `deviceA` at `9571-9573`.
- Fix: Change each table-row first token from `<mac>` to `<name_or_mac>`. Additionally for `espnowkeyex`, fix its in-handler usage too: `<name_or_mac>  paired device name OR target peer MAC (AA:BB:CC:DD:EE:FF)`.

**`espnowpairsecure` — fallback usage prints a non-existent space-separated command**
- Says: On bad invocation the handler prints "Usage: espnow pairsecure ..." for the user to copy.
- Actually: The registered command is the single token `espnowpairsecure`; copying the space-separated form yields an unknown-command error. The table-row usage at `14201` is correct — only the handler's fallback is wrong.
- Evidence: `System_ESPNow.cpp:12363` (`"Usage: espnow pairsecure <mac_address> <device_name> [mesh]"`) vs registration `14201`.
- Fix: Change the fallback to `"Usage: espnowpairsecure <mac_address> <device_name> [mesh]"` (single token).

**`espnowmeshttl` — documents `adaptive` as an enable, but the handler blind-toggles it**
- Says: Description/usage present `adaptive` as a value you set to enable adaptive mode (`espnowmeshttl [1-10|adaptive]`).
- Actually: The handler treats `adaptive` as a blind TOGGLE of `gSettings.meshAdaptiveTTL`, so the identical command flips on/off on alternating calls. The handler's own error string at `9921` even says "or 'adaptive' to toggle," contradicting the table wording.
- Evidence: toggle `System_ESPNow.cpp:9910-9911`; result text `9913-9914`; internal error `9921`.
- Fix: "`espnowmeshttl [<1..10>|adaptive]` (adaptive TOGGLES adaptive mode on/off; a numeric TTL sets it and disables adaptive)."

**`espnowmeshtopo` — "(master only)" not enforced**
- Says: "Discover mesh topology (master only)."
- Actually: The handler gates only on `meshEnabled()` and a non-zero active-peer count; neither it nor `requestTopologyDiscovery()` checks `MESH_ROLE_MASTER`, so any mesh-enabled worker/backup node can run it and broadcast TOPO_REQ frames. Per the project's own note, mesh role is advisory.
- Evidence: `System_ESPNow.cpp:10776`, `10781-10785`, `10805` (no role check); `requestTopologyDiscovery()` guard `7361-7362`; table row `14143`.
- Fix: Drop "(master only)": "Discover mesh topology (run on the master; role not enforced). (async — read results with `espnowtoporesults`)."

**`espnowtimesync` — "(master only)" not enforced**
- Says: "Broadcast NTP time to mesh (master only)."
- Actually: Gates only on `meshEnabled()` and a valid local epoch (>=100000), then calls `v4_broadcast_time_sync()`, which is an unconditional broadcast. No role check — any mesh-enabled node will broadcast a TIME_SYNC frame.
- Evidence: `System_ESPNow.cpp:10813`, `10817-10820`, `10826` (no role check); `v4_broadcast_time_sync` `2202-2210`; table row `14145`.
- Fix: Drop "(master only)": "Broadcast NTP time to mesh (intended for the master; role not enforced). (async broadcast; delivery only, no reply)."

**`espnowrequestmeta` — names `espnowdeviceinfo` as the reply sink, but that only shows self**
- Says: "(async — updates cache; view with `espnowdevices`/`espnowdeviceinfo`)" and "...update the local cache shown by `espnowdevices` and `espnowdeviceinfo`."
- Actually: The reply is stored by `processMetadata` into the per-peer cache `gMeshPeerMeta`, rendered by `espnowdevices`/`espnowrooms`/`espnowfind`. `cmd_espnow_deviceinfo` reads ONLY this device's own `gSettings.*` and never touches `gMeshPeerMeta`, so a peer's requested metadata will NEVER appear there.
- Evidence: registration `System_ESPNow.cpp:14178`; reply path `6540-6552`; self-only handler `10135-10139` (no `gMeshPeerMeta` read); cache consumers `10219-10221`, `10285-10346`, `10369-10391`.
- Fix: Remove `espnowdeviceinfo` from both strings. Description: "(async — updates cache; view with `espnowdevices`)." Usage: "...update the local peer cache shown by `espnowdevices` / `espnowrooms` / `espnowfind`."

**`espnowsend` — undocumented leading `json` flag and undocumented encryption requirement**
- Says: Table description/usage list exactly two args (`espnowsend <name_or_mac> <message>`), say nothing about encryption ("Send message (auto-routes via mesh if enabled)"), and imply any send works.
- Actually: (1) The handler accepts an optional LEADING `json` flag before the target, shifting the arg base by one; its internal usage documents `[json]` but the table does not. (2) The handler hard-requires system-wide encryption — with it off it never sends and only returns an error; a code comment confirms plaintext send was deliberately removed.
- Evidence: json flag `System_ESPNow.cpp:12896-12900` vs table usage `14165`; encryption gate `12886-12889`; removal comment `12880-12885`.
- Fix: "`espnowsend [json] <name_or_mac> <message>` (a leading `json` flag returns `{schema,ok,msgId}` for delivery-status polling). Requires ESP-NOW encryption enabled (set a mesh passphrase first); plaintext send was removed."

**`espnowbroadcast` — renders a TX success bool as a per-device "count"**
- Says: "Returns the count sent; one-way broadcast, no per-device reply" — implying the number reflects how many devices were reached.
- Actually: There is no per-device count. The transmit returns one bool for the single broadcast frame; the handler maps it to 1-on-success / 0-on-failure, so the result is always "Broadcast sent to 1 device(s)" regardless of peer count (or "0 device(s) (1 failed)" on TX failure).
- Evidence: `System_ESPNow.cpp:11732-11744`; `v4_broadcast_text` returns bool at `2528`.
- Fix: "Returns whether the single broadcast frame was transmitted (one frame to all peers), NOT a per-device delivery count; no per-device reply."

**`espnowsensorstream` — lists camera/microphone/apds as streamable, but they have no data builder**
- Says: Usage lists `camera`, `microphone`, `apds` among streamable sensors with no caveat, and the handler returns "OK: Sensor streaming started" for them.
- Actually: Those three have an unconditionally null data builder (no `#if` guard — null on every board). `startSensorDataStreaming` only rejects `sensorType >= REMOTE_SENSOR_MAX`, so toggling them sets the enabled flag and returns OK, but the broadcaster's `if (!spec.builder) continue;` silently drops every tick — no data streams. (rtc/presence/fmradio/input are `#if`-gated and legitimately board-dependent.)
- Evidence: null builders `System_ESPNow_Sensors.cpp:139/140/151`; broadcaster drop `Sensors.cpp:600`; validation `Sensors.cpp:325`; OK return `Sensors.cpp:921`; usage `System_ESPNow.cpp:14174`.
- Fix: Drop `camera|microphone|apds` from the accepted-sensor list in the usage string, or annotate them "(no data builder — toggle is a no-op)."

**`espnowmeshadaptivettl` — toggle has no wire effect (mesh is single-hop)**
- Says: "Set adaptive TTL: `<0|1>`" — presents adaptive TTL as a functional mesh feature.
- Actually: `gSettings.meshAdaptiveTTL` (and `meshTTL`) are only written and read for status text — never fed into any send. Every frame's `ttl` is a hardcoded per-callsite literal (1/2/3); no TX path reads these settings, and the mesh is single-hop so TTL would be inert anyway.
- Evidence: setter `System_ESPNow.cpp:14278`; status-only reads `9903/9944/9962-9964`; hardcoded TTLs `2177/2526/7331/11799`.
- Fix: "Set adaptive TTL flag: `<0|1>` (stored but currently has no effect — mesh is single-hop and per-frame TTL is a hardcoded literal, not read from this setting)."

**`espnowworkerstatusinterval` — setting has zero consumers and no live reporting loop**
- Says: "Set worker status interval: `<5000-120000 ms>`" — implies it controls how often a worker reports status.
- Actually: `gSettings.meshWorkerStatusInterval` is never read (write-only). There is no live worker-status reporting loop at all — the sender was removed 2026-05-21. The separate `gWorkerStatusConfig.intervalMs` is also only read for display, not by any timer.
- Evidence: setter `System_ESPNow.cpp:14273`; only decl/ctor/SettingEntry exist (`System_Settings.h:671/213`); removal note `System_ESPNow.cpp:2364`; display reads `10605/10618`.
- Fix: Note the field is stored but unused and that worker-status reporting is currently removed (no live sender). If reintroduced, wire one source into the loop and document which is authoritative.

**`espnowtopodiscoveryinterval` — no periodic discovery loop reads it**
- Says: "Set topology discovery interval: `<0-300000 ms>`" — implies periodic automatic discovery on a timer.
- Actually: `gSettings.meshTopoDiscoveryInterval` is never read; discovery is only triggered manually (e.g. `espnowmeshtopo`). The setting does nothing.
- Evidence: setter `System_ESPNow.cpp:14274`; only decl/ctor/SettingEntry (`System_Settings.h:672/214`) — no consumer.
- Fix: Note the field is inert (no auto-discovery timer reads it; discovery is manual), or implement a periodic trigger that consumes it.

**`espnowtopoautorefresh` — no auto-refresh path reads the flag**
- Says: "Set auto refresh topology: `<0|1>`" — implies enabling automatic topology refresh.
- Actually: `gSettings.meshTopoAutoRefresh` is never read by any code path; toggling it has no effect.
- Evidence: setter `System_ESPNow.cpp:14275`; only decl/ctor/SettingEntry (`System_Settings.h:673/215`) — zero readers.
- Fix: Mark inert ("stored but no auto-refresh path reads it") or implement the consumer.

**`espnowtxqueuesize` — no TX queue is sized from it**
- Says: "Set TX queue size: `<1-16>`" — implies it sizes the ESP-NOW TX/retry queue.
- Actually: `gSettings.espnowTxQueueSize` is never read by any consumer; no queue is created/sized from it. Written by this setter and `espnowbuffers`, read only for display. The `espnowbuffers` "takes effect after reinit" note is also false — no reinit reads it.
- Evidence: setter `System_ESPNow.cpp:14298`; display/setter-only reads `13915/13933/13952`; decl `System_Settings.h:736`.
- Fix: Note the field is stored but not applied (no TX queue is sized from it), or wire it into queue creation.

**`espnowrxbuffersize` — no allocation consumes it**
- Says: "Set RX buffer size: `<64-512>`" — implies it sizes the RX/deferred-message buffer.
- Actually: `gSettings.espnowRxBufferSize` is never read; the deferred/RX buffer is not sized from it. Write/display only.
- Evidence: setter `System_ESPNow.cpp:14299`; reads only at `13916/13935/13956`; decl `System_Settings.h:737`.
- Fix: Note the value is stored but not applied, or wire it into the RX buffer allocation.

**`espnowchunksize` — chunk size is hardcoded, not read from the setting**
- Says: "Set chunk size: `<100-212>`" — implies it controls large-message fragmentation chunk size.
- Actually: `gSettings.espnowChunkSize` is never read on the TX path. Chunking uses a hardcoded `v4ChunkSize = ESPNOW_V4_MAX_PLAINTEXT - 2` (200 bytes), ignoring the setting.
- Evidence: setter `System_ESPNow.cpp:14300`; hardcoded `11848` and used at `11876`; display/setter-only reads `13917/13937/13960`.
- Fix: Note the setting is not honored (chunk size fixed at `ESPNOW_V4_MAX_PLAINTEXT-2`), or make the chunker read `gSettings.espnowChunkSize`.

**`espnowfilechunksize` — file chunk size is hardcoded, not read from the setting**
- Says: "Set file chunk size: `<100-216>`" — implies it controls file-transfer chunk size.
- Actually: `gSettings.espnowFileChunkSize` is never read. File transfer uses the same hardcoded `v4ChunkSize` (`file.read(fd->data, v4ChunkSize)`), so the configured value is ignored.
- Evidence: setter `System_ESPNow.cpp:14301`; file send `11906` (with `v4ChunkSize` from `11848`); display/setter-only reads `13918/13939/13964`.
- Fix: Note the setting is not honored (file chunk size is fixed), or wire `gSettings.espnowFileChunkSize` into the file-transfer reader.

**`espnowmeshes` — `setpassphrase` (and `listjson`) subcommands undocumented**
- Says: Description/usage list only `[list|add|remove|enable|setdefault|rename]`.
- Actually: The handler also dispatches built-in `setpassphrase` and `listjson` subcommands. Neither appears in the table description/usage at `14134`; the handler's own fallback usage at `11585` includes `setpassphrase` but still omits `listjson`.
- Evidence: dispatch `System_ESPNow.cpp:11560` (`listjson`), `11577-11579` (`setpassphrase`) vs table `14134`.
- Fix: Add `setpassphrase <label> <passphrase>` to the accepted-subcommand list/usage (or note it is equivalent to `espnowsetpassphrase`). `listjson` is an internal/web-UI variant and may be left out by design.

**`bondstream` — "(worker only)" is false; master-side remote-control behavior undocumented**
- Says: "Stream sensor data to bonded master (worker only): `bondstream <sensor> <on|off>`" / "the worker streams to the bonded master."
- Actually: The handler has no worker-only gate (checks only `bondModeEnabled`) and is documented in-code as bidirectional. On a MASTER, `startSensorDataStreaming()` does not stream local data — it sends `sendBondStreamCtrl()` telling the WORKER to start/stop. So "(worker only)" is false and the master-side remote-control path is undocumented.
- Evidence: guard `System_ESPNow.cpp:13761` (no `isBondMaster()`); comment `13756`; diagnostic header `13769`; master path `System_ESPNow_Sensors.cpp:335-347`.
- Fix: Drop "(worker only)": "Toggle bond sensor streaming (works on both roles). On a WORKER this starts/stops streaming this device's sensor to the bonded master; on a MASTER it commands the bonded worker to start/stop that sensor. Streamed data appears on the master via `espnowsensorstatus` / `GET /api/sensors/remote`."

**`bondtestsensor` — usage advertises `gamepad`, which the parser rejects (canonical token is `input`)**
- Says: "Usage: bondtestsensor [thermal|tof|imu|gps|gamepad|fmradio]."
- Actually: The handler maps via `stringToSensorType()`, which has no `gamepad` case — the canonical token is `input` — and silently falls through to the default `REMOTE_SENSOR_THERMAL`. So `bondtestsensor gamepad` quietly tests THERMAL. The usage also omits accepted tokens `input`, `rtc`, `presence`.
- Evidence: mapping `System_ESPNow.cpp:13863`; mapper cases + default `System_ESPNow_Sensors.cpp:204-216`; `sensorTypeToString(REMOTE_SENSOR_INPUT)=="input"` at `193`.
- Fix: "Usage: bondtestsensor [thermal|tof|imu|gps|input|fmradio|rtc|presence]" (replace `gamepad` with `input`, add missing tokens).

---

## Low severity

**`espnowpairsecure` — undocumented mesh-passphrase prerequisite**
- Says: "Pair device with encryption: `espnowpairsecure <mac> <name> [mesh]`" — usage lists no prerequisite, implying it works standalone.
- Actually: The handler hard-requires a mesh passphrase already set (`encryptionEnabled`) and refuses before parsing args; the prerequisite surfaces only as a runtime error.
- Evidence: gate `System_ESPNow.cpp:12357-12358`; description/usage `14201` omit it.
- Fix: Add to the usage: "Requires a mesh passphrase first — run `espnowsetpassphrase <mesh> <passphrase>`."

**`espnowregenidentity` — stale "once Phase 3.3 lands" future-tense note**
- Says: Success output says re-pairing happens "once Phase 3.3 lands," framing KEY_EX as not-yet-shipped.
- Actually: Phase 3.3 (KEY_EX) has shipped and is a live registered command (`espnowkeyex`, described as Phase 3.3), so the future-tense note is stale; re-pairing is required now.
- Evidence: note `System_ESPNow.cpp:9758` vs live registration `14113`.
- Fix: Present tense: "NOTE: all previously paired peers must be re-paired (re-run KEY_EX / `espnowpairsecure` on both ends)."

**`espnowmeshttl` — mesh TTL (1-10) is an inert routing knob**
- Says: Presents mesh TTL as a live functional routing knob ("Get/set mesh TTL").
- Actually: `gSettings.meshTTL` is only written and displayed — never put in an outgoing frame header nor decremented on RX/forward. Frame-header `ttl` is fed literals (mostly 1); there is no RX TTL-decrement/re-forward path.
- Evidence: write/display-only sites `System_ESPNow.cpp:9903/9914/9924/9927/9944/9962` + registration `14277`; hardcoded sends `1501/2177/8325`; decl `System_Settings.h:675`.
- Fix: Note mesh TTL is currently advisory/unused (single-hop star mesh does not decrement TTL on RX), or drop the implication that it limits hop count. *(Merge target: pairs with the medium `espnowmeshadaptivettl` finding — same root cause, TTL settings are not read on any TX path.)*

**`espnowrooms` — "(master)" reads as a role requirement that does not exist**
- Says: "List rooms and their devices (master)." — the annotation implies master-role-only.
- Actually: The handler performs no role check and `requiresAdmin` is false; it iterates `gMeshPeerMeta` and prints on any device. A worker/backup gets full output. Cosmetic.
- Evidence: registration `System_ESPNow.cpp:14159` (admin=false); handler `10276-10356` (no role gate; e.g. loops `10319-10346`).
- Fix: Drop "(master)": "List rooms and their devices from the synced peer-metadata cache." If noting coverage, phrase as a non-gating caveat: "(aggregated from this node's cached peer metadata)."

**`espnowbroadcast` — no length note; long messages fail silently (no fragmentation)**
- Says: Usage carries no length note; sibling `espnowsend` documents a 1024-byte cap with fragmentation, implying broadcast handles long text similarly.
- Actually: Broadcast is single-frame only. Messages longer than `ESPNOW_V4_MAX_PAYLOAD` (218 bytes) are silently rejected by `v4_broadcast_text` (returns false), reported as "sent to 0 device(s) (1 failed)" with no explanation. No fragmentation path here.
- Evidence: reject `System_ESPNow.cpp:2521`; `ESPNOW_V4_MAX_PAYLOAD` = `250 - 32` = 218 at `System_ESPNow_Wire.h:30`.
- Fix: "espnowbroadcast <message> (single frame; <= 218 bytes — longer text is not fragmented and fails silently as '0 device(s) (1 failed)')."

**`espnowbuffers` — inconsistent admin gating vs the per-field setters**
- Says: `espnowbuffers` has `requiresAdmin=false`, while the four dedicated setters (`espnowtxqueuesize`/`rxbuffersize`/`chunksize`/`filechunksize`) all have `requiresAdmin=true`.
- Actually: `cmd_espnow_buffers` has no internal admin check and writes the exact same `gSettings.espnow{TxQueueSize,RxBufferSize,ChunkSize,FileChunkSize}` fields via `setSetting`. A non-admin can mutate via `espnowbuffers` what the dedicated commands admin-gate. (Low impact only because the fields are currently inert.)
- Evidence: `System_ESPNow.cpp:14211` (admin=false) vs `14223-14226` (all true); handler writes `13952/13956/13960/13964`; body `13904-13971` has no auth check.
- Fix: Align gating — set `espnowbuffers` `requiresAdmin=true` to match the per-field setters (or downgrade the setters to false) so the same writes share one permission level.

**`bondtestsensor` — worker-only requirement enforced but undocumented**
- Says: "Test v3 sensor data transmission ... the test frame appears on the bonded master's remote-sensor cache" — no role note.
- Actually: `sendBondedSensorData()` returns false immediately on a bond MASTER, so on a master `bondtestsensor` always returns FAILED regardless of args. The worker-only requirement is enforced but never documented.
- Evidence: `System_ESPNow.cpp:12000-12001` (`if (isBondMaster()) return false;`), called at `13880`; FAILED return `13888-13889`.
- Fix: Add "(worker only)": "Test v3 sensor data transmission (worker only — a master cannot send sensor data): `bondtestsensor [sensor_type]`."

**`bondstream` — invalid-sensor error text lists `gamepad`, which the command rejects**
- Says: Invalid-sensor error: "Unknown sensor. Valid: thermal, tof, imu, gps, gamepad, fmradio, rtc, presence."
- Actually: `stringToSensorType("gamepad")` matches no case and returns the default THERMAL, so the round-trip validation rejects `gamepad` as Unknown. The error text lists a token the command itself rejects; the canonical token is `input`.
- Evidence: validation `System_ESPNow.cpp:13808-13811`; mapper has no gamepad case `System_ESPNow_Sensors.cpp:204-216`; `sensorTypeToString(REMOTE_SENSOR_INPUT)=="input"` at `193`.
- Fix: Change `gamepad` to `input`: "Unknown sensor. Valid: thermal, tof, imu, gps, input, fmradio, rtc, presence." *(Same root cause as the medium `bondtestsensor` arg-signature finding — `gamepad` vs `input`.)*

**`bondrole` — documented as set-only, but no-arg is a getter**
- Says: Set-only: "Set bond mode role: `bondrole <master|worker>`" / "Usage: bondrole <master|worker>."
- Actually: With no argument the handler is a GETTER — it prints the current role and returns without setting. The command is get/set; the help omits the query form.
- Evidence: getter branch `System_ESPNow.cpp:13627-13632`; setter only on non-empty matching arg `13644`.
- Fix: "Get/set bond mode role: `bondrole [master|worker]`" and "Usage: bondrole [master|worker] (no arg shows current role)."

---

## Lower-confidence / judgment calls

- **`espnowmeshes` `listjson` omission** — Documenting `listjson` is a judgment call; it reads as an internal/web-UI variant and may be intentionally excluded from human-facing usage. Only `setpassphrase` is clearly a documentation gap.
- **`espnowmeshttl` (1-10) inertness vs `espnowmeshadaptivettl`** — These two are the same root cause (no TTL setting is read on any TX path; mesh is single-hop). Kept as separate entries because they are distinct registered commands with distinct help strings, but a single shared "TTL is inert" caveat could cover both.
- **`espnowbuffers` admin-gating inconsistency** — This is a gating-consistency observation, not strictly a help-vs-behavior mismatch, and its impact is near-zero while the four buffer/chunk fields remain inert. Severity could arguably be "won't-fix until the fields are wired up."
- **`espnowrooms` "(master)" vs `espnowmeshtopo`/`espnowtimesync` "(master only)"** — All three are role-overclaims, but `espnowrooms` is purely cosmetic (no async/broadcast side effect), whereas the other two let a non-master emit frames; grouping rooms as Low while the other two are Medium reflects that behavioral difference, not a difference in factual confidence.
- **Inert-config cluster framing** — The seven write-only settings (`workerStatusInterval`, `topoDiscoveryInterval`, `topoAutoRefresh`, `txQueueSize`, `rxBufferSize`, `chunkSize`, `fileChunkSize`) plus `espnowworker`/`espnowmeshadaptivettl` are all the same class of defect (stored-but-never-consumed). They are listed individually for actionability, but a reviewer may prefer to treat them as one "inert ESP-NOW config surface" remediation item.
