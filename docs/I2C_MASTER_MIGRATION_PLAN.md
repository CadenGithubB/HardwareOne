# I2C Driver Migration Plan — legacy `driver/i2c.h` → `driver/i2c_master.h`

_Created 2026-06-08. Goal: move the firmware's I2C stack onto ESP-IDF's new
`i2c_master` driver, to (a) get off the deprecated legacy driver and (b) likely
cure the BLE-coexistence Int-WDT crash at the source instead of via band-aids._

## Why / context (settled facts)

- **Everything funnels through one file:** every I2C device (OLED, gamepad, every
  sensor, the firmware's own wrappers) talks via Arduino `Wire`/`Wire1`, which
  sits on `components/arduino/cores/esp32/esp32-hal-i2c.c` (435 lines), which
  `#include "driver/i2c.h"` (legacy). That HAL is the single chokepoint.
- **No "upgrade Arduino" shortcut:** vendored arduino-esp32 **3.3.5** AND upstream
  `master` both still use the legacy driver for `Wire`. So the only path is to
  **fork that one HAL file** onto `i2c_master`.
- **Arduino stays** — it's load-bearing (String in 203/298 files, WiFi/BLE/timing,
  65 files using Adafruit libs). We keep the `Wire` API and swap the engine under it.
- **Coexistence knobs port over** (verified in IDF v5.3.1 source):
  - Glitch filter → `i2c_master_bus_config_t.glitch_ignore_cnt` (our `7` ports 1:1).
  - ISR core-pin → new driver allocates its ISR on the **calling core**
    (`esp_intr_alloc_intrstatus`), same as legacy, so `beginBusOnCpu1` still works.
  - Bonus: `intr_priority` knob (legacy doesn't expose it).
  - New driver's async/completion ISR design likely avoids the legacy
    "re-arm-in-ISR" storm entirely → may make the band-aids unnecessary.

## Part A — Investigation (read-only; close unknowns BEFORE writing code)

| # | Unknown | How to check | Decision it informs |
|---|---|---|---|
| A1 | Direct legacy-driver calls beyond Wire? | grep firmware for `i2c_driver_install/param_config/set_pin/cmd_link/master_cmd_begin/filter_enable/driver_delete` + `<driver/i2c.h>` | each must be ported/removed |
| A2 | Is `esp_driver_i2c` linkable here? | check it's in IDF; whether arduino's CMake `REQUIRES` needs it | CMake change needed? |
| A3 | HAL public API surface | read `esp32-hal-i2c.c`; list fns + callers | exact set to re-implement |
| A4 | Scan/probe path | trace `beginTransmission/endTransmission` scan; `i2c_master_probe` | HAL heuristic vs firmware scan change |
| A5 | Device-handle cache + per-device clock | study `I2CDeviceManager` clock stack vs `i2c_device_config_t.scl_speed_hz` | cache design (trickiest) |
| A6 | Repeated-start write-then-read | confirm sensors + `i2c_master_transmit_receive` | mapping correctness |
| A7 | Clock-stretch / timeout (Seesaw) | new driver per-device timeout | timeout values |
| A8 | ISR core-pin survives | re-read `i2c_new_master_bus` (sync, calling-core) | coexistence pin OK |
| A9 | Memory footprint (DRAM ~82 KB free) | new bus/device/queue allocs; `trans_queue_depth` | no DRAM regression |
| A10 | Fork + rollback mechanics | confirm vendored edit is built; design compile toggle | reversibility |

## Part B — Integration (phased, reversible, HW-gated)

Guiding principle: **everything behind a compile toggle, so any failure is one
reflash away from the known-good legacy driver.** Essential — this touches all
I2C and only the user can HW-test it.

- **P0 — Safety scaffold.** `HW_I2C_USE_MASTER_DRIVER` flag (default OFF). Wrap the
  existing HAL in `#if !HW_I2C_USE_MASTER_DRIVER` (intact fallback).
- **P1 — New HAL** (under the toggle, identical signatures): bus-handle table,
  device-handle cache, `glitch_ignore_cnt=7`, transmit/receive/transmit_receive,
  `i2c_master_probe` scan path, clock mapping (A5).
- **P2 — Firmware cleanup.** Drop direct `i2c_filter_enable` (glitch now in config);
  keep `beginBusOnCpu1`.
- **P3 — Build green** with toggle ON.
- **P4 — HW gate ① (user):** OLED renders, gamepad reads, scan/dashboard lists
  devices, fuel gauge reads. Broken → flip toggle off, reflash, debug.
- **P5 — HW gate ② (user):** glasses + gamepad together (original crash). Watch Int-WDT.
- **P6 — Band-aids:** if storm-immune, optionally remove pause-during-connect / CPU1 pin.
- **P7 — Finalize.** Default toggle ON (or delete legacy path); document the fork in
  docs/ + memory note so future Arduino-core updates re-apply it.

## Part A — Findings (2026-06-08)

**The investigation reframed the whole project.** The `i2c_master` HAL we were going
to hand-write **already exists** in your Arduino core: `esp32-hal-i2c-ng.c` (uses
`driver/i2c_master.h`, full per-address `dev_handles[]` cache, `transmit/receive/
transmit_receive`, native `i2c_master_probe` for scans, sets `glitch_ignore_cnt=7`).
It's selected by IDF version: legacy HAL `#if IDF < 5.4.0`, the `-ng` HAL `#if IDF >= 5.4.0`.
You're on **5.3.1**, so you get legacy. **So the migration is not "write a HAL" — it's
"get onto IDF ≥ 5.4 (or force the existing HAL)."**

- A1 (firmware legacy-i2c calls): only `i2c_filter_enable(port,7)` ×2 + `#include
  <driver/i2c.h>` in `System_I2C_Manager.cpp`. The `-ng` HAL sets the glitch filter
  itself → just **remove those** (legacy calls would conflict with the master driver).
- A2 (component): `driver` meta-component already requires `esp_driver_i2c` → **no CMake change**.
- A3–A7 (HAL surface/scan/cache/clock/writeRead): **all solved by the existing `-ng` HAL** — Espressif already did this design.
- A8 (ISR core-pin): `-ng` `i2cInit` calls `i2c_new_master_bus` on the calling core →
  `beginBusOnCpu1` still pins bus-0's ISR to CPU1. ✓
- A9 (memory): `-ng` uses `trans_queue_depth=0` (sync) → minimal allocs.
- **API compat:** all 9 `i2c_master_*` fns the `-ng` HAL uses exist in 5.3.1. BUT the
  `-ng` HAL sets two **5.4-only** fields (`bus.flags.allow_pd`, `dev.flags.disable_ack_check`),
  which is why Espressif gated it at 5.4. Forcing `-ng` on 5.3.1 means `#if`-ing out those
  2 lines (a fork). → favors the **IDF upgrade** path over the fork.

### Decision → Option 1: upgrade IDF 5.3.1 → 5.5.1 (within Arduino 3.3.5's `>=5.3,<5.6`)

Scope of an IDF 5.3.1 → 5.5.1 upgrade **for this repo**:

| Area | Risk | Notes |
|---|---|---|
| **i2c migration** | tiny | Arduino auto-flips to `-ng`; just remove the 2 `i2c_filter_enable` + the `<driver/i2c.h>` include. Keep `beginBusOnCpu1`. |
| **Managed components** | low | `esp32-camera`/`esp-sr`(speech) are **disabled for feathers3**; component manager re-resolves `libsodium`/`tflite`/`camera` to 5.5-compat versions automatically. |
| **Mic / I2S** | none | already on the **new** I2S driver (`esp_driver_i2s`), and off for feathers3. |
| **sdkconfig** | moderate-mechanical | mostly carries over; re-verify a few keys: `BT_ALLOCATION_FROM_SPIRAM_FIRST` (known-flaky), mbedtls keys (5.5 ships newer mbedtls), `ESP_INT_WDT_TIMEOUT_MS`. Reconfigure warns on removed keys. |
| **Direct IDF API** | low-moderate | firmware uses mostly-stable headers (esp_http_server, freertos, esp_wifi, esp_now, Bluedroid). **No `-Werror`** → new toolchain warnings won't block. Expect a few deprecation fixes. |
| **Toolchain** | low | `git checkout v5.5.1 && git submodule update && ./install.sh esp32s3`; cosmetic new warnings. |
| **HW revalidation** | **the real cost** | full re-test: WiFi, BLE/G2, ESP-NOW, web, OLED, gamepad, battery, + the coexistence crash scenario. Only the user can do this. |

**Mechanics:** in `/Users/morgan/esp/esp-idf`: `git checkout v5.5.1 && git submodule
update --init --recursive && ./install.sh esp32s3 && . ./export.sh`; then in the repo
`idf.py fullclean && HW_BOARD=feathers3 idf.py build`, fix breakages, re-verify sdkconfig.

**Verdict:** more tractable than a generic IDF jump — the risky IDF-coupled features
(camera/speech) are off for this board, Arduino already supports 5.5, the mic is already
migrated, and there's no `-Werror`. Payoff: i2c_master **free + official** (no fork) +
a year of SDK fixes + the best-tested Arduino(3.3.5)+IDF(5.5) pairing.

## Outcome — DONE & HW-validated (v0.95.5, 2026-06-09)

Shipped via Option 1 (IDF 5.3.1 → **5.5.1** upgrade). The whole SDK jump — across
**both** boards — cost exactly **one** functional code change.

**Code changes (the entire migration):**
- **ESP-NOW send callback** signature: `const uint8_t* mac` → `const esp_now_send_info_t* tx_info`
  (IDF 5.4+ changed `esp_now_send_cb_t`). The only functional break.
- **I2C cleanup** in `System_I2C_Manager.cpp`: dropped `#include <driver/i2c.h>`, the dead
  `i2cPortForBus()` helper, and both `i2c_filter_enable()` calls — the `i2c_master` (`-ng`)
  HAL applies the glitch filter itself (`glitch_ignore_cnt=7`). `beginBusOnCpu1` retained.
- **sdkconfig**: dropped dead `CONFIG_TINYUSB_CDC_ENABLED` (board uses HWCDC); `sdkconfig` /
  `dependencies.lock` regenerated for 5.5 (managed components re-resolved: esp32-camera 2.1.4,
  esp-sr 1.9.5, esp-tflite 1.3.5, libsodium 1.0.20).

**What carried over for free:** Arduino auto-flips `Wire` to `esp32-hal-i2c-ng.c` at IDF ≥ 5.4,
so the legacy driver + its "old driver" deprecation nag are gone with no fork. ISR core-pin,
glitch filter, scan/probe path, per-address device cache — all handled by the `-ng` HAL.

**HW validation:**
- **FeatherS3** (`HW_BOARD=feathers3`, 16 MB): WiFi/BLE-G2/web/OLED/gamepad all green;
  glasses + gamepad together with **no Int-WDT** (the original coexistence crash); ~77 KB free DRAM.
- **XIAO Sense** (`HW_BOARD=xiao_s3`, 8 MB): camera (OV3660), PDM mic, WiFi/BLE/web all green;
  Octal PSRAM confirmed (8 MB). The camera's two-try init + JPEG-truncation-at-extreme-settings
  are **pre-existing** quirks (PSRAM-DMA off by design — see `System_Camera_DVP.cpp:366-405`),
  **not** migration regressions; the new SCCB driver just prints the first-try failure explicitly.

**Coexistence band-aids RETAINED** (CPU-1 ISR pin + pause-during-connect) as defense-in-depth.

**Flashing note:** crossing IDF versions, use a full `erase-flash` so stale 5.3.1 NVS/filesystem
state never lingers. XIAO commands **must** be prefixed `HW_BOARD=xiao_s3` (bare `idf.py`
defaults to feathers3/16 MB).
