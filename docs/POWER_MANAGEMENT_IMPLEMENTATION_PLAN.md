# Workload-driven power management: implementation plan

**Status: phase 1 implemented 2026-10-26 (uncommitted). Host tests green;
firmware builds and the hardware checks in 5.8 pending. Phase 0 not started.**

Revised 2026-10-26. This replaces an earlier, larger draft from the same day.
The code below is the sketch the implementation followed; the real code is in
`System_Power.cpp` (`powerDecide`, `powerPolicyTick`) and `HardwareOne.cpp`
(`powerDisplayTick`). One change from the plan: Adaptive **replaced**
PowerSaver at mode index 2 instead of being added as a sixth mode. With the
busy-flag boost applying to every mode, PowerSaver and Adaptive would have
been identical (floor in use, floor idle, full speed when busy), and a sixth
mode brought the G2 row and JSON-buffer hazards in 5.3 for no behaviour. The
static asserts and the buffer measurement from 5.3 were added anyway.

The goal is a device that stays connected over Bluetooth all day, is used
now and then through the glasses or its own screen, and sometimes records or
transcribes a note. It must work on the P4X-EYE (radio on a separate C6 chip
over SDIO) and on boards with a built-in radio, such as the XIAO S3, FeatherS3
and classic ESP32 boards.

Background: [CROSS_BOARD_POWER_POLICY.md](CROSS_BOARD_POWER_POLICY.md) covers
radio and sleep differences between boards.
[P4_BATTERY_BUDGET.md](P4_BATTERY_BUDGET.md) covers battery sizing and the
measurement method.

## 1. Summary

- **Run at the lowest supported clock by default, and go to full speed only
  while heavy work is running.** Heavy work means transcription, camera, or
  recording.
- **Learn about heavy work from the busy flags those modules already
  publish.** Do not add a request or handle API. The power loop reads the flags
  on every pass. When a workload finishes, fails or is cancelled, its flag
  clears and the clock drops. There is nothing to release, so nothing can leak.
- **Separate the clock from the screen.** Commands from the glasses, phone or
  web stop lighting the local screen. Waking the screen stops restoring
  400 MHz.
- **Run the clock logic on every board, including boards with no screen.**
  Today it only runs in builds with a display.
- **Measure before any sleep work.** The CPU clock may not be the largest
  drain on the board. Sleep milestones start only after battery-side
  measurements show where the power goes.

Phase 1 is a small change. Phase 0, the measurements, runs alongside it.
Everything after that is chosen by what phase 0 finds.

## 2. What the code does today

All of this was checked against the source on 2026-10-26.

| Fact | Where | Consequence |
| --- | --- | --- |
| One function owns every clock change. It takes its lock without waiting and drains I2C buses first, waiting up to 2 s per bus. | `powerSetCpuFrequencyMhz` in [System_Power.cpp](../components/hardwareone/System_Power.cpp) | Keep it as the only clock writer. Contention is normal and brief, so callers must retry rather than fail. |
| Default mode is Performance (400 MHz on P4 rev3, 240 on S3/ESP32), with a 10-minute idle timeout. | `System_Settings.h` constructor (`powerMode(0)`, `powerSaveTimeoutMinutes(10)`) | The device runs at full speed whenever it isn't idle. |
| Every command except internal and UART ones counts as activity. | `executeCommand` in [System_Utils.cpp](../components/hardwareone/System_Utils.cpp) | **One glasses tap lights the local screen and runs the P4 at 400 MHz for 10 minutes.** |
| The idle routine is compiled only when there is a display. | `powerSaveTick` inside `#if ENABLE_OLED_DISPLAY` in [HardwareOne.cpp](../components/hardwareone/HardwareOne.cpp) | Builds without a screen, such as the XIAO S3 pocket-assistant deployment, never lower the clock. |
| STT stops the clock from dropping, but never raises it. It also wakes the local screen to get the clock back. | the `sttAnySessionActive()` branch of `powerSaveTick` | **Bug:** in PowerSaver mode STT runs at 100 MHz, which the code's own comment says is about 3x slower than speech. |
| Both STT workers clear `sttAnySessionActive()` as their last step before `vTaskDelete`, after the transcript is saved. The continuous worker also stops speaker labelling and parks the model first. | end of `sttWorker` and `sttContinuousWorker` in [System_STT.cpp](../components/hardwareone/System_STT.cpp) | It already covers the whole STT lifetime, including the `vTaskDelete` exit. |
| `micRecordingBusy()` is true from STARTING through FINALIZING. | [System_Microphone.cpp](../components/hardwareone/System_Microphone.cpp) | It already covers recording until the file is finished. |
| `cameraCaptureBusy()` is true while a stream or recording session is open. | camera module | The idle routine already reads it. |

Using the P4 datasheet figures quoted in
[P4_BATTERY_BUDGET.md](P4_BATTERY_BUDGET.md), an idle P4 core draws about
6–16 mA more at 400 MHz than at 100 MHz. That is worth saving, but it is a
chip figure, not a board figure. The C6, PSRAM (which stays at 250 MHz
whatever the CPU clock), the backlight and the camera/LCD rail may each cost
more. Only phase 0 can tell.

## 3. The approach: read busy flags instead of holding requests

The power loop works out a target clock on every pass:

```cpp
// System_Power.cpp — sketch, not compiled.

// Heavy workloads answer "busy" through their own modules. The power loop reads
// those answers each pass instead of holding a request, so a worker that is
// cancelled, fails or deletes itself clears its own flag and the boost ends.
static bool powerWorkloadBusy() {
#if ENABLE_LOCAL_STT
  extern bool sttAnySessionActive();
  if (sttAnySessionActive()) return true;
#endif
#if ENABLE_CAMERA_SENSOR
  extern bool cameraCaptureBusy();
  if (cameraCaptureBusy()) return true;
#endif
  return micRecordingBusy();
}

// The clock the device should be at right now. `interactive` is true while
// there has been activity within the idle timeout, or while the local panel is
// lit, so UltraSaver's 40 MHz idle clock never runs under a lit screen.
static uint32_t powerTargetMhz(bool interactive) {
  const uint8_t mode = gSettings.powerMode;
  if (mode == POWER_MODE_LOCKED || powerWorkloadBusy()) return POWER_CPU_PERFORMANCE_MHZ;
  uint32_t mhz = interactive ? getPowerModeActiveCpuFreq(mode)
                             : getPowerModeIdleCpuFreq(mode);
  // The UART REF_TICK clamp from today's powerSaveTick moves here unchanged.
  return mhz;
}
```

The tick acts only when the target **changes**. It does not keep forcing the
live clock back to the target:

```cpp
void powerPolicyTick() {
  static uint32_t appliedMhz = 0;          // last target this tick confirmed
  static unsigned long nextAttemptMs = 0;  // backoff after a refused change
  const unsigned long now = millis();
  const uint32_t target = powerTargetMhz(powerInteractive(now));
  if (target == appliedMhz || (long)(now - nextAttemptMs) < 0) return;
  if (!powerSetCpuFrequencyMhz(target)) {
    // A raise is retried sooner: a transcription may be waiting on it.
    nextAttemptMs = now + (target > getCpuFrequencyMhz() ? kRaiseRetryMs : kLowerRetryMs);
    return;
  }
  appliedMhz = target;
}
```

Why this shape:

- **Nothing can leak.** There is no handle to release, so a cancelled,
  failed or self-deleted worker can't leave the CPU boosted. This removes the
  whole class of bugs the earlier draft needed generation counters, move-only
  handles and explicit `reset()` calls to prevent.
- **It is how the code already works.** `powerSaveInhibited()` already reads
  `micRecordingBusy()` and `cameraCaptureBusy()`. This extends the same idea
  from "don't go idle" to "raise the clock".
- **Manual changes are not fought.** `cpufreq` and `power mode` still set the
  clock directly. Their choice lasts until the next policy change: idle entry,
  wake, or a workload starting or ending. That is close to today's behaviour.
  The clock owner already does nothing when asked for the current clock.
- **A late boost is acceptable.** The boost arrives on the next main-loop pass,
  or later if the clock lock is busy. Audio is buffered, so starting a few
  milliseconds slow is harmless. The earlier draft refused to start STT if the
  boost wasn't ready, which turns a brief lock contention into a failed
  transcription.

Limits, accepted for phase 1:

- If the main loop stalls, the boost is late. A refused raise waits for
  `kRaiseRetryMs`. Today the only backoff is 5 s; phase 1 adds a shorter one
  for raises (proposed 500 ms, to be measured).
- A continuous STT session holds full speed for its whole life, including
  time spent waiting for speech. The STT snapshot also has `inferenceActive`
  and `captureActive`, which a later refinement can use (section 7).
- A workload with no busy flag can't ask for speed. The fix is to add a flag
  in that module. Add a request API only if a real caller ever needs one that
  a flag can't serve.

## 4. Phase 0: measure (alongside phase 1, required before any sleep work)

There is no measured board current in the repository. The P4X-EYE battery
monitor reads voltage only. The MAX17048 fuel gauge supported on other boards
estimates charge but does not measure current either.

**Equipment.** A battery-side current meter: a Nordic PPK2 in ampere-meter
mode, or an INA226 breakout wired in line with the battery. Disconnect USB for
every battery-path run. Integrate voltage × current to get watt-hours. The
method is in [P4_BATTERY_BUDGET.md](P4_BATTERY_BUDGET.md#measurements-that-resolve-the-estimate).

**Pinning a clock for a test.** Use existing modes with the idle timeout set
to 0, which keeps the device interactive. No new command is needed:
- 400/240 MHz: Locked
- 200/160 MHz: Balanced, timeout 0
- 100/80 MHz: PowerSaver, timeout 0

**P4X-EYE runs, on current firmware.** Glasses connected (both arms), plus
phone and ring if you use them. 10–30 minutes per state unless noted.

| # | State | What it answers |
| --- | --- | --- |
| 1 | Connected, screen dark, camera never started, at 100 / 200 / 400 MHz | How much the CPU clock matters at idle |
| 2 | Same as 1 at 100 MHz, screen lit at default brightness | Backlight and panel cost |
| 3 | All Bluetooth closed (`closeble`), screen dark, 100 MHz | Cost of keeping links up |
| 4 | Same as 3 with the C6 held in reset | The C6's total share |
| 5 | Camera started then stopped, compared with run 1 | Whether a stopped camera on the shared rail costs anything |
| 6 | A 1-minute recording; a 30 s note transcribed at 400 vs 200 MHz | Energy per note and completion time |
| 7 | One glasses tap, then the following 10 minutes | Cost of today's "tap lights the screen and runs at 400 MHz" behaviour |

**XIAO S3, headless.** Run 1 at 80 / 160 / 240 MHz, and run 3. This gives a
built-in-radio baseline for the S3 sleep milestone.

Record results in [P4_BATTERY_BUDGET.md](P4_BATTERY_BUDGET.md), replacing its
placeholder loads. After phase 1, repeat runs 1, 6 and 7 in Adaptive mode.

**What the results decide** (section 8):

- the clock's share of idle power, which shows how much phase 1 actually
  saves;
- the largest remaining consumer, which picks the next milestone: C6,
  backlight/rail, or P4 sleep;
- whether recording and camera need full speed (phase 2).

## 5. Phase 1 scope

### 5.1 Split the idle routine in two

Today `powerSaveTick` in `HardwareOne.cpp` does two jobs: blanking the panel
and changing the clock. Split them:

- **Clock:** `powerPolicyTick()` moves into
  [System_Power.cpp](../components/hardwareone/System_Power.cpp), next to the
  clock owner. It is declared in `System_Power.h` and called on every
  main-loop pass in **all** builds.
- **Panel:** a display tick stays behind `#if ENABLE_OLED_DISPLAY`. It blanks
  after the timeout, wakes on local activity, keeps the existing camera and
  microphone inhibitors (they only delay blanking; they never wake the panel),
  and makes **no** clock calls.

```cpp
// hardwareone_loop() — sketch
radioCompanionTick();
powerPolicyTick();             // every build, with or without a screen

#if ENABLE_OLED_DISPLAY
powerDisplayTick();            // blank / wake the panel only
localDisplaySessionTick();     // login timeout stays separate
#endif
```

The panel tick loses all of today's clock bookkeeping: `savedCpuMhz`,
`wakePending`, and the partial-change recovery. The clock tick's
change-only design replaces it. The STT branch that wakes the panel is
removed; STT now raises the clock through `powerWorkloadBusy()`.

Panel blanking keeps stamping the sleep cooldown, as it does today. Whether it
should is listed in section 10.

### 5.2 Activity: local vs remote

Today one activity timestamp drives both the panel and the clock. Phase 1
records two:

```cpp
// System_Power.h — sketch
// A real interaction. `local` means someone is at the device: it wakes the
// panel. Remote activity (glasses, phone, web, ...) only keeps the clock
// interactive in modes that want speed while in use.
void powerSaveNoteActivity(bool local);
```

- `executeCommand` passes `local` according to `ctx.transport`. The proposed
  local sources are `SOURCE_LOCAL_DISPLAY` and `SOURCE_VOICE`; everything else
  is remote. Internal, UART and passive transcription commands still record
  nothing, as today.
- Physical input already reaches the panel tick through
  `inputActivitySequence()`, and stays local.
- "Interactive" for the clock means remote or local activity within the
  timeout, or the panel lit. A timeout of 0 keeps it interactive.

In Adaptive mode, both the interactive clock and the idle clock are the
lowest supported clock. In practice, only busy workloads raise it.

### 5.3 Add an Adaptive power mode and make it the default

*As built: Adaptive took PowerSaver's place (index 2) rather than becoming a
sixth mode; see the status note at the top. The hazards below were still
closed with static asserts and a measured buffer.*

Add a mode rather than a separate on/off setting, so every existing mode keeps
one meaning. The mode sets the clock used while the device is in use; busy
workloads raise any mode to full speed.

| Mode | In use | Idle | Busy workload |
| --- | --- | --- | --- |
| **Adaptive (new default; was PowerSaver)** | floor | floor | full speed (fixes the STT bug) |
| Performance | full speed | floor | full speed |
| Balanced | middle | floor | full speed |
| UltraSaver | floor | 40 MHz | full speed |
| Locked | full speed | full speed | full speed |

Floor, middle and full speed are 100/200/400 MHz on P4 rev3, 90/180/360 on
older P4, and 80/160/240 on S3 and ESP32 (`System_Power.h`).

Append the mode as `POWER_MODE_ADAPTIVE 5`. Do not renumber, so Locked stays 4
and no numeric uses of existing modes shift. Change the default in
`System_Settings.h` to the new constant.

Files to change. Two of them break **silently** if missed:

- `System_Power.h`: define and `POWER_MODE_COUNT` 6.
- `System_Power.cpp`: `gPowerModes` row (`{"Adaptive", POWER_INTERACTIVE_FLOOR_MHZ, <brightness>}`),
  the `power mode adaptive` keyword in `cmd_power`, and the settings entry
  (max 4 → 5, options string).
- **`G2_Page_Power.cpp`: `kPowerModeCmds[POWER_MODE_COUNT]` would compile with
  a null sixth entry and crash when tapped. `POWER_MAX_ROWS` is a hard-coded 6
  (back + 5 modes), so a sixth mode overflows `gRows`.** Set it to
  `1 + POWER_MODE_COUNT` and add a `static_assert` that `kPowerModeCmds` has
  no gaps.
- **Both 1 KB power JSON buffers** (`cmd_power` and `handlePowerStatus` in
  `WebPage_Power.cpp`) were sized for a 785 B worst case with five modes, and
  `serializeJson` truncates silently. Re-measure with six modes and the new
  fields (5.4).
- The OLED power screen and the web page build their lists from the table.
  Check that six rows fit on the OLED screen.
- `System_Settings.h` comment on `powerMode`; `docs/USERGUIDE.md`.

### 5.4 Telemetry

Add two fields to `buildPowerJson`: `targetMhz`, and `boost`, which names the
busy workload (`"stt"`, `"camera"`, `"recording"`) or is `"none"`. This lets
phase 0 runs and the UIs show why the clock is where it is. Leave out sleep
support and blocker fields until a sleep path exists.

### 5.5 Behaviour changes to document

1. New default mode: Adaptive.
2. Commands from the glasses, phone, web, serial, MQTT and ESP-NOW no longer
   light the local screen. The list of sources is in section 10.
3. Transcription no longer lights the local screen.
4. Transcription, camera and recording run at full speed in every mode. This
   fixes slow STT in PowerSaver.
5. Boards without a screen now drop to the idle clock after the timeout.

### 5.6 Not in phase 1

Out of scope: processor sleep of any kind; SDK power management or tickless
idle; Wi-Fi power save; per-workload clock levels; C6 changes; display rail
changes; `lightsleep`/`deepsleep` command changes.

### 5.7 Host tests

Extend the existing suites rather than writing new ones:

- `test_power_save_tick.py` extracts production code from `HardwareOne.cpp`
  and `System_Power.cpp` by marker. Point its markers at the moved functions,
  and keep the existing scenarios (refusals, foreign change, partial failure,
  UART clamp, Locked).
- Update scenarios whose behaviour changes on purpose: `wake_stt` no longer
  wakes the panel; `wake_command` splits into local and remote.
- Add scenarios:
  - a headless compile (`ENABLE_OLED_DISPLAY=0`) where the clock idles and
    wakes;
  - Adaptive staying at the floor through a remote command;
  - Adaptive and PowerSaver boosting during STT with the panel dark;
  - recording and camera boosting;
  - a refused raise retrying after the short backoff;
  - `cpufreq` lasting until the next change;
  - UltraSaver never at 40 MHz while the panel is lit;
  - the G2 row count and command table matching `POWER_MODE_COUNT`.
- Remove the `esp32s3` profile added during the earlier investigation.
  `System_Power.h` only distinguishes P4 from everything else, so that profile
  compiles the same code as `esp32` and inflates the scenario count.

### 5.8 Firmware builds and hardware checks

Build with `tools/build_board.sh` on the P4X-EYE, the XIAO S3 headless
deployment, FeatherS3 (OLED) and one classic ESP32 board. Code gated by board
can hide breaks on boards that weren't built.

On hardware:

| Board | Check |
| --- | --- |
| P4X-EYE | Glasses tap with the screen dark: the screen stays dark, `power json` shows floor and `boost: none` |
| P4X-EYE | Transcription started from the glasses with the screen dark: full speed during, back to floor once it ends, transcript correct, no growing backlog |
| P4X-EYE | Quick-note recording: valid file, correct duration |
| P4X-EYE | PowerSaver mode: transcription keeps up with speech |
| P4X-EYE | Local button: screen wakes; I2C sensors read normally across clock changes |
| XIAO S3 headless | Drops to 80 MHz after the timeout; a command makes it interactive again in Performance mode |
| FeatherS3 | Panel blank and wake as before; UltraSaver reaches 40 MHz only with the panel dark |

**Phase 1 is done when** the host tests pass, all four boards build, and the
hardware checks pass.

## 6. Constraints carried forward

These come from the earlier investigation and still apply to every phase:

- One clock owner. If a later phase uses ESP-IDF's automatic frequency
  scaling, it replaces the manual clock writes for that build; the two never
  run together. The SDK's `ESP_PM_CPU_FREQ_MAX` lock means maximum, not a
  middle clock.
- Never change the clock inside an I2C transaction or a critical section.
- Wi-Fi stays at `WIFI_PS_NONE` until the recorded I2C/watchdog interaction in
  `System_WiFi.cpp` is understood. ESP-NOW needs the radio awake to receive.
- Never reset or hold the C6 while links are meant to stay up.
- A wrapper that only pauses the companion watchdog is not "host sleep".
- No automatic deep sleep while connected use is expected.
- Judge sleep by measured residency and current, not by configuration flags or
  a green build.

## 7. Phase 2: per-workload clock levels (after phase 0 runs 6 and 7)

Phase 1 sends every busy workload to full speed. That is safe, and cheap
because these workloads are occasional. Lower them one at a time, only with
measurements:

- **Recording:** try the floor. Keep it there if a 10-minute recording shows
  no overruns and a valid file.
- **STT:** hold full speed only while `inferenceActive`, and drop while a
  continuous session is just listening. Keep it only if transcripts stay
  correct and no backlog builds up.
- **Camera:** stream, single capture and preview may need different levels.

This turns `powerWorkloadBusy()` into a function that returns a level instead
of a yes/no. The busy-flag design still holds.

Also compare energy per completed note at 400 and 200 MHz (run 6). A faster
run can use less energy in total.

## 8. Later milestones, chosen by phase 0

Each milestone starts only if phase 0 shows it targets a real share of idle
power. Each ends either with a measured saving or a written reason it can't be
done.

| If phase 0 shows... | Milestone | Notes |
| --- | --- | --- |
| The C6 is a large share | **C6 power management.** Try power management and tickless idle in the C6 companion firmware. | The C6 build already has `CONFIG_BT_LE_SLEEP_ENABLE=y` but power management off. This is a companion-firmware change and needs its own reflash and qualification. |
| Backlight, panel or the camera rail is a large share | **Rail and backlight.** Work in `HAL_Display_P4Eye` / `HAL_Camera_P4`. | The LCD and camera share GPIO12. Any rail switching stays in the HAL. |
| The P4 at floor clock is still a large share | **P4 connected light sleep over ESP-Hosted.** Treat as research. | The pinned ESP-Hosted v2.12.13 docs describe light sleep as forthcoming. First prove a wake path on the EYE board's real wiring, then HCI buffering during sleep. If no path exists without a hardware change, record that and stop. |
| (S3, independent of the P4 results) | **S3 connected sleep:** SDK power management, tickless idle and BT modem sleep in an opt-in profile. | A well-documented Espressif setup, but this app uses Bluedroid and several links. Check the board's sleep-clock crystal. Never use the internal RC clock with a BLE connection. |
| (any) | **Explicit suspend audit:** `lightsleep` ignores the sleep call's result and native radio shutdown hooks are empty. | Separate from connected idle. May disconnect. Every preparation step needs a rollback. |

Design each milestone's API when that milestone starts, around what it found.
Do not add radio-admission types, blocker enums or sleep fields to the power
JSON in advance.

## 9. What changed from the earlier draft

Kept:
- keeping the screen, CPU speed and permission to sleep separate;
- one clock owner;
- no SDK frequency scaling alongside manual clock writes;
- the Wi-Fi, ESP-NOW and C6 constraints;
- measuring at the battery and comparing energy per note;
- qualifying the S3 and the P4 separately.

Dropped, and why:

| Earlier proposal | Why it was dropped |
| --- | --- |
| Request registry with generation counters, move-only RAII handles, 8 owner IDs and 5 result codes | The workloads already publish busy flags that cover their whole lifetime. `vTaskDelete` skips destructors anyway, so the handles still needed manual `reset()` calls. |
| `Ready` meaning the boost is in effect before work starts, and failing STT otherwise | The clock lock never waits, so contention is routine and brief. A slow first few milliseconds is better than a refused transcription. |
| A separate `workloadPowerEnabled` setting alongside the modes | Every mode would then behave two ways. A new mode is clearer and shows up in the existing mode lists. |
| Radio admission types, blocker enums, sleep fields in the power JSON | There is no caller until a sleep path exists. Design them in the milestone that needs them. |
| A RAM-only `cpufreq` override | With the change-only tick, `cpufreq` already lasts until the next policy change. Phase 0 pins clocks with existing modes. |

## 10. Decisions to settle before or during phase 1

1. **Which command sources light the local screen?** Proposed: physical
   input, `SOURCE_LOCAL_DISPLAY` and `SOURCE_VOICE`. Serial, web, Bluetooth,
   glasses, MQTT and ESP-NOW would not.
2. **Adaptive's display brightness.** Proposed 80%, the same as Balanced.
3. **Retry interval for a refused raise.** Proposed 500 ms; lowering keeps 5 s.
4. **Should panel blanking keep stamping the sleep cooldown** that
   `lightsleep`/`deepsleep` share? Proposed: keep it for phase 1.
5. **Where Adaptive appears in lists.** Appending puts it last in the OLED,
   G2 and web lists. Moving it first means renumbering every mode, which is
   allowed because devices are erased before flashing, but touches every
   numeric use of the mode constants.
