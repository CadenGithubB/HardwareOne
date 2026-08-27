# Native G2 Notifications (EFS / `android_notification`) — Implementation Plan

Status: **SOLVED / native card renders on HW (2026-07-21).** Phase 1 send path works once the
notification subsystem is primed on **sid 0x04** (`g2notifenable`: NotificationControl enable +
NotificationWhitelistCtrl whitelistDisable). Root cause: the per-app notification whitelist is
enforced-by-default and was empty (glasses never configured by the official Even app), so the
firmware filtered every card *before* parsing the EFS file — the zero-RX/no-`START_ERR` symptom. See
§12.10. NEXT: auto-wire the sid-0x04 priming into `runSessionPrelude`, then Phase 2 (`NSINK_G2` sink).
Phases 2–3 (pipeline sink + settings) still PLAN.
Goal: push **true native notification cards** to the Even Realities G2 lens — the same
card the official Even app produces — instead of the current full-screen lens hijack.

Companion docs:
- [NOTIFICATION_EVENT_INTEGRATION_PLAN.md](NOTIFICATION_EVENT_INTEGRATION_PLAN.md) — this is the
  concrete mechanism for that plan's **Phase 3 "G2 allowlist" sink** (its open item #3).
- [G2_PROTOCOL.md](G2_PROTOCOL.md) — the transport/envelope reference this builds on.
- `docs/g2_proto/efs_transmit.proto` — the vendored EFS enums (bodies were missing; now
  reconstructed below).
- `docs/FlutterApp-main/lib/src/services/g2_manager.dart::sendAndroidNotification` +
  `.../core/crc.dart` — the authoritative reference implementation this plan mirrors.

---

## 1. Why this matters / what "native" buys us

Today `g2notify` / `g2ShowNotification` is a **self-described placeholder**
([G2_Glasses.cpp:13337](../components/hardwareone/G2_Glasses.cpp)): *"NOT a real overlay — it
uses the full-screen text path, so it wipes whatever is currently on the lens. Replace the
implementation once the JSON-over-EFS notification protocol is reversed."* And the device
event/notification pipeline (`systemEventsNotifyTick`) has **no G2 sink at all** — nothing
from the event system ever reaches the lens.

The G2 **base firmware** has a first-class notification subsystem. Pushing a small JSON blob
over the **Even File Service (EFS)** makes the firmware:
- render its **own** notification card (native look, correct fonts/layout),
- **auto-wake** the display,
- **respect the user's silent / DND** state on its own,
- overlay the card **without destroying** whatever page is currently shown.

We already speak the entire transport it rides on. This is the truest form of "use the base
firmware for the ESP32's functions."

---

## 2. Wire format (fully specified + locally validated)

The send is **4 frames, fire-and-forget** (the reference app's ack-wait code is all commented
out — it does not wait for `eEvenFileServiceRsp`). All 4 ride the **standard `AA 21` envelope**
we already build; the EFS body is **raw bytes, not protobuf**.

**SIDs** (already defined in `System_G2_Protocol.h`):
- `G2_SID_FILE_CMD  = 0xC4` — file-send command channel (host→device)
- `G2_SID_FILE_RAW  = 0xC5` — file-send raw-data channel (host→device)

**Flag byte: `0x00`** on all four frames (`reserveFlag=false` in the reference), **NOT** the
`0x20` (`G2_FLAG_REQUEST`) that the image/EvenCore paths use. ⚠️ Copy-pasting from
`sendImageBmp*` would send the wrong flag.

### Frame sequence (send to the RIGHT temple `gR` only)

| # | Channel (sid) | Payload |
|---|---|---|
| 1 | `0xC4` CMD | **START** — 77 bytes (layout below) |
| 2 | `0xC4` CMD | **DATA-announce** — single byte `[0x01]` |
| 3 | `0xC5` RAW | **RAW** — the notification JSON, UTF-8 (fragmented if >232 B) |
| 4 | `0xC4` CMD | **RESULT_CHECK** — single byte `[0x02]` |

### START payload layout (77 bytes)

```
offset size field
0      1    sub-cmd = 0x00  (EVEN_FILE_SERVICE_CMD_SEND_START)
1      4    fileType   u32 LE = 1  (ANDROID_MSG_JSON_NOTIFICATION)
5      4    dataLength u32 LE = exact byte count of the RAW JSON
9      4    fileDataCrc32 u32 LE  (see §3)
13     64   path, UTF-8, zero-padded to 64 bytes = "user/notify_whitelist.json"
```

⚠️ The reference uses the literal path `"user/notify_whitelist.json"` **even for a
notification** (fileType=1). Replicate verbatim. `dataLength` MUST equal the exact byte count
of the JSON used for both the CRC and the RAW frame — a mismatch aborts the transfer silently.

### The JSON body (`android_notification`)

```json
{"android_notification":{"msg_id":<int++>,"action":0,"app_identifier":"<pkg>","title":"<title>","subtitle":"<subtitle>","message":"<body>","time_s":<unixSeconds>,"date":"<YYYYMMDDThhmmss local>","display_name":"<app name>"}}
```

- Compact (no whitespace), field order as above.
- `msg_id` — monotonic counter (see open decisions re: reboot persistence).
- `action` — always `0`.
- `date` — **local** time, compact `strftime("%Y%m%dT%H%M%S")`. Note: existing
  `Clock::formatISO8601Local` (dashes+colons) and `formatFilenameLocal` (underscores) do **not**
  match — a one-line `strftime` is needed here.
- `time_s` — unix seconds (local epoch as the reference computes it).

### No prerequisite

`sendAndroidNotification` sends **only** these 4 frames — no service-0x02 `NotificationControl`,
no `displayWake`, no whitelist push. The 4 frames alone display the card. (A one-time
`NotificationControl`/whitelist enable is kept as a **named fallback hook** in case HW testing
shows DND swallows it — see §6.)

---

## 3. CRC — reuse `r1Crc32` (already in the tree, validated)

The START frame's `fileDataCrc32` is **CRC-32C, poly `0x1EDC6F41`, init `0`, MSB-first, no
final XOR** — *not* the standard reflected CRC-32C, and *not* `esp_crc32_le`.

**We already have an exact, byte-for-byte port**: `r1Crc32()`
([System_R1_Protocol.cpp:82](../components/hardwareone/System_R1_Protocol.cpp), decl
`System_R1_Protocol.h:176`) — its comment states it is a direct port of the same
`docs/FlutterApp-main/lib/src/core/crc.dart::fileDataCrc32`. It sits behind the **identical
compile gate** (`#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES`) as `System_G2_Protocol.cpp`, so it
is **always linked whenever G2 is enabled** — the earlier "board-gated" concern does not apply.

**Usage:** `uint32_t crc = r1Crc32((const uint8_t*)json, jsonLen);` then serialize **little-endian**
into START offset 9 (r1Crc32 returns host-order).

**Golden vector (validated locally, must be unit-tested before HW):**

```
payload = {"android_notification":{"msg_id":1,"action":0,"app_identifier":"com.google.android.apps.dynamite","title":"Kimberly Cummins","subtitle":"","message":"Bubeeee","time_s":1773627504,"date":"20260315T221824","display_name":"Chat"}}
r1Crc32(payload) == 1910672472   (0x71E28C58)
```

Recommended cleanup (optional, low priority): hoist `r1Crc32` to a neutral name
(`crc32c()` in `System_G2_Protocol.{h,cpp}`) so the G2 path doesn't reach into the R1 header and
a future R1 refactor can't silently break it.

---

## 4. Architecture / data flow

Two entry points, one worker-side sender. **Nothing sends BLE inline** — the 4-frame burst
holds the write mutex + inter-frame delays, so it must run on the lens-applier worker, never on
the main loop or the BLE notify task.

```
 (A) event pipeline (the real payoff)          (B) direct (CLI / web / test)
 subsystem → systemEventPost(kind,…)            cmd_g2nativenotify  /  web btn
   → 48-slot event ring                              │
   → systemEventsNotifyTick()  [MAIN LOOP]           │
       ├ resolve G2 viewer once (g2HijackAuthContext().user → notifViewerResolve)
       ├ notifRuleForViewer(kind,v).sinks & NSINK_G2 ?
       ├ notifG2FieldsForEvent(e) → {app,display,title,subtitle,body}
       └ heap LensUiJob{NativeNotif, NativeNotifSpec} ─┐   ┌─ g2SendNativeNotificationAsync(...)
                                                       ▼   ▼
                                          g2EnqueueLensJob()  (depth-8 gPageSwapQueue)
                                                       │
                                                       ▼
                                   g2_page_swap_w worker  (pageSwapWorkerLoop)
                                     case LensJobKind::NativeNotif:
                                       g2SendNativeNotification(spec…)
                                         → g2BuildNotifyJson → r1Crc32
                                         → 4 EFS frames on gR (0xC4/0xC5, flag 0x00)
                                             → sendEnvelope / sendPbFragmented
                                                 → firmware EFS reassembler
                                                     → SVC_ANDROID_ParseNotification
                                                         → native card + auto-wake + DND
```

---

## 5. Reused framework (honor the existing G2 stack — do not reinvent)

| Function | File | Role in EFS path |
|---|---|---|
| `r1Crc32` | System_R1_Protocol.cpp:82 | `fileDataCrc32` for the START frame (see §3) |
| `g2BuildEnvelope` | System_G2_Protocol.cpp (decl .h:212) | Build frames 1/2/4 and frame 3 when ≤232 B |
| `sendEnvelope` / `sendEnvelopeNoMutex` | G2_Glasses.cpp:6877 / 6772 | Ship single-fragment frames to one temple |
| `sendPbFragmented` | G2_Glasses.cpp:6945 | Ship frame 3 (RAW JSON) when >232 B; sid/flag are params → pass `0xC5`/`0x00` |
| `allocSeq` | G2_Glasses.cpp:1009 | One fresh seq per frame (4 total) |
| `g2EnqueueLensJob` | G2_Glasses.cpp:9966 (decl G2_HijackCmd.h:240) | Move the blocking send onto the worker |
| `pageSwapWorkerLoop` (`g2_page_swap_w`) | G2_Glasses.cpp:9799 | The worker; `LensJobKind::Toast` at :9880 is a stub slot |
| `g2statsRecordTx` / `sidStatFind` | System_G2_Protocol.cpp:1275/1264 | Auto-tracks 0xC4/0xC5 on first TX — `g2protostats` shows counts free |
| `notifDeviceRuleFor` / `notifRuleForViewer` / `notifViewerResolve` | System_Notifications.cpp:263/273/229 | The full gating stack — `NSINK_G2` reuses it unchanged |
| `notifFormatEvent` | System_Notifications.cpp:455 | Produces the card **body** (reuse, don't duplicate) |
| `systemEventFamilyName` / `systemEventKindFamily` / `systemEventKindName` | System_Events.cpp | Derive **title** + synthetic **app_identifier** |
| `g2HijackAuthContext` | G2_HijackCmd.cpp:177 | Resolves the G2 **viewer identity** (glasses' `pairedByUser`) for mute/admin gating |
| `handleSettingCommand` + `SETTING_EDITOR_CMD` | System_Settings.cpp:2395/2528 | Real per-setting save command (no auto-register) |

---

## 6. New functions

| Function | File | Purpose |
|---|---|---|
| `g2SendNativeNotification(appId, displayName, title, subtitle, body)` | G2_Glasses.cpp (static; worker-only) | The blocking 4-frame EFS orchestrator. Build JSON → `r1Crc32` → START(77B) → DATA `[0x01]` → RAW → RESULT `[0x02]`, all on `gR`, flag `0x00`, distinct `allocSeq` per frame, small inter-frame `vTaskDelay`. **Must NOT call `noteOurShutdownSent()`** (no ShutdownPage is sent). |
| `g2BuildNotifyJson(…)` | G2_Glasses.cpp (static) | Serialize the exact `android_notification` schema (monotonic `msg_id`, `time_s`, `strftime %Y%m%dT%H%M%S`). Must reproduce the golden CRC. |
| `g2SendNativeNotificationAsync(appId, displayName, title, subtitle, body)` | G2_Glasses.cpp (public, G2_Glasses.h) | Callable from any context (CLI/web/event sink). Allocates `NativeNotifSpec`+`LensUiJob`, `g2EnqueueLensJob`; frees both on enqueue failure. |
| `NativeNotifSpec` + `LensJobKind::NativeNotif` | G2_HijackCmd.h (struct/enum/union) + worker switch | Typed carrier for the strings across the queue (`CustomSpec::run` is `void(*)()` — can't carry data). |
| `notifG2FieldsForEvent(e, app,display,title,subtitle,body)` | System_Notifications.cpp (static) | One `SystemEvent` → the 4 native strings: body=`notifFormatEvent`, title=`systemEventFamilyName`, app/display=synthetic per-family package, subtitle=`e.who`. |
| `NSINK_G2 = 1<<3` | System_Notifications.h:27 | New sink bit; slots into `NotifRule.sinks` so the whole gating stack applies to the lens. |
| `g2IsSilentMode()` *(optional)* | G2_Glasses.cpp (exposes `gSilentMode`) | Belt-and-suspenders bandwidth skip when the glasses report DND. **Not** required (firmware honors its own DND); beware `gSilentMode == -1` early-session. |
| `g2EnableNativeNotify()` *(fallback only)* | G2_Glasses.cpp | One-time-per-connect `NotificationControl`/whitelist push. Ship **without** it; hook at connect (§ below) only if HW shows the 4 frames alone don't display. |

---

## 7. Changed functions

| Function | File | Change |
|---|---|---|
| `pageSwapWorkerLoop` | G2_Glasses.cpp:9799 | Add `case LensJobKind::NativeNotif:` → run `g2SendNativeNotification`, delete spec. **No gen-guard** (a native card isn't tied to a hijack view generation). The `Toast` stub at :9880 is the natural slot / sibling. |
| `LensJobKind` enum + `LensUiJob` union | G2_HijackCmd.h:203-224 | Add `NativeNotif` enumerator + `NativeNotifSpec* nativeNotify`. |
| `systemEventsNotifyTick` | System_Notifications.cpp:585 | (1) widen the early-out mask at :615 to include `NSINK_G2`; (2) add an `NSINK_G2` branch: cheap-gate on `g2BothConnected()` **and** `!hijack-active` (`g2GetHijackPage`), resolve the G2 viewer once, build fields, enqueue a `LensUiJob`; (3) keep the 10 s staleness gate (don't push boot backlog); (4) add a per-kind/global **cooldown** so the lens isn't spammed; (5) add `g2Pushed/g2Filtered/g2Dropped` to `NotifPipeStats` + `cmd_notifstats`. |
| `notifDefaultRuleFor` | System_Notifications.cpp:50 | OR `NSINK_G2` into a **curated allowlist** of glanceable kinds only (Phase-3 open item #3) — **not** blanket-added. |
| `notifDeviceRuleFor` | System_Notifications.cpp:263 | If a `notifG2` master is adopted: `if (!gSettings.notifG2) r.sinks &= ~NSINK_G2;` |
| `struct SystemSettings` + ctor | System_Settings.h (~:868 / ~:309) | Add `bool g2NativeNotif;` default `false`. |
| SettingEntry table + `settingEditorCommands[]` | System_Settings.cpp:2555 (+ a settings module row) | `SETTING_EDITOR_CMD(cmd_set_g2nativenotif, "g2nativenotif")` + row → persistence + `setting_changed` + web schema, no auto-register. |
| CLI command table (`cliCommands[]`) | G2_Glasses.cpp:13657 | Add `{ "g2nativenotify", …, cmd_g2nativenotify, … }` beside `g2notify`. |
| `cmd_g2notify` comment / `g2ShowNotification` | G2_Glasses.cpp:13337 / 11980 | Update the "once the protocol is reversed" comment; **optionally** (later) re-point `g2ShowNotification` at the async native path. |
| WebPage_Bluetooth.h G2 panel | :360-370 / :960-974 | Optional parity: native-notify toggle + test button; the `g2NativeNotif` setting auto-appears in `/api/settings`. |

---

## 8. Downstream effects

1. **`systemEventsNotifyTick` gains a per-loop branch.** Must cheap-gate on `g2BothConnected()`
   first so absent glasses cost nothing (no viewer resolve, no JSON build).
2. **Second producer into the depth-8 `gPageSwapQueue`.** Notifications now compete with
   interactive page-swaps for 8 slots and one worker. A notif storm could delay tap-driven
   navigation (or vice-versa). Consider a per-kind cooldown / drop-oldest for notif jobs, or a
   dedicated low-priority queue if it bites.
3. **`NSINK_G2` on a kind means that kind is now also gated by the G2 owner's per-user mutes +
   device admin/off policy** — desirable, but editing the shared allowlist mask changes three
   surfaces (banner/toast/G2) at once.
4. **`g2statsRecordTx` auto-tracks 0xC4/0xC5** on first TX — `g2protostats` starts showing counts
   with zero new code (both sids already named).
5. **Registering the `g2NativeNotif` SettingEntry** auto-exposes it on `/api/settings`, the bond
   schema (`sendBondSchema`), and the OLED/G2 settings-inspector — a togglable row everywhere for
   free. Verify that's desired.
6. **`setting_changed` fires** when `g2nativenotif` is saved — automations keying on it will see
   it (harmless).
7. **Do not entangle with `gNotifyGen` / `LensJobKind::Notify`** (the existing full-screen
   overlay's auto-clear). Native EFS cards are fire-and-forget; the firmware renders and dismisses
   them itself — no clear-timer job.

---

## 9. Risks & the hardware-validation checklist

Highest-risk items first. All "⚠️ HW" items must be confirmed on a real unit before wiring the
pipeline.

1. ⚠️ **HW — Does the auto-wake card clobber an active hijack page?** When the firmware overlays
   its native card, the *likely* events are `FG_ENTER`/`FG_EXIT` on sid `0xE0`, already handled
   benignly (they only refresh `gHijackStartedMs` / set `gFirmwareOverlayUp` —
   G2_Glasses.cpp:2595-2609). **But** if it also emits `DISPLAY_OFF` (:2965) or `SYSTEM_EXIT`
   etype 7 (:2744) while we are **not** in an expected-echo window, our handlers treat it as real
   teardown and **clobber the hijack** (invalidate `containerReady`, clear overlay,
   `sendHijackShutdown`, post `SYSEVT_G2_HIJACK_EXITED`, stop live-text workers). *Mitigation if it
   fires:* a **new** native-notify echo-suppression window (distinct from `ourShutdownEchoActive()`
   — different semantics: we did **not** shut down), consulted in the `DISPLAY_OFF`/`SYSTEM_EXIT`
   branches. Observe first with `g2protostats` / a ring dump around a live send while a hijack page
   is up.
2. ⚠️ **HW — Frame contiguity.** `sendEnvelope`/`sendPbFragmented` each **release** the write mutex
   between logical sends, so a concurrent heartbeat / camera stream / hijack image push could
   interleave a frame mid-sequence and break EFS reassembly. *Mitigation:* a burst-scoped wrapper
   that holds `t.writeMutex` across all 4 frames, **or** send only in a quiescent window (gate on
   `!hijack-active`) and accept occasional loss (fire-and-forget tolerates it). Confirm whether the
   firmware even requires contiguity.
3. ⚠️ **HW — Flag byte / no-prerequisite / target arm.** Confirm flag `0x00` is honored (vs `0x20`),
   that the 4 frames alone display (no whitelist push), and that `gR`-only is correct (left temple
   is silent on async events — G2_Glasses.cpp:674/1297).
4. **Golden-CRC fragility.** `r1Crc32` is correct **only** if `g2BuildNotifyJson` reproduces the
   exact bytes (field order, no whitespace, escaping, `YYYYMMDDThhmmss`, integer formatting).
   **Unit-test `g2BuildNotifyJson` + `r1Crc32` against `1910672472` before wiring send.**
5. **START layout / endianness.** Path zero-padded to exactly 64 B; START totals 77 B;
   `fileType`/`dataLength`/`crc` are u32 **LE**; `dataLength` == exact JSON byte count.
6. **Do not carry `noteOurShutdownSent()`** over from the image path — EFS sends no ShutdownPage;
   calling it would falsely drive the hijack FSM (`HijackEvent::ShutdownSent`).
7. **Main-loop blocking.** The sink MUST enqueue, never send inline (write mutex + inter-frame
   delays would stall the system tens-to-hundreds of ms).
8. **Clock validity.** `time_s`/`date` need a synced clock; on `SYSEVT_RTC_POWER_LOSS` / pre-time-sync
   boot the fields are bogus. Decide whether to suppress native notifs until time is valid.

---

## 10. Recommended phasing

**Phase 1 — Standalone send path + CLI test (isolated, low-risk). ✅ IMPLEMENTED 2026-07-21.**
`g2SendNativeNotification` + `g2BuildNotifyJson` + `g2JsonAppendEscaped` +
`g2SendNativeNotificationAsync` + `NativeNotifSpec`/`LensJobKind::NativeNotif` (worker case) +
`cmd_g2nativenotify` (admin-only). CRC reuses `r1Crc32` via a local extern decl. Compile-verified
(`-Werror`). **This is the HW-validation vehicle** for every ⚠️ item in §9 — does the card display,
flag `0x00`, no prerequisite, which arm, and the echo/contiguity behavior. No pipeline wiring yet.

HW test steps:
1. `g2nativenotify selftest` → expect `CRC OK (1910672472)` (proves the CRC on-device).
2. Connect glasses, `g2nativenotify Test|Hello from HardwareOne` → watch for a native card.
3. Open a hijack page (e.g. `g2sensors`), fire a notify, and watch whether the card **clobbers**
   the page (the §9.1 `DISPLAY_OFF`/`SYSTEM_EXIT` echo risk) — `g2protostats` shows the sid traffic.

**Phase 2 — Wire into the event pipeline (the payoff, = the other plan's Phase 3).**
`NSINK_G2` + `notifG2FieldsForEvent` + the `systemEventsNotifyTick` branch + the curated per-kind
allowlist + `gSettings.g2NativeNotif` master + `notifstats` counters + cooldown. Now ESP32 events
surface as native cards, gated by per-user mutes and device policy.

**Phase 3 — Consolidation (optional).** Re-point `g2ShowNotification`/`g2notify` at the native
path; web UI parity; hoist `r1Crc32` → shared `crc32c()`.

---

## 11. Open decisions (need your call)

1. **Per-kind G2 allowlist defaults** (Phase-3 open item #3): recommend a small curated set —
   `mesh text_rx`, peer/bond online-offline, `battery_low/critical`, `login_fail`, `user_request` —
   rather than mirroring banner/toast.
2. **`app_identifier` scheme:** synthetic per-family package (`one.hardware.mesh` → firmware groups
   by icon/family) vs a single `one.hardware` with family in title/display_name.
3. **`msg_id` persistence:** session counter (resets on reboot) vs persisted monotonic (avoids
   firmware dedup collisions after reboot-and-resume).
4. **Auth tier for `g2nativenotify`:** match `g2notify` (`requiresAdmin=false`) or admin-gate it
   (it renders a trusted-looking card). Guests already blocked from G2 mutation.
5. **Settings module home** for `g2NativeNotif`: the camera/image group (where `g2StreamToneMap`
   lives) vs a bluetooth/G2 module (more semantically correct — affects which JSON file it persists
   to).
6. **Suppress vs defer** native notifs during an active hijack session (suppression is simpler and
   matches "transient sink"; deferral needs a holding buffer).
7. **Contiguity mitigation** (from §9.2): add the burst-scoped mutex wrapper up front, or ship
   without and rely on quiescent-window gating + fire-and-forget tolerance.

---

## 12. Hardware test log & findings (2026-07-21)

**Status: BLOCKED on the firmware side.** The ESP32 send path is complete and correct; the G2
firmware (v**2.2.4.342**) does not render the card and never engages its file service. Below is
everything established, so this isn't re-derived.

### 12.1 Test rig
- Board: FeatherS3 (ESP32-S3), our firmware v0.99.0.
- Glasses: `Even G2_32` (L=`dc:d3:41:4f:5a:a0`, R=`c8:8d:65:00:97:69`), **firmware 2.2.4.342**
  (read live from sid 0x09 — newer than every RE source we have: 2.0.x–2.2.0.24).
- Debug: `debugg2 1` + `debugg2dump 1`. Diagnostics via `g2protostats verbose` + the `[G2-DUMP]`
  ring.

### 12.2 CONFIRMED WORKING (ESP32 side — the whole client is correct)
- **CRC**: `g2nativenotify selftest` → `CRC OK (1910672472)`. `r1Crc32` is the exact EFS
  `fileDataCrc32` on-device. Golden vector holds.
- **Frames**: all 4 go out on the RIGHT temple (`gR`), flag `0x00`, `ok=1`. Byte-verified in the
  `[G2-DUMP]` ring against the reference:
  - START `sid=C4 len=79` pb=77 `[00 01 00 00 00 D5 00 00 00 F8 62 94 …]` = subcmd `00`,
    fileType `01` (u32le=1), dataLen `0xD5`=213, crc `F8 62 94 AE`=`0xAE9462F8`, 64-byte path.
  - DATA `sid=C4 len=3` `[01]` · RAW `sid=C5` (`{"android_no…`) · RESULT `sid=C4 len=3` `[02]`.
- **SIDs**: `UX_EVEN_FILE_SERVICE_CMD_SEND_ID = 196 = 0xC4`, `RAW = 197 = 0xC5` (verified against
  the reference's own `service_id_def.pbenum.dart`). Correct.
- **Full FlutterApp connect handshake replicated & ACKED by the firmware** (via `g2devcfg`):
  - `AUTHENTICATION` (sid 0x80 cmd 4) → `secAuth=1` on both temples.
  - `PIPE_ROLE_CHANGE(RIGHT)` (sid 0x80 cmd 5) → `ack` (bytes match reference `buildPipeRoleChangeRight`).
  - `TIME_SYNC` (sid 0x80 cmd 128) → `ack` (with a valid NTP-synced clock).

### 12.3 CONFIRMED NOT WORKING (firmware side)
- **`0xC4`/`0xC5` get ZERO RX** — no ack, no `eEvenFileServiceRsp` error code — while the firmware
  actively replies on **every other** service in the same session: `0x80` (auth/pipe/time/ring
  polls), `0xE0` (heartbeat acks), `0x09` (battery/version), `0x01` (app-launch/gesture), `0x0D`
  (state events). The EFS handler simply never engages.
- **No card ever renders**, and no `DISPLAY_OFF`/`SYSTEM_EXIT` is triggered *by our send* (the
  `SYSTEM_EXIT` seen earlier was the Apps hijack menu exiting, unrelated). So §9.1's echo risk never
  even came into play.

### 12.4 RULED OUT (things that are NOT the cause)
| Hypothesis | Verdict |
|---|---|
| Frame/envelope framing | ❌ byte-identical to reference (envelope, `status=0x00`, CRC-in-length, 1-based serial, consecutive syncId) |
| Flag byte (`0x00` vs `0x20`) | ❌ reference uses `reserveFlag:false` → `0x00`; we match |
| Wrong characteristic (EFS pipe 7401?) | ❌ reference sends EFS on the same `…5401` write char as everything else |
| Wrong SIDs | ❌ 196/197 confirmed |
| Missing authentication | ❌ `secAuth=1` achieved; still inert |
| Missing `PipeRoleChange` (pipe theory) | ❌ acked; still inert — **disproves the "EFS pipe dormant" theory** |
| Missing `TimeSync` / bad clock | ❌ NTP-synced, time-sync acked; still inert |
| Wear / hijack / screen-off display state | ❌ tried worn, not-worn, in-hijack, out-of-hijack, screen-off |
| Frame contiguity (mutex released between frames) | ❌ observed the 4 frames going out back-to-back with no interleave; still inert |
| CRC / dataLength mismatch | ❌ internally consistent; selftest proves the algorithm |

### 12.5 KEY REALIZATIONS
1. **The reference (`docs/FlutterApp-main`) is unvalidated community RE and likely INCOMPLETE.** Its
   entire notification mechanism is *only* the 4 EFS frames. It **defines** `NotificationControl`
   (`notifEnable`, `autoDispEnable`, `dispTime`, `avoidDisturbEnable`) and `NotificationWhitelistCtrl`
   protobufs on the notification service (**sid 0x04 = `UI_FOREGROUND_NOTIFICATION_ID`**) — but
   **never sends them**. The real Even app almost certainly configures these during first-time setup,
   and that state persists on the glasses. We never send them either.
2. **The reference's EFS ack-waiting is entirely commented out** → the firmware may not ack this flow
   at all, so "zero RX" is *ambiguous*: it does not prove the transfer failed. It could be
   succeeding but not *displaying* (a whitelist / notif-enable / feature-off gate), OR failing
   silently.
3. **Firmware 2.2.4.342 is newer than all RE.** The EFS notification path may have changed or been
   gated in this version.
4. The path field is the odd `user/notify_whitelist.json` even for a notification (fileType=1) — the
   reference hardcodes it; unclear if the firmware routes by fileType (ignoring path) or if the
   whitelist file must be populated first.

### 12.6 REMAINING HYPOTHESES (untested)
- **H1 — Notifications disabled by default.** Our glasses were (likely) never fully configured by the
  official app, so the notification subsystem may be off. Fix: send `NotificationControl{notifEnable=1,
  autoDispEnable=1}` on **sid 0x04** once. *Needs a new builder.*
- **H2 — Empty per-app whitelist.** `one.hardware` isn't allowed. Fix: push a
  `NOTIFICATION_JSON_WHITELIST` (fileType=0) file listing the app first. *Needs code.*
- **H3 — Firmware 2.2.4.342 changed the format.** Only a fresh capture would reveal it.

### 12.7 THREE PATHS FORWARD
1. **Capture the official Even app** pushing a real notification on 2.2.4.342 (Android HCI btsnoop /
   nRF sniffer) → replicate confirmed-working bytes. **Definitive; ends the guessing.** Pivotal
   precondition: confirm notifications actually work via the official app on these glasses at all.
2. **Blind code experiment**: add H1 (`NotificationControl` enable, sid 0x04) + H2 (whitelist push,
   fileType=0) before the send. One reflash, tests both untried knobs. Still a guess.
3. **Ship the EvenAI-card fallback** (the original "Option A"): route notifications to the HW-proven
   EvenAI front-pane card (`g2ShowEvenAIReply`, sid 0x07). Not "native" but puts notifications on the
   lens today; revisit native EFS later.

### 12.8 Reproduction (from a fresh connect)
```
login <user> <pass>
debugg2 1
debugg2dump 1
g2devcfg auth          # → secAuth=1
g2devcfg role right    # → PIPE_ROLE_CHANGE ack
g2devcfg time          # → TIME_SYNC ack  (needs a valid clock: wifiadd + openwifi first for NTP)
g2nativenotify Test|Hello       # frames go out ok=1; firmware silent on 0xC4/0xC5; no card
g2protostats verbose            # 0xC4 TX=N RX=0, 0xC5 TX=N RX=0
```
`g2nativenotify selftest` validates the CRC without sending (expect `CRC OK (1910672472)`).

### 12.9 Experiment implemented 2026-07-21 — sid-0x04 notification enable + whitelist bypass
Motivated by two up-to-date sources agreeing there is NO app-side native step we're missing:
Commute773/g2-r1-re-tools-and-guide (proto/g2/notification.proto) AND the refreshed FlutterApp-main
(which pushes native notifications byte-identically to us and *also* never sends NotificationControl/
whitelist — but adds a "Replace native notifications" mode that renders via an EvenHub plugin instead).
The lone untried native lever is the notification CONTROL service on **sid 0x04** (which we've never
written). `NotificationWhitelistCtrl` carries only a `whitelistDisable` toggle (no app list) → the
per-app allow-list is enforced-by-default and lives in a separate fileType=0 EFS file; an
unconfigured device likely filters every card. Build OK (full ESP-IDF, `-Werror`), not HW-tested.

New code:
- `g2BuildNotifCtrlEnable` + `g2BuildNotifWhitelistDisable` (System_G2_Protocol.{h,cpp}) — sid 0x04
  `NotificationDataPackage`, flag 0x20, same `g2Pb*` pattern as the DevCfg builders.
- `cmd_g2notifenable` CLI (admin) — sends NOTIF_CTRL{notifEnable=1,autoDispEnable=1} then
  WHITELIST_CTRL{whitelistDisable=1} to the right arm.
- `G2_SID_NOTIFICATION = 0x04` + `G2_NOTIF_*` constants.

Test: `g2devcfg auth` → `g2notifenable` (**watch for a sid=0x04 RX / NOTIFICATION_COMM_RSP — an ack
there, where 0xC4/0xC5 was silent, is the tell**) → `g2nativenotify Test|Hello`. If a card renders or
0xC4/0xC5 finally acks, the empty-whitelist theory is confirmed and we fold these into
`runSessionPrelude`. If still silent → native EFS is a firmware/environment dead end on 2.2.4.342;
pivot to the EvenHub-rendered path (Option A), now validated by FlutterApp's `EvenHubNotificationsPlugin`.

### 12.10 SOLVED 2026-07-21 — sid-0x04 priming was the gate
`g2notifenable` fired on sid 0x04 and, for the first time, the firmware **answered** where
`0xC4/0xC5` was always silent:
```
TX sid=04 cmd=1 magic=226 [08 01 10 E2 01 1A 04 08 01 10 01]  → NotifEnable=1/AutoDisp=1
RX sid=04 cmd=1 magic=226 [08 01 10 E2 01 1A 04 08 01 10 01]  ✅ ACK (flag 0x00)
TX sid=04 cmd=3 magic=227 [08 03 10 E3 01 32 02 08 01]        → WhitelistDisable=1
RX sid=04 cmd=3 magic=227 [08 03 10 E3 01 32 02 08 01]        ✅ ACK
```
Then `g2nativenotify Test|Hello` → the identical 4 EFS frames → **native card rendered on the lens**
("HardwareOne … Test / Hello", timestamped). Confirmed root cause: the notification **whitelist is
enforced-by-default and empty** on a device never configured by the official app, so the firmware
dropped every card before parsing EFS. `whitelistDisable=1` (turn filtering off) + `notifEnable=1`
opened the gate — the one lever neither the reference app nor we had ever pulled.

Productionization:
1. **Auth is NOT required.** HW-confirmed 2026-07-21: `g2notifenable` alone (no `g2devcfg auth`)
   opened the gate — sid-0x04 acks, then the card rendered. So we prime with just
   `notifEnable + whitelistDisable` and avoid the auth-triggered BLE re-bond/disconnect entirely.
2. **DONE — auto-wired into connect.** `g2AutoNotifPrimeIfReady(t)` (G2_Glasses.cpp, right arm only)
   sends `g2BuildNotifCtrlEnable` + `g2BuildNotifWhitelistDisable` once per connect, right after
   `g2AutoTimeSyncIfReady` in the connect flow. Native notifications now work with zero manual steps:
   just connect → `g2nativenotify <title>|<body>` (or, soon, the event pipeline). Full build OK.
   `g2notifenable` CLI kept as a manual re-arm / diagnostic.
3. **Phase 2 — DONE (built, awaiting HW test), see §13.**

---

## 13. Phase 2 IMPLEMENTED 2026-07-21 — event pipeline → native G2 cards (`NSINK_G2`)

The device's own event/notification pipeline now emits native G2 cards, sharing the exact same
gating stack as the OLED banner and web toast sinks. Built OK (full ESP-IDF, `-Werror`); not yet
HW-tested.

**New sink bit.** `NSINK_G2 = 1<<3` (System_Notifications.h) slots into `NotifRule.sinks` alongside
BANNER/QUEUE/TOAST, so the whole four-layer rule stack (compiled default → device off/admin policy →
sink masters → per-user mutes) applies to the lens with no new gating logic.

**Curated allowlist (per-kind opt-in).** `notifG2GlanceableKind()` grants `NSINK_G2` to a tight,
glanceable set — `TEXT_RX, FILE_RX, PEER_ONLINE/OFFLINE, BOND_ONLINE/OFFLINE, BATTERY_LOW/CRITICAL,
LOGIN_FAIL` — layered in inside `notifDeviceRuleFor` *before* the off/master strips. Everything else
stays OLED/web/queue only. (Device policy can only remove/restrict a kind, never add it to the lens,
so the on-lens set is curated in code by design.)

**Master toggle.** `gSettings.notifG2` (default ON) mirrors `notifBanners/Toasts/Queue`: a
`SETTING_BOOL` entry in `notifSettingEntries` + real per-setting command `notifydeviceg2 <0|1>`
(`SETTING_EDITOR_CMD`, no auto-register) that strips `NSINK_G2` in `notifDeviceRuleFor`.

**The sink branch** (`systemEventsNotifyTick`, gated `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES`):
cheap `isG2Connected()` gate → resolves the G2 viewer once per tick from `g2HijackAuthContext().user`
(the glasses' paired owner, so admin-level kinds + that user's mutes gate the lens) → for a passing
event, `g2SendNativeNotificationAsync("one.hardware", "HardwareOne", <family name>, "", <one-liner>)`.
Title = event family (`systemEventFamilyName`), body = the shared `notifFormatEvent` line (no
duplication). Fire-and-forget enqueue onto the lens worker — never sends BLE inline; honors the same
10s staleness gate + per-kind cooldowns as banner/toast. The sid-0x04 priming (§12.10, auto on
connect) is the per-connect setup; this EFS push is the per-event action.

**Stats.** `NotifPipeStats` gains `g2Pushed / g2Filtered / g2Dropped`, surfaced in the `[NOTIF]`
debug line (`debugnotifications 1`) and `notifstats`.

Files: System_Notifications.{h,cpp}, System_Settings.{h,cpp}.

HW test: connect glasses (auto-primes), then trigger a glanceable event — e.g. a failed login
(`login x y`) → `SYSEVT_LOGIN_FAIL`, or a mesh `TEXT_RX` from another node — and watch for a native
card. `debugnotifications 1` shows `g2=N`; `notifstats` shows `g2-cards`. `notifydeviceg2 0/1`
toggles the whole sink; `notifyusermute` / `notifydevicekind` gate individual kinds.

---

## 14. Phase 2b — importance tiers + per-user level (supersedes §13's G2 allowlist)

Reworked the preference model so it's **uniform per-user across every interface** and the glasses
aren't special-cased. The hardcoded G2 "glanceable allowlist" (§13) is **removed**. Built OK; not
HW-tested.

**Two axes, cleanly separated.**
- *Permission* (role) = what you're **allowed** to see — unchanged (admin-mask + `viewer.isAdmin`).
- *Preference* (per-user) = what you **want** to see — now a single importance floor + per-kind
  overrides, applied identically on OLED banner, web toast, and G2.

**Cross-cutting importance tiers** (`notifKindTier`, orthogonal to family):
- **Alert** — security/safety/faults (battery_critical, thermal, login_fail/locked, banned, crash,
  storage_formatted, factory_reset, config/auth/secret faults). Always interrupts.
- **Standard** *(default floor)* — presence (peer/bond), inbound (text/file), wifi/ble up-down,
  battery_low, service faults. The everyday useful set.
- **Verbose** — chatty/info (setting_changed, sensor start/stop, wifi_net_added, usb, login_ok,
  gestures, battery_full). History-only unless opted in.

**Per-user controls (uniform on all interfaces):**
- `notifylevel <verbose|standard|alert>` — the floor. **Interrupt surfaces (banner/toast/G2) fire at
  this tier and above.** Default **standard**. Lower tiers still land in the notification-center
  history — nothing is lost, it just doesn't pop.
- `notifyusershow <kinds|none>` — **force-on**: interrupt me for these even below my floor.
- `notifyusermute <kinds|none>` — **force-off** (existing): never notify me of these, any surface.
- Precedence: mute (off) > force-on > tier floor. Stored per-user (`notifyLevel` /
  `notificationForced` / `notificationMuted`), resolved once per tick via the existing prefs cache.

**G2 is now just an interrupt surface.** `notifDeviceRuleFor` grants `NSINK_G2` wherever a kind
already has an OLED banner or web toast, so banner/toast/G2 fire on the same kinds; the per-user
floor curates all three together. Device master `notifydeviceg2 <0|1>` still turns the whole lens
sink off.

**⚠️ Behavior change beyond G2:** the importance floor now gates the OLED banner and web toast too
(that's the "uniform" part). By default, **Verbose kinds no longer pop a banner/toast** — they go to
history only. A user who wants the old firehose sets `notifylevel verbose`.

Files: System_Notifications.{h,cpp}, System_Settings.cpp.

HW test: `notifylevel` (show default=standard); trigger an Alert (`login x y` → login_fail) → pops on
OLED + web + glasses; trigger a Verbose kind (change a setting) → history only, no pop; `notifylevel
verbose` → verbose kinds pop again; `notifyusershow setting_changed` → that one pops even at standard;
`notifyusermute peer_online` → never. Uniform for the same user on every interface.

### 14a. Fix — force-on now promotes queue-only kinds (HW gap found 2026-07-21)

`notifyusershow setting_changed` did nothing because `SYSEVT_SETTING_CHANGED` is compiled
`{NSINK_QUEUE}` (queue-only, no interrupt sink — it fires on every config write). Force-on as first
built only lifted the tier *floor*; it couldn't grant a kind an interrupt sink it never had, and the
tick's per-sink branches gated on the *device* rule, so the branch never even entered.

Fix: force-on now **promotes** any real notification kind to every device-enabled interrupt surface.
- `notifRuleForViewer`: `forcedOn → r.sinks |= notifDeviceInterruptSinks()` (interrupt sinks minus
  device masters). Event-only / device-off kinds (NONE) still can't be resurrected.
- Tick: early-out relaxed to `r.sinks == NSINK_NONE` (any notification kind proceeds); each surface
  now decides via `notifRuleForViewer(kind, viewer)` (not the device rule), so force-on is honored on
  banner/toast/G2. Toast broadcasts whenever the toast master is on and the per-session predicate
  decides. `bannersFiltered`/`g2Filtered` only count when the device offered the sink but the viewer
  denied it. Built OK.

Note: force-on is per-user + uniform, so `notifyusershow setting_changed` shows it on **OLED + web +
glasses**, rate-limited by the kind's 1.5s cooldown. It's a deliberate max-visibility override.

---

## 15. OLED banner queue + faster scroll (2026-07-21)

Notification *retention* is already system-wide (48-deep event ring + notification-center history) —
so the only real gap was the OLED, whose banner is a **single animated ribbon** (`gOledPairingRibbon`)
that back-to-back notifications clobbered. Web toasts stack client-side; G2 cards are firmware-managed;
the companion app is **not a notification sink** (it gets CLI output via `MSG_ROUTE_BLE`/`outble`, not
the pipeline) and would stack client-side anyway — so the fix is OLED-local, not system-wide.

Changes (OLED_UI.cpp):
1. **Faster marquee scroll.** `RIBBON_SCROLL_PX_PER_FRAME = 2` (was 1) → ~20 px/s at the panel's ~10 FPS;
   the visible-duration budget is divided by the same constant, so long banners also *clear ~2x sooner*.
   Tunable (bump to 3 for faster). Applied at both scroll-timing sites + the scroll step.
2. **Banner queue (depth 12).** `oledNotificationBannerShow()` enqueues when the ribbon is busy (a prior
   banner still animating, or a pairing ribbon) instead of clobbering; `oledBannerQueuePump()` shows the
   next the moment the ribbon returns to idle (called from `oledPairingRibbonUpdate`). Overflow drops the
   **oldest pending** (record still in history). FIFO ring, `sBannerQueue[12]`.

Net: bursts of different-kind notifications now display one-by-one at full duration, faster per banner.
Same-kind dedup still handled upstream by the per-kind cooldown. Built OK.

Possible follow-up (not built): add a real `NSINK_APP` sink so the companion app receives notifications
through the same tier/mute pipeline (it would stack client-side, so still no on-device queue needed).
