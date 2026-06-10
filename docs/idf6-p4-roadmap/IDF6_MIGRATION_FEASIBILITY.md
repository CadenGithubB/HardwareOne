# ESP-IDF v5.5.1 → v6.0 Migration Feasibility

_Created 2026-06-09 (app v0.95.5). Maps ESP-IDF v6.0's breaking changes onto what
**this** codebase actually uses, to decide whether/when a 6.0 migration is viable._

## TL;DR — DEFER. One hard blocker, one real code item, otherwise clean.

- 🔴 **BLOCKER (upstream): arduino-esp32 3.3.5 declares `idf: ">=5.3,<5.6"`**
  (`components/arduino/idf_component.yml:51`). **IDF 6.0 is out of range.** Arduino is
  load-bearing here (`String` in ~200 files, `Wire`/`Wire1`, `WiFi`, `BLEClient`,
  `millis`/`digitalWrite`, 65 files of Adafruit libs) and cannot be dropped. **No
  firmware change closes this gate** — it needs a future arduino-esp32 (a 4.x major)
  that supports IDF 6.0, on Espressif's schedule.
- 🟠 **Biggest code item once unblocked: mbedTLS 4.0 / PSA Crypto.** 46 direct
  `mbedtls_*` call sites + `mbedtls/aes.h`/`mbedtls/sha256.h` in `System_ESPNow*`.
  mbedTLS 4.0 removes most legacy crypto primitives → these must move to PSA Crypto,
  or (cleaner here) to **libsodium**, which we already link and which is unaffected.
- ✅ **Everything else we wrote is clean** against 6.0's removals — no removed FreeRTOS
  compat calls, no `esp_log_buffer_*`, no cJSON, no `wifi_provisioning`, no removed
  WiFi/ESP-NOW/Bluedroid APIs in our code.
- 🟡 **Two dormant legacy-driver exposures**, compiled only for board configs we don't
  ship today: `System_Battery`'s ADC backend, and two vendored libs (NeoPixel, TinyUSB).

**Verdict: not feasible now (Arduino blocks it). Revisit when arduino-esp32 ships a
6.0 release. Meanwhile, de-risk by pre-migrating the crypto + battery-ADC code on
5.5.1 — work that pays off regardless of IDF version.**

## Why v6.0 is a different class of jump than 5.3→5.5

5.3.1→5.5.1 was "Arduino auto-switches `Wire` to the `i2c_master` HAL; fix one
callback." v6.0 is a *major* release that (a) **removes every legacy peripheral driver**
(ADC/DAC/I2S/Timer/PCNT/MCPWM/RMT/Temp/SDM), (b) **replaces the crypto stack**
(mbedTLS 4.0 / PSA-first), (c) changes the **default C/C++ standards** (gnu23/gnu++26)
and **default libc** (Newlib→Picolibc), and (d) makes **warnings errors by default**.
The blast radius is the whole SDK, not one subsystem.

## Methodology

Scanned `components/hardwareone` (our code), `components/hardwareone_libs` (vendored
Adafruit/etc.), and `main/` for every API/header v6.0 removes or moves; findings cited
file:line below. The Arduino core and managed components (esp-sr, camera, tflite) are
upstream — their 6.0 readiness is their maintainers' job. Ours is the Arduino *version
constraint* (the blocker) plus our own direct IDF usage.

> ⚠️ Scan caveat: zsh does not word-split unquoted variables, so the directory list
> must be passed literally to `grep -r` (a `$DIRS` var silently searches a bogus path
> and reports false "clean"). Results below were re-run with literal paths.

## Impact matrix

| v6.0 breaking change | Our exposure | Severity | Evidence |
|---|---|---|---|
| **arduino-esp32 supports only IDF `<5.6`** | Load-bearing; can't drop | 🔴 **BLOCKER** | `arduino/idf_component.yml:51` |
| **mbedTLS 4.0 / PSA — legacy crypto APIs removed** | 46 `mbedtls_*` sites; `aes.h`+`sha256.h` | 🟠 High | `System_ESPNow*.cpp` |
| **Legacy ADC driver removed** | `System_Battery` ADC backend (dormant) | 🟡 Med (cond.) | `System_Battery.cpp:31,100,124` |
| **Default warnings = errors (`-Werror`)** | Many warnings (ours + esp-sr + camera) | 🟡 Med (mitigable) | XIAO build log |
| **C23 / C++26 default standard** | Whole codebase recompiles under new std | 🟡 Med | — |
| **Newlib → Picolibc default libc** | Heavy `printf`/`String`/`snprintf` use | 🟡 Med | — |
| **http_server/client +~40 KB each (PSA)** | Web server (yes); client (OTA, if used) | 🟡 Med (flash) | 8% app free on XIAO |
| **Legacy RMT removed** | `Adafruit_NeoPixel` vendored lib (dormant) | 🟢 Low (cond.) | `Adafruit_NeoPixel/esp.c:125` |
| **`periph_ctrl.h` / `timer_group` removed** | `Adafruit_TinyUSB` vendored lib (dormant) | 🟢 Low (cond.) | `Adafruit_TinyUSB_esp32.cpp:73` |
| **Cert bundle drops deprecated CAs; TLS suite trims** | HTTPS *client* fetches (if any) | 🟢 Low | — |
| Removed FreeRTOS compat (`vTaskDelayUntil` fn, `xQueueGenericReceive`, `xTaskGetAffinity`…) | None in our code | ✅ None | clean |
| `esp_log_buffer_hex/char`, `esp_log_internal.h` removed | None | ✅ None | clean |
| cJSON moved to managed component | We use ArduinoJson | ✅ None | clean |
| `wifi_provisioning` moved out of IDF | We have our own web setup | ✅ None | clean |
| Removed WiFi/ESP-NOW APIs (`esp_wifi_config_espnow_rate`, `esp_wifi_set_ant`, `WIFI_AUTH_WPA3_EXT_PSK`, `esp_interface.h`, `WIFI_BW20`…) | None direct | ✅ None | clean |
| Bluedroid renames (`esp_bt_dev_set_device_name`→gap, `esp_spp_init`…) | None direct (Arduino wraps these) | ✅ None | clean |
| Removed deprecated headers (`esp_spi_flash.h`, `esp_spiram.h`, `intr_types.h`…) | None in our code | ✅ None | clean |

## The blocker, in detail

`components/arduino/idf_component.yml:51` → `idf: ">=5.3,<5.6"`. We vendor arduino-esp32
**3.3.5**, and the entire firmware sits on the Arduino comfort layer. You cannot build
this project against an IDF the Arduino core refuses. IDF-6.0 support is the arduino-esp32
**4.0** major. Status as of 2026-06 (Espressif's
[release roadmap](https://github.com/espressif/arduino-esp32/wiki/Release-Roadmap-and-Management)):

| Channel | Version | Date | IDF base | Usable here? |
|---|---|---|---|---|
| Stable | **3.3.10** | 2026-06-05 | 5.5.4 | ✅ our current 5.x line |
| Pre-release | **4.0.0-alpha1** | 2026-05-27 | 6.0.1+ | ❌ alpha; **missing components** |

The decisive caveat: **4.0.0-alpha1 ships without several managed components — including
`ESP_SR` (esp-sr / speech), which our XIAO build uses** (also Matter, RainMaker). So even
the alpha can't build our full feature set. The 4.0 roadmap date (2026-05-02) has already
slipped to an alpha. **Trigger to revisit: 4.0 reaching *stable* with esp-sr ported** —
neither has happened. **This is a wait-on-upstream, not a task we can complete.**

## The crypto item (the real pre-work)

mbedTLS 4.0 migrates to TF-PSA-Crypto and removes most legacy `mbedtls_*` primitives.
Our exposure:
- `System_ESPNow.cpp` includes `mbedtls/aes.h` + `mbedtls/sha256.h`; **46** `mbedtls_*`
  call sites across our code (AES for the ESP-NOW encrypt / bond / RCE channel, SHA-256).
- **libsodium is unaffected** — a standalone managed component (Ed25519 `crypto_sign_*`,
  `crypto_hash_sha256`, `sodium_memcmp`), independent of mbedTLS. Those paths survive 6.0
  untouched.

**Recommended path (de-risks today, IDF-version-agnostic):** move the remaining
`mbedtls/aes.h`+`sha256.h` usage onto **libsodium** (already linked) — e.g.
`crypto_aead_*` / `crypto_secretbox_*` for AES, `crypto_hash_sha256` for digests. That
**deletes** the mbedTLS-4.0 surface instead of porting it to PSA, and it's a safe change
on 5.5.1 right now.

## Dormant legacy-driver exposures (conditional)

Compile only for board configs we don't currently ship, so they don't block today's
boards — but they'd need fixing before any 6.0 build that enables them:
- **`System_Battery.cpp` ADC backend** (`#if BATTERY_BACKEND_ADC` — per the file:
  "Feather V1/V2, XIAO Plus"): legacy `driver/adc.h`, `esp_adc_cal.h`,
  `adc1_config_width/_channel_atten`, `adc1_get_raw`, `esp_adc_cal_*` — all removed in
  6.0. Our shipped boards use the I2C fuel-gauge backend (FeatherS3) or have no battery
  monitoring (XIAO Sense), so this path is dark. Migration target:
  `esp_adc/adc_oneshot.h` + `esp_adc/adc_cali.h`.
- **Vendored libs:** `Adafruit_NeoPixel/esp.c` uses legacy `driver/rmt.h`;
  `Adafruit_TinyUSB` uses `periph_ctrl.h` + `timer_group_struct.h`. Both removed in 6.0.
  TinyUSB is unused on our HWCDC boards; NeoPixel only if a status-LED build enables it.
  Both ride on the arduino-esp32 update anyway.

## Codebase-wide frictions (whole build, not a specific call)

- **`-Werror` by default.** Our build — plus esp-sr/camera managed components — emit many
  warnings (`-Wformat-truncation`, unused-function, `-Wvolatile`, missing-field-init).
  On 6.0 these fail the build. Quick mitigation: `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS=y`.
  Better long-term: actually fix the warnings (mostly cosmetic, all ours).
- **C23 / C++26 + Picolibc.** New language standards and a new default libc can surface
  latent UB / format / locale issues across ~200 files. Unpredictable until built; budget
  a debugging pass.
- **Flash budget.** XIAO app partition is at **8% free** today (~460 KB). mbedTLS-4.0/PSA
  adds ~41 KB to `http_server` and ~37 KB to `http_client`; total crypto-stack growth
  could be ~80–120 KB. It still fits (the 8 MB XIAO slot is ~5.4 MB), but headroom shrinks
  — run `idf.py size` post-migration. The 16 MB Feather has ample room.

## Why this matters: the ESP32-P4X path

The reason to track IDF 6.0 at all is **ESP32-P4X (silicon rev 3.x)** — IDF 6.0 makes P4
**rev 3.0 the default target** ("Supported ESP32-P4 Version3 silicon" / "Changed ESP32P4
REV3 as default"), so it's the correct base for P4X. But IDF 6.0 is only *one* link in the
chain, and **not the hardest**. Full dependency order to run *this* (ESP-NOW-centric)
firmware on P4X:

1. **arduino-esp32 4.0 stable + esp-sr ported** — in progress (alpha only). Gates the build.
2. **IDF 6.0 migration** — this doc; mostly the mbedTLS→libsodium crypto item. Tractable.
3. 🔴 **ESP-NOW over esp-hosted — the real wall.** The P4 has no native radio; WiFi/BLE/
   ESP-NOW run on a companion C6 via `esp-hosted`, which exposes **65+ WiFi RPCs and zero
   ESP-NOW RPCs** (no `esp_now_*`), with no public roadmap. Since this firmware is built
   around ESP-NOW (V4 mesh, bond/RCE, sensors, file transfer), this is the gate that actually
   decides P4 feasibility — and it's **independent of IDF 6.0 and Arduino 4.0**. See
   [`ESP32_P4_PORT_ASSESSMENT.md`](ESP32_P4_PORT_ASSESSMENT.md) §4 for the three routes to close it.

**Crypto→libsodium is also P4 prep:** libsodium is portable C (RISC-V-safe, no per-chip
hardware-crypto path), so moving off `mbedtls/*` is simultaneously the IDF-6.0 fix *and*
P4-portability prep. It does **not** move the ESP-NOW needle — that stays the dominant blocker.

## Verdict & recommendation

1. **Do not attempt the 6.0 migration now** — arduino-esp32 3.3.5 forbids it, and Arduino
   is non-negotiable here.
2. **Watch arduino-esp32 4.0** to reach *stable* **with esp-sr ported** (4.0.0-alpha1 exists
   on IDF 6.0.1 but is missing esp-sr + others). That release is the trigger to revisit.
3. **De-risk in the meantime, on 5.5.1, with changes that pay off regardless of IDF:**
   - Port the `mbedtls/aes.h`+`sha256.h` usage to **libsodium** (kills the biggest 6.0 item).
   - Migrate `System_Battery`'s ADC backend to the `esp_adc` oneshot/cali driver (also
     clears a deprecation today).
   - Drive down the compiler-warning count so a future `-Werror` default is a non-event.
4. **When the time comes**, expect: bump IDF + Arduino together, flip
   `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS=y` initially, full HW re-validation on both
   boards (the 5.5.1 effort is the template), and a `size`/heap check for the crypto-stack
   flash growth.

**Bottom line:** the firmware *itself* is in good shape for 6.0 — direct exposure is one
crypto module plus two dormant driver paths. The migration is gated almost entirely on
the Arduino layer catching up, so this is a "wait, and pre-migrate the crypto" situation,
not a rewrite.
