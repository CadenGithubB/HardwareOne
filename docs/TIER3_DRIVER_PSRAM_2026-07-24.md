# Tier 3 — driver objects → PSRAM (placement-new): change & downstream-effects report

**Date:** 2026-07-24 · **Status:** analysis + the two WORTH-IT conversions IMPLEMENTED (built green
FeatherS3, uncommitted, awaiting HW). 9 driver objects analyzed + adversarially verified (18 agents).

> **IMPLEMENTED:** `gMLX90640` (i2csensor_mlx90640.cpp: new@527→ps_alloc+placement-new, 2 deletes@531/1462→
> `ps_delete`) and `gOledFileManager` (OLED_Mode_FileBrowser.cpp: new@625→placement-new, delete@1442→
> `ps_delete`), both `+#include <new>`. Grep-verified zero raw `delete`/plain-`new` survive. These are
> RUNTIME heap allocs, so the static `.dram0.*` map is unchanged — the ~9.8 KB is freed from the internal
> HEAP at runtime (when thermal runs / the Files browser is open). **Also implemented:** `gFilesFm`
> (G2_Page_Files.cpp: the G2 files-page's own FileManager) — `new@171→ps_alloc("g2.filemgr")+placement-new`
> in `ensureFm()`; **0 delete sites** (create-once-keep, so no `ps_delete` pair), `+#include System_MemUtil.h`
> + `<new>`. ~5 KB more when the G2 Files app is open. The five compiled-out sensors + `gDisplay` were
> skipped per §3. **Total Tier-3 realized: ~14.8 KB internal heap, conditional (thermal / OLED Files / G2 Files).**

## 0. Headline

Only **two** of the nine driver objects are a real internal-DRAM win **on the shipping FeatherS3
board**: `gMLX90640` (~4.7 KB, thermal) and `gOledFileManager` (~5 KB, OLED file browser) — **~9.8 KB
combined, both conditional** (resident only while thermal runs / the browser is used). Everything else
is either **compiled out on FeatherS3** (future-proofing that can't even be compile-tested there) or a
**mirage** (a tiny shell whose real DRAM cost is a library-internal `Adafruit_I2CDevice` that stays in
DRAM regardless), or the **hot framebuffer** (`gDisplay`, must stay internal).

| Object | On FeatherS3? | Real DRAM win | `delete` sites | Verdict |
|---|---|--:|:--:|---|
| **`gMLX90640`** | ✅ compiled (thermal) | **~4,748 B** | 2 | **WORTH-IT** |
| **`gOledFileManager`** | ✅ compiled (OLED) | **~5,028 B** | 1 (+1 ctor) | **WORTH-IT** |
| `gVL53L4CX` | ❌ `#if`'d out (TOF=0) | 0 (9.4 KB *if* enabled) | 5 | MARGINAL / future-proofing |
| `gDisplay` | ✅ compiled, **hot** | 96 B shell only | 3 (+1 missed ctor) | EXCLUDE (framebuffer) |
| `gBNO055` | ❌ `#if`'d out (IMU=0) | ~16 B | 4 | EXCLUDE |
| `gAPDS9960` | ❌ GC'd (APDS=0) | ~24 B | 2 | EXCLUDE |
| `gGamepadSeesaw` | ✅ compiled | ~20 B | 0 (2 ctor) | EXCLUDE (shell) |
| `gAnoSeesaw` | ❌ not compiled | ~24 B | 0 | EXCLUDE |
| `gPwmDriver` | ❌ `#if`'d out (SERVO=0) | 0 (~12 B) | 1 | EXCLUDE |

## 1. The change, per object (the mechanical shape)

The proven template is `gPA1010D`, and the shared helpers already exist in `System_MemUtil.h`:
`ps_new<T>(pref, args…)` (= `ps_alloc(sizeof(T)) + placement-new`) and `ps_delete<T>(p)`
(= `p->~T(); free(p)`). So each conversion is:

1. Add `#include <new>` and `#include "System_MemUtil.h"` (several driver files have **neither**).
2. **Construction** `gT = new T(args)` → `void* b = ps_alloc(sizeof(T), AllocPref::PreferPSRAM, "tag"); if(!b){fail-closed} gT = new(b) T(args);` (or `ps_new<T>` if the alloc-tracker tag isn't needed). **The post-`new` null check becomes dead** — placement-new never returns null; move the null check to `b`.
3. **Every** `delete gT` → `ps_delete(gT); gT = nullptr;`.
4. Access sites (`gT->method()`) are **unchanged** — the pointer type stays `T*`.

## 2. Downstream effects (the actual question)

### 2a. The #1 effect: the teardown-conversion surface + the "miss-one" trap
Placement-new'd objects come from `ps_alloc` (a `heap_caps_malloc` wrapper). A surviving raw
`delete gT` runs the **global** `operator delete` on that block — destructor paired with the wrong
allocator = **heap corruption**. So **every** construction and destruction site must convert *in
lockstep*, and the edit must be **grep-verified** (`grep -n 'delete gT'` → zero; `new T` → only the
placement form), not just compile-tested. The surface is larger than it looks: `gVL53L4CX` has **5**
delete sites (4 inside init failure-branches + 1 in the task exit), `gBNO055` **4**, `gDisplay` **3**.

**The verifiers caught sites the finders missed** — proving the trap is real even under careful
analysis: `gDisplay` has a **second live construction site** at `OLED_Utils.cpp:4625` (boot-animation
path) beyond `HAL_Display.cpp:64`; `gGamepadSeesaw` has **two** construction sites (`:199`, `:285`).

### 2b. Board-gated delete sites hide from a green build
`gDisplay`'s delete at `OLED_Utils.cpp:3527` is in the `#else` (SPI/TFT) branch — **compiled out on
FeatherS3**. A green FeatherS3 build will *not* flag it if it's missed. Same class of hazard as
`docs`-noted "board-gated code hides compile breaks." Any conversion touching a multi-display or
multi-sensor file must be reviewed across **all** board branches, not just the one that compiles here.

### 2c. Most objects can't be compile-tested on the primary board
`gVL53L4CX`, `gBNO055`, `gAPDS9960`, `gAnoSeesaw`, `gPwmDriver` are **preprocessed out** on FeatherS3
(`CUSTOM_ENABLE_TOF/IMU/APDS=0`, `INPUT_DEVICE_TYPE=SEESAW`, `ENABLE_SERVO=0`). Converting them = editing
code you cannot build here; it only realizes on an `I2C_LEVEL_FULL` / sensor-installed build. Treat as
future-proofing and validate on a board that has the sensor.

### 2d. The "shell vs sub-allocation" honesty check
Adafruit sensor drivers `new` an `Adafruit_I2CDevice` (~16–24 B) inside `begin()` via a plain library
`new` → it lands in **internal DRAM** (<16 KB, ALWAYSINTERNAL) no matter where the parent shell lives.
So for the thin shells (`gBNO055` ~16 B, `gAPDS9960` ~24 B, seesaw ~20 B, `gPwmDriver` ~12 B) the move
frees only the tiny shell while the real per-object cost stays in DRAM — **a mirage**. Only `gMLX90640`
(the ~4.7 KB `paramsMLX90640` calibration: `alpha/offset/kta/kv[768]` — inline) and `gOledFileManager`
(the ~4.9 KB `cachedEntries[64]` — inline POD, no `String`) carry their bulk **inside the shell**.

### 2e. PSRAM-latency downstream (only matters for the two winners + `gDisplay`)
- `gDisplay`: **~2,259 access sites**, hundreds of dereferences per render frame + a 1024-B framebuffer
  scan per I2C push. Putting the shell in PSRAM taxes the single hottest object in the firmware → EXCLUDE.
- `gMLX90640`: the frame calc walks the 768-px calibration, but only at the thermal frame rate (a few
  Hz) → verified **per-poll-cold**, acceptable.
- `gOledFileManager`: touched per render frame *only while the Files browser is on screen* → cold.

### 2f. Constructor side-effects & lifecycle (checked, all clear)
Every ctor stores config only (addr/wire/memset) — no I2C — so placement-new in PSRAM does no hardware;
`begin()` does the I2C afterward. No MCU ISR/DMA dereferences any of these objects (sensor "interrupts"
are the chip's own GPIO pins polled over I2C). Create/destroy is serialized by the sensor-enable flags +
per-bus I2C mutex; the PSRAM swap changes only alloc/dealloc calls, not ordering — preserve any
`StopMeasurement()`-before-destroy sequencing.

### 2g. Pre-existing bugs surfaced (orthogonal, flag-only, NOT regressions)
`Adafruit_MLX90640` / `_APDS9960` / `_seesaw` / `_PWMServoDriver` don't `delete i2c_dev` on some paths
(the implicit/compiler destructor doesn't free it) → a small `Adafruit_I2CDevice` leak on those paths.
Pre-existing; unaffected by this refactor.

## 3. Recommendation

- **Do:** `gMLX90640` + `gOledFileManager` — the only two that free real DRAM on the shipping board
  (~9.8 KB conditional), both compiled here so the edit is **compile- and HW-testable**, both full-shell
  wins, both small teardown surfaces (2 and 1 sites). Note `gOledFileManager`'s sole delete
  (`OLED_Mode_FileBrowser.cpp:1442`, `resetOLEDFileBrowser`) is currently **dead code** (zero callers) —
  it must still convert so it doesn't arm a landmine for whoever re-wires that teardown later.
- **Skip:** the five compiled-out sensors (future-proofing on code we can't verify here; the miss-one
  risk isn't worth it until a board actually needs them) and the thin-shell mirages.
- **Never:** `gDisplay` (hot framebuffer).

If we later want the future-proofing anyway, do it per-sensor on a build that compiles it, grep-verified.
