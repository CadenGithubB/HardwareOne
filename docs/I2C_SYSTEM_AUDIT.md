# I2C System Audit + Codebase Modularity Report

_Generated 2026-06-08. Scope: `components/hardwareone`. Read-only audit; findings + work plan below._

## Part 1 — The I2C System

### Architecture: solid and modular
| Layer | Status | Notes |
|---|---|---|
| `I2CDeviceManager` (singleton, dual-bus) | ✅ Modular | per-bus mutex / Wire / clock-stack / metrics; one `getInstance()` |
| Transaction wrappers (4-arg legacy → bus 0, 5-arg bus-aware) | ✅ Unified | both delegate to `executeTransaction()` |
| `I2CDevice` (per addr+bus health, adaptive timeout, auto-disable) | ✅ Modular | |
| Per-bus clock stack (8-deep push/pop) | ✅ Modular | |
| Bus recovery (per-bus, 9-clock toggle) | ✅ Modular | per-bus as of 2026-06-08 |
| Scan/discovery + CPU1 ISR pinning | ✅ Unified | |
| `System_PollPause` (per-bus pause) | ✅ Modular | extracted + per-bus 2026-06-08 |

The core I2C infra is one of the cleanest subsystems. The lag is in the **drivers**, not the framework.

### Driver migration status — dual-bus port ~half done
What matters (for dual-bus capability AND the pause gate) is the **poll-path** transaction:

| Driver | Poll transaction | Bus capability |
|---|---|---|
| seesaw, ano_encoder, ds3231, pa1010d, rda5807 | 5-arg bus-aware (`gSettings.<x>Bus`) | ✅ migrated — either bus |
| max17048 (fuel gauge) | 5-arg bus-aware | ✅ migrated (battery-driven, no poll-gate) |
| sths34pf80 (presence) | init 5-arg, **poll 4-arg → bus 0** | ⚠️ hybrid (gate fixed → bus 0) |
| bno055, mlx90640, vl53l4cx, apds9960 | 4-arg / direct `Wire1` | ❌ legacy, bus-0-locked |
| pca9685 (servo) | 4-arg, no poll loop | ❌ legacy, bus-0-locked |

6 migrated, 1 hybrid, 5 legacy-bus-0. The legacy/hybrid drivers hardcode bus 0; their `gSettings.<x>Bus` setting is not honored (header documents: "an unmigrated driver works ONLY when its configured bus is 0"). All legacy drivers are compiled-out by default.

### Utilization: clean, with small warts
- Consumers (web, OLED/HAL, G2, ESP-NOW, battery, automation) go through the bus-aware wrappers / manager singleton.
- ~~Dead code: OLED_TRANSACTION macro~~ — **CORRECTION:** the audit-agent flagged this as dead, but it is used ~20× (first-time-setup, setup wizard, network mode). It was 4-arg legacy (bus 0) → took **bus 0's mutex while the Adafruit driver wrote to bus 1's wire** (wrong-bus mutex/clock). ✅ Fixed: now 5-arg bus-aware via `gSettings.oledBus`.
- **Latent bus bug:** `OLED_Utils.cpp` input auto-start used legacy 3-arg `i2cPingAddress(...)` (implicit bus 0); wouldn't detect an input device on bus 1. ✅ Fixed: now pings `gSettings.inputBus`.
- **Direct `Wire`:** only `System_Camera_DVP.cpp` (camera SCCB on fixed pins, isolated boot probe) — acceptable.

### Legacy ESP-IDF driver
Stack sits on deprecated `driver/i2c.h`. Migrating to `driver/i2c_master.h` is medium effort with a real blocker: the CPU1-ISR-pinning + glitch-filter coexistence fixes are legacy-driver-specific. **Recommendation:** migrate the 5 legacy *drivers* to the bus-aware wrappers first (cheap, removes bus-0 lock); treat the ESP-IDF driver swap as a separate deliberate project.

> **DONE (v0.95.5, 2026-06-09):** the ESP-IDF driver swap shipped — upgrading IDF 5.3.1→5.5.1 auto-switches Arduino's `Wire` to the `i2c_master` (`-ng`) HAL, so the firmware is off the legacy driver. The CPU1-ISR-pin + glitch-filter coexistence knobs carried over (glitch now applied by the `-ng` HAL). HW-validated on FeatherS3 (glasses+gamepad, no Int-WDT) and XIAO Sense. See `idf6-p4-roadmap/I2C_MASTER_MIGRATION_PLAN.md`.

## Part 2 — Other lagging / non-modular sections

### Inline-`extern` anti-pattern (pervasive — same class as the old pause idiom)
| Symbol | Re-declared in | Should live in |
|---|---|---|
| `executeOLEDCommand` | 15 files | OLED-commands header |
| `httpd_handle_t server` | 13 files | web-server header |
| `EspNowState* gEspNow` | 12 files | ESP-NOW header |
Worst files: `System_Utils.cpp`, `OLED_Utils.cpp`, `WebServer_Server.cpp`, `System_ESPNow.cpp`.

### Other deprecated APIs
- `System_Battery.cpp` — legacy `driver/adc.h`
- `System_Camera_DVP.cpp` — legacy GPIO/LEDC

### Other smells
- Manual save/restore (not RAII): `i2csensor_bno055.cpp:~157` `gImuEnabled` prev/restore.
- Duplicate mechanisms: `bleEnableStream()` vs `bleIdfEnableStream()`.

### Subsystem modularity ranking
```
Cleanest:  Battery · AuthIdentity · I2C/PollPause · Notifications · Debug
Good:      WiFi · Settings
Messy:     OLED (OLED_Utils.cpp ~6.5k LOC + scattered state) · WebServer/WebPage
CRITICAL:  ESP-NOW (System_ESPNow.cpp ~14.2k LOC, 14 header externs)
           G2 Glasses (G2_Glasses.cpp ~17.4k LOC FSM, per-page scattered globals)
```
**Top 3 worth the I2C-style treatment:** G2 Glasses, ESP-NOW, OLED — extract a manager singleton, split monoliths, stop inlining externs.

## Work plan / status

### Done (this session)
- ✅ Pause → `System_PollPause` module, per-bus, RAII, no inline externs
- ✅ `sths34pf80` pause gate corrected (bus 0)
- ✅ `System_Automation.cpp` missing-include — chip spawned

### Done (low-risk, contained — completed 2026-06-08)
- [x] `OLED_TRANSACTION` macro made bus-aware (was a live bus-0-mutex bug, NOT dead code)
- [x] OLED input auto-start now honors `gSettings.inputBus`
- [x] `executeOLEDCommand` — 25 inline externs across 13 files removed (already declared in `OLED_Utils.h`)
- [x] `server` (httpd_handle_t) — 13 inline externs across 7 files → new lightweight `WebServer_Handle.h`
- [x] `gEspNow` — 12 inline externs across 7 files removed (already declared in `System_ESPNow.h`)

### Deferred (large / own effort / needs opt-in)
- Migrate 5 legacy I2C drivers to bus-aware wrappers (untestable without the hardware; low priority)
- ~~ESP-IDF `driver/i2c_master.h` migration (medium, coexistence blocker)~~ — **DONE v0.95.5** (via IDF 5.5.1 upgrade)
- `System_Battery` legacy ADC migration (functional today; risky to touch)
- Monolith refactors: G2 Glasses, ESP-NOW, OLED manager extraction (each a multi-session project)
