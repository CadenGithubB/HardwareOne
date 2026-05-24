# SensorCacheGuard — Implementation Plan

**Status:** Awaiting approval. No code changes yet.
**Scope:** Add `SensorCacheGuard` RAII helper to `System_Mutex.h` and migrate
all sensor-cache mutex callsites to use it.

---

## 1. Why

Every sensor in the codebase has a cache struct (`ImuCache`, `TofCache`,
`ThermalCache`, `GamepadCache`, `APDSCache`, `GPSCache`, `RTCCache`,
`PresenceCache`, `FMRadioCache`) with a `SemaphoreHandle_t mutex` as its
first field. **61 call sites** across **19 files** access these caches
through a manually-bookended `xSemaphoreTake` / `xSemaphoreGive` pair:

```cpp
if (gImuCache.mutex && xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
  // ... read/write gImuCache.* ...
  xSemaphoreGive(gImuCache.mutex);
}
```

**Three problems with this pattern:**

1. **Latent mutex leaks on early return.** Any `return`, `break`, or
   exception inside the held block bypasses `xSemaphoreGive` and pins the
   mutex forever. A spot-check of a few drivers shows at least one
   callsite already has an early-return path inside the held block; more
   are likely. The pattern compiles fine and runs fine — until the
   leak trigger fires.
2. **Verbose duplication.** Same 4-line idiom repeated 61 times.
3. **No reentrancy check.** A future helper called inside the held
   block that also tries to lock would deadlock. (The existing
   `FsLockGuard` / `I2sMicLockGuard` / `JsonBufferGuard` in
   `System_Mutex.h` all handle reentrancy. Sensor caches don't.)

The codebase already follows this RAII pattern for 9 other mutexes
(`FsLockGuard`, `I2cLockGuard`, `I2sMicLockGuard`, `JsonBufferGuard`,
`MeshRetryGuard`, `FileTransferGuard`, `TopoStreamsGuard`,
`ExecIdentityGuard`, `NotificationContextGuard`). Sensor caches are the
last holdout — they were missed historically because each instance lives
in its own struct rather than as a centrally-managed resource.

---

## 2. Design

### 2.1 New type — added to `System_Mutex.h`

```cpp
/**
 * SensorCacheGuard - RAII guard for per-sensor cache mutexes.
 *
 * Sensor cache structs (gImuCache, gTofCache, gGpsCache, ...) all use the
 * convention `SemaphoreHandle_t mutex = nullptr;` as their first field. This
 * guard takes that mutex by handle. Unlike the other guards in this file, it
 * has no "the" mutex — each sensor cache has its own, so the handle is
 * stored in the guard for destruction.
 *
 * Timeouts are caller-specified because reads (UI, snapshot) and writes
 * (driver task) have different patience: UI uses 5ms ("show '...' if busy"),
 * writes use 100ms ("must succeed"). The default of CACHE_MUTEX_TIMEOUT_MS
 * (100ms) matches the most common driver-side usage.
 *
 * Usage:
 *   {
 *     SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(5), "g2.imuRead");
 *     if (g.held) {
 *       // read/write gImuCache.* fields safely
 *       if (errorCondition) return "ERROR";  // ← marker auto-released
 *     }
 *   }  // marker auto-released here too
 */
struct SensorCacheGuard {
  bool held;
  SemaphoreHandle_t mutex;  // remembered for destructor (each cache has its own)

  explicit SensorCacheGuard(SemaphoreHandle_t m,
                             TickType_t timeoutTicks = pdMS_TO_TICKS(CACHE_MUTEX_TIMEOUT_MS),
                             const char* owner = nullptr);
  ~SensorCacheGuard();

  // Non-copyable, non-movable
  SensorCacheGuard(const SensorCacheGuard&) = delete;
  SensorCacheGuard& operator=(const SensorCacheGuard&) = delete;
};
```

### 2.2 Implementation — added to `System_Mutex.cpp`

```cpp
SensorCacheGuard::SensorCacheGuard(SemaphoreHandle_t m,
                                    TickType_t timeoutTicks,
                                    const char* owner)
    : held(false), mutex(m) {
  if (mutex) {
    // Reentrant-safe (matches existing FsLockGuard / I2sMicLockGuard convention)
    if (isHeldByCurrentTask(mutex)) return;
    if (xSemaphoreTake(mutex, timeoutTicks) == pdTRUE) {
      held = true;
    }
  }
}

SensorCacheGuard::~SensorCacheGuard() {
  if (held && mutex) {
    xSemaphoreGive(mutex);
  }
}
```

### 2.3 Migration pattern — before / after

**Before:**
```cpp
if (gImuCache.mutex && xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
  if (gImuCache.imuDataValid) {
    snprintf(out, cap, "Y%d P%d R%d", ...);
  } else {
    snprintf(out, cap, "no data");
  }
  xSemaphoreGive(gImuCache.mutex);
} else {
  snprintf(out, cap, "...");
}
```

**After:**
```cpp
SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(5), "g2.imuRead");
if (g.held && gImuCache.imuDataValid) {
  snprintf(out, cap, "Y%d P%d R%d", ...);
} else if (g.held) {
  snprintf(out, cap, "no data");
} else {
  snprintf(out, cap, "...");
}
```

Net effect: drops the explicit `xSemaphoreGive` line, makes early `return`
safe, preserves per-callsite timeout, adds the `owner` debug string.

### 2.4 Decisions baked in

| Decision | Choice | Why |
|---|---|---|
| Where it lives | `System_Mutex.h` / `.cpp` | Matches the 7 existing guards |
| Constructor takes | `SemaphoreHandle_t` (not the cache struct) | Single overload; works uniformly for all 9 cache types; no template instantiation per cache type |
| Default timeout | `pdMS_TO_TICKS(CACHE_MUTEX_TIMEOUT_MS)` = 100ms | Matches most common driver-write usage |
| Per-callsite timeout | Configurable via constructor | Preserves existing variance (5/10/50/100ms have different intent) |
| `owner` parameter | Optional, defaults to `nullptr` | Matches existing guard convention; useful for diagnostics |
| Reentrancy check | Yes (`isHeldByCurrentTask`) | Matches existing guard convention; cheap; future-proof |
| `held` accessor | Public `bool held` field | Matches existing guard convention |
| Copy / assignment | Deleted | Matches existing guard convention |
| Move | Deleted | RAII guards shouldn't move (would require extra null-check in dtor) |

---

## 3. Migration scope — every callsite

**Total: 61 callsites across 19 files.**

### 3.1 Driver-side writes (i2csensor-*.cpp) — 22 sites

These are the cache writers inside the sensor task loops. Higher timeouts
(50-100ms) because the task NEEDS to acquire the lock to update the cache.

| File | Sites | Timeouts |
|---|---|---|
| `i2csensor-bno055.cpp` | 5 | 100, 50, CACHE_MUTEX_TIMEOUT_MS, 10, 100 |
| `i2csensor-sths34pf80.cpp` | 4 | 100, 100, 50, 50 |
| `i2csensor-vl53l4cx.cpp` | 3 | 100, 50, CACHE_MUTEX_TIMEOUT_MS |
| `i2csensor-rda5807.cpp` | 3 | 50, 50, 50 |
| `i2csensor-ds3231.cpp` | 3 | 50, 50, 100 |
| `i2csensor-seesaw.cpp` | 2 | 100, 50 |
| `i2csensor-pa1010d.cpp` | 2 | 100, 50 |
| `i2csensor-apds9960.cpp` | 2 | 100, 50 |

### 3.2 Cross-cutting readers — 39 sites

| File | Sites | Timeouts | Notes |
|---|---|---|---|
| `G2_Page_Sensors.cpp` | 9 | 5 (all) | UI snapshot reads for the G2 sensor page |
| `System_SensorLogging.cpp` | 6 | 10 (all) | Sensor data CSV logging |
| `OLED_SetupWizard.cpp` | 5 | 10 (all) | Gamepad reads during setup |
| `WebPage_Sensors.cpp` | 4 | 50 (all) | Web API JSON builders |
| `System_Automation.cpp` | 4 | 50 (all) | Automation rule evaluation |
| `Bluetooth.cpp` | 3 | 10 (all) | BLE streaming readers — **see special case below** |
| `OLED_Utils.cpp` | 2 | 10 (all) | OLED status displays |
| `System_FirstTimeSetup.cpp` | 1 | 10 | Gamepad during first-time setup |
| `System_SetupWizard.cpp` | 1 | 10 | Gamepad during wizard |
| `OLED_Mode_Map.cpp` | 1 | 5 | Map mode joystick read |
| `System_I2C.cpp` | 1 | `timeout` param | **see special case below** |

### 3.3 Special cases

**`System_I2C.cpp:728` (lockThermalCache helper).** This file has a manual
helper:
```cpp
bool lockThermalCache(TickType_t timeout) {
  return gThermalCache.mutex && (xSemaphoreTake(gThermalCache.mutex, timeout) == pdTRUE);
}
void unlockThermalCache() {
  if (gThermalCache.mutex) xSemaphoreGive(gThermalCache.mutex);
}
```
Used by 2 callers (`i2csensor-mlx90640.cpp:992`, `WebPage_Sensors.cpp:123`).
**Leave the helper as-is for this migration**, since:
- The helper itself is a public API with manual lock/unlock semantics
- Migrating its 2 callers to use `SensorCacheGuard` directly would mean
  deleting the helper, which is a bigger refactor
- The current helper is correct (not buggy)
- A future cleanup can replace the helper-pair with direct `SensorCacheGuard`
  use, but that's out of scope for this plan

**`Bluetooth.cpp:2436, 2451, 2464` (parallel struct re-declarations).**
This file has its own `extern struct ThermalCache { ... } gThermalCache;`
declarations (and same for TofCache, ImuCache) with field layouts that
differ from the canonical structs. The 3 `xSemaphoreTake` calls themselves
are safe to migrate — they access `gThermalCache.mutex` which is the first
field in both the canonical and local struct definitions, so the offset is
the same regardless. **The migration will fix the mutex pattern at these 3
sites without touching the underlying parallel-struct issue** (that's
item A2 from the improvements list — separate fix).

---

## 3.4 Three patterns to migrate (triple-check finding)

The 61 callsites are NOT all the same shape. Triple-check audit found three distinct patterns; the migration treats each differently:

### Pattern 1 — Block-scoped take/give (59 callsites)

```cpp
if (gImuCache.mutex && xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(N)) == pdTRUE) {
  // ... read/write cache ...
  xSemaphoreGive(gImuCache.mutex);
}
```

**Migration:**
```cpp
SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(N), "purpose");
if (g.held) {
  // ... read/write cache ...
}  // dtor releases
```

Some Pattern 1 callsites have explicit `xSemaphoreGive(...); return X;` early-exits
(e.g. `sths34pf80.cpp:213`, `vl53l4cx.cpp:517`). **The explicit give becomes
redundant** after migration — the dtor handles it on every return path.
Migration drops the explicit give.

### Pattern 3 — Function-scoped with intentional early release (2 callsites)

```cpp
if (!gImuCache.mutex || xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
// ... copy cache values to local vars ...
xSemaphoreGive(gImuCache.mutex);   // ← release BEFORE the rest of the function
// ... math on local vars (intentionally NOT holding the lock) ...
```

The two callsites:
- **`i2csensor-bno055.cpp:631`** (imuActionDetect) — copies accelX/Y/Z, gyroX/Y/Z, roll, pitch into locals; releases; then does sqrt + history-buffer math without the lock
- **`i2csensor-vl53l4cx.cpp:409`** (tofPoll) — copies object data into the cache; releases; then calls `VL53L4CX_ClearInterruptAndStartMeasurement()` hardware without the lock

**Migration uses explicit block scope to preserve the early-release semantics:**

```cpp
{
  SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(10), "imu.actionDetect");
  if (!g.held) return;
  // ... copy cache values to local vars ...
}  // dtor releases here, BEFORE the math runs
// ... math on local vars (still not holding the lock) ...
```

Without the explicit `{}` block, the guard would hold the lock for the entire
function, regressing the intentional early-release behavior.



Smallest-blast-radius first; build between each step so any regression is
bisectable.

| Step | Action | Sites | Risk |
|---|---|---|---|
| 0 | Add `SensorCacheGuard` to `System_Mutex.{h,cpp}` | 0 | None — pure addition |
| 1 | Build verify (no callers yet — should be a no-op build) | 0 | None |
| 2 | Migrate single-callsite files | 3 (`OLED_Mode_Map.cpp`, `System_FirstTimeSetup.cpp`, `System_SetupWizard.cpp`) | Trivial |
| 3 | Migrate `OLED_Utils.cpp` | 2 | Low |
| 4 | Migrate `OLED_SetupWizard.cpp` | 5 | Low (all same sensor, same timeout) |
| 5 | Migrate `System_Automation.cpp` | 4 | Low |
| 6 | Migrate `System_SensorLogging.cpp` | 6 | Low |
| 7 | Migrate `WebPage_Sensors.cpp` | 4 | Low |
| 8 | Migrate `Bluetooth.cpp` | 3 | Low (note: doesn't fix the parallel-struct issue, just the mutex pattern) |
| 9 | Migrate `G2_Page_Sensors.cpp` | 9 | Medium — densest, but UI-only |
| 10 | Migrate each `i2csensor-*.cpp` driver, one at a time | 22 | Medium — driver hot paths |
| 11 | Final audit + build verify | 0 | None |

**Build verify after each step.** This is more frequent than the initial
audit suggested, but the safety win is worth it — if step 9's 9-site
migration introduces a subtle bug, only that file is in flight, and
bisecting between steps is trivial.

---

## 5. Risks and mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Build breaks due to incorrect substitution | Low-medium | Build after each step; each Edit is precise |
| Behavior change due to timeout drift | Low | Every Edit preserves the per-callsite timeout literal; the default is for callers that don't specify |
| Reentrant deadlock newly exposed | Very low | `isHeldByCurrentTask` check matches existing guard convention; no current callers nest |
| Bluetooth.cpp parallel-struct field access still wrong | N/A (pre-existing) | Migration doesn't make this worse — orthogonal issue |
| Performance regression from added member field (`mutex`) | Negligible | 4 bytes on stack per guard; same total work as inline pattern |
| `owner` debug string adds memory pressure | None | Optional parameter; defaults to `nullptr` |

---

## 6. What the guard does NOT change

To be explicit about scope:

- **Does not** change the cache struct definitions (the `gImuCache` etc.
  layouts stay identical).
- **Does not** change which mutexes exist or how they're created
  (`xSemaphoreCreateMutex()` in driver init code stays).
- **Does not** centralize the cache mutex inventory (each cache keeps its
  own mutex; the guard just helps callers use it safely).
- **Does not** rename `CACHE_MUTEX_TIMEOUT_MS` or move it again
  (already canonical in `System_Mutex.h` from earlier work).
- **Does not** address the `Bluetooth.cpp` parallel-struct field-layout
  bug (item A2 — separate ticket).
- **Does not** touch the `lockThermalCache` / `unlockThermalCache` helper
  pair (deferred — current callers are correct).

---

## 7. Verification

**After step 0 (guard added, no callers):**
- `idf.py build` succeeds with zero callsite changes
- New symbol resolves correctly (smoke test by reading any callsite's
  header file)

**After each migration step:**
- `idf.py build` succeeds
- Spot-check the diff: `git diff <file>` shows only the take/give pattern
  replaced
- Audit grep: `grep -cE "xSemaphoreTake\(g[A-Za-z]*Cache\.mutex" <file>`
  should drop to 0 in the migrated file

**Final audit:**
- `grep -rnE "xSemaphoreTake\(g[A-Za-z]*Cache\.mutex" components/hardwareone/`
  returns only the `lockThermalCache` helper at `System_I2C.cpp:728` (the
  intentionally-deferred case)
- `grep -rnE "xSemaphoreGive\(g[A-Za-z]*Cache\.mutex" components/hardwareone/`
  returns only the `unlockThermalCache` helper
- Binary size: small change expected (slight reduction from removing repeated
  inline pattern, possible slight increase from `owner` string literals if used)
- Build: clean, partition usage stable

---

## 8. Time estimate

| Work | Estimate |
|---|---|
| Add guard to System_Mutex.{h,cpp} + build verify | 5 min |
| Migrate small files (steps 2-3) | 15 min |
| Migrate medium files (steps 4-9) | 45 min |
| Migrate driver files (step 10) | 45 min |
| Final audit + build | 10 min |
| **Total** | **~2 hours** |

---

## 9. Open questions for review

1. **`owner` parameter usage.** Should I supply meaningful owner strings
   per callsite (e.g. `"g2.imuRead"`, `"sensorLog.write"`,
   `"automation.thermalEval"`), or omit them (default `nullptr`) for the
   initial migration? Meaningful strings cost ~10-20 bytes of flash per
   unique string but make `xSemaphoreGetMutexHolder`-based diagnostics
   useful. **Recommendation:** supply meaningful strings — the cost is
   trivial and the diagnostic value compounds over time.

2. **One commit or per-step commits.** I can do this in one big commit
   ("SensorCacheGuard + 61-callsite migration") or one commit per step
   for bisectability. **Recommendation:** one commit. Each step is
   mechanically safe; bisecting within the commit is rare in practice.

3. **Deletion of `lockThermalCache` / `unlockThermalCache`.** Should I
   include that in this migration (deleting the helper, migrating its 2
   callers directly to `SensorCacheGuard`) or defer? **Recommendation:**
   defer. It's the kind of cleanup that's better as a follow-up once the
   pattern is established.

---

## 10. Approval needed before I start

Please review and respond:
- **Approve** to proceed exactly as planned, or
- **Modify** any of: timeout default, owner-string policy, commit
  granularity, special-case handling, execution order, or
- **Deny** if there's a deeper concern.

This document will not be committed — it's a planning artifact. Once you
approve, I'll execute and report progress per step.
