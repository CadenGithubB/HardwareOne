# ESP-SR voice table decoupling — design investigation (2026-08-07)

> **IMPLEMENTED 2026-08-08 (Option A), uncommitted — pending user HW test.** Measured: **-7,424 B** on the XIAO carrier build (5,213,616 → 5,206,192; estimate was 7-11 KB). Implementation deltas vs this plan: (1) the table has **39 routes, not 38** — `ramflush` (`system`→`ramflush`) had a multi-line usage string that hid it from the extraction grep; the deleted-ctor sweep caught it and it is preserved as a route; (2) **28 super-admin rows** (16 OTA + 12 `/*requiresSuperAdmin=*/` -annotated across 7 files) were re-formed to the new 6-arg shape; (3) the duplicate `voicecancel` registry row (which existed only to carry the "nevermind" phrase) collapsed to one command + two routes; (4) SR-on was **compile-verified** by building `System_ESPSR.cpp.obj` alone with the flag flipped — a full SR link is impossible on the 8MB XIAO profile (SR partition layouts are 16MB-only, pre-existing constraint); behavioral voice testing still needs an SR bench build. Build gotcha discovered: any cmake reconfigure without `HW_BOARD=xiao_s3` defaults S3 flash to 16mb and regenerates `partitions.csv` with a 16MB layout that fails on the 8MB sdkconfig — `partitions.csv` is gitignored/generated, so this is recoverable but confusing; always pass `HW_BOARD` when a reconfigure may trigger.

Companion docs: `BUILD_FLAG_COVERAGE_AUDIT.md` (finding cam #0), `BUILD_FLAG_FIX_PLAN.md` (chunk E1 — this doc replaces E1's inline-`#if` proposal with a structural design).

## Part 1 — What the voice columns actually are today (verified mechanics)

**The spine.** `CommandEntry` (`System_Utils.h:57-109`) has 9 fields; three are voice routing: `voiceCategory`, `voiceSubCategory`, `voiceTarget` (12 B per row on Xtensa). Two constexpr constructors exist solely to feed them: a 2-level form (category+target, sub=nullptr) and a 3-level form. The trailing-defaulted design means rows that don't use voice pass nothing.

**The producers.** Exactly **38 of ~940 rows** set any voice phrase (verified by initializer-shape grep): 4 in `System_ESPSR.cpp` itself, 4 camera, 3 WiFi, 3 core utils, 2 each in Bluetooth/Microphone/NeoPixel/HAL_Input, and 2 per i2c sensor driver (8 files). All positional args; the other ~900 rows carry three null pointers each. 19 files also carry a `// Columns: ... voiceCategory ...` comment — those comments are the ONLY non-ESPSR occurrences of the field names in the whole repo.

**The registry.** Modules already register at boot: `registerCommands()` flattens every module's table into `commandRegistry[]` → exposed as `gCommands[]`/`gCommandsCount` (`System_Command.cpp:69-102`). This is live, runtime, and already feature-correct — a compiled-out module never registers, so the registry is the ground truth for "which commands exist in this build."

**The one consumer.** `System_ESPSR.cpp` (fully `#if ENABLE_ESP_SR`-wrapped) walks `gCommands[]` in 6 loops:
- grammar build per menu level: collect distinct `voiceCategory` phrases (`loadCategories`), then subcategories (`loadSubCategoriesForCategory`, ~720), then targets (`loadTargetsForCategory`, 449; `loadTargetsForSubCategory`, ~782) — each phrase goes to MultiNet via `esp_mn_commands_add(id, phrase)` and into `gVoiceCliMappings[] {id → entry->name}` (line 400);
- reverse lookups when navigating (~632-686);
- global phrases: `addSpecialPhrases()` (424) scans for `voiceCategory=="*"` rows (note: **no row in the repo currently uses `"*"`** — cancel/help are handled separately by phrase-substring checks at ~869; the global mechanism is currently producer-less).

**Dispatch.** On recognition: `findCliCommandForId(id)` → `executeVoiceCommandAsArmedUser(cliCmd, out, 2048)` (~1082) — the command is re-resolved **by name** through the normal pipeline, which enforces admin/super-admin under the armed voice identity. ESP-SR never touches `handler`, `help`, or the auth flags directly.

**Conclusion of the investigation: the coupling surface is exactly four strings per voice-capable command — `{cliName, category, subCategory, target}` — and the join key (`cliName`) is re-resolved at dispatch anyway.** The 12 B × ~940 rows of spine cost buys nothing that a 38-row side table wouldn't provide.

## Part 2 — The separate-table designs

### Option A — SR-owned route table (smallest diff)

```c
// System_ESPSR.cpp (already #if ENABLE_ESP_SR — whole file)
struct VoiceRoute { const char* cli; const char* cat; const char* sub; const char* target; };
static const VoiceRoute kVoiceRoutes[] = {
  { "openble",  "connection", "bluetooth", "open"  },
  { "closeble", "connection", "bluetooth", "close" },
  // ... 38 rows total
};
```

The 6 loops switch from `gCommands[i]->voiceCategory` to `kVoiceRoutes[i].cat`; `addVoiceCliMapping(id, route.cli)` unchanged.

- **Feature sync comes free:** at grammar-build time, skip any route whose `findCommand(route.cli)` doesn't resolve. The command registry is already the per-build feature truth (a BT-off build never registered `openble`), so one liveness check makes the voice grammar correct in **every** flag combination with zero `#if`s — better than today, where correctness relies on the row being compiled out with its table.
- **Cost:** 38×16 B ≈ 0.6 KB + phrase strings, all inside the SR-gated TU → **zero in SR-off builds, and ~10.5 KB net saved even in SR-ON builds** (the columns leave every row).
- **Trade-off:** ownership inversion — Bluetooth's voice phrases now live in the SR file. Rename drift (someone renames `openble`) is caught at runtime, not compile time; mitigate with a WARN pass at SR init listing unresolved routes (and optionally a `srroutes` debug command). With 38 rows and ~19 owner files, centralization is defensible; this is the pragmatic choice.

### Option B — feature-owned tables + registration (most house-consistent)

Each feature keeps its phrases next to its command table, inside its existing file wrap:

```c
// Bluetooth.cpp, inside #if ENABLE_BLUETOOTH
static const VoiceRoute bleVoiceRoutes[] = { {"openble","connection","bluetooth","open"}, ... };
// at init: srRegisterVoiceRoutes(bleVoiceRoutes, 2);   // inline no-op stub when ENABLE_ESP_SR=0
```

- Feature gating is structural (table dies with the file wrap); ownership preserved; rename drift is adjacent-code-visible.
- Needs a small registration API + storage in ESP-SR (mirror of `registerCommands`), and the `System_ESPSR.h` stub block already provides the no-op pattern.
- SR-off cost of the tables in feature files: the no-op inline stub leaves them unreferenced → `-fdata-sections` + GC drops them. That's "free by optimizer grace"; a deterministic form adds `#if ENABLE_ESP_SR` around each table (16 small, feature-owned sites — still far better than 3 columns in the core struct).

### Options C/D (rejected)

X-macro codegen and linker-section registries (`__attribute__((section(".voice_routes")))`) both work but add machinery a 38-row problem doesn't justify; the `used` attribute in D actively fights GC in SR-off builds unless per-site `#if`'d anyway.

### Recommendation

**Option A**, with the liveness filter and an init-time WARN for unresolved routes. It is a ~300-line mechanical diff, deletes the fields AND the 3-level constructor, and its sync story (resolve against the live registry) is strictly stronger than today's. Revisit Option B only if voice coverage is expected to grow a lot (say >100 routes), where per-feature ownership starts paying.

## Part 3 — Migration cost & test plan (for whenever this is implemented)

1. Delete the 3 fields + the 3-level constructor from `CommandEntry`; drop the voice args from the 2-level constructor. (~40 lines, one header.)
2. The 38 voice rows now fail to compile → trim their trailing args (mechanical, 19 files; the other ~900 rows are untouched because voice args were trailing/defaulted — the same property the `requiresSuperAdmin` comment relies on: after removal, `requiresSuperAdmin` must remain last).
3. Add `kVoiceRoutes` + liveness filter + WARN pass; rewrite the 6 ESP-SR loops. (~120 lines, one SR-gated file.)
4. Update the 19 `// Columns:` comments.
5. **Test plan needs an SR bench build** — SR=0 on both current boards. `partitions_sr_8mb.csv` + the SR sdkconfig defaults exist; voice flows (category → target, 3-level via sensor commands, cancel/help, auth-rejection path) must be re-validated on hardware. Size check: `idf.py size` delta on the XIAO build should show ~7-11 KB .rodata reduction with zero behavior change (SR is off there).

Risks: none to non-SR builds (fields unread outside SR — verified); SR behavior risk concentrated in the 6 rewritten loops; the currently-unused `"*"` global mechanism should either be carried over or deleted explicitly.

## Part 4 — How this generalizes (the modularity recipe)

The decoupling rule this establishes for **other** systems: **a cross-cutting consumer owns its own table, keyed by a name the core registry already resolves, and filters by liveness at runtime.** The command name is the stable join key; the registry (already feature-correct) is the truth oracle. Anything wanting per-command metadata (a future automation trigger list, MQTT command exposure, a G2 quick-actions palette) should follow the same shape rather than adding columns to `CommandEntry`.

Anti-goals: no generic "command attributes" framework (over-engineering for tables this small); no second registration bus when the existing module registration or a static consumer-owned table suffices.

## Part 5 — Census: other feature-slices riding in shared spines

**Method & verification status:** 5 finder lenses (structs / enums-bitmasks / runtime-state / string-tables / API-threading) + 5 adversarial verifiers at high effort, with every finding from the earlier BUILD_FLAG_COVERAGE_AUDIT excluded up front. **32 findings: 18 CONFIRMED, 14 ADJUSTED (corrections mostly strengthen them), 0 REFUTED.** Every verdict rests on a preprocessor guard-stack walk + exhaustive consumer grep. Run `wf_0e8ce2de-228`.

**Config baseline caveat:** `System_BuildConfig.h` evolved during the investigation into the **carrier profile** (BT=1, G2=1, I2C_FEATURE_LEVEL=0 "for BT flash headroom", DISPLAY=0, MAPS=0 "for BT flash headroom", ESPNOW=1, AUTOMATION=1, SR/EI/LLM/MQTT/GAMES=0). Costs below are stated per-config; "today" = carrier profile.

### 5.1 Two deletions with zero risk (verifier-proven orphans)

1. **`gFileTransferMutex` is fully orphaned** — `FileTransferGuard` is never instantiated and the raw handle is never taken, repo-wide. ~90-95 B internal DRAM allocated at every boot of **every** build, plus boot-fail coupling in `initMutexes()`. Delete the handle + guard class outright. (`System_Mutex.cpp:18-45`)
2. **`CommandContext::httpReq` and `::replyHandle` are write-only** — zero readers in any build (web handlers read the request via `AuthContext::opaque` instead). Three `= req` writers, twelve ritual `= nullptr` sites across ten non-web files, and the hand-rolled `httpd_req_t` forward-declaration block (`System_CommandTypes.h:10-16`) exists solely to keep the dead field compiling. Delete both fields + the forward-decl + the 12 nulling sites. (`System_CommandTypes.h:83-84`)

### 5.2 Census table

| # | Spine | Feature slice | Verdict | Cost (per relevant config) | Alternative |
|---|---|---|---|---|---|
| S0 | `SensorCacheSnapshot` union struct (`System_SensorLogging.h:27-105`) | all 8 optional sensor field-blocks + R1 | CONFIRMED | arch-only (~220 B stack transient/sample; flash = the audited CSV-formatters item) | per-sensor registration `{bit, fill, formatText, formatCsv}` — dissolves the union |
| S1 | `initMutexes()` boot pool (`System_Mutex.cpp:18-45`) | 7 of 8 mutexes single-feature (maps, mic, 4×espnow, web) | ADJUSTED | ~90 B DRAM per dead mutex; carrier: ~190 B (maps + orphan); WiFi-only: ~550-650 B | feature-owned creation (precedent in-tree: `gMeshRetryMutex` lazy-init at `System_ESPNow.cpp:10646`) |
| S2 | `gMemoryRequirements[]` (`System_MemoryMonitor.cpp:72-88`) | all 12 rows feature-owned; carrier: 10 dead | ADJUSTED | ~220-280 B flash | per-row `#if` (sibling `nonI2CSensors[]` already does it) or register-at-init |
| S3 | `FileEntry::childCount` (`System_FileManager.h:44`) | G2 folder-badge column in shared file-browser cache | ADJUSTED | **0 B RAM (padding absorbs it)**; only scanner flash in OLED-on/G2-off builds | G2-owned path→count cache; parallel-array variant saves nothing |
| S4 | `CommandContext::captureOutput` + executor capture branch (`System_CommandTypes.h:82`, `HardwareOne.cpp:743-783`) | web-only output capture on the always-on executor | ADJUSTED | ~300-600 B flash in HTTP-off builds + 12 B/task TLS slot | `#if ENABLE_HTTP_SERVER` field+branch; NOTE: "web installs a sink" variant is **infeasible** — capture buffer is per-task TLS on cmd_exec_task |
| S5 | Local-display session globals + transports (`System_User.h:63-65` et al.) | OLED login session state | ADJUSTED | ~24-32 B BSS + ~150 B flash in OLED-off builds | transport provider table; must also gate the `display` keyword in cmd_login/logout and the G2 OLED-Login row (see 5.4) |
| E0 | OLED mode name/slug maps ×3 (`OLED_Utils.cpp:4420-4599`) | BT/G2/R1/SR/maps/sensors/mic rows inside the OLED spine | CONFIRMED | 0 today (OLED off); ~0.5-1.5 KB in OLED-on features-off builds | extend `REGISTER_OLED_MODE_MODULE` registration; fixes proven drift (see 5.4) |
| E1 | RamFlush `RF_*` intent/name switches (`System_RamFlush.cpp:234-288`) | every optional feature has a case; `ramFlushReadLive` IS gated, these two aren't | CONFIRMED | ~0.2-0.4 KB + **this is what pins ~12 `*AutoStart` Settings fields** (fix-plan C1 keep-lists) | register functions, not Settings-field pointers — answers the in-file aliasing objection (`:122-124`) |
| E2 | Sensor-log mask bits + alias parse + printf arms (`System_SensorLogging.cpp:1456-1501`) | per-sensor + R1 rows | ADJUSTED | ~0.1-0.3 KB (R1 rows live today, BT=1) | extend the file's own per-sensor `#if` idiom |
| E3 | `SOURCE_*`/`ORIGIN_*`/`NOTIF_SOURCE_*` translation tables ×6 parallel spines (`System_User.h:29-40` + 6 TUs) | per-transport rows (MQTT+VOICE dead today) | CONFIRMED | ~0.1-0.2 KB; the payoff is drift risk across 6 parallel switches | one descriptor table per enum, rows `#if`'d once — or document as deliberate like FeatureRegistry |
| T0 | `sensorHeapCosts[]` + `sensorautostart` (`System_I2C.cpp:2004-2148`) | per-sensor rows | ADJUSTED | **0 today** (whole i2c module GC'd at I2C=0); ~1-1.5 KB in I2C-on/sensors-off builds | per-sensor registration from CMake-gated driver files |
| T1 | `sPathRules[]` + `cleanupDirs[]` (`System_Filesystem.cpp:1341-1405, 132`) | espnow ×3, maps, G2 icon-pack rows | ADJUSTED | carrier: ~35-47 B (`/maps` row + cleanup entry) | per-row `#if` mirroring the mkdir gates; **CAVEAT: rows double as namespace reservations — a dropped row lets users squat `/maps` via the PERM_ALL catch-all; keep restrictive defaults** |
| T2 | Power-mode table `displayBrightnessPercent` column (`System_Power.cpp:21-127`) | OLED brightness column + apply/persist block | CONFIRMED | ~100-200 B; **live today: every power-mode change persists `oledBrightness` and logs "Display: N%" on a display-less build** | listen for the already-posted `SYSEVT_POWER_MODE_CHANGED` in OLED code |
| T3 | `NonI2CSensorEntry::mlSettingsModule` (`System_SensorRegistry.h:54`) | "edgeimpulse" column emitted in status JSON | CONFIRMED | ~16-30 B; JSON self-contradicts today (`eiCompiled:false` + `mlModule:"edgeimpulse"`) | nullptr-gated ternary or an EI stub helper |
| T4 | `adminRequiredForLine` help-nav allowlist (`System_Utils.cpp:4408-4413`) | wifi/espnow/automations literals | CONFIRMED | tens of bytes; real issue is drift (bluetooth compiled but missing) | derive from `getCommandModules()` + alias map (mind the plural "automations" alias) |
| ST0 | `knownTasks[]`+`taskAlive[]`+`appTasks[]` task tables (`System_TaskUtils.cpp:533-572`, `System_Utils.cpp:3754-3768`) | 11 of 13 rows feature-owned; binds to stub handles on feature-off builds | CONFIRMED | ~300-450 B today | creation-time registry fed by `xTaskCreateLogged` (already receives name/stack/handle) — also closes the proven G2-task coverage gap |
| ST1 | Debug/broadcast output spine hardcoding sensor task handles (`System_Debug.cpp:796-802, 950-955`) | thermal/imu/tof/fm shutdown-race checks (3-vs-4 inconsistency proven) | CONFIRMED×2 | ~60-100 B + 4 stub reads per debug line today | dying task sets a TLS mute flag / atomic draining-handle — one generic check, covers G2 tasks too |
| ST2 | `ExecReq::deferredFn/deferredArg` + bypass branch (`System_CommandTypes.h:148-150`) | all producers are optional radio features | CONFIRMED×2 | arch-only (~8 B per transient ~6.3 KB ExecReq; also `line[2048]` sized "for ESP-NOW chunking") | `HW_NEED_DEFERRED_EXEC` derived flag — or accept as generic infrastructure (defensible) |
| A0 | Login lockout/audit living inside `WebServer_Server.cpp` | HTTP-off builds silently lose serial/UART brute-force lockout + login auditing; 4 hand-rolled `#if/#else` scaffolds in consumers | CONFIRMED | arch-only (0 bytes) — **it's a security behavior gap, not a size one** | move transport-agnostic lockout core to an always-on TU (the TODO at `WebServer_Server.cpp:441-444` already proposes this) |
| A1 | `tgRequireAuth` dual bodies (`System_User.cpp:159-281`) | full body vs stub split on HTTP; duplicated G2 recovery literal; stub ends in fail-open `return true` | CONFIRMED | ~150-300 B; the hazard is drift-equals-auth-bypass (comment in-file admits it) | single body + registered web session-resolver |
| A2 | Bond `remote:`/`@` prefix parse before its guard (`System_Utils.cpp:4635-4674`) | bond-mode grammar probed on every command in every build | CONFIRMED | ~250-400 B in bond-off builds + 3 startsWith/cmd | registered prefix-interceptor (`cliModeDispatchInput` nearby is exactly this shape) |
| A3 | Targeted-BLE notify block in the result-delivery spine (`HardwareOne.cpp:867-880`) | MSG_ROUTE_BLE branch + String build | CONFIRMED | ~100-200 B in BT-off builds | transport-registered delivery hook |
| A4 | Login/session transport switch spines ×6 (`System_User.cpp:85-746`, `System_Utils.cpp:5398-5459`) | per-transport cases + keyword literals | CONFIRMED | ~200-400 B in BT+OLED-off builds | transport provider table (same fix as S5/E3) |
| A5 | notifG2 row + NSINK_G2 resolution + g2 counters in the notif module (`System_Notifications.cpp:374-424, 734-927`) | G2 sink slice | CONFIRMED | ~150-250 B in G2-off builds; `notifydeviceg2` is a dead-but-working knob there | per-sink registration (the guarded delivery block at 881 already has the right shape) |

(Deduped: the mutex pool, memory-requirements table, path rules, debug-spine, and deferred-exec items were independently found by two lenses each — verdicts agreed; merged above.)

### 5.3 What the census says architecturally

The 32 instances cluster into **five spine types**, and four of them already have an in-tree registration precedent to copy:

1. **Boot-time resource pools** (mutexes) → feature-owned lazy init; precedent: `gMeshRetryMutex`.
2. **Diagnostic/introspection tables** (memory requirements, task tables, heap costs) → register at feature init / task creation; precedents: `nonI2CSensors[]` per-row `#if`s, `xTaskCreateLogged`.
3. **Name/translation switch spines** (transports, origins, OLED modes, RamFlush names, help-nav aliases) → one descriptor table per enum, or registration; precedent: `REGISTER_OLED_MODE_MODULE`.
4. **Shared context/queue structs carrying transport-specific fields** (capture, httpReq, deferred fn) → delete the dead ones, `#if` or derive-flag the rest; the executor's TLS design constrains alternatives (see S4).
5. **Namespace/permission tables** (path rules) → per-row `#if` **with restrictive-default preservation** — the one cluster where naive row removal creates a security regression.

The forward-looking house rules this suggests: *(a)* a shared spine may not name a feature — features register into spines; *(b)* if a slice can't be registered, it gets the feature's `#if` at the slice site; *(c)* an enum id may stay stable across builds (RF_*/SOURCE_* charter), but its name-strings, switch cases, and table rows follow rule (a) or (b).

### 5.4 Functional bugs surfaced in passing (verifier-proven, independent of size)

- **`OLED_LLM` is missing from all three mode name/slug maps** despite being registered with a menu row — live drift proof of the E0 pattern (`OLED_Utils.cpp`).
- **The G2 lens can mint a local-display session nothing reads** on OLED-less builds — `login <u> <p> display` dispatches from `G2_Glasses.cpp:4844` (G2-gated, not OLED-gated), and `cmd_login` accepts the `display` keyword in every build (S5).
- **Every power-mode change on a display-less build persists `oledBrightness` to flash** and logs a Display % line (T2) — live in the carrier build today.
- **HTTP-off builds lose login lockout/auditing on serial and UART** (A0) — relevant the day a headless build ships.
- **`/api` status JSON reports `eiCompiled:false` while advertising `mlModule:"edgeimpulse"`** (T3) — live today.
- **Filesystem G2 icon-pack mkdir guards on bare `#if ENABLE_G2_GLASSES`** — violates the BT&&G2 house rule; latent until a BT=0/G2=1 header exists (T1).

### 5.5 Relationship to the fix plan

This census extends `BUILD_FLAG_FIX_PLAN.md` rather than replacing it: S-/E-/T-/ST-/A-items are candidates for a new **Batch 6 (structural decoupling)**, and E1 directly informs chunk **C1** (the RamFlush registration design is what would unlock gating the `*AutoStart` fields). The two 5.1 deletions are safe enough to ride along with any batch.
