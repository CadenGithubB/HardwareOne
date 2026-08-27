# ENABLE_NEOPIXEL — adversarially-reviewed change plan (2026-08-08)

> **IMPLEMENTED 2026-08-08, uncommitted — pending user HW test. Measured: -39,952 B** on the XIAO carrier build (5,206,192 → 5,166,240; partition headroom 253 KB → 287 KB). **Five times the ~6-8 KB estimate**, and the map file explains why: the old dummy `Adafruit_NeoPixel(0,-1)` global's constructor sat in `.init_array` of every pixel-less build, pinning the ENTIRE vendored Adafruit_NeoPixel library (including its IRAM `espShow`/RMT path) into the link. Deleting the dummy unhooked it — zero `Adafruit_NeoPixel`/`espShow` references remain in the map, and no `led*` command/schema strings remain in the binary. This is the census's `.init_array`-residual pattern (ImageManager, cam #10) at library scale — **worth a dedicated hunt: any always-constructed global whose class comes from a vendored library pins that library into every build.**
> Implementation deviation from the checklist: the setup-wizard gate was done surgically (only the two `gSettings.ledStartupEffect` touch points wrapped; the `ledEffects[]` table, accessors, `SYS_ITEM_LED` plumbing, and `SetupWizardResult` field stay compiled since they reference no gated state and the LED wizard page is already runtime-hidden via `systemPageHasLED()` → `isNeoPixelCompiled()`), avoiding item-index surgery in a state machine with no compile coverage on this config. ~200 B of runtime-hidden wizard residue accepted.
> NOT compile-verified: pixel-board configs (FeatherS3) — the real-definition branches are re-guarded versions of previously working code and the header now includes BuildConfig before branching, but per the board-gated-breaks rule, the first FeatherS3 build is the proof.

Status: reviewed, then implemented (see banner). 3 independent adversarial reviewers (coverage / behavior / design), all SHIP_WITH_CHANGES; every correction below carries file:line evidence from the review run (`wf_a0598cbc-b27`). Expected reclaim on the XIAO carrier build: **~6-8 KB flash + ~60 B RAM** (commands ×2 families, module rows, settings module, effect engine, G2 LED page, neopixel icon, color table via GC).

## The flag

`System_BuildConfig.h`, AFTER the board blocks (battery-monitor pattern):
```c
#ifndef NEOPIXEL_PIN_DEFAULT
  #define NEOPIXEL_PIN_DEFAULT -1     // fallback so the flag default can't silently misfire on a future board block
#endif
#ifndef ENABLE_NEOPIXEL
  #define ENABLE_NEOPIXEL (NEOPIXEL_PIN_DEFAULT >= 0 && NEOPIXEL_COUNT_DEFAULT > 0)
#endif
#if ENABLE_NEOPIXEL && (NEOPIXEL_PIN_DEFAULT < 0)
  #error "ENABLE_NEOPIXEL=1 requires a board with NEOPIXEL_PIN_DEFAULT >= 0 (no dev-rig story without a pin)."
#endif
```
Preprocessor-only; CMake never greps it; `System_NeoPixel.cpp` stays in the unconditional SRCS list. The count term mirrors `isNeoPixelCompiled`'s existing test. The local fallback in `System_NeoPixel.cpp:14-16` is then removed.

## Review-mandated fixes folded into the plan (would have been compile breaks)

1. **`System_NeoPixel.h` must `#include "System_BuildConfig.h"`** — the .cpp includes its own header before BuildConfig, so header stubs would redefine against real definitions on every PIXEL board (invisible on the XIAO build — the classic board-gated-break trap). [design blocker]
2. **Setup wizard**: `System_SetupWizard.cpp:453-456` reads and `:824-826`/`:873-875` write `gSettings.ledStartupEffect` unconditionally → gate those sites + the `ledEffects[]` wizard rows (`:157-165`) + `SetupWizardResult::ledStartupEffect` on `ENABLE_NEOPIXEL`. Runtime hiding already works via `systemPageHasLED()` → `isNeoPixelCompiled`. [all 3 lenses]
3. **`OLED_Mode_LED.cpp`**: wrap the WHOLE body (it reads `ledBrightness` at `:72/:138/:187`), not just registration, and gate the `oledLEDModeInit()` anchor at `OLED_Utils.cpp:3389/:3415`. Update the now-false "no ENABLE_NEOPIXEL macro exists" comments (`OLED_Mode_LED.cpp:27`, `OLED_Utils.cpp:5036/:5357`). [coverage blocker]
4. **Flip four pin-based UI gates to the flag** (they'd leave live UI dispatching into stubs on override builds): `OLED_Utils.cpp:5040` (Sensors submenu LED row), `:5360-5366` (getMenuAvailability), `G2_Page_Sensors.cpp:285` (ledG2FormatValue — also reads `ledBrightness`) and `:415-419` (LED row). [coverage+behavior blockers]

## The rest of the checklist

- `System_NeoPixel.cpp`: gate pixels object (delete the dummy branch), `initNeoPixelLED`, `setLEDColor`, effect engine + `ledEffectTick`, `runLEDEffect`, 3 `cmd_led*` + `neopixelCommands[]`, `ledSettingEntries`/`ledSettingsModule`. Color utilities (`getRGBFromName`, `getClosestColorName`, `blendColors`, `rainbowColor`, `colorTable`, `ledEffectNames`, `ledBrightnessNextPreset`) stay unconditional — GC handles them. (Review correction: APDS's `setLEDColor`/`getClosestColorName` calls are **commented out** (`i2csensor_apds9960.cpp:367-369`) — `getClosestColorName` has zero live consumers.)
- Header inline stubs when off: `initNeoPixelLED`, `setLEDColor`, `ledEffectStart/Stop/Active/Tick`, `runLEDEffect`.
- **Power rail**: split the `NEOPIXEL_POWER_PIN` block out of `initNeoPixelLED` into an unconditional `boardPowerRailInit()` — protects Feather V2 (GPIO2 also powers STEMMA QT/I2C). FeatherS3's GPIO39 is independently driven (`System_I2C_Manager.cpp:272`, `OLED_Utils.cpp:6821`) — no conflict; the `NEOPIXEL_I2C_POWER` legacy branch is dead (no board defines it) — drop it.
- `System_Utils.cpp:~2884`: gate the `neopixel` gCommandModules row. `System_Hardware.cpp`: gate `ledCommands[]` (6 rows) + handlers + its `led` module row.
- Settings: gate `ledBrightness`, `ledStartupEffect/Color/Color2/Duration` fields + ctor inits; **keep `ledStartupEnabled`** (FeatureRegistry row at `System_FeatureRegistry.cpp:254` reads it). Gate `System_Settings.cpp:2275` (extern) + `:2350` (registration) — NOT OLED_Settings.cpp (review: no reference there). `isNeoPixelCompiled` reports `ENABLE_NEOPIXEL`.
- G2: extend `G2_Page_LED.cpp/.h` wrap with `&& ENABLE_NEOPIXEL`; gate `kLedG2Page` (`G2_Glasses.cpp:4657-4668`) + `g2RegisterPage(kLedG2Page)` (`:7006`) — frees one of the 20 page-registry slots.
- `HardwareOne.cpp:2112-2142`: REPLACE the existing pin-based gate around the startup-effect block with `#if ENABLE_NEOPIXEL` (as-is it breaks override builds against gated fields).
- **Web**: gate the LED live-control card/JS in `WebPage_Settings.h:78-110` + `renderLedLiveControls`/76-color list (`:1084-1145`) — live on today's build; also extra uncounted savings.
- Icons: gate the `neopixel` icon (`System_Icons.cpp:6344-6402`, row `:7608`) — ~1 KB.
- Voice routes: the two `led` rows in `kVoiceRoutes` need **no gate** — E1's liveness filter drops routes whose command isn't registered (the boot WARN for them is the designed behavior). Optional: `#if` them anyway to silence the WARN.
- Cosmetic sweep (optional): help/placeholder text advertising `ledcolor` as the canonical example (`System_Utils.cpp:3144`, `System_Automation.cpp:4293`, `WebPage_Automations.h` ×4).

## Behavior changes (reviewed and accepted)

- `ledcolor` etc. become "Unknown command" on pixel-less builds — reviewer verdict: this **matches** house convention (all compiled-out features drop their tables) and arguably fixes an OK-contract violation (today returns "LED set to red" with no LED). Automation sequences do NOT abort on a failing action (fire loop runs every command); espnowremote semantics unchanged (reply text only). Conditional automations can still be *created* referencing ledcolor (syntax validator doesn't resolve commands) → per-fire error; documented, not fixed here.
- `hardware.led` vanishes from settings.json (module-driven persistence; stale sections ignored on load); web settings LED pane disappears dynamically (`if (mod.name === 'led')`).
- `features json` still lists led with `compiled:false`; `features toggle led` refuses cleanly.

## Test plan

XIAO carrier build (expect ~6-8 KB smaller; `ledcolor` → Unknown command; no LED page on G2 Hardware list) + FeatherS3 build (everything present + LED works = no regression) + FeatherS3 with `#define ENABLE_NEOPIXEL 0` override (compiles; I2C still powered on V2-class boards via boardPowerRailInit; no LED UI anywhere).
