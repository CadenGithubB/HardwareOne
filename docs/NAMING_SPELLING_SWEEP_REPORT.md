# Naming + Spelling Sweep — B1 / B2 / B3 Findings

Pre-work investigation report. Pure read-only audit; no code changes yet.

---

## TL;DR

| Task | Surface | Risk | Recommendation |
|------|---------|------|----------------|
| **B1** 3-letter abbrev casing (gGPS→gGps, gRTC→gRtc, gAPDS→gApds, gIMU→gImu, gOLED→gOled) | **993 refs across 16 identifiers** | **LOW** — purely internal C++ symbols, never serialized | Do it; matches existing convention |
| **B2** `fsMutex` → `gFsMutex` | **15 refs in 3 files** | **TRIVIAL** | Do it |
| **B3** Spelling normalization in user strings | **Much smaller than implied** — see below | **MEDIUM (footguns)** | Do `Wifi` and most `ESPNOW` user-strings; **DO NOT mass-rename `bluetooth`** |

User's stated B3 counts (Wifi 27, ESPNOW 88, bluetooth 49) overstate the renamable surface significantly. Detailed below.

---

## B1 — 3-letter abbreviation casing

### Distinct identifiers and per-name surface

| Old | New | Refs |
|-----|-----|-----:|
| `gGPSCache` | `gGpsCache` | 91 |
| `gGPSSubmenu` | `gGpsSubmenu` | 2 |
| `gGPSSubmenuCount` | `gGpsSubmenuCount` | 3 |
| `gRTCCache` | `gRtcCache` | 58 |
| `gRTCWatermarkMin` | `gRtcWatermarkMin` | 5 |
| `gRTCWatermarkNow` | `gRtcWatermarkNow` | 4 |
| `gAPDSCache` | `gApdsCache` | 67 |
| `gIMUActions` | `gImuActions` | 94 |
| `gIMUWatermarkMin` | `gImuWatermarkMin` | 9 |
| `gIMUWatermarkNow` | `gImuWatermarkNow` | 8 |
| `gOLEDConfirmState` | `gOledConfirmState` | 32 |
| `gOLEDConsole` | `gOledConsole` | 46 |
| `gOLEDEspNowState` | `gOledEspNowState` | 335 |
| `gOLEDFileManager` | `gOledFileManager` | 36 |
| `gOLEDKeyboardState` | `gOledKeyboardState` | 169 |
| `gOLEDMapRenderer` | `gOledMapRenderer` | 16 |
| **TOTAL** | | **975** |

(Counts include declarations, definitions, externs, and call sites.)

### Why this is safe

1. **No string-reflection.** I grepped for `"gGPS"`, `"gRTC"`, `"gAPDS"`, `"gIMU"`, `"gOLED"` and `"fsMutex"` as string-literal substrings — **zero hits**. These names exist only as C++ symbols.

2. **No persistence dependency.** None of these identifiers appear as NVS keys, JSON field names, MQTT topic fragments, BLE characteristic UUIDs, file path components, or CLI command tokens.

3. **Matches existing convention.** The codebase already has many camelCase neighbors:
   - `gOledDialog`, `gOledEnabled`, `gOledList`, `gOledPairingRibbon`, `gOledProgress`, `gOledToast`
   - `gWifiNetworkCount`, `gWifiNetworks`, `gWifiPendingDeadlineMs`, `gWifiUserCancelled`
   - `gEspNowAppPing`, `gEspNowCapturePart`, `gEspNowHbTaskHandle`, `gEspNowRxRing` (8+ neighbors)
   - The doc comment in `System_Mutex.h:114-115` is already half-renamed: it writes
     `gImuCache, gTofCache, gGpsCache, ... gApdsCache, gGamepadCache, gRTCCache, ...`
     — three new-style names side-by-side with one old-style (`gRTCCache`).

4. **No phantom conflicts.** Two apparent "conflicts" (`gGpsCache`, `gApdsCache`) turned out to be that same doc comment in `System_Mutex.h` — the comment uses the *new* names aspirationally. Renaming will make code match its own documentation.

### Downstream surfaces verified clean

- `extern` declarations (in `System_SensorStubs.h`, `i2csensor-*.h`, `OLED_Utils.h`, etc.) — will need to be updated alongside the rename. All co-located in the same `find/replace`.
- The `gIMUWatermarkNow/Min` pair has an in-function extern at `System_Utils.cpp:2080` — must also update.
- Header guards / file names — unaffected (we already addressed those in commit `13a0629`).

### Files touched (estimate)

~50-70 files. Mass `find … -exec sed -i` is appropriate; sequencing isn't required because each rename is independent within its identifier.

### Risk verdict: **LOW**.

---

## B2 — `fsMutex` → `gFsMutex`

### Scope

- **15 references total** across 3 files: `System_Mutex.cpp` (13), `System_Mutex.h` (1 extern), `HardwareOne.cpp` (1 use).
- The user's stated "~30 callsites" overestimates by 2×.

### Why

`fsMutex` is the only mutex in `System_Mutex.h` without the `g` prefix. All the others (`g_loraTaskSyncMutex`, `gI2cMutex`, `gSensorCacheMutex`, `gWatchdogMutex`, etc.) have it. Pure consistency cleanup.

### Risk verdict: **TRIVIAL**.

---

## B3 — Spelling normalization in user-facing strings

This is the task that needs the most scoping discipline. The user's stated counts conflate **user-visible labels** with **machine-readable identifiers**. Renaming the latter would break:
- Module slugs (Settings page tabs, OLED mode IDs, CLI module names)
- HTML `data-panel` / `id` attributes (paired with JS selectors)
- JSON field names (paired with server emit + client read)
- BLE GATT URN strings (`org.bluetooth.service.*`) — these are **IETF/SIG-assigned** identifiers
- CLI command tokens (parsed via `strcmp`)

### B3a: `Wifi` → `WiFi`

- **Stated count: 27. Actual instances of the exact substring `"...Wifi..."` in a string literal: 1.**
- That single hit is a **comment** at `G2_Page_Network.cpp:967` describing why a function is named `enqueueWifiRedrawFromCallback` (function-name discussion — not a user-facing string).
- The 27 the user saw is almost certainly counting identifier substrings like `enqueueWifiRedrawFromCallback`, `gWifiNetworks`, etc. — **all internal C++ symbols that follow the established `gWifi*` convention and should stay**.
- 285 occurrences of `WiFi` already exist (user-facing labels).

**Verdict:** No-op. The codebase is already consistent. Skip this part of B3.

### B3b: `ESPNOW` → `ESP-NOW` in user strings

- **Stated count: 88. Actual `"ESPNOW"` substring in string literals: 70.**
- Distinct contents (sample): `"ESPNOW App"`, `"ESPNOW: ON"`, `"ESPNOW: OFF"`, `"ESPNOW: Radio Off"`, `"ESPNOW Not Init"`, `"(ESPNOW not compiled)"`, `"[ESPNOW] ..."` log prefixes, `"[G2-ESPNOW-APP] ..."` log prefixes.
- **One special case:** `System_Debug.cpp:2176` has `if (flag & DEBUG_ESPNOW_CORE) return "ESPNOW";` — this is the **display name for the debug-flag category**, which surfaces in the web settings page. Renaming changes what users see in flag toggles. Compare to the actual Settings entries which use `"esp-now"` module-id and `"ESP-NOW"` (with hyphen) label already (`System_Settings.cpp:1366-1373`). Renaming to `"ESP-NOW"` here would actually fix an inconsistency.

**Identifier strings that MUST stay as `espnow` or `ESPNOW` (verified):**
| Where | String | Why |
|-------|--------|-----|
| `System_ESPNow.cpp:380` | `/system/espnow/devices.json` | File path |
| `System_ESPNow.cpp:381` | `/system/espnow/mesh_peers.json` | File path |
| `System_ESPNow.cpp:3480-3905` | `/espnow/received/...`, `/sd/espnow/...` | File paths |
| `System_ESPNow.cpp:10597` | `"espnow", "network.espnow"` | Settings module ID |
| `System_FeatureRegistry.cpp:222` | `{ "espnow", "ESP-NOW", ... }` | Feature ID (left); label (right) |
| `System_MemoryMonitor.cpp:76` | `{ "espnow", ... }` | Module budget key |
| `OLED_Utils.cpp:3772` | `if (slug == "espnow")` | Slug parser |
| `OLED_Utils.cpp:5442` | `"unavail.start.bluetooth"`, similar `unavail.start.espnow` | OLED translation key |
| `WebPage_Settings.h:1070` | `'esp-now':'ESP-NOW'` | Module-ID → display-name map |
| `WebServer_MigrationTool.cpp:60-61` | path constants | File paths |

These are NOT the strings we're renaming — they don't contain the literal substring `ESPNOW` in uppercase (they're already `espnow` lowercase or `esp-now` hyphenated). So the rename is bounded to the 70 user-string occurrences.

**Verdict:** Apply `ESPNOW → ESP-NOW` to all 70 string-literal occurrences. Includes log prefixes (we use `[ESPNOW]`, become `[ESP-NOW]`). Does **not** touch identifier strings (file paths, slugs, JSON keys) because none of them spell `ESPNOW` in uppercase.

### B3c: `bluetooth` → `Bluetooth` in user strings

This is **the dangerous one**. Counts:
- 84 lowercase `bluetooth` string-literal occurrences
- Of those, the breakdown by category:

| Category | Example | Action |
|----------|---------|--------|
| **BLE GATT service URN** (`org.bluetooth.service.*`) | `"org.bluetooth.service.heart_rate"` | **MUST NOT CHANGE** — IETF/Bluetooth-SIG standard |
| **Settings module ID** | `"bluetooth"` in setting-entry tuple, `"network.bluetooth"` JSON path | **MUST NOT CHANGE** |
| **Feature registry ID** | `{ "bluetooth", "Bluetooth", ... }` | **MUST NOT CHANGE** (left); right side is already "Bluetooth" |
| **Memory monitor key** | `{ "bluetooth", 61440, ... }` | **MUST NOT CHANGE** |
| **OLED mode slug** | `if (slug == "bluetooth") return OLED_BLUETOOTH;` | **MUST NOT CHANGE** |
| **CLI transport token** | `"login %s %s bluetooth"`, `transportStr == "bluetooth"` | **MUST NOT CHANGE** |
| **HTML data-attribute** | `data-panel='bluetooth'` (paired with JS that reads `c.bluetooth`) | **MUST NOT CHANGE** |
| **JS object key** | `if(c.bluetooth){...}`, `var bt=c.bluetooth;` | **MUST NOT CHANGE** |
| **JSON emit key** | `JsonObject bt = conn["bluetooth"].to<JsonObject>();` | **MUST NOT CHANGE** |
| **Source enum string** | `case SOURCE_BLUETOOTH: source = "bluetooth"` (transport name in JSON) | **MUST NOT CHANGE** |
| **Persistence key** | `bluetooth.peers`, `bluetooth.settings`, `bluetooth.settings.g2` | **MUST NOT CHANGE** |
| **Translation slug** | `"unavail.start.bluetooth"` | **MUST NOT CHANGE** |
| **Genuinely user-facing prose** | `"Invalid transport. Use: serial, display, or bluetooth"`, `"...bluetooth"` in help text | Could change but is **also a CLI tokens reference** — leaving lowercase here helps the user copy/paste the token verbatim |

**Verdict:** The total renamable surface for `bluetooth → Bluetooth` is essentially **zero or close to it**. Almost every lowercase `bluetooth` is either a machine token, a JSON key, a CSS/HTML attribute, or a CLI argument value. Mass-renaming would silently break setup wizard navigation, OLED mode switching, BLE peer persistence, dashboard panels, and CLI login.

**Recommendation: SKIP B3c entirely.** The cost (high; broad coupling) vastly exceeds the benefit (zero user-visible change after we deduct the must-not-rename categories).

---

## Recommended Execution Plan

### Approach: 3 commits, smallest blast radius first

#### Commit 1 — B2: `fsMutex → gFsMutex` (warm-up)
- Edit `System_Mutex.h`, `System_Mutex.cpp`, `HardwareOne.cpp`.
- Build verify.
- 5 minutes.

#### Commit 2 — B1: Mass camelCase rename (16 identifiers, ~975 refs)
- Verified-safe rename. No persistence/protocol impact.
- Strategy:
  ```
  find components -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.c' \) -print0 \
    | xargs -0 sed -i '' -e 's/\bgOLEDConfirmState\b/gOledConfirmState/g' \
                          -e 's/\bgOLEDConsole\b/gOledConsole/g' \
                          ...
  ```
  (one `sed` invocation per OS — macOS BSD sed needs `-i ''`).
- Also clean up `System_Mutex.h:115` doc comment to use `gRtcCache` for consistency with the line's other new-style names.
- Build verify.
- Estimated touched files: 50-70. No expected build breakage because all references are inside the codebase.

#### Commit 3 — B3 (reduced scope): `ESPNOW → ESP-NOW` in 70 user strings + 1 Wifi comment
- Mechanical: `sed -i '' 's/ESPNOW/ESP-NOW/g'` applied selectively to string contents — but be careful: identifiers like `DEBUG_ESPNOW_CORE`, `ESPNOW_DEVICES_FILE` (constant name), `ENABLE_ESPNOW` macro, `gEspNow*` identifiers must NOT change.
- Safer: enumerate the 70 hits and do them as `Edit` calls so each substitution is reviewed in context. (sed will hit too many false positives on macro names like `ENABLE_ESPNOW`.)
- Fix the one `Wifi` comment at `G2_Page_Network.cpp:967` while we're here.
- **Skip `bluetooth → Bluetooth`** entirely per the analysis above. Mention in commit message that the lowercase form is preserved deliberately because it's a stable identifier in 12+ contexts.

### Total estimated impact
- ~70 files touched
- ~1050 character-level edits
- 0 expected runtime behavior changes
- 0 expected persistence/protocol changes
- 3 commits, all behind clean builds

### Sequencing rationale
- B2 first because it's tiny and provides confidence that the mutex/sensor-cache wiring isn't accidentally regressed by sweep tooling.
- B1 second because it's the heaviest mechanical change and benefits from a clean baseline.
- B3 last (and reduced) because it has the most judgment calls and we want B1's churn to be settled before touching strings that might appear in commit-pair diffs near identifier renames.

---

## What is *not* in this report (intentionally)

- **Function-name renames** (e.g., `enqueueWifiRedrawFromCallback`, `g2GpsThing`). The user's task narrowed to *variables*; we follow that. The existing function-name conventions are already mostly correct (`gWifi*`, `gEspNow*`, etc.).
- **Type/struct names** (e.g., `OLEDKeyboardState`, `GPSCache`). The user said "12+ identifiers" referring to globals; type names are a separate sweep. Note that the rename would arguably want `GpsCache` etc. too, but that's a larger blast radius (changes every member access and every `extern` declaration's TYPE half, not just the variable half) and is best deferred.
- **`OLED_` prefix in C++ enum values** (`OLED_BLUETOOTH`, `OLED_ESPNOW`). These are `SCREAMING_SNAKE` macros/enums where the abbreviation casing convention is intentional. Out of scope.
