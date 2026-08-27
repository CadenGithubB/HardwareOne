# Vendored-library pinning hunt — 2026-08-08

Follow-on from the NeoPixel result (a dummy `Adafruit_NeoPixel(0,-1)` global pinned that whole library into every pixel-less build: **−39,952 B**). This hunt asked the general question: *what else is linked into today's image only because some always-compiled reference names it?*

**Method:** two finders (map-truth + source-truth) over a snapshot of the real XIAO carrier link map, each followed by a high-effort adversarial verifier that re-traced every pull chain and re-priced from allocated sections. **8 findings: 3 CONFIRMED, 5 ADJUSTED, 0 refuted.** Verifiers corrected the fix for two of the three big items — as written they would not have worked.

Config: XIAO Sense carrier (BT/G2/ESPNOW/HTTP/HTTPS/AUTOMATION/camera/mic on; I2C level 0, no OLED/sensors, SR/EI/LLM/MQTT/MAPS/GAMES off).

## Recoverable in TODAY'S binary — ~45.5 KB

### 1. libsodium argon2 + AEGIS — 24,371 B (23,319 flash + 1,052 DRAM) · CONFIRMED

`sodium_init()` unconditionally calls `_crypto_pwhash_argon2_pick_best_implementation()`, `_crypto_aead_aegis128l_…` and `_crypto_aead_aegis256_…`, which drag in argon2 password hashing, both AEGIS AEADs and a soft-AES implementation. The app uses **none** of them (only chacha20poly1305, blake2b, hmacsha256, kx, ed25519 — verified: zero hits for `crypto_pwhash|aegis|argon2` in the app). `sodium_init()` is genuinely required, called from `System_BleSecureChannel.cpp:85`, `System_CaptureCrypto.cpp:109`, `System_ESPNow_Crypto.cpp:29`.

Verifier re-summed from allocated VMAs: argon2-fill-block-ref 13,838 + aegis128l_soft 4,973 + aegis256_soft 3,612 + softaes 1,867 (incl. a **1,024 B AES T-table in internal DRAM**) + 3 dispatch stubs = 24,371 exactly.

**Fix:** local patch over the managed component (same practice as `docs/arduino-local-patches/`): drop `crypto_pwhash/argon2/*` and `crypto_aead/aegis*/*` from libsodium's CMake sources and supply three no-op `_pick_best_implementation` stubs so `core.c` still links. **Applies to every build that links libsodium**, not just this config.

### 2. Arduino Wire family — 18,020 B · CONFIRMED

`Wire.cpp` 5,456 + `esp32-hal-i2c-ng` 5,269 + `esp32-hal-i2c-slave` 7,295, all allocated in a build with `I2C_FEATURE_LEVEL=0` and zero I2C devices. The verifier walked every `_ZN7TwoWire*` entry in the map's Cross Reference Table and proved the referrer set is **exactly three objects**, so the fix list is provably complete:

1. `System_I2C.cpp:1968-1975` (`Wire.beginTransmission/endTransmission` in the `sensorinfo` command) and `:3214` (`Wire1.setClock`) — guard-walked to **depth zero**, outside the file's own `#if ENABLE_I2C_SYSTEM` regions.
2. `System_I2C_Manager.cpp:52-53` — `wires[0] = &Wire1; wires[1] = &Wire;` takes the address of both library globals.
3. `System_Camera_DVP.cpp:305-323` — a leftover `=== DEBUG: SCCB/I2C Probe ===` block inside `initCamera()`.

**All three must go together — fixing any subset saves zero bytes.** #3 is the trivially safe one and is a precondition for the others paying anything: esp32-camera's own `sccb-ng.c` owns that bus through `esp_driver_i2c`, so the Arduino Wire scan is pure diagnostic. (`esp_driver_i2c` itself stays either way — the camera references it independently — correctly excluded from the 18,020.)

Sub-finding worth its own note: **7,295 B of that is I2C *slave*-mode HAL** the product never uses. The verifier corrected the mechanism — it is not `Wire.end()` but the `.ctors` root → `TwoWire` **vtable** → `begin(uint8_t)` (an `override final` of a pure virtual in `HardwareI2C.h:28`). Consequence: *you cannot `#if` those members out* — the class would go abstract and the `Wire`/`Wire1` globals would fail to compile. The correct patch forces the existing `SOC_I2C_SUPPORT_SLAVE=0` code path (keep the overrides, stub their bodies).

### 3. SmartConfig / ESPTouch / AirKiss — 3,079 B · CONFIRMED

`smartconfig.c` + `smartconfig_ack.c` + prebuilt `libsmartconfig.a` (sniffer, esptouch, esptouch_v2, airkiss). The firmware never offers SmartConfig provisioning.

**Verifier corrected the fix:** `esp_smartconfig_start`'s text is *already* GC'd, so patching `WiFiSTA.cpp::beginSmartConfig` (the obvious target) removes nothing. The live root is `WiFiGeneric.cpp:421` — `esp_smartconfig_stop()` inside the `ARDUINO_EVENT_SC_SEND_ACK_DONE` arm of the always-registered event callback, plus the `SC_EVENT_*` mapping at `:112-130`. Those already sit inside `#if !CONFIG_ESP_WIFI_REMOTE_ENABLED`, the natural place to widen. Verification step: `libsmartconfig.a` should vanish from the map's archive-inclusion section.

## Latent / hygiene — 0 B today

| Item | Today | Latent | Note |
|---|---|---|---|
| Wire + `esp_driver_i2c` full stack | 0 B (camera masks it) | 34,564 B | Materialises on a camera-off **and** I2C-off build (plain XIAO, or slim FeatherS3) |
| `Adafruit_GFX` via `System_Utils.cpp:5173` icon helpers | 0 B (fully GC'd) | ~2-4 KB | Verifier **corrected an overreach**: the finder priced it at 16,248 B, but `--gc-sections` is per-section — a live `drawBitmap` pulls only that chain + base vtable, not `drawChar`/fonts/canvas. No GFX subclass is constructed here, so unlike NeoPixel there is no ctor root to defeat GC. Fix is still right (wrap `~5171-5340` in `#if ENABLE_OLED_DISPLAY`); verify by "not linked", not "zero allocated bytes" |
| `Preferences prefs;` at `HardwareOne.cpp:387` | ~45 B | — | Dead global of a vendored type: repo-wide grep finds **one** hit (its own definition). Verifier corrected the claim that deleting it frees a `.ctors` slot — the slot is kept alive by neighbouring `String` globals regardless |

## The generalisable rule

The NeoPixel case was one instance of a family. Ranked by how the pin happens:

1. **Global object of a library type** → its ctor lands in `.ctors`/`.init_array`, a KEEP root, which pins the ctor, the vtable, and everything the vtable's slots reach. Worst case, because GC cannot help. (NeoPixel: 40 KB. Preferences: 45 B.)
2. **Library init function that dispatches to every implementation it ships** → `sodium_init()`. Unfixable app-side; needs a component patch. (24 KB.)
3. **One unconditional reference in an always-compiled TU** → `System_I2C.cpp`'s `Wire` calls, `System_Utils.cpp`'s `#include <Adafruit_SSD1306.h>`. Cheap to fix, but only if *every* referrer is fixed — the map's Cross Reference Table tells you exactly who they are.

**Two verification lessons, both learned the hard way here:** "zero allocated bytes" ≠ "not linked" (check the archive-inclusion section), and a proposed `#if` around a virtual override may not compile — check whether the member satisfies a pure virtual before prescribing its removal.

## Suggested order

1. **Camera SCCB debug probe deletion** (`System_Camera_DVP.cpp:305-323`) — trivially safe, unlocks the Wire work.
2. **Wire references** (`System_I2C.cpp`, `System_I2C_Manager.cpp`) → 18,020 B with step 1.
3. **libsodium component patch** → 24,371 B, biggest single item, but touches a managed component.
4. **SmartConfig patch** → 3,079 B (patch `WiFiGeneric.cpp`, not `WiFiSTA.cpp`).
5. Hygiene: `Preferences` global, `Adafruit_GFX` include gating.

Steps 1-4 ≈ **45.5 KB** on top of the 40 KB already reclaimed. Note that 3 and 4 are patches to vendored/managed components, which the project already does under `docs/arduino-local-patches/` — they need the patch-snapshot + verify workflow, not just an edit.
