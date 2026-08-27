# Controls Write-Integrity Audit — Android app × firmware settings contract

> Investigation of what actually happens for **every** settings-module entry the Android BLE app can
> write through its generic controls renderer. 407 entries across 34 modules traced end-to-end
> (app editor → wire line → `findCommand` → handler parse → store), each verdict adversarially
> re-verified against source. Per-entry matrix: [`controls-write-audit.csv`](controls-write-audit.csv).
> Companion overview: [`UI_PARITY_AUDIT.html`](UI_PARITY_AUDIT.html) (Android column).
> Audited 2026-07-21 at the app working tree + firmware HEAD.

## 1. Verdict

**276 of 407 writable entries (68%) are broken; 123 (30%) work.** The app's Device Settings /
Sensors / Camera / Mic tuning cards are, outside the sensor modules, largely a display-only UI that
*looks* writable:

| Verdict | Count | Meaning |
|---|---|---|
| WORKS | 82 | verb matched, token accepted, value applied + persisted |
| WORKS_QUIRK | 41 | applies, with a caveat (admin gate, locale float, reboot-required, …) |
| **DEAD** | **243** | no registered verb matches the jsonKey → `Unknown command`, nothing written |
| **WRONG_VALUE** | **23** | handler runs but writes **false** for both toggle directions (`on` parses as 0) |
| **MISFIRE** | **6** | the jsonKey matches a **different feature's** verb — a real command fires |
| REJECTED | 3 | handler errors on the raw select token (`0:error`, `1:Master (…)`) |
| MANGLED | 1 | raw option token stored verbatim (`systemLogFlags`) |
| APP_READONLY | 8 | 6 intended (secrets/diagnostics); **2 unintended** (`maps.zoom`, `llm.temperature` — `min=max=0` suppresses the slider) |

Every audited module is user-reachable: 22 curated cards in DeviceSettingsScreen
(DeviceSettingsScreen.kt:59-80), a dynamic OTHER-group catch-all for anything else
(:114-117), 12 sensor modules on SensorsScreen panels, camera/microphone double-exposed.
Worst modules: **espnow 0/37 clean · bluetooth 0/6 · llm 0/16 (+1 unintended read-only) ·
edgeimpulse 0/7 · power 0/6 · notifications 0/3 · batteryLog 0/2 · debug 6 misfires + 151 dead**.
Cleanest: mqtt (15 WORKS), thermal (16 WORKS), tof (7/7), the sensor auto-start family.

## 2. Root cause — a four-part failure

1. **The contract lies.** `buildModuleControlsJson` emits only `o["key"] = e->jsonKey` under the
   comment *"also the set command (case-insensitive)"* (System_Settings.cpp:2634-2652). That claim
   is **false** for at least bluetooth, power, espnow, llm, edgeimpulse, maps, cli, microphone,
   anoencoder swaps, sensorlog, systemlog, notifications, batteryLog: their setters live under
   different cmdKey verbs — or under no verb at all. The web UI is unaffected because the
   settings-schema serializer *does* emit cmdKey (System_Settings.cpp:2767).
2. **The dispatcher prefix-matches.** `findCommand` (System_Command.cpp:~86-118) does
   case-insensitive longest-prefix matching over the whole line. A jsonKey that happens to equal any
   registered verb executes that verb — the MISFIRE class. (Same logic inlined on the BLE lane at
   System_Utils.cpp:4621.)
3. **Parsers are stricter than the app's tokens.** The app sends `on`/`off` for bools
   (ControlWidgets.kt:49) and the raw option token for selects (:135); `handleSettingCommand` BOOL
   parses only `1|true` (System_Settings.cpp:2436) and replies "Configuration updated" either way —
   `on` **silently writes false**. The debug sub-flag family parses via `argInt` — `on` reads 0.
   The i2c `toInt` handlers reject even `true`.
4. **Failure is invisible.** Errors surface only in the Console log; no settings screen observes
   `ble.incoming`; there are zero snackbars/toasts. ControlToggle has no local state (a failed write
   just never moves), and ControlSlider/ControlText are remember-keyed on the *fetched* value, so
   after a failed write the UI **keeps showing the user's unapplied value indefinitely** — it lies
   rather than snapping back. The 700 ms re-fetch (ConsoleViewModel.kt:1331-1334) swaps in the
   device snapshot with no diff or error check.

## 3. The six misfires (all in the Debug card, all one tap from Device Settings → System)

1. **`capture` — DANGEROUS.** The camera-group flag (System_Settings.cpp:1659) prefix-matches the
   image-manager verb `capture` (System_ImageManager.cpp:661, **non-admin**) → `cmd_capture`
   (:515-529) runs the shutter on any token, both toggle directions: `captureAndSave` **writes a real
   image to storage whenever the camera is open**, else returns a harmless `Capture failed` (:536-537).
   The debug flag is never written. Unintended shutter, storage growth, privacy exposure — the only
   side-effecting write in the audit. **Resolved by the systemic `cmd`-field fix (§5), not a per-row
   gate:** once routed, `capture` → `debugcameracapture` (an admin-gated flag setter) and the collision
   is gone with zero interface-specific code. The row stays live until that fix ships on firmware + app.
2. **`broadcast`** (sse group, :1597) → core verb `broadcast` (System_Utils.cpp:2584, admin):
   on an admin session, toggling spams the literal text "on"/"off" to every connected console.
3. **`login`** (auth group, :1583) → user-auth verb `login`: single token fails `hasMinArgs(2)` →
   usage error. Harmless *only* because the token is one word.
4. **`files`** (storage group, :1606) → admin filesystem verb `files`: rejects `on` as an unquoted
   path. Harmless.
5. + 6. **`events`** (sse group :1596 and g2 group :1763) → core verb `events`: dumps the
   system-event ring into the console. Read-only; toggle inert.

**Latent:** `llm.temperature` collides with the chip-temperature verb `temperature`
(System_Utils.cpp:2559) — masked today only because `min=max=0` renders no editor. If firmware adds
ranges before the `cmd` field ships, the write becomes a misfire.

## 4. Safe-token rules (verified to break zero currently-working entries)

1. **BOOL → lowercase `1` / `0`, never `true`.** Fixes all 23 WRONG_VALUE entries; every rated
   WORKS bool handler accepts `1/0`; the i2c `toInt` handlers reject `true`; `cmd_mqttsubscribe`
   rejects mixed case.
2. **SELECT → send the value-part** (strip a leading `<word>:` renderer prefix and a trailing
   `|label` / `:label` suffix; never offer `#|…` header rows). Fixes all 3 REJECTED entries
   (`logLevel`, `mqttTLSMode`, `bondRole`); value-part == whole token for the plain-name selects.
3. **FLOAT → format with `Locale.ROOT`.** `trimFloat` (ControlWidgets.kt:180) uses the device
   locale; on comma-decimal locales every float slider silently writes a truncated value that passes
   range checks. Applies to all float sliders, not just imu.
4. **Per-verb exceptions** (need a hand-map even after the `cmd` field lands):
   `batterylog` accepts **only** `on|off` (`1/0` = silent no-op) and its interval is **seconds**,
   not the ms the setting stores (÷1000); `automation system` accepts only `enable|disable`;
   `outserial` wants numeric `0/1`; `wifiautoreconnect` must never receive an empty arg (writes
   false); the power verbs accept the app's exact current tokens (labels included) once routed.

## 5. The fix — three coordinated parts

- **A. Firmware:** emit the entry's cmdKey in `controls json` — `if (e->cmdKey) o["cmd"] = e->cmdKey;`
  at System_Settings.cpp:~2652, mirroring the settings-schema serializer at :2767. Multi-word
  cmdKeys (`power mode`, `sensorlog autostart`, `log autostart`) must pass through verbatim. This is
  also the **only** disambiguator for the debug card, where ~34 distinct toggles currently send the
  identical line `enabled on`. Fix the false doc comment at :2634-2636 in the same change.
- **B. App:** prefer `cmd` over `key` in `setControl` (parse the new field in SensorControls.kt),
  apply the token rules above, and **surface failures**: watch the reply, show an error on the
  settings screens, and force re-key of slider/text state so a failed write visibly reverts.
- **C. Auth surfacing:** enforcement is real (authorizeCommand, System_Utils.cpp:4364 → 4233 →
  3225-3234; BLE lane Bluetooth.cpp:816-829) and most target verbs are admin (some superadmin:
  `serialrequireauth`, `displayrequireauth`, `blerequireauth`, `blesecret`, `espnowusersync`).
  A non-admin session currently fails invisibly; the same error-surfacing must show
  "admin required".

**Stays broken even after A+B+C — needs specific firmware work:**
`espnow.captureToSd` / `captureSkipHeartbeats` (cmdKey verbs not registered anywhere);
`systemLogPath` (no setter verb exists; needs a settingEditorCommands row);
`blename` / `bletxpower` (handler skips the first args word the dispatcher never passes —
single-token writes display-current instead of setting; handler bug);
`maps.zoom` + `llm.temperature` (declare real min/max — maps zoom's 0.5 floor doesn't fit the int
min field; and land the `cmd` field before ranging llm.temperature, see §3 latent);
`sensorLogMask` (single-select can't express a bitmask; needs a multi-select or the
`sensorlog sensors <names>` path); `systemLogFlags` (STRING stores the raw token; needs a
multi-select + canonical formatter). Correctly unwritable, leave alone: `bleSecureChannelSecret`
(BLE-refused by design), `mqttPassword`, `crashCount`, `lastResetReason`, `rtcTimeHasBeenSet`,
`wifi.enabled`.

**Conditional writes that work but need their errors surfaced:** `blesecure on` without a secret,
`ntpServer` (live 5 s DNS+NTP probe — offline rejects), `blemode` (client mode drops the app's own
link), `cameraFramesize` (up-to-60 s synchronous restart), 7 camera tuning handlers that error while
the camera is stopped, `eicontinuous` without a loaded model, and the reboot-required family
(i2c pins/buses, `httpsEnabled`, `oledHistorySize`, `anoEncoderI2cAddr`).

## 6. Provenance and confidence

71 agents: 2 extractors, 36 per-module verdict tracers, 32 adversarial verifiers, 1 critic.
Verification issued 12 corrections, 9 of which flipped verdicts — notably three debug entries the
bulk pass called DEAD that are actually MISFIREs (`events` ×2, `broadcast`), and the imu float
entries downgraded for the locale defect. Two "admin flag is unenforced" claims were **disproven**
(enforcement confirmed at System_Utils.cpp:3225-3234). Known residual: command-registry census
numbers vary by extraction pass (~850-870 single-word names) — no verdict depends on the census;
"snaps back" symptom wording in older notes is mechanically wrong (see §2.4); the per-entry CSV
carries the corrected verdicts.
