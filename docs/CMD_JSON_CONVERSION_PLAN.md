# CLI Command → JSON Conversion Plan

> Plan to give every data-returning CLI command a JSON output mode so web pages parse
> JSON over `/api/cli` (pattern **b**) instead of scraping text — letting the bespoke REST
> surface shrink toward the command bus. Ranked by **ease (complexity + feasibility)**, easiest first.
> Companions: [WEB_API_INVENTORY.md](WEB_API_INVENTORY.md), [WEB_PAGE_API_MAP.md](WEB_PAGE_API_MAP.md).

## Overview

- **Commands assessed:** 186 across 21 modules
- **Already JSON-capable:** 24 (Wave 0 — ratify, near-zero code)
- **By complexity:** trivial 71, low 95, medium 17, high 3
- **By feasibility:** high 175, medium 10, low 1
- **By category:** action 105, setter 24, data-read 19, already-json 16, status 9, toggle 7, list 6

The conversion needs no new web transport: /api/cli already returns each command's const char* as text/plain with HTTP status set from the result, and /api/cli/batch nests raw outputs in results[] — so a command emitting {"schema":1,"success":...} is parsed by the page with JSON.parse() over the existing endpoint. Standardize on the codebase-wide schema-1 flat envelope (used across ~21 command modules — NOT ESP-NOW-specific; ESP-NOW is just its largest user) with `success` as the always-present status flag, gate it per-call via the existing argWantsJson (anywhere) / argLeadingTokenIsJson (free-text/path args) helpers so serial+OLED keep human text as default, and reuse CompactJson / PSRAM_JSON_DOC builders. Roll out in 5 waves: (0) ratify ~30 already-JSON commands and fix the espnowmeshes-missing-schema and ei detect/file error-path bugs; (1) trivial setters/toggles/actions; (2) low-complexity status/getters that let /api/mqtt/status, /api/sensors/remote, /api/sensors/camera/status retire; (3) side-effecting actions + file/espnow mutations retiring /api/files/create and /api/espnow/{metadata,remotecap,remotemanifest}; (4) hard multi-subcommand/paged/confirmation/blocking commands. Irreducible: MJPEG stream, camera frame, SSE /api/events, file/backup upload+binary downloads, and unbounded lists (automation list, ei model list, espnowtoporesults) that need paging or dedicated chunked endpoints. The dominant systemic risk is the shared static return buffer (getDebugBuffer, 1024B) which is non-reentrant under concurrent web/BLE/serial callers and silently truncates — this must be solved before Wave 1, alongside paging, dual-output drift, schema-bump discipline, JSON-side redaction parity, and async-ack semantics. Source-of-truth endpoint list is docs/WEB_API_INVENTORY.md; reference impls are cmd_ringstatus (G2_Ring.cpp:1180), cmd_espnow_mode (System_ESPNow.cpp:9974), and fmradioread which /sensors already polls as JSON.

## The JSON Envelope (the centralized output contract)

There are **two** JSON envelopes in the firmware today, from organic divergence — neither is a designed standard:

- **Command JSON** (`json` modes across ~21 `System_*` modules): `{"schema":1, …}` — has *versioning*, but no consistent status flag. `schema` appears ~50 times; ESP-NOW is just the biggest user, not the origin.
- **Web-endpoint JSON** (the `/api/*` handlers): `{"success":true/false, "error":…}` — has a *status flag*, but no `schema`.

They solve orthogonal problems (`schema` = "what shape/version", `success` = "did it work"), so the standard **takes the good half of each**. All data-returning commands AND web endpoints emit ONE flat object:

```jsonc
{
  "schema": 1,             // int, REQUIRED. Shape/version. Already the de-facto command convention (~50 uses, ~21 modules); web endpoints currently LACK it and gain it. Add fields freely under schema 1; bump only on rename/removal/retype.
  "success": true,         // bool, REQUIRED, ALWAYS present. Did this request succeed? Chosen over the command-side "ok" spelling for clarity. Pages branch on this — not on text-scraping, and not on HTTP status (only ~29 of the server's handlers set one). For /api/cli/batch each result carries its own success.
  "error": "<token>",      // string, present ONLY when success=false. Short machine token preferred (e.g. "not_initialized","no_model_loaded","out_of_range"); optional human detail in a separate "message".
  // ...payload...         // command-specific fields at TOP LEVEL (flat): wifiscan -> connected/ssid/ip/rssi; camerabrightness -> value/range/saved; bondstatus -> bonded/role/peerMac.
}
```

Conventions to keep uniform:
- Booleans are real JSON `true`/`false` (not `"true"`/`"1"` strings).
- `success` is ALWAYS present — including on status reads that emit no status flag today — so every client branches on exactly one field.
- Setters return `{schema, success, <key>:<value>, range:[lo,hi], saved:true}`; on validation failure `{schema, success:false, error:"out_of_range", range:[lo,hi]}`.
- Actions/mutations return `{schema, success, …echo of what changed…}`; async ones add the existing poll handle (reqId/msgId/target mac). `success:true` means **accepted/queued locally**, NOT "operation finished."
- Lists nest under a named array key (`files:[…]`, `sessions:[…]`, `networks:[…]`, `peers:[…]`) — never a bare top-level array — with an optional `"truncated":true` when the builder hit its buffer cap.
- Build with the existing helpers (`PSRAM_JSON_DOC` + ArduinoJson `serializeJson` for variable/nested; `CompactJson` for tiny fixed acks). Add one tiny `envelopeOpen(doc)` / `envelopeError(buf,"token")` pair so `schema`+`success` are never hand-typed inconsistently.

### Standardization & migration (do as part of Wave 0)

Adopting the one envelope is a bounded, coordinated change:
- **Web endpoints (~22 handlers):** keep the `success` word, **add `"schema":1`**. No client change — the ~35 page-JS sites reading `.success` are unaffected.
- **Commands emitting `ok` (~18 sites) + the ~22 page-JS sites reading command `.ok`:** rename `ok` → `success`. Dual-emit both during the transition, then drop `ok`.
- **Status-read commands with no status flag today:** add `success:true`.
- **New command conversions (Waves 1–4):** use `{schema, success, error, …}` from the start.
- **Skip anything slated for retirement** — don't migrate a handler you're about to delete.
- **Bugs to fix in passing:** `espnowmeshes listjson` omits `schema`; `ei detect` error/no-detection paths still return non-envelope text.

## Opt-In Mechanism (keep human text the default)

JSON is OPT-IN per call so the serial console / OLED keep their human text as the default — never a global switch. Two complementary, already-implemented detectors gate it (System_Utils.cpp):

1. argWantsJson(args) — true if a bare "json" token appears at ANY word boundary. Use for commands whose later args can never themselves be the literal "json" (most setters/actions/toggles: `camerabrightness 1 json`, `bondstatus json`, `wifiscan json`). This is what most existing JSON commands use.
2. argLeadingTokenIsJson(args) — true ONLY if the FIRST token is exactly "json". Use where later args are free text that could contain "json" (`files json "/path"`, `llm json <prompt>`, `espnowsend json <msg>`, `g2show json <text>`). Already used by files/llm to avoid false-triggering.

Rule of thumb for the rollout: pick argLeadingTokenIsJson for any command that takes a free-text / path / message arg; argWantsJson for everything else. Document the choice in each handler.

Transport: the web layer needs ZERO new endpoint or content-type. /api/cli already returns the handler's const char* verbatim as text/plain with HTTP status set from the result; the page just calls JSON.parse(body) and reads .success instead of regex-scraping. /api/cli/batch already nests each command's raw output as a string in results[], so JSON-mode commands batch transparently (page does JSON.parse(results[i])).

Tradeoffs vs a transport-set format flag (e.g. an Accept header or ?format=json on /api/cli): a header would let the web layer request JSON uniformly without the page mutating the command string, and would centralize the choice — but it bifurcates from BLE/serial/OLED callers that issue the same command strings, breaks the "one command string works on every transport" property the bond/espnow remote-exec paths rely on, and can't express the leading-vs-anywhere distinction. Recommendation: stay with the in-band "json" token (it travels with the command over web, BLE, serial, espnowremote, and bond/exec identically); optionally have the shared hw.* JS helper append " json" automatically when a caller opts in, so page code stays clean without a new transport contract.

## Solve First — Cross-Cutting Risks

These are inherited by every wave; the shared return buffer in particular must be resolved **before Wave 1**.

1. Shared static return buffer is the #1 concurrency hazard: most handlers serialize into getDebugBuffer() (single 1024B static) or other shared static char[]/String. Web + BLE + serial + espnowremote can call concurrently; the JSON of one caller can be overwritten mid-send by another, yielding torn/invalid JSON. Mitigation: either a small per-transport buffer pool keyed off the AuthContext origin, a short mutex around build+copy-to-response, or build into a caller-owned stack/PSRAM buffer. This must be decided BEFORE Wave 1 because every wave inherits it.
2. Hard 1024B/512B/256B ceilings + silent truncation: serializeJson and snprintf truncate to fit and produce invalid JSON with no error. Every list/status command needs an explicit length check after serialize and must set "truncated":true (or return success:false,error:"too_large") rather than emit a short object. Sizing audit needed for espnowmeshstatus (50+ peers), userlist, sessionlist, files list.
3. Paging for unbounded lists: ls/files, automation list, ei model list, espnowtoporesults can all exceed any single buffer. The command framework can't chunk, so these need offset/limit args returning {items:[...],offset,limit,total,truncated} — or stay on dedicated chunked HTTP endpoints. Don't pretend a non-paged list command is safe just because typical data is small.
4. Dual human+JSON maintenance: every converted command now has two output paths that can drift (a field added to text but not JSON, or vice-versa). Risk of the OLED/serial text silently regressing while only the web path is tested. Mitigation: build the data once into a struct/JsonDocument and render both human and JSON from it where feasible; add a smoke test that runs each command with and without 'json'.
5. Schema versioning discipline: "schema":1 only helps if bumped on breaking changes and pages tolerate unknown fields. Define the rule now (add fields freely under schema 1; bump to 2 only on rename/removal/retype) and make pages read defensively (feature-detect fields). The existing espnowmeshes-missing-schema bug shows the field gets forgotten — add a CI/grep check that every json branch sets schema.
6. Auth/identity & redaction parity: /api/cli already redacts passwords/SIDs from text output via redactOutputForLog before send. JSON payloads must be redacted the SAME way (pendinglist already strips password; userlist/sessionlist must not leak secrets in structured form). Per-task TLS auth identity means a converted command's permission checks must remain inside the handler, not assumed from the web layer.
7. Async ack semantics: many actions are fire-and-forget (espnow*, bond*, ringconnect, llm). The JSON ack only confirms LOCAL queueing; real completion arrives later via /api/espnow/messages or status polling. The envelope must carry the existing poll handle (reqId/msgId/target) and pages must not treat success:true as 'operation finished'. Mis-modeling this would make the web UI report success prematurely.

## Irreducible — Stays a Dedicated Endpoint

Cannot move to command-JSON (binary / stream / SSE / upload / unbounded-without-paging).

| Item | Why it can't fold into `/api/cli` |
|------|-----------------------------------|
| `/api/sensors/camera/stream (MJPEG)` | Continuous multipart/x-mixed-replace byte stream. A command handler returns a single const char* and cannot chunk/stream frames. |
| `/api/sensors/camera/frame` | Returns a raw JPEG binary blob, not text/JSON. Cannot ride the const char* return contract. |
| `/api/events (SSE)` | Server-sent-events long-lived stream of notifications/sensor-status pushes. Inherently push + open-ended; the request/response /api/cli model can't hold it open. |
| `/api/files/upload, /api/restore, /api/backup` | Multipart/binary file UPLOAD (request body is the payload) and large binary backup download. No command-string equivalent; body streaming required. |
| `/api/files/read, /api/files/view, /api/recordings/file, /api/videos/file, /api/icon, /favicon.ico, /apple-touch-icon*` | Serve raw file/image/video bytes (download or inline view). Binary asset delivery, not a JSON envelope. |
| `/api/automations (GET raw list) and 'automation list' subcommand` | automations.json is unbounded and is currently broadcast out-of-band, not returned as const char*. Without paging it overflows the 1024B return buffer; keep the dedicated chunked endpoint for the full list (per its own assessment recommendation). |
| `ei model list / 'edgeimpulse models'` | Unbounded model directory listing exceeds the 512B static buffer and truncates. Needs paging or a dedicated chunked /api/edgeimpulse/models endpoint before it can be JSON-over-cli. |
| `espnowtoporesults` | Builds from gTopoResultsBuffer which grows unbounded with mesh size (50+ peers, deep chains) and already risks overflowing its 2048B static buffer. Needs paging or a dedicated endpoint. |
| `dashboard, /llm/* generate+result+stream, all page handlers` | 'dashboard' and 'set' are not CLI commands (page/no-op). LLM generate streams tokens and result polling is its own async protocol; these stay dedicated. HTML page handlers stream full documents, incompatible with the envelope. |
| `/api/bond/fs/* and /api/espnow/messages` | Poll/transfer plumbing for the async remote-exec + file-pull protocols. They carry framed/streamed peer data and serve as the poll endpoints the JSON acks point AT — they are the substrate, not candidates to fold into it. |

## Rollout Waves

### Wave 0 — Document the already-done (ratify the contract)

~30 commands already emit a schema-1 envelope or are trivially close. Zero/near-zero code: verify the envelope, fix the two known bugs (espnowmeshes listjson missing schema field; ei detect/ei file error+no-detection branches still return non-envelope text), and switch the consuming pages from text-scrape to JSON.parse. Highest value-per-effort because it proves the pattern and immediately lets the sensors page stop scraping. fmradioread is the reference: /sensors already polls it as JSON via /api/sensors?sensor=fmradio.

**Commands (24):** `bleinfo`, `bondstatus`, `espnowdeviceinfo`, `espnowdevices`, `espnowfetch`, `espnowlist`, `espnowmeshrole`, `espnowmeshstatus`, `espnowmode`, `espnowremote`, `espnowsend`, `espnowstatus`, `fmradioread`, `ls (files)`, `pendinglist`, `ringstatus`, `sessionlist`, `srstatus`, `time`, `userlist`, `wifiscan`, `espnowmeshes (add schema)`, `ei detect (envelope error path)`, `ei file (envelope error path)`

### Wave 1 — Trivial setters/toggles/actions (cplx=trivial)

Single-value setters, boolean toggles, and fire-and-forget actions. Each is a few lines: detect json arg, snprintf/CompactJson one flat object. No unbounded payload, no async complexity. These are the bulk of the [0]-ranked list and they back the camera/LED/BLE/speech/auth-toggle web panels.

**Endpoints retired by this wave:** `/api/sensors/camera/status`

**Commands (46):** `beginwrite`, `blerequireauth`, `bonddisconnect`, `camerabrightness`, `cameracontrast`, `cameraexposure`, `camerafps`, `camerasaturation`, `cameravideodelete`, `closecamera`, `closegps`, `closeimu`, `closeinput`, `closemic`, `closemqtt`, `closethermal`, `closetof`, `displayrequireauth`, `ei confidence`, `ei enable`, `ei model load`, `ei model unload`, `ei track clear`, `ei track enable`, `espnowbondmodeenabled`, `g2clear`, `g2deinit`, `g2init`, `g2nav`, `g2scan`, `g2show`, `g2verbose`, `imustop`, `ledclear`, `mapunload`, `micbitdepth`, `micgain`, `micsamplerate`, `openmic`, `reboot`, `ringdisconnect`, `serialrequireauth`, `srraw`, `srstop`, `srtuninggain`, `wifiautoreconnect`

### Wave 2 — Low-complexity data-reads & status (foundations for whole pages)

Status/getter commands whose JSON envelope lets an entire bespoke status endpoint retire. Adding json to ei status, g2status/g2battery, srtuning, blmode/blename/bletxpower, the espnow* getters, and mqttclientenabled means the /sensors, /speech, /bluetooth, /mqtt and parts of /espnow pages can read /api/cli instead of dedicated handlers.

**Endpoints retired by this wave:** `/api/mqtt/status`, `/api/sensors/remote`

**Commands (26):** `bleadv`, `bleautostart`, `bledisconnect`, `blename`, `bletxpower`, `bleautoreconnect`, `blemode`, `blestream`, `bondresync`, `bondrole`, `bondstream`, `ei`, `ei model`, `ei model info`, `ei status`, `ei track`, `ei track status`, `espnowmeshmaster`, `espnowmeshbackup`, `g2battery`, `g2status`, `ledbrightness`, `ledcolor`, `mqttclientenabled`, `srdyngain`, `srtuning`

### Wave 3 — Action commands with side effects (open/close/start, file ops, espnow mutations)

Mutations and start/stop actions that return an echo of what changed plus (for async) the existing poll handle. Slightly more care: capture state for the ack, keep human text for serial. Converting the file-op and espnow-pair/meta family lets the file-explorer modal and /espnow page stop text-scraping /api/cli results.

**Endpoints retired by this wave:** `/api/files/create`, `/api/espnow/metadata`, `/api/espnow/remotecap`, `/api/espnow/remotemanifest`

**Commands (73):** `banuser`, `bondconnect`, `cameraframesize`, `camerahmirror`, `cameraquality`, `camerarecord`, `camerarotate`, `camerasave`, `cameravflip`, `certgen`, `closeble`, `closeespnow`, `closefmradio`, `closeg2`, `filecreate`, `filerename`, `fmradiomute`, `fmradioseek`, `fmradiotune`, `fmradiovolume`, `g2ai`, `g2mic`, `g2notify`, `g2reopen`, `imustart`, `mapload`, `micdelete`, `micrecord`, `mkdir`, `openble`, `opencamera`, `openespnow`, `openfmradio`, `openg2`, `opengps`, `openimu`, `openinput`, `openmqtt`, `openthermal`, `opentof`, `ringconnect`, `rmdir`, `savesettings`, `sessionrevoke`, `srstart`, `unbanuser`, `useradd`, `userapprove`, `userdemote`, `userdeny`, `userpromote`, `userresetpassword`, `usersync`, `wifiadd`, `wifidisconnect`, `espnowbackupenable`, `espnowbroadcast`, `espnowfriendlyname`, `espnowpair`, `espnowpairsecure`, `espnowrequestmeta`, `espnowroom`, `espnowsendfile`, `espnowsensorstream`, `espnowsetname`, `espnowsetpassphrase`, `espnowstationary`, `espnowtags`, `espnowunpair`, `espnowusersync`, `espnowzone`, `espnowmeshtopo`, `wificonnect`

### Wave 4 — Medium/hard: multi-subcommand, paged, confirmation, blocking

Routers and big readers that need per-subcommand shaping, paging, or async restructuring before JSON is safe. sensorlog/log status panels, automation mutations, and confirmation-flow deletes. Do these last; some sub-parts stay on dedicated endpoints (see irreducible). ledeffect/srautotune need async-or-document treatment because they block the handler thread.

**Endpoints retired by this wave:** `/api/automations/export (mutation acks move to cli; raw list stays)`

**Commands (8):** `sensorlog (status/state subcommands)`, `log (status subcommands)`, `automation (status/list-meta + mutation acks, NOT raw list)`, `filedelete (one-shot confirm path only)`, `userdelete (confirm-flow acks)`, `srautotune (ack-only, full state via srstatus)`, `ledeffect (async start ack)`, `g2bmp (ack with byte/fragment counts)`

---

## Master Ranked List

All commands sorted by **ease** (lower = simpler + more feasible = do first). `ease = complexity-rank + feasibility-rank`. ✅ = already has a JSON mode.

| # | Ease | Command | Page | Category | Cplx | Feas | Payload | JSON? | Why / note |
|---|------|---------|------|----------|------|------|---------|-------|-----------|
| 1 | 0 | `beginwrite` | /settings,/dashboard | action | trivial | high | none |  | Simple state setter — sets gDeferWrites flag to true. No dynamic data. Handler is 3 lines. Could em… |
| 2 | 0 | `bleinfo` | /bluetooth | already-json | trivial | high | medium | ✅ | Already has dual-mode: argWantsJson() checks for 'json' arg, then returns schema-compliant JSON via… |
| 3 | 0 | `blerequireauth` | /settings | toggle | trivial | high | none |  | BOOL_CMD macro expands to settingBoolToggle(gSettings.bluetoothRequireAuth, ...). No-args shows cur… |
| 4 | 0 | `bonddisconnect` | /bond | action | trivial | high | small |  | Mutation: clears bondModeEnabled flag, resets peer MAC, calls resetBondSync(), sets bondPeerOnline… |
| 5 | 0 | `bondstatus` | /bond | already-json | trivial | high | small | ✅ | ALREADY SUPPORTS JSON via argWantsJson() opt-in at line 13536. When JSON requested, serializes Ardu… |
| 6 | 0 | `camerabrightness` | /sensors | setter | trivial | high | none |  | Integer setter (-2..+2), persisted. Uses static buffer. Good JSON candidate: {"schema":1,"success":true\… |
| 7 | 0 | `cameracontrast` | /sensors | setter | trivial | high | none |  | Same pattern as camerabrightness. Simple JSON envelope suitable. |
| 8 | 0 | `cameraexposure` | /sensors | setter | trivial | high | none |  | Auto-exposure compensation (-2..+2). Persisted setting. Uses static buffer. Simple JSON wrapper. |
| 9 | 0 | `camerafps` | /sensors | setter | trivial | high | none |  | Stream FPS setting (1-20), persisted. Uses static buffer buf[96]. Simple setter with no live applic… |
| 10 | 0 | `camerasaturation` | /sensors | setter | trivial | high | none |  | Analogous to brightness/contrast. All three follow the same persisted numeric pattern. |
| 11 | 0 | `cameravideodelete` | /sensors | action | trivial | high | none |  | Deletion action, straightforward result. Can emit {"schema":1,"success":true\|false,"message":"...","fil… |
| 12 | 0 | `closecamera` | /sensors | action | trivial | high | none |  | Stop command with trivial output. Straightforward json wrap: {"schema":1,"success":true,"message":"..."}. |
| 13 | 0 | `closegps` | /sensors | action | trivial | high | small |  | Stop handler. Calls handleDeviceStopped(I2C_DEVICE_GPS). Single-line ack. Direct JSON adoption. |
| 14 | 0 | `closeimu` | /sensors | action | trivial | high | small |  | Stop/dequeue handler. Calls handleDeviceStopped(I2C_DEVICE_IMU). Single-line ack, ~60 bytes. Can di… |
| 15 | 0 | `closeinput` | /sensors | action | trivial | high | small |  | Stop handler. Calls handleDeviceStopped(I2C_DEVICE_INPUT). Single-line ack. Direct JSON adoption. |
| 16 | 0 | `closemic` | /sensors | action | trivial | high | none |  | Handler cmd_micstop (line 777) calls stopMicrophone() and returns static text. No JSON mode. Trivia… |
| 17 | 0 | `closemqtt` | /mqtt | action | trivial | high | small |  | Minimal action command — always succeeds (stopMQTT has no error path). Currently returns only strin… |
| 18 | 0 | `closethermal` | /sensors | action | trivial | high | small |  | Stop handler. Calls handleDeviceStopped(I2C_DEVICE_THERMAL). Single-line ack, ~65 bytes. Direct JSO… |
| 19 | 0 | `closetof` | /sensors | action | trivial | high | small |  | Stop handler. Calls handleDeviceStopped(I2C_DEVICE_TOF). Single-line ack. Direct JSON adoption. |
| 20 | 0 | `displayrequireauth` | /settings | toggle | trivial | high | none |  | No-args displays 'displayRequireAuth = true\|false'; with arg [on\|off\|0\|1] toggles via setSettin… |
| 21 | 0 | `ei confidence` | /sensors | setter | trivial | high | small |  | Setter + query. JSON mode: {"schema":1,"success":true,"minConfidence":float} on set/query. No payload co… |
| 22 | 0 | `ei detect` | /sensors | data-read | trivial | high | medium | ✅ | Command ALREADY partially outputs JSON (via buildDetectionJson for detected objects). Payload: sing… |
| 23 | 0 | `ei enable` | /sensors | setter | trivial | high | small |  | Setter + query command. Takes 0 or 1 arg (0/1 toggle). Output fits easily in 512B buffer. JSON conv… |
| 24 | 0 | `ei file` | /sensors | data-read | trivial | high | medium | ✅ | Identical payload to 'ei detect' but reads from file instead of camera. Already uses buildDetection… |
| 25 | 0 | `ei model load` | /sensors | action | trivial | high | small |  | Action that loads TFLite model. JSON mode: {"schema":1,"success":true,"modelPath":str,"inputWidth":int,"… |
| 26 | 0 | `ei model unload` | /sensors | action | trivial | high | small |  | Simple action. JSON mode: {"schema":1,"success":true,"unloadedModel":str} on success; {"schema":1,"success":f… |
| 27 | 0 | `ei track clear` | /sensors | action | trivial | high | small |  | Simple action. JSON mode: {"schema":1,"success":true,"clearedCount":int}. |
| 28 | 0 | `ei track enable` | /sensors | setter | trivial | high | small |  | Setter + query. JSON mode: {"schema":1,"success":true,"trackingEnabled":bool} on set/query. |
| 29 | 0 | `espnowbondmodeenabled` | /settings | toggle | trivial | high | none |  | ESPNOW_SETTING_CMD macro expands to handleSettingCommand(findEspnowEntry("bondModeEnabled"), ...).… |
| 30 | 0 | `espnowdeviceinfo` | /espnow | already-json | trivial | high | small | ✅ | Read-only device metadata (settings). JSON already implemented with schema envelope. Uses getDebugB… |
| 31 | 0 | `espnowdevices` | /espnow | already-json | trivial | high | medium | ✅ | JSON branch: serializes gMeshPeerMeta[] (bounded by MESH_PEER_MAX, typically ~50). Uses local stati… |
| 32 | 0 | `espnowfetch` | /espnow | already-json | trivial | high | small | ✅ | Uses argWantsJson(argsInput) check; already returns JSON on request. Async fetch — returns reqId fo… |
| 33 | 0 | `espnowlist` | /espnow | already-json | trivial | high | medium | ✅ | Already JSON-only. Uses gEspNow->listBuffer (1024 bytes, allocated in PSRAM on init). Shared buffer… |
| 34 | 0 | `espnowmeshrole` | /espnow | already-json | trivial | high | small | ✅ | Get-or-set command. JSON path is read-only (getter), text also supports setter. Settings-backed (gS… |
| 35 | 0 | `espnowmeshstatus` | /espnow | already-json | trivial | high | medium | ✅ | Complex aggregation: peers array (built via buildMeshStatusPeersJson helper), unpaired devices, ret… |
| 36 | 0 | `espnowmode` | /espnow | already-json | trivial | high | small | ✅ | Multi-purpose command: getter (read mode) and setter (write mode). JSON mode already implemented fo… |
| 37 | 0 | `espnowremote` | /espnow | already-json | trivial | high | small | ✅ | Remote command execution — async result via espnowmessages polling. Already uses argWantsJson check… |
| 38 | 0 | `espnowsend` | /espnow | already-json | trivial | high | small | ✅ | JSON mode already present. The command checks for leading 'json' arg (line 12896). Returns {"schema… |
| 39 | 0 | `espnowstatus` | /espnow | data-read | trivial | high | small | ✅ | Already implemented with JSON envelope. Uses getDebugBuffer (1024 bytes, shared). Safe — read-only… |
| 40 | 0 | `fmradioread` | /sensors | data-read | trivial | high | small | ✅ | Handler: cmd_fmradio_status() at line 670-709. ALREADY IMPLEMENTS JSON MODE (argWantsJson check at… |
| 41 | 0 | `g2clear` | /bluetooth | action | trivial | high | none |  | Calls g2ClearDisplay(). Binary ack. JSON: {schema:1,success:true\|false}. |
| 42 | 0 | `g2deinit` | /bluetooth | action | trivial | high | none |  | Always returns success. Calls deinitG2Client(). Candidates for JSON: {schema:1,success:true}. |
| 43 | 0 | `g2init` | /bluetooth | action | trivial | high | none |  | Simple binary ack. Handler calls initG2Client(). Can wrap response in {schema:1,success:true\|false,erro… |
| 44 | 0 | `g2nav` | /bluetooth | toggle | trivial | high | none |  | Parses [on\|off\|toggle], updates gG2MenuNavEnabled global. Bare command returns current state. JSO… |
| 45 | 0 | `g2scan` | /bluetooth | action | trivial | high | none |  | Synchronous scan result (true/false). Maps to {schema:1,success:true,found:true} or {success:false}. No buffe… |
| 46 | 0 | `g2show` | /bluetooth | action | trivial | high | none |  | Takes text arg, calls g2ShowText(). Binary success/fail. JSON: {schema:1,success:true\|false,error:...}. |
| 47 | 0 | `g2verbose` | /bluetooth | toggle | trivial | high | none |  | Parses [on\|off\|toggle], updates gG2ScanVerbose global. Bare command returns current state. JSON:… |
| 48 | 0 | `imustop` | /games | action | trivial | high | small |  | Simple handler that calls handleDeviceStopped() and returns a fixed string. Output is a single-line… |
| 49 | 0 | `ledclear` | /settings | action | trivial | high | none |  | No arguments, no dynamic output (returns hardcoded string). Does not use gDebugBuffer. Lightest han… |
| 50 | 0 | `ls (files)` | /files,/cli | data-read | trivial | high | large-unbounded | ✅ | Already has JSON modes! (1) cmd_files with no args returns human-readable listing via broadcastOutp… |
| 51 | 0 | `mapunload` | /maps | action | trivial | high | none |  | Handler returns static string literals, no buffer allocation. Takes no arguments. Calls MapCore::un… |
| 52 | 0 | `micbitdepth` | /sensors | setter | trivial | high | small |  | Handler cmd_micbitdepth (line 991) is getter/setter. On set, calls stopMicrophone() then initMicrop… |
| 53 | 0 | `micgain` | /sensors | setter | trivial | high | small |  | Handler cmd_micgain (line 968) is getter/setter (no arg=read, arg=set). Returns text via gMicCmdBuf… |
| 54 | 0 | `micsamplerate` | /sensors | setter | trivial | high | small |  | Handler cmd_micsamplerate (line 926) is getter/setter. On set, calls stopMicrophone() then initMicr… |
| 55 | 0 | `openmic` | /sensors | action | trivial | high | none |  | Handler cmd_micstart (line 769) calls initMicrophone() and returns static text via gMicCmdBuffer[51… |
| 56 | 0 | `pendinglist` | /settings | data-read | trivial | high | small | ✅ | Already has opt-in JSON mode via argWantsJson(). Uses static 2KB PSRAM buffer. Sanitizes output by… |
| 57 | 0 | `reboot` | /settings,/dashboard | action | trivial | high | none |  | Calls broadcastOutput() once, then ESP.restart(). No dynamic data. JSON mode trivial: skip broadcas… |
| 58 | 0 | `ringdisconnect` | /bluetooth | action | trivial | high | none |  | Calls g2RingDisconnect(). Simple fire-and-forget. JSON: {schema:1,success:true}. |
| 59 | 0 | `ringstatus` | /bluetooth | already-json | trivial | high | small | ✅ | ALREADY IMPLEMENTS JSON MODE! Uses argWantsJson() to detect 'json' arg, builds CompactJson response… |
| 60 | 0 | `serialrequireauth` | /settings | toggle | trivial | high | none |  | No-args displays 'serialRequireAuth = true\|false'; with arg [on\|off\|0\|1] toggles via setSetting… |
| 61 | 0 | `sessionlist` | /settings | data-read | trivial | high | small | ✅ | Already has opt-in JSON mode via argWantsJson(). Uses static 2KB PSRAM buffer. Builds session list… |
| 62 | 0 | `set` | /settings,/dashboard | already-json | trivial | high | none |  | NOT A CLI COMMAND. No handler registered in settingsCommands[] or any module. 'set' may have been c… |
| 63 | 0 | `sr` | /speech | list | trivial | high | none |  | Multi-subcommand router. Handler is a stub returning usage string. Not a data-bearing command. JSON… |
| 64 | 0 | `srraw` | /speech | toggle | trivial | high | small |  | Binary toggle command: srraw on\|off. No args = status query. Current output is plain text status +… |
| 65 | 0 | `srstatus` | /speech | already-json | trivial | high | medium | ✅ | Already emits JSON via buildESPSRStatusJson(line 2672). Uses PSRAM_JSON_DOC macro + ArduinoJson. St… |
| 66 | 0 | `srstop` | /speech | action | trivial | high | none |  | Stops ESP-SR pipeline and disarms voice. No dynamic allocation — returns const char* literals only.… |
| 67 | 0 | `srtuninggain` | /speech | setter | trivial | high | small |  | Setter command: srtuninggain <0.1-10.0>. Query (no args) returns current value + usage. Ack include… |
| 68 | 0 | `time` | /settings,/dashboard | data-read | trivial | high | small | ✅ | ALREADY HAS JSON MODE via argWantsJson(). Handler manually builds JSON in static 256-byte PSRAM buf… |
| 69 | 0 | `userlist` | /settings | data-read | trivial | high | small | ✅ | Already has opt-in JSON mode via argWantsJson(). Uses static 2KB PSRAM buffer. Copies users array i… |
| 70 | 0 | `wifiautoreconnect` | /settings | setter | trivial | high | none |  | Simple on/off toggle. Handler uses getDebugBuffer() for text output (shared static buffer). Could e… |
| 71 | 0 | `wifiscan` | /settings | already-json | trivial | high | large-unbounded | ✅ | ALREADY HAS JSON MODE (argWantsJson check at line 399). Returns static String jsonResult built inli… |
| 72 | 1 | `banuser` | /settings | action | low | high | none |  | State-changing: sets 'banned':true + optional banReason in users.json. Revokes active sessions. Ret… |
| 73 | 1 | `bleadv` | /bluetooth | action | low | high | none |  | Controls BLE advertising (start/stop/toggle). Takes optional subcommand arg. Returns simple ack. Co… |
| 74 | 1 | `bleautostart` | /bluetooth | status | low | high | small |  | Implemented via BOOL_CMD(bleautostart, gSettings.bluetoothAutoStart, "[BLE] Auto-start") macro whic… |
| 75 | 1 | `bledisconnect` | /bluetooth | action | low | high | none |  | Disconnects active BLE client. Returns ack. Could adopt JSON: {"schema":1,"success":true\|false,"message… |
| 76 | 1 | `blename` | /bluetooth | status | low | high | small |  | Gets/sets BLE device name (1-29 chars). Uses getDebugBuffer for output (shared but safe). Could ado… |
| 77 | 1 | `blerequireauth` | /bluetooth | status | low | high | small |  | Implemented via BOOL_CMD(blerequireauth, gSettings.bluetoothRequireAuth, "[BLE] Require auth") macr… |
| 78 | 1 | `bletxpower` | /bluetooth | status | low | high | small |  | Gets/sets TX power level (0-7 scale). Uses getDebugBuffer for output (shared but safe). Could adopt… |
| 79 | 1 | `bondconnect` | /bond | action | low | high | small |  | Mutation: resolves peer by MAC/name, enables bondModeEnabled, sets bondPeerMac and role via MAC com… |
| 80 | 1 | `bondresync` | /bond | status | low | high | small |  | Command is ASYNC (dispatches requests, returns immediately). Uses shared 1024-byte getDebugBuffer()… |
| 81 | 1 | `cameraframesize` | /sensors | setter | low | high | none |  | Sets resolution index (0-10) from setting. May trigger camera restart. Uses static buffer. Can emit… |
| 82 | 1 | `camerahmirror` | /sensors | setter | low | high | none |  | Boolean toggle stored in gSettings. Uses static return on error. Can add json: {"schema":1,"success":tru… |
| 83 | 1 | `cameraquality` | /sensors | setter | low | high | none |  | JPEG quality (0-63, inverted: lower=better). Can be applied live without restart. Uses static buffe… |
| 84 | 1 | `camerarecord` | /sensors | action | low | high | small |  | Control + status command. Static buffer (out[160]) only used in stop case, not shared. Can return {… |
| 85 | 1 | `camerarotate` | /sensors | setter | low | high | none |  | Compound setter (sets both hmirror+vflip). Can emit {"schema":1,"success":true\|false,"enabled":bool,"hm… |
| 86 | 1 | `camerasave` | /sensors | action | low | high | small |  | Captures and saves frame to storage (LittleFS/SD/Both per setting). Uses static buffer result[128].… |
| 87 | 1 | `cameravflip` | /sensors | setter | low | high | none |  | Boolean toggle. Same pattern as camerahmirror. JSON conversion is straightforward. |
| 88 | 1 | `closeble` | /bluetooth | action | low | high | none |  | Alias for cmd_blestop (line 1984). Deinitializes BLE stack. Returns simple ack. Could adopt JSON: {… |
| 89 | 1 | `closeespnow` | /espnow | action | low | high | small |  | Deinitialize ESP-NOW subsystem (stops tasks, clears state). Synchronous. Small output. JSON: {"sche… |
| 90 | 1 | `closefmradio` | /sensors | action | low | high | none |  | Handler: cmd_fmradio_stop() at line 509-521. Action only—synchronous stop, calls fmRadioDeinit() to… |
| 91 | 1 | `closeg2` | /bluetooth | action | low | high | none |  | Parses optional 'full' arg. Disconnects and optionally frees GATT cache. Returns different ack per… |
| 92 | 1 | `debugimudata` | /games | setter | low | high | small |  | This command is referenced but not yet implemented. Web page calls it with 'debugimudata 0' and 'de… |
| 93 | 1 | `debugsensorsgeneral` | /games | setter | low | high | small |  | This command is referenced but not yet implemented. Web page calls it with 'debugsensorsgeneral 0'… |
| 94 | 1 | `ei` | /sensors | list | low | high | small |  | Multi-subcommand router; main handler outputs help text (~200 bytes). Subcommands are: enable, dete… |
| 95 | 1 | `ei continuous` | /sensors | action | low | high | small |  | Control command (starts/stops a task, no data output). JSON mode: return {"schema":1,"success":true,"run… |
| 96 | 1 | `ei model` | /sensors | list | low | high | small |  | Router for model subcommands. JSON mode: return subcommand list as JSON array or structured help ob… |
| 97 | 1 | `ei model info` | /sensors | data-read | low | high | small |  | Static data read. JSON mode: {"schema":1,"success":true,"path":str,"sizeBytes":int,"input":{"width":int,… |
| 98 | 1 | `ei status` | /sensors | data-read | low | high | small |  | Data-read command outputting static+setting values, no unbounded data. JSON mode: return {"schema":… |
| 99 | 1 | `ei track` | /sensors | list | low | high | small |  | Router for tracking subcommands. JSON mode: return subcommand list. |
| 100 | 1 | `ei track status` | /sensors | data-read | low | high | small |  | Already has buildStateChangeJson() function that generates JSON for tracked objects. JSON mode: cal… |
| 101 | 1 | `espnowbackupenable` | /espnow | action | low | high | small |  | Toggle backup master feature (boolean). Uses getDebugBuffer() on get; inline string on set. Small o… |
| 102 | 1 | `espnowbroadcast` | /espnow | action | low | high | small |  | One-way broadcast via v4_broadcast_text(). No JSON mode. Returns simple ack (device count or error)… |
| 103 | 1 | `espnowfriendlyname` | /espnow | action | low | high | small |  | Get/set/clear friendly name (max 47 chars). Uses metaGetSet() helper with getDebugBuffer(). Small o… |
| 104 | 1 | `espnowmeshbackup` | /espnow | action | low | high | small |  | Get/set backup MAC for mesh (similar to meshmaster, uses BROADCAST_PRINTF). Synchronous. JSON: {"sc… |
| 105 | 1 | `espnowmeshes` | /espnow | already-json | low | high | small | ✅ | Multi-subcommand: 'espnowmeshes list' (text), 'espnowmeshes listjson' (JSON data-read), 'add/remove… |
| 106 | 1 | `espnowmeshmaster` | /espnow | action | low | high | small |  | Get/set master MAC for mesh (no dedicated buffer — uses BROADCAST_PRINTF to OLED/serial). Synchrono… |
| 107 | 1 | `espnowmeshtopo` | /espnow | action | low | high | none |  | Async action command (initiates, doesn't return results). Populates gTopoRequestId and calls reques… |
| 108 | 1 | `espnowpair` | /espnow | action | low | high | small |  | Synchronous device pairing (unencrypted). No JSON mode. Uses static getDebugBuffer(). Small output.… |
| 109 | 1 | `espnowpairsecure` | /espnow | action | low | high | small |  | Secure pair + KEY_EX initiate. No JSON mode. Uses static getDebugBuffer(). Synchronous local add; a… |
| 110 | 1 | `espnowrequestmeta` | /espnow | action | low | high | none |  | Action command (async, fire-and-forget). Calls requestMetadata(targetMac, true) and returns text ac… |
| 111 | 1 | `espnowroom` | /espnow | action | low | high | small |  | Get/set/clear room (max 31 chars). Uses metaGetSet(). Small output. JSON: {"schema":1,"success":true,"ro… |
| 112 | 1 | `espnowsendfile` | /espnow | action | low | high | small |  | File validation (size cap ~128MB) + async file send via v3 protocol. No JSON mode yet. Uses static… |
| 113 | 1 | `espnowsensorstream` | /espnow | action | low | high | small |  | Enable/disable sensor data stream from worker to master (11 sensor types: thermal, tof, imu, gps, i… |
| 114 | 1 | `espnowsetname` | /espnow | action | low | high | small |  | Get/set device name (max 20 chars, alphanumeric + - _). No JSON mode. Uses static getDebugBuffer().… |
| 115 | 1 | `espnowsetpassphrase` | /espnow | action | low | high | small |  | Set/clear mesh encryption passphrase (8-128 chars, mesh-specific). Synchronous. Uses static getDebu… |
| 116 | 1 | `espnowstationary` | /espnow | action | low | high | small |  | Get/set stationary boolean flag. No JSON mode. Uses getDebugBuffer(). Small output. JSON: {"schema"… |
| 117 | 1 | `espnowtags` | /espnow | action | low | high | small |  | Get/set/clear tags (max 63 chars, comma-separated). Uses metaGetSet(). Small output. JSON: {"schema… |
| 118 | 1 | `espnowunpair` | /espnow | action | low | high | small |  | Synchronous unpair + crypto identity cleanup. No JSON mode. Uses static buffers (errBuf or getDebug… |
| 119 | 1 | `espnowusersync` | /espnow | action | low | high | small |  | Toggle user credential sync across devices (boolean). Uses getDebugBuffer(). Small output. JSON: {"… |
| 120 | 1 | `espnowzone` | /espnow | action | low | high | small |  | Get/set/clear zone (max 31 chars). Uses metaGetSet(). Small output. JSON: {"schema":1,"success":true,"zo… |
| 121 | 1 | `filecreate` | /files,/cli | action | low | high | small |  | Creates an empty file. Uses getDebugBuffer() (shared static 1KB buffer) for output, snprintf'd with… |
| 122 | 1 | `filerename` | /files,/cli | action | low | high | small |  | Renames a file from oldPath to newPath (newName is a bare filename, not a full path). Uses getDebug… |
| 123 | 1 | `fmradiomute` | /sensors | action | low | high | none |  | Handler: cmd_fmradio_mute() at line 648-668 (shared for both fmradiomute and fmradiounmute commands… |
| 124 | 1 | `fmradioseek` | /sensors | action | low | high | none |  | Handler: cmd_fmradio_seek() at line 569-610. Parses direction [up\|down], defaults to up. Calls rad… |
| 125 | 1 | `fmradiotune` | /sensors | action | low | high | none |  | Handler: cmd_fmradio_tune() at line 523-567. Parses freq argument (accepts '103.9' or '10390' forma… |
| 126 | 1 | `fmradiovolume` | /sensors | action | low | high | none |  | Handler: cmd_fmradio_volume() at line 612-646. Parses volume 0-15. If no arg, reads current volume… |
| 127 | 1 | `g2ai` | /bluetooth | action | low | high | none |  | Front-pane AI card pipeline. Takes text arg. Returns result ack. JSON: {schema:1,success:true\|false,err… |
| 128 | 1 | `g2battery` | /bluetooth | data-read | low | high | small |  | Kicks async battery request, returns cached values. Static 64-byte buffer. JSON: {schema:1,success:true,… |
| 129 | 1 | `g2mic` | /bluetooth | action | low | high | none |  | Parses <on\|off\|start>. Builds AudioCtrl command, sends to both temples. JSON: {schema:1,success:true\|… |
| 130 | 1 | `g2notify` | /bluetooth | action | low | high | none |  | Parses optional [<seconds>] <text>. Calls g2ShowNotification(). JSON: {schema:1,success:true\|false,erro… |
| 131 | 1 | `g2reopen` | /bluetooth | action | low | high | none |  | Re-opens hijacked Blocks app. Checks right temple connected + not pluginDead, then dispatches handl… |
| 132 | 1 | `g2status` | /bluetooth | data-read | low | high | small |  | Uses static 256-byte buffer (EXT_RAM_BSS_ATTR). Handler calls getG2Status() which formats a fixed-s… |
| 133 | 1 | `ledbrightness` | /settings | setter | low | high | small |  | Handler uses shared static gDebugBuffer (1024 bytes, single instance per system). No concurrent ree… |
| 134 | 1 | `ledcolor` | /settings | setter | low | high | small |  | Handler uses shared gDebugBuffer. Single argument (color name string). Output is < 100 bytes. Error… |
| 135 | 1 | `mapload` | /maps | action | low | high | small |  | Handler uses shared static buffer via getDebugBuffer() (1024 bytes). Loads map file from path argum… |
| 136 | 1 | `micdelete` | /sensors | action | low | high | none |  | Handler cmd_micdelete (line 878) is multi-subcommand ('all' vs quoted filename). Returns text via g… |
| 137 | 1 | `micrecord` | /sensors | action | low | high | none |  | Handler cmd_micrecord (line 798) is multi-subcommand (no args=status, 'start'/'1', 'stop'/'0'). Ret… |
| 138 | 1 | `mkdir` | /files,/cli | action | low | high | small |  | Creates a directory (idempotent — existing dir is success). Uses getDebugBuffer() for all responses… |
| 139 | 1 | `mqttclientenabled` | /mqtt | setter | low | high | small |  | Stateful query/setter command. Query path (no args) returns setting status. Set path (args='0'\|'1'… |
| 140 | 1 | `openble` | /bluetooth | action | low | high | none |  | Alias for cmd_blestart (line 1983). Initializes BLE stack and starts advertising. Returns simple ac… |
| 141 | 1 | `opencamera` | /sensors | action | low | high | none |  | Initialization command. Returns simple static string. Easy to add json mode: {"schema":1,"success":true\… |
| 142 | 1 | `openespnow` | /espnow | action | low | high | small |  | Initialize ESP-NOW subsystem (radio + state struct). Synchronous for serial/OLED/BLE; can be deferr… |
| 143 | 1 | `openfmradio` | /sensors | action | low | high | none |  | Handler: cmd_fmradio_start() at line 488-507. Action only—no data returned; queues async initializa… |
| 144 | 1 | `openg2` | /bluetooth | action | low | high | none |  | Parses optional [left\|right\|auto] arg. Initiates async connect task. Returns textual ack or error… |
| 145 | 1 | `opengps` | /sensors | action | low | high | small |  | Start handler for PA1010D GPS. Uses i2cPingAddress() with bus parameter. Uses ensureDebugBuffer()/g… |
| 146 | 1 | `openimu` | /sensors | action | low | high | small |  | Start/enqueue handler for BNO055 IMU. Uses i2cPingAddress() check, calls enqueueDeviceStart(). Sync… |
| 147 | 1 | `openinput` | /sensors | action | low | high | small |  | Start handler, delegates to cmd_sensorstart_queued() which uses getDebugBuffer()/ensureDebugBuffer(… |
| 148 | 1 | `openmqtt` | /mqtt | action | low | high | small |  | Stateless action command. On error path (line 1139), uses shared getDebugBuffer(1024) to format sta… |
| 149 | 1 | `openthermal` | /sensors | action | low | high | small |  | Start handler for MLX90640 thermal sensor. Same pattern as openimu. Uses enqueueDeviceStart(). Outp… |
| 150 | 1 | `opentof` | /sensors | action | low | high | small |  | Start handler for VL53L4CX ToF sensor. Identical pattern to openimu/openthermal. Uses enqueueDevice… |
| 151 | 1 | `ringconnect` | /bluetooth | action | low | high | none |  | Optional [mac] arg for direct BLE connect without scan. Calls g2RingConnect() or g2RingConnectMac()… |
| 152 | 1 | `rmdir` | /files,/cli | action | low | high | small |  | Removes an empty directory. Uses getDebugBuffer() (1KB static buffer) via snprintf. Reentrancy: sha… |
| 153 | 1 | `savesettings` | /settings,/dashboard | action | low | high | medium |  | Clears gDeferWrites and calls writeSettingsJson(). Output is simple success/failure. JSON wrapper t… |
| 154 | 1 | `sessionrevoke` | /settings | action | low | high | none |  | State-changing: terminates active sessions (web SID or transport-level). Multi-subcommand (sid\|use… |
| 155 | 1 | `srstart` | /speech | action | low | high | small |  | Starts ESP-SR pipeline and arms voice if auth context permits. Returns success status with optional… |
| 156 | 1 | `srtuning` | /speech | data-read | low | high | medium |  | Query-only status command, read-only. Formats tuning parameter snapshot. Buffer size 520 bytes is f… |
| 157 | 1 | `unbanuser` | /settings | action | low | high | none |  | State-changing: removes 'banned' + 'banReason' fields from users.json entry. Returns text ack via s… |
| 158 | 1 | `useradd` | /settings | action | low | high | none |  | State-changing: creates new user in users.json + per-user settings file. Returns text ack. To add J… |
| 159 | 1 | `userapprove` | /settings | action | low | high | none |  | State-changing: moves user from pending_users.json to users.json. Creates per-user settings file. R… |
| 160 | 1 | `userdemote` | /settings | action | low | high | none |  | State-changing: downgrades user role from 'admin' to 'user' in users.json. Calls demoteUserFromAdmi… |
| 161 | 1 | `userdeny` | /settings | action | low | high | none |  | State-changing: removes user from pending_users.json. Returns text ack via getDebugBuffer(). Wrappa… |
| 162 | 1 | `userpromote` | /settings | action | low | high | none |  | State-changing: elevates user role to 'admin' in users.json. Calls promoteUserToAdminInternal(). Re… |
| 163 | 1 | `userresetpassword` | /settings | action | low | high | none |  | State-changing: resets password in per-user settings, revokes all sessions for target user. Returns… |
| 164 | 1 | `usersync` | /settings | action | low | high | none |  | State-changing/async: syncs user account to remote device via ESP-NOW. Returns text ack on sender;… |
| 165 | 1 | `wifiadd` | /settings | action | low | high | small |  | Mutating action: adds WiFi network to in-memory array (gWifiNetworks) and persists to settings.json… |
| 166 | 1 | `wifidisconnect` | /settings | action | low | high | none |  | Stateful action (disconnects WiFi, optionally stops HTTP server). Handler builds text response in g… |
| 167 | 2 | `bleautoreconnect` | /bluetooth | status | medium | high | small |  | Multi-peer command: bleautoreconnect <peer-name> [on\|off]. No arg forwards to cmd_blepeers. Uses sta… |
| 168 | 2 | `blemode` | /bluetooth | status | medium | high | small |  | Gets/sets BLE mode (server vs G2 client). Currently returns plain text only. Uses shared debug buff… |
| 169 | 2 | `blestream` | /bluetooth | status | medium | high | small |  | Multi-subcommand: no-arg (show status), 'on'/'off' (toggle all streams), 'sensors'/'system'/'events… |
| 170 | 2 | `bondrole` | /bond | setter | medium | high | small |  | Mutation: parses master\|worker\|0\|1 argument, sets bondRole via setSetting(), resets handshake if… |
| 171 | 2 | `bondstream` | /bond | status | medium | high | small |  | Command has TWO paths: (1) No-args STATUS: broadcasts ~10 lines via broadcastOutput() queue (bond s… |
| 172 | 2 | `imustart` | /games | action | medium | high | small |  | Handler calls enqueueDeviceStart() to queue the IMU sensor for initialization. Current output is pl… |
| 173 | 2 | `sensorlog` | /logging | data-read | medium | high | medium |  | Multi-subcommand handler. Output goes to static 1024-byte debug buffer (shared with other CLI comma… |
| 174 | 2 | `srdyngain` | /speech | data-read | medium | high | small |  | Multi-key config query + setter. Subcommands: on\|off\|min X\|max X\|target X\|alpha X\|reset. No a… |
| 175 | 2 | `wificonnect` | /settings | action | medium | high | small |  | Long-running action (connection timeout ~12 seconds in connectWiFiIndex). Handler does broadcastOut… |
| 176 | 3 | `certgen` | /settings,/dashboard | action | medium | medium | small |  | Long-running (30-60s for RSA, ~1s for ECDSA P-256). Uses shared gDebugBuffer (1024 bytes) for retur… |
| 177 | 3 | `ei model list` | /sensors | list | medium | medium | large-unbounded |  | ISSUE: Unbounded payload. If many models in /EI Models/, output can exceed 512B static buffer, caus… |
| 178 | 3 | `espnowtoporesults` | /espnow | data-read | medium | medium | large-unbounded |  | Large unbounded output: builds text from gTopoResultsBuffer (a global String accumulating peer list… |
| 179 | 3 | `filedelete` | /files,/cli | action | medium | medium | small |  | Multi-path delete: (1) interactive confirm mode via cliRequestConfirm() for stateful console (captu… |
| 180 | 3 | `g2bmp` | /bluetooth | action | medium | medium | medium |  | Loads BMP from VFS (heap-allocated), applies tuning, pushes multi-fragment. Static 220-byte return… |
| 181 | 3 | `ledeffect` | /settings | action | medium | medium | small |  | Handler uses gDebugBuffer. Complex multi-arg parsing: effect type, 2 optional colors, duration (100… |
| 182 | 3 | `srautotune` | /speech | action | medium | medium | medium |  | State machine: start -> cycles through gain configs -> stop. Subcommands: start\|stop\|status. Retu… |
| 183 | 3 | `userdelete` | /settings | action | medium | medium | none |  | State-changing with two-step confirmation flow (prompt → yes/no). Deletes user from users.json, rev… |
| 184 | 4 | `automation` | /automations | action | high | medium | large-unbounded |  | Multi-subcommand handler (9 subcommands: system, list, add, enable, disable, delete, sanitize, reco… |
| 185 | 4 | `log` | /logging | data-read | high | medium | medium |  | NOT found in System_SensorLogging.cpp. Task description mentions 'log' as a logging command but it… |
| 186 | 5 | `dashboard` | /settings,/dashboard | list | high | low | large-unbounded |  | NOT A CLI COMMAND. 'dashboard' is a web page (/dashboard) served by a dedicated HTTP handler, not a… · retires /dashboard (dedicated HTTP handler) |

