# Routing MQTT + Voice command execution through `cmd_exec` — change & impact report

**Date:** 2026-07-24 · **Status:** ✅ IMPLEMENTED — built green (feathers3/esp32s3), uncommitted, awaiting HW.
Edits landed exactly as described in §2: `System_ESPSR.cpp` `executeVoiceCommandAsArmedUser` (body only,
3 call sites untouched), `System_MQTT.cpp` local command path (static `mqtt.cmdResult` buffer removed),
`System_CommandTypes.h` enum (`ORIGIN_MQTT`/`ORIGIN_VOICE`) + `HardwareOne.cpp:759` audit switch. Defaults
chosen: **sync** (not async) for MQTT, `outputMask = MSG_ROUTE_FILE`, `captureOutput = false`. The read-only
httpd status helpers were left direct by design (§5).

## 1. Goal

Make **all mutating command execution single-threaded** by routing the two remaining
direct `executeCommand()` callers — MQTT and Voice — through `submitAndExecuteSync()`
(the `cmd_exec_task` queue) that web / OLED / serial / ESP-NOW / G2 already use.

**Why:** `executeCommand()` takes **no lock** (`System_Utils.cpp:4440`). Command
serialization exists *only* because the main interfaces funnel through the single-threaded
`cmd_exec_task`. MQTT (`System_MQTT.cpp:483`) and Voice (`System_ESPSR.cpp:195`) run
**arbitrary, mutating** commands directly on their own tasks, so an MQTT/voice command can
execute concurrently with a CLI command and race global state (`gSettings`, wifi, the
function-static response buffers) with nothing serializing them.

## 2. What changes (three edits)

### 2a. `System_ESPSR.cpp` — `executeVoiceCommandAsArmedUser()` (~line 180)
Body only; **signature unchanged**, so the 3 call sites (`:986`, `:1067`, `:1103`) are untouched.

- **Before:** builds a `SOURCE_VOICE` `AuthContext`, calls `executeCommand(vctx, cliCmd, out, outSize)` **on the SR task**.
- **After:** builds a `Command` (`origin = ORIGIN_VOICE`, `auth = vctx`, `outputMask = MSG_ROUTE_FILE`, `captureOutput = false`), calls `submitAndExecuteSync(uc, s)`, copies `s` into `out`. Execution now happens **on `cmd_exec_task`**.

### 2b. `System_MQTT.cpp` — local command path (`:465`–`:498`)
The `#if ENABLE_ESPNOW` mesh-routing block above it (`room:`/`tag:`/`device:` → `cmd_espnow_*`) is **unchanged**.

- **Before:** `SOURCE_MQTT` `AuthContext` → `executeCommand(ctx, command, cmdResult, CMD_RESULT_MAX)` **on the esp-mqtt event task**, result JSON-published.
- **After:** build a `Command` (`origin = ORIGIN_MQTT`, `auth = ctx`, `outputMask = MSG_ROUTE_FILE`, `captureOutput = false`), `submitAndExecuteSync(uc, resultStr)`, publish `resultStr`. The static `cmdResult` PSRAM buffer is removed (result comes back as a `String`).

### 2c. `System_CommandTypes.h` + `HardwareOne.cpp` — two new audit origins
Add `ORIGIN_MQTT` and `ORIGIN_VOICE` to the `CommandOrigin` enum and two `case`s to the
**only** switch that reads it (`HardwareOne.cpp:759`, source-label for the command feed):
`ORIGIN_MQTT → "mqtt"`, `ORIGIN_VOICE → "voice"`. Mirrors how `ORIGIN_ESPNOW` /
`ORIGIN_LOCAL_DISPLAY` were previously split out for audit attribution. `ctx.origin` is
read nowhere else (verified), so this is label-only and cannot affect control flow.

## 3. Downstream impacts

| Area | Impact | Assessment |
|---|---|---|
| **Concurrency (the point)** | MQTT/voice commands now serialize on `cmd_exec_task` with every other command. No two command handlers can run concurrently anymore. | ✅ The fix. Eliminates the state + static-buffer race. |
| **Task stacks** | Commands run on `cmd_exec`'s purpose-sized **8 KB** stack instead of the esp-mqtt / SR task stacks. | ✅ Removes a latent stack-overflow surface on those tasks; may allow shrinking them later (more internal DRAM). |
| **Latency / serialization** | An MQTT/voice command waits for `cmd_exec` to be free — behind any in-flight CLI command (e.g. PBKDF2 ~12 s). | ⚠️ Behavior change. Arguably correct (commands *should* serialize), but a busy CLI now delays MQTT/voice. |
| **New timeouts** | `submitAndExecuteSync` enforces a **2 s enqueue** timeout (queue full → `"[ERROR] Command queue full"`) and a **60 s execution** timeout (→ `"[ERROR] Command timed out"`, `cmd_exec` finishes + cleans up safely). Today MQTT/voice have *no* timeout. | ⚠️ A >60 s command now returns an error to MQTT/voice while it keeps running. No command today runs >60 s except PBKDF2 (~12 s). |
| **MQTT event task blocking** | The esp-mqtt event task blocks on the semaphore for the command duration — **it already blocks the same duration today** (synchronous `executeCommand`). Now *bounded* at 60 s. | ✅ No regression; slightly better (bounded). Async is a future option (§5). |
| **Output routing** | Sets `outputMask = MSG_ROUTE_FILE` → the command + output are written to the file log/feed, consistent with every other interface. Today the direct path logs receipt (`SYSEVT_REMOTE_CMD_RX`) but not the output line. `captureOutput = false` → the response is the command's return value, exactly as today. | ⚠️ Minor, deliberate: output is now file-logged. If you want zero broadcast, set `outputMask = 0`. |
| **Audit trail** | Command feed now attributes MQTT→`"mqtt"` and voice→`"voice"` instead of an unlabelled/`system` default. | ✅ More accurate. |
| **Identity / auth** | Unchanged. `AuthContext.transport` (`SOURCE_MQTT`/`SOURCE_VOICE`) still drives `CommandIdentityScope` + notification source inside `executeCommand`. `authorizeCommand` runs identically. | ✅ No change. |
| **PSRAM allocation** | Each MQTT/voice command now allocates an ~8 KB `ExecReq` from PSRAM for its duration (freed after). Removes the two static PSRAM result buffers (`mqtt.cmdResult`, `sr.cmdOut`). | ✅ Net neutral/positive on PSRAM; PSRAM is abundant. |
| **Mesh routing (MQTT `target:`)** | The `room:`/`tag:`/`device:` path is **not touched** — it already goes through `cmd_espnow_*`, not local `executeCommand`. | ✅ Out of scope, unchanged. |

## 4. Risks & mitigations

- **Deadlock:** `submitAndExecuteSync` blocks the caller waiting for `cmd_exec`. Safe here —
  MQTT command exec is triggered by an inbound broker message and voice by recognized speech;
  **neither is ever reached from inside a command handler**, so a handler on `cmd_exec` cannot
  be waiting on the task it is running on. (Verified: the 3 voice call sites are in the SR
  recognition FSM; the MQTT site is in `mqtt_event_handler`.)
- **esp-mqtt keepalive:** blocking the event task ≤60 s is within default keepalive; and it is
  no worse than today's unbounded synchronous block. If it ever bites, switch MQTT to
  `submitCommandAsync` (§5) so the event task never blocks.
- **Queue contention:** the 2 s enqueue timeout could reject an MQTT/voice command if `cmd_exec`
  is saturated. In practice the queue is short and commands are brief; the failure is loud
  (`"queue full"`) not silent.

## 5. Considered but NOT doing now

- **Async MQTT** (`submitCommandAsync` + a callback that publishes the response): the MQTT event
  task would never block. Cleaner, but more code (capture `responseTopic`/`user`/`cmd` in
  `userData`, manage its lifetime). Recommended as a follow-up if event-task blocking proves
  a problem; sync first keeps the change minimal and behavior-preserving.
- **Routing the read-only httpd status helpers** (`runInternalStatusCmd`, BT/ESPNow status)
  through `cmd_exec`: deliberately left direct — the dashboard polls them every ~1 s and
  queuing them behind a long CLI command would freeze the UI. They are read-only, so their
  only exposure is output-text races on shared static read buffers (low severity).

## 6. Test plan (HW)

1. MQTT command round-trips: send a read command and a mutating command (`set …`), confirm the
   JSON response still returns and the change applies.
2. Voice single-stage command still executes and the `[Voice] OK` banner/notification fires.
3. Concurrency: issue an MQTT command while a slow CLI command runs on the web CLI — confirm
   they serialize (no interleaved/garbled output) and both complete.
4. Long command: confirm a PBKDF2-class command over MQTT completes (<60 s) rather than timing out.
5. Command feed shows `mqtt` / `voice` source labels.
