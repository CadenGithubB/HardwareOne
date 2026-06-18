# ESP-NOW JSON command contract (Phase 1)

For the Android app talking to one device over the BLE secure channel. Each
command below has a `json` variant: send the command with a `json` token and it
returns a single JSON object instead of human text. The **text** output is
unchanged — only the `json` form is new (additive). Drive these exactly like the
existing `status json` / `files json` calls.

Conventions (match the rest of the firmware's JSON):
- Every object starts with `"schema": 1`.
- Errors use `{"schema":1,"ok":false,"error":"<reason>"}`. Absence of `ok:false`
  means success.
- Roles/modes are **strings**, not ints. Empty optional strings are `""` (never
  the human placeholder `"(not set)"`).
- MACs are upper-case colon-hex strings.

---

## `bondstatus json`
Disabled:
```json
{ "schema":1, "enabled":false, "role":"master" }   // role: master | worker
```
Enabled:
```json
{ "schema":1, "enabled":true, "role":"master",
  "peer":"AA:BB:CC:DD:EE:FF", "peerName":"gamepad",
  "online":true, "syncState":"synced",            // syncState: synced | syncing | offline
  "sync":{ "cap":true, "manifest":true, "settingsRx":true, "settingsTx":true },
  "heartbeatsSent":42, "heartbeatsReceived":40, "lastHeartbeatAgo":3 }
```

## `espnowdeviceinfo json`
```json
{ "schema":1, "name":"node1", "friendlyName":"Living Room", "room":"living",
  "zone":"downstairs", "tags":"a,b", "stationary":true,
  "meshRole":"worker",                              // worker | master | backup
  "mac":"AA:BB:CC:DD:EE:FF" }
```
(any of name/friendlyName/room/zone/tags may be `""`)

## `espnowmode json`
```json
{ "schema":1, "enabled":true, "mode":"mesh" }       // mode: direct | mesh
```

## `espnowmeshrole json`
```json
{ "schema":1, "role":"worker",                      // worker | master | backup
  "masterMac":"AA:..", "backupEnabled":false, "backupMac":"" }
```

## `espnowencstatus json`
```json
{ "schema":1, "running":true, "encrypted":true,
  "passphraseSet":true, "passphraseLength":16, "keyFingerprint":"A1B2C3D4" }
```
(`passphraseSet`/`passphraseLength`/`keyFingerprint` present only when `encrypted:true`)
Not initialized → `{ "schema":1, "ok":false, "error":"ESP-NOW not initialized" }`

## `espnowsensorstatus json`
Master (aggregates remote sensor cache). NOTE: `sensors` is an **array of
sensor-type strings** — which sensors that device *has* — NOT a readings object.
(Live readings stream separately; this is presence, not values.)
```json
{ "schema":1, "broadcast":true, "role":"master",
  "devices":[ { "mac":"AA:..", "name":"node2", "sensors":["thermal","imu"] } ] }
```
Worker (its own stream toggles):
```json
{ "schema":1, "broadcast":true, "role":"worker",
  "streaming":{ "thermal":false,"tof":false,"imu":true,"gps":false,
                "input":false,"fmradio":false,"camera":false,"microphone":false } }
```

---

## Already JSON (now carry `schema:1`)
- `espnowlist json` → `{ "schema":1, "devices":[{mac,name,encrypted,meshId}], "count":N }`
- `espnowmeshstatus json`:
  ```json
  { "schema":1,
    "peers":[ { "mac":"…", "name":"…", "alive":true, "activityAlive":true,
                "heartbeatCount":12, "ackCount":10, "lastHeartbeat":1234,
                "lastAck":1230, "lastRxActivity":1235,
                "secondsSinceActivity":2, "secondsSinceHeartbeat":2 } ],
    "totalPeers":1,
    "unpaired":[ { "mac":"…", "name":"Unknown", "rssi":-60,
                   "heartbeatCount":3, "secondsSinceLastSeen":5 } ],
    "totalUnpaired":1, "retryQueue":[…], "activeRetries":0 }
  ```
  (NOTE: `peers[]` has **no** `rssi` — only `unpaired[]` does. Errors now `{ "schema":1, "ok":false, "error":… }`.)

---

## `espnowmessages json [sinceSeq] [mac]`  — the async-result retrieval (Phase 3 unblock)
This is how a BLE client gets the **results** of relayed remote ops. The relay
commands (`espnowremote`/`espnowbrowse`/`espnowfetch`) only return a "sent" ack;
the actual peer output arrives asynchronously and lands in a message buffer.
Poll this command to read it.
```json
{ "schema":1, "messages":[
  { "seq":42, "reqId":1234567, "piece":1, "of":1, "mac":"AA:BB:CC:DD:EE:FF",
    "name":"node2", "msg":"<peer output / text>", "enc":true, "ts":1234567, "type":3,
    "sent":false, "sendState":0 }
]}
```
- `sent` — **direction.** `false` = received from the peer; `true` = a message
  **we** sent to that peer. Sent messages are now recorded in shared per-device
  history (at the `espnowsend` chokepoint) so every interface shows the same
  conversation, not just the UI that sent it. A client that renders its own
  optimistic "sending…" bubble should **de-dupe the polled echo by `reqId`**
  (which equals the send's `msgId`); a `sent:true` record with no matching local
  bubble originated on another interface and should be rendered as outgoing.
- `sendState` — **durable delivery state of a sent message** (`sent:true` rows
  only; `0` and meaningless for received). `0` = pending/Sent (awaiting ACK),
  `1` = delivered (ACK received), `2` = timeout / no ACK, `3` = failed (e.g. the
  encrypted-session handshake never completed). This is stamped onto the stored
  record when the ACK resolves, so it stays correct **after a reload** and long
  after the in-RAM `sendstatus` tracker entry is swept (~30 s). Prefer this over
  the legacy `/api/espnow/sendstatus` poll + `deliveries[]` snapshot for showing
  a bubble's ✓ → ✓✓; those remain only for live pending→delivered upgrades inside
  the tracker's retention window.
- `piece` / `of` — **chunked messages.** A message longer than one ESP-NOW frame
  is stored as **separate records**, each one fragment (≤200 B), all sharing the
  same `reqId` (the group id). `piece` is the 1-based fragment index, `of` is the
  total. The device does **not** reassemble — the client must **group by `reqId`,
  order by `piece`, and concatenate `msg`** to get the full message. `of:1` means
  a normal single-frame message (no stitching needed). If pieces are missing
  (lost / aged out of the ring), render what you have as partial.
- `seq` — the buffer's own monotonic cursor (for incremental polling, below).
- `reqId` — **correlation id**: the `msgId` of the request that produced this message. For a relayed remote op (`espnowremote`/`browse`/`fetch`) this equals the `reqId` returned by the send ack (see below), so you can match a result to the exact request that caused it. `0` for unsolicited messages (peer text, file-event logs) that aren't answers to a request.
- `sinceSeq` — pass the highest `seq` you've seen to get only newer messages (incremental poll). Omit/`0` = all buffered.
- `mac` — optional, filters to one peer.
- `type` — message-type int (text vs command-response vs file-transfer).
- **Pattern:** send `espnowremote … json` → read `reqId` from the ack → poll `espnowmessages json <lastSeq>` and pick the message whose `reqId` matches. (Both success and failure responses now echo the request's `reqId`, so a failed remote command is correlatable too.) Same as the web's `/api/espnow/messages` loop, now reachable over BLE.

### Relay send acks now carry the ticket (`espnowremote` / `espnowbrowse` / `espnowfetch` ` json`)
These were text-only "sent" acks; they now have a `json` variant that returns the `reqId` to wait for:
```json
{ "schema":1, "ok":true, "reqId":1234567 }        // request accepted + sent; poll espnowmessages for reqId
{ "schema":1, "ok":false, "error":"not paired" }  // or: "not initialized" | "encryption required" | "usage" | "send failed" | "no peer entry" | "self target" | "bad path arg"
```
The text form is unchanged. Only `… json` returns the object. `reqId` is the same value that comes back in the `espnowmessages` `reqId` field above.

## Additional reads (also JSON now — same `… json` pattern)
```jsonc
// espnowstatus json
{ "schema":1, "initialized":true, "channel":6, "mac":"AA:BB:CC:DD:EE:FF", "pairedDevices":2 }
// (not initialized) → { "schema":1, "ok":false, "error":"ESP-NOW not initialized" }

// espnowstats json
{ "schema":1, "messagesSent":120, "messagesReceived":118, "messagesFailed":2,
  "streamSent":40, "streamReceived":40, "streamDropped":0,
  "heartbeatsSent":50, "heartbeatsReceived":48,   // present only in mesh mode
  "filesSent":1, "filesReceived":0, "uptimeSec":3600 }

// bondshowcap json — THIS device's capability summary (the same masks peers exchange).
// Decode the masks with the CAP_FEATURE_* / CAP_SERVICE_* / CAP_SENSOR_* bits.
{ "schema":1, "device":"node1", "mac":"AA:..", "role":"master", "fwHash":"1A2B3C4D",
  "featureMask":1023, "serviceMask":7, "sensorMask":13,
  "flashMB":16, "psramMB":8, "wifiChannel":6, "uptimeSec":3600 }
```
> `bondshowcap json` is the gateway's *own* capabilities. The per-**peer** "what can this peer do"
> (decoded remote masks) is a separate Phase-2 command (`remotecap`), not this.

---

## NOT in Phase 1 (still text-only — do not build JSON parsers for these yet)
- Action/setter commands (`openespnow`, `espnowsetname`, `espnowmode <set>`, `pair`, `send`, `bondconnect`, …) — result is ok/fail text.
- Relay commands (`espnowremote`/`browse`/`fetch`) — the `… json` ack now returns `{ok,reqId}` (see "Relay send acks" above); the actual async result still arrives via **`espnowmessages json`**, matched by `reqId`.
- Diagnostics with no app consumer (`routerstats`, `saturation`, `sessions`, `subs`, `identity`).
- `espnowmeshtopo`/`espnowtoporesults` — topology (nested multi-hop). Still text; deferred to the web sweep / Phase 4 (complex + web-primary; the app doesn't need it until then).
