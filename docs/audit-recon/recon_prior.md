# RECON — Prior-Audit Index (know-before-you-file)

**Purpose:** so Track A/B/C agents do not re-report known issues. Before filing anything,
grep this file. If your finding appears below as *still open*, file it only as
`KNOWN — see docs/X.md` **and only if the status has changed** or the prior doc was wrong.
If it appears as *fixed/implemented*, do not file it — verify instead (several "fixes" here
were shipped uncommitted and awaiting HW, so the code may or may not match the doc).

**Critical meta-fact:** almost every "IMPLEMENTED" banner in `docs/` means *built green,
uncommitted, awaiting hardware test*. Several are now committed (v0.98.9 → v0.99.3). Treat
every status line as a claim about a moment in time, not about the current working tree.

---

## 0. Where fresh findings are most likely (read this first)

Ranked by "prior work stopped before it got here."

| Rank | Gap | Why it's fertile |
|---|---|---|
| 1 | **`HOT_PATH_HEAP_AUDIT.md` fix-verification pass** | PAUSED mid-revision. **Every "Fix:" line in that doc is UNVERIFIED**, and the one fix that *was* hand-checked was proven wrong. A `FixVerify` pass over 11 prescriptions was launched and cut off before returning anything. |
| 2 | **Two unverified non-allocation bugs it tripped over** | `System_Automation.cpp:2282` `nextFire()` integer division, and `WebServer_Server.cpp:4811` batch-endpoint redaction bypass. Both explicitly marked **Unverified** and never followed up. Direct hits for A16 and B10/B14. |
| 3 | **The `gSessions` / ALWAYSINTERNAL security claim** | Marked **🔒 Unverified. Confirm before any sdkconfig change.** Claims ESP-NOW AEAD session keys already live in PSRAM (violating the no-secrets-in-PSRAM rule) and that lowering `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` would silently relocate 32 B keys and typed passwords into plaintext PSRAM. Direct hit for **B9**. |
| 4 | **Contiguity-vs-String framing never reconciled** | `HOT_PATH_HEAP_AUDIT_MECHANICS.md` never reached synthesis (payload truncated) and *contradicts* its parent doc. Its central claim — that lazy-alloc conversions **trade a byte win for a permanent mid-heap divot** — indicts several `LAZY_ALLOCATION_AUDIT.md` changes that have since **shipped**. Nobody has re-ranked. Direct hit for **A5**. |
| 5 | **Watchdogs, ISRs, boot path, GPIO/strapping, sdkconfig reality-check** | **No prior audit covers these at all.** A2/A3/A6/A17/A20/A21 have zero prior-art collisions — anything you find there is new. |
| 6 | **`esp_err_t` return values (A9), I2C robustness (A10), NVS wear (A12)** | No dedicated prior doc. Only incidental mentions. |
| 7 | **Track B (auth/security) as a whole** | The only prior artifact is a memory-file snapshot from 2026-07-20 (§7 below), not a docs audit. 8 ranked items still open, none re-verified since. |

---

## 1. `docs/PRE_1_0_CODE_HEALTH_AUDIT.md` — 2026-07-14
**Covers:** whole-codebase sweep for (1) hot-path allocation/String churn, (2) dead/unused code, (3) duplicated logic. 275 findings (92 hot-path / 142 dead-code / 40 duplication) from 28 parallel auditors over ~275k lines.

**⚠️ Its own verification pass was dropped for spend.** Only findings marked `✓verified` were re-read. Its preamble warns that this codebase dispatches CLI commands by string, token-pastes via X-macros, references web handlers from HTML blobs, and gates by board — all of which make live symbols look dead.

**Its 5-item short list, current status (per the later HOT_PATH audit's reconciliation §"Reconciliation"):**

| # | Item | Status |
|---|---|---|
| 1 | `executeCommand` per-command waste (`System_Utils.cpp:4272/:4180/:4303`) | **STALE — DONE. Do not re-do.** `normalizedCmd` has zero hits; the dead `String lc` is gone; the command-line copy is now deliberate + commented at `:4194`. Line numbers drifted ~90. |
| 2 | thermal/ToF task busy-spin (`i2csensor_mlx90640.cpp:1488`, `i2csensor_vl53l4cx.cpp:732`) | **STILL OPEN** — `vTaskDelay` sits inside the `enabled && connected && !pollPaused` gate with no else-branch; spins a core at 100% during every pollPause window. Marked "real CPU bug, ✓verified status looks sound, should still be fixed." **Overlaps P13/A6/A7 — verify before re-filing.** |
| 3 | BLE notify copies payload ~4× per fragment (`Bluetooth.cpp:1191`) | **REAL mechanism, frequency claim REFUTED.** ~30 KB/s steady state is not reachable (`outble` bit can't survive reboot). Downgraded to DISPUTED/burst-cost. Do not re-file as a steady-state drain. |
| 4 | Delete `BLE_IDF.cpp/.h` (1,155 lines behind `ENABLE_BLE_IDF_EXPERIMENTAL`=0) | **OPEN** (not revisited; also listed as dead weight in LAZY §1). Linker-GC'd, so hygiene not bytes. |
| 5 | Logging fast-path: `isDebugFlagSet()` two out-of-line calls per `DEBUGF` (`System_Debug.h:502`); debug layer heap-copies each line (`System_Debug.cpp:278/834`) | **CONFIRMED** by both later lenses, tier corrected to edge-triggered per-event. The `:834` half is DISPUTED on tier (window is milliseconds, not 30 s). |

**Other still-open content:** the dead-code section lists ~142 candidates across ~60 files (`System_Utils.cpp` 6, `G2_Glasses.cpp` 9, `System_ESPNow.cpp` 10, `OLED_Utils.cpp` 6, …). §3 has 40 duplication/shared-helper findings. **Track C3 note: this doc over-reported dead code — re-verify every candidate before believing it.**

---

## 2. `docs/HOT_PATH_HEAP_AUDIT.md` — 2026-07-16 · ⛔ **PAUSED**
**Covers:** internal-DRAM allocation churn in steady-state paths, via a 27-lane sweep with two adversarial lenses (hotness + allocation arena). 27 CONFIRMED (21 unique sites), 33 disputed.

**Paused 2026-07-16 on a usage limit. What it never reached:**
1. **Fix verification.** Every `Fix:` line is unverified. The original #1 fix (`out.reserve()` before `out = f.readString()`) was **proven wrong by hand** — reserving forces `String::move()` onto memmove instead of a buffer steal, making it strictly worse. The 11-prescription `FixVerify` pass returned nothing.
2. **Re-ranking against the mechanics challenger** (see §3).
3. **Complete counts.** The synthesis payload truncated: the REFUTED bucket (17), part of DISPUTED, and 10 critic findings never arrived.

**Solid / trustworthy parts:** the three String mechanics — `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` (every String <16 KB is internal DRAM); the 14-char SSO threshold; 16-byte-granular exact-fit growth (`+=` in a loop ≈ N/16 relocating reallocs). And the `readText()` per-byte finding, hand-verified.

**Still-open short list (items 1–8):**
- **1.** `readText()` (`System_Utils.cpp:804`) reads every file one byte at a time via inherited `Stream::readString`. Fix must **bypass `Stream::readString` entirely** — `reserve()` is not the lever.
- **2.** `isAdminUser()` re-reads `users.json` off flash on every call (every command, every guarded FS op, every directory entry). Fix: cache against `gIdentityGeneration` (which already exists).
- **3.** `findCommand()` walks a 1222-entry registry with no early exit, building a heap String from 263 names, **twice per dispatch** (~526 malloc/free round-trips per command).
- **4.** Hoist the redundant `findCommand` walk out of `authorizeCommand`.
- **5.** `automations.json` read 2–3× per scheduler tick.
- **6.** `buildFilesListing` accumulates the whole JSON body with no `reserve()`.
- **7.** `VFS::open` normalizes the path twice, on every FS operation.
- **8.** `redactCmdForAudit` makes two full copies before checking whether any rule applies.
- **Systemic S1–S5:** bypass `Stream::readString`; `const char*` overloads across the VFS/log seam (`System_VFS.h:115/:145`); split `getStorageType` from `normalize` (`System_VFS.cpp:315`); stop serializing PSRAM JsonDocuments into internal-DRAM Strings (correct idiom already in-tree at `WebServer_MigrationTool.cpp:337-342`); honor the caches already built (`ExecIdentityGuard` TLS vs `resolveRole()` hitting flash; `gUserPrefsCache` vs the admin bit on the next line).

**🔴 Two unverified NON-allocation bugs — highest-value orphans in the whole docs tree:**
- `System_Automation.cpp:2282` — `nextFire()`'s `intervalMs / 1000` integer division has no floor. An interval under 1000 ms reportedly yields `nextAt == firedAt` → permanently due → full read+parse+serialize+**flash write every main-loop pass**. Flash-wear risk. *Confirm such an interval can be set from any interface.*
- `WebServer_Server.cpp:4811` — the batch endpoint reportedly returns the **unredacted** output to the HTTP client while only the broadcast sink gets the redacted copy. Possible credential disclosure.

**🔒 Unverified security claim (Track B9):** `gSessions` (ESP-NOW AEAD session keys) already lives on the PSRAM heap, violating the standing no-secrets-in-PSRAM rule. And lowering `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` would **silently relocate short secret allocations (32 B keys, session tokens, typed passwords) into plaintext PSRAM** — today they are internal only *by accident* (they're small). Corroborated independently by `LAZY_ALLOCATION_AUDIT.md` §1 and §5.

---

## 3. `docs/HOT_PATH_HEAP_AUDIT_MECHANICS.md` — 2026-07-16 · ⛔ **NEVER REACHED SYNTHESIS**
**Covers:** the structural half of the same audit — systemic internal-DRAM fragmentation mechanics. Recovered verbatim from the run journal after the result payload truncated.

**It contradicts its parent doc.** Headline: *"this is a CONTIGUITY problem, not a byte problem. String churn is the stirrer, not the killer."* It ranks String/`reserve()` work **5th**.

Still-open structural claims (none acted on, none re-ranked):
- **Budget:** ~99.9 KB internal DRAM gone to statics before the first malloc; ~68.5 KB of permanent task stacks (app 31.5 + system 37.0); measured fragmentation 65–77% (quoted from `G2_Glasses.cpp:14615-14624` / `:14840-14850` developer comments, not fresh measurement). Normal LLM-loaded free heap (29–39 KB) is already **below** `System_MemoryMonitor.cpp`'s own `HEAP_WARNING_THRESHOLD` of 40,960 B.
- **The indictment:** *"a lazily-allocated permanent block is strictly worse for fragmentation than an eager boot-time static."* It names LAZY audit items #2 (`sEventBuf` 7,872 B), #5 (`sensor_queue_task`), #17 (`gAutoCache`) as trading a byte win for a permanent mid-heap divot. **Those conversions have since shipped.** Nobody has re-examined them under this lens.
- **The real big-contiguous consumer:** per-UI-action G2 task stacks (`g2_bmp_view` 6144, `g2_cam_view` 6144, `g2_bmp_full` 8192, `g2_map_page` 8192, … ~12 of them), each needing one *unbroken* internal block.
- **Diagnostics gaps (cheap, unclaimed):** `CONFIG_HEAP_TASK_TRACKING` is the highest-value flag **not** enabled. `gPsAllocFallbacks` is "the single best-designed diagnostic in the memory subsystem" but is observable only via an `ESP_LOGW` and a global nobody prints — recommend surfacing it in `[MEMSAMPLE]`. **And the tagged `ps_realloc` overload (`System_MemUtil.h`, last of four) is missing the `__psAllocReportFallback()` call its three siblings have** — a hole in that diagnostic.
- Confirms: heap poisoning OFF, heap tracing OFF, no allocation attribution at IDF level.
- **Explicitly says the logging lane is already solved — do not "fix" it.**

---

## 4. `docs/LAZY_ALLOCATION_AUDIT.md` — 2026-07-16, extended 2026-07-25
**Covers:** allocations paid eagerly (at boot / feature-init) that could be first-use. 118-agent audit; every finding ≥256 B adversarially verified against source + linker map. **Coverage accounting proves zero unaudited application DRAM symbols ≥512 B** — so a "you missed this big static" finding is unlikely to be new.

**Shipped:** 2026-07-16 batch freed 32.7 KB `.dram0.bss` (LLM worker stack, automation `sEventBuf`, OLED console ring, ESP-NOW RX ring, broadcast trackers, peer identities → PSRAM). 2026-07-25 batch added lazy gates for web JSON/mirror buffers, remote sensor cache, debug pool, OLED CLI history, `gAutoCache`, BLE reserve, G2 workers/RX buffers, MQTT `externalSensors`, `gFrameRing`, `gAllocTracker`. **Governing decision: allocate lazily, do NOT free on disable** — every free-on-disable path races an unguarded cross-task reader.

**Still open:**
- **~867 KB PSRAM ESP-NOW working set retained after `closeespnow`** (`deinitEspNow` deliberately keeps everything, `:9299`). `PeerMessageHistory` = 162,016 B *each*; initial 5-slot block = 810,080 B. 18 TUs deref `gEspNow`. Alternative: shrink `MESSAGES_PER_DEVICE` (250 is the SPIRAM-build value).
- **Map tile cache ~1.38 MB PSRAM** pinned by merely browsing past OLED Map mode (`OLED_Mode_Map.cpp:1079-1087` auto-loads the first `.hwmap`). `unloadMap()` exists and frees cleanly but is never called from an exit path.
- **Framework gap blocking several teardowns:** `OLEDModeEntry` has `onEnterFunc` but **no `onExit` hook** — needed at the `requestOLEDMode()` chokepoint (`OLED_Utils.cpp:2840`).
- G2 page/tap/FSM workers are lazy now but persist until reboot; shutdown blocked by §6.3 queue-drain/self-delete hazards.
- `espnow_tx` dispatcher (~6.5 KB DRAM) survives `closeespnow`; no `espnowtx::stop()` exists.
- HTTPS PEM Strings (~2.5 KB) survive `closehttp` (`cmd_httpstop` never clears them).
- `gLLMResultBuf` (8 KB PSRAM) survives `llmunload`. `GPSTrackManager::_points` (120 KB) — **and the httpd reader iterates `getPoints()` unsynchronized against GPS-task `appendPoint`** (a real race, noted in passing).
- `WaypointManager::_waypoints` — free-in-unloadMap is a **UAF as proposed** (httpd/OLED hold raw `getWaypoint()` pointers without `MapCacheGuard`).
- `gAutoMemoId`/`gAutoMemoNextAt` (1,536 B PSRAM every boot, provably never read) — **DEAD, delete** (`System_Automation.cpp:906-912`).
- `gMLX90640` `begin()` plain-news an `Adafruit_I2CDevice` that the existing delete **already leaks** each stop/start cycle.
- `sLoginAttempts` is **not web-scoped** — the serial console auth gate uses it from the main loop.

**Pre-existing bug found during review, explicitly NOT fixed:** `OLED_Mode_CLIInput.cpp:155/164` — the "did this command produce output?" check compares `getLineCount()` across the submit, but `count` **saturates at `capacity`**. Once 50 lines exist both reads return 50 forever, so the equality is always true and a duplicate fallback line is appended on every command. Needs a monotonic `totalAppends` counter.

**Also recorded:** a Cyrillic-homoglyph identifier (`reasмSize`) was found and fixed in `System_ESPNow.cpp:8926-8929`; a whole-codebase scan found no others in first-party code. Scanner at `scratchpad/homoglyph_scan.py`.

---

## 5. `docs/HEAP_OFFLOAD_SWEEP_2026-07-24.md`
**Covers:** DRAM→PSRAM and DRAM→flash offload of static buffers. **Tiers 1+2 IMPLEMENTED** (built green feathers3/esp32s3, uncommitted at time of writing): measured −32,384 B = **31.6 KB internal DRAM freed**.

**Still open:** Tier 3 (driver objects → placement-new, ~20 KB conditional) is list-only — but see `TIER3_DRIVER_PSRAM_2026-07-24.md`, which reports `gMLX90640` and `gFilesFm` as **since implemented**. `gCommandModules` constexpr-count refactor (840 B) unclaimed.

**Its §4 "Confirmed exclusions" list is the single most useful anti-re-litigation table in the tree** — do not propose PSRAM moves for: `gEventRing` (filled inside `taskENTER_CRITICAL(&gEventMux)`), `gAllocTracker`, `gOledEspNowState` (typed remote password), `gSettings` (wifi/mqtt passwords), `gAutoCache`, `gDisplay` (framebuffer, hot), `gSc`/`gMeshDerivedKeys`/`gIdentity` (crypto keys — **never** PSRAM), `bleMessageHistory` (raw inbound `login <user> <pass>` copied here before parse, `Bluetooth.cpp:708`), `gPeerBuffer`/`gTopoStreams`/`gMeshRetryQueue`/`gBlePeerData` (non-POD, String members), `sWizard` (wifi/mqtt password), `prompt_tokens`/`gPromptDiag` (LLM hot path).

**§6 corrections to prior findings:** `systemLogSettingEntries` const→flash is **not clean** (runtime `String::c_str()` forces dynamic init) — do not re-propose. `gOledFileManager` is **not** "never freed" (there is a `delete` at `OLED_Mode_FileBrowser.cpp:1442`).

---

## 6. `docs/SETTINGS_LIFECYCLE_AUDIT.md` — 2026-07-03
**Covers:** settings.json read/write/merge lifecycle; reconstruction of a "deleting settings.json" incident. Plan-only for remediation (§8 staged, user tests on HW between phases).

**Still-open confirmed findings — heavy overlap with pitfalls P38/P39 and Track C2:**

| # | Sev | Finding | Where |
|---|---|---|---|
| 4.1 | **HIGH** | Corrupt settings.json → deterministic defaults-overwrite at next boot. Failed load sets no flag; `lastResetReason` write always fires after a failed load; merge-read also fails → `doc.clear()` → pure defaults atomically renamed over the user's file. **Even a transient read failure ends the same way.** | `System_Settings.cpp:1152-1159`, `HardwareOne.cpp:1242-1243`, `System_Settings.cpp:1032-1037` |
| 4.2 | **HIGH** | One failed `LittleFS.begin()` → `format()` the entire data partition. Cannot distinguish corruption from `ESP_ERR_NO_MEM`. No retry, no N-consecutive-failure marker, no confirmation. | `System_Filesystem.cpp:54-68` |
| 4.3 | MED | Boot corrupt-check deletes (not quarantines) on a one-byte heuristic; the `/settings.json` entry is dead legacy | `System_Filesystem.cpp:153-168` |
| 4.4 | MED | `writeSettingsJson()` has 5 return-false paths; **every call site ignores the result** (~20 sites). Violates the OK:/Error: contract. | `System_Settings.cpp:2205-2243, 2512-2517` |
| 4.5 | MED | **No `gSettings` mutex**; `buildSettingsJsonDoc` walks heap-backed Strings with no lock. Confirmed concurrent writers: the **unpinned** `g2_tap_disp` worker, OLED input on loopTask, MQTT/voice tasks. Free-while-reading heap race. | `System_Settings.h:1011-1033`, `:1043` |
| 4.6 | MED | `gDeferWrites` can be stranded true → all changes RAM-only until next `savesettings`/reboot | `System_Settings.cpp:104, 2506-2517`; `WebServer_Server.cpp:4779`; `WebPage_Bond.cpp:1507-1523` |
| 4.7 | LOW-MED | Primary path's success check is `bytesWritten == 0` — a *partial* serialize ≥1 byte commits and renames over the good file. Fallback ignores `serializeJson`'s result and returns **true**. | `System_Settings.cpp:1073, 1087-1101` |
| 4.8–4.11 | LOW | WiFi-connect full-file rewrite per (re)connect for a boot-relative `millis()`; empty-list resurrect; runtime-only values leaking to disk (`setMeshRole`, power-mode brightness); merge-read failure silently drops orphaned keys | see doc |

**Environmental (not overwrite bugs, but real):** settings share the single 2796K LittleFS partition with all log churn; **IDF persists last-used WiFi credentials in cleartext in NVS** because nothing calls `esp_wifi_set_storage(WIFI_STORAGE_RAM)` — outside the AES-at-rest scheme, surviving `factoryreset` *and* a LittleFS format (**direct Track B9 hit**); `users.json` is truncate-rewritten (`"w"`, non-atomic) every boot to bump `bootCounter`.

**§6 claims that did NOT survive verification — do not re-file:** the users.json-corrupt→unauthenticated-device chain (refuted: LittleFS copy-on-write); "FTS restore accepts unauthenticated settings replacement" (triple-gated; `registerMigrationRestoreHandler` has **zero callers** — dead code, should be deleted); "fallback truncate = corruption on power loss" (refuted outright — `LFS_F_ERRED` blocks sync of failed writes); "boot usually rewrites settings.json" (wrong premise — `setSetting` is change-gated).

> ⚠️ **P38 note for A13:** the generic "truncate-then-write loses data on power loss" pitfall is **refuted on this stack** by littlefs copy-on-write. File the *residues* (weak success check, success-on-failure) instead, and do not re-derive the refuted version.

---

## 7. `docs/ESPNOW_COMMENT_AUDIT.md` — 2026-07-16
**Covers:** truthfulness of ESP-NOW comments against code. 93 findings. A fix pass ran the same day; §13 records six findings the fixers **rejected** after re-verification.

**Fixed 2026-07-16 (verified in the ELF):** `espnowbuffers` ×4 write-only settings deleted; `espnowmeshes add` 2nd-arg rejection; `espnowregenidentity --confirm-wipe-all-bonds` now actually calls `peerIdentityForget()`; dead `chunksTimedOut` counter deleted.

**Still open — need a decision (§14):**
| # | Defect | Risk |
|---|---|---|
| 1 | `System_MemoryMonitor.cpp:84` admits ESP-NOW at **327,680 B** PSRAM free; init actually needs **~855 KB** (2.6× under). The 360 KB figure in `System_ESPNow.cpp:9241` is pre-100→250-era arithmetic. | Guard passes, init fails |
| 2 | **ODR violation:** `V4RxCtx` re-declared at `System_ESPNow_Handlers_Crypto.cpp:43`, **missing `isAuthenticated` and `isSessionEncrypted`** (sizeof 16 vs 20). Those two dropped fields are the frame's authentication signals. First read of `ctx.deviceName` there silently reads an auth flag. | **Track B8/B15 — latent auth bypass** |
| 6 | `sessionAllocate` reuse path leaves `aeadKeyRxPrev[32]`, `prevKeysValidUntilMs`, `rekeyEphPrivKey[32]`, `bondToken[16]` uncleared while the sibling free-slot branch memsets | Stale key material outlives a re-handshake |
| 7 | Six user-facing usage strings print the dead two-word CLI form (`espnow buffers/broadcast/browse/fetch/remote/send`) | "Unknown command" |
| 8 | `:15497` log string still says "older logical messages dropped" — inverted, it drops **newest** | Misleads at runtime |

**Also still open, recorded but not in the fix table:**
- **§2.2** `growPeerHistoryArray` needs **2× transiently** (5→8 slots = 2.1 MB peak; 13→16 ≈ 4.7 MB). With a model loaded (~7.3 MB of 8 MB) growth cannot succeed — "grows as peers are discovered" silently doesn't hold.
- **§3.1 booby trap:** `System_ESPNow.cpp:8828` claims placement-new leaves all Strings valid — but `EspNowState`'s ctor runs `memset(devices, 0, sizeof(devices))` **after** those 80 Strings are constructed. `c_str()` returns NULL for friendlyName/room/zone/tags. Deleting the manual `= ""` at `:10312` as "redundant" reintroduces a documented LoadProhibited crash.
- **§3.2 booby trap:** `System_ESPNow.cpp:2776` clamps to 6143 with a comment saying the budget is 6400. The clamp is buffer-derived (`ps_alloc(6144)` − 1). "Correcting" 6143→6399 **overflows the heap by 256 B**.
- **§4 wire-contract falsehoods:** `Wire.h:426` says CONFIRM transcript = 88 bytes; it is **87** → every external Ed25519 signature fails. `Identity.h:90` says `SUBSCRIBE_UPDATE = 70`; it is **130** (70 is `FS_LIST_REQ`) — opcode-collision trap.
- **§7 inverted claim, security-relevant:** `System_ESPNow.cpp:3692` says FS_LIST/STAT/GET are gated on `REQ_PAIRED + REQ_BOND_MODE + REQ_SESSION_ENC`. Rows carry only `REQ_PAIRED|REQ_SESSION_ENC` — **no bond gate on remote filesystem access** (deliberate per `:4437`, but the comment lies). **Track B8.**
- **§5 the 4× stack myth, third act:** `Handlers_Crypto.cpp:716`, `FsList.h:51`, `FsList.cpp:463/902` still quote 4×-inflated stack budgets ("22 KB" vs real 6.5 KB) and **use them to justify the architecture**. Also `Sensors.cpp:586` and `Tx.cpp:107` label `uxTaskGetStackHighWaterMark` output as "words"; IDF returns bytes.
- **§7** `Saturation.h:17` says the ring is "static DRAM… No PSRAM" — it is `EXT_RAM_BSS_ATTR`, i.e. PSRAM. Exactly backwards.
- **§7** `FsList.h:150` says RX entrypoints are "Called from BTC_TASK (tiny stack)" — BTC_TASK is the Bluedroid BLE task, nowhere in this path. Hands callback authors the wrong stack budget.
- **§8** stale dispatch/index maps: `G2_Page_ESPNow.cpp:969`, `OLED_ESPNow.h:84` (routes `espnowsetpassphrase`/`espnowmeshmaster` writes; 4 of 5 indices wrong), `G2_Page_ESPNow.cpp:1152`.

> 🚧 **Blocking note carried by that doc:** *do not start the lazy-per-peer history redesign (`LAZY_ALLOCATION_AUDIT.md` Tier 1) until §2.1, §2.2 and §6 are settled — that redesign's premise is memory arithmetic this audit found to be 6.5× wrong.*

---

## 8. `docs/ESPNOW_DEAD_CODE_AUDIT.md` — 2026-07-21
**Covers:** dead code in the ESP-NOW subsystem. 130-agent map/find/verify sweep. **53 DEAD** (both verifiers agreed), 0 board-gated, 0 uncertain, 2 flagged-then-cleared-LIVE. **Nothing has been deleted.**

Method note worth reusing: it first mapped the indirect-usage surfaces (`kV4HandlerTable[]`, `espNowCommands[]`, `espnowSettingsModule`, web routes, OLED/G2 registries, function-pointer/deferred sites, X-macro names, `#if ENABLE_BONDED_MODE`) so "no direct caller" wouldn't produce false positives.

Groups (all still open, all pending a removal-verification pass — §"Flat checklist" at the end is built for that): dead TX senders (`sendChunkedResponse` `:907`, `v4_send_text` `:2540`, `v4_broadcast_topo_request` `:2284`, `v4_send_topo_request` `:2269`); the 4-member `gLastSentFriendlyName/Room/Zone/Tags` cluster (`:358-361`, definition-only); orphaned accessors (`espnowCollapsedPeerMessages`, `espnowtx::getStats`, `sendStatusGet`, 3 OLED helpers); the **whole unwired OLED remote-file-browse feature** (4 functions, no enum routes to them); vestigial `MSG_TYPE_*`/`PAYLOAD_*` string macros; unused mesh tuning constants, V4 flag bits, size constants; dead header inlines; uninstantiated structs; a write-only field. **Reserved event-category enum bits are dead as identifiers but must be KEPT for wire stability.**

---

## 9. `docs/LOGGING_TAG_AUDIT.md` — 2026-07-19/20 · investigation only, nothing changed
**Covers:** how log lines acquire subsystem tags, how `/logging` parses/filters them, taxonomy gaps. Every claim cites the line it was read from.

**All findings still open:**
- **2.1 HIGH** — the viewer cannot parse the four always-on admin logs. `buildTimestampPrefix()` emits `[YYYY-MM-DD HH:MM:SS.mmm] | ` but every viewer regex requires `^\[(\d+)\]`, so **errors.log, system-events.log, the [EVLOG] stream, and i2c_errors.log render entirely as UNKNOWN gray rows**. The `[ms=N]` no-NTP fallback fails too.
- **2.2 HIGH** — the SR debug bank (bits 88-93) has **zero emitters**; `System_ESPSR.cpp:46-47` defines `DEBUG_SRF` as `DEBUG_MICF(...)`, so SR is gated on `DEBUG_MICROPHONE`. Toggling any SR flag does nothing.
- **2.3 MED** — ERROR/WARN macros pass `0xFFFFFFFF` as the category mask; `getDebugCategoryName` returns the **first** match = bit 0 = `DEBUG_AUTH`, so every ERROR/WARN line in the system log reads `[AUTH]`.
- **2.4 MED** — ~40 writer-side sub-category names are unreachable (parent checked first for every family except G2).
- **2.5 MED** — the Bluetooth debug bank (bits 64-67) has zero emitters; biggest missing tag family in the system.
- **2.6 MED** — split tag vocabulary (`USERS`/`USER`, `HTTP`/`WEB`, `CLI`/`CMD`, …); the filter dropdown treats each as separate.
- **2.7–2.11 LOW/MED** — mixed-case inline tags invisible to the uppercase-first regex (`[AutoStart]` ×30); ghost `[SECURITY]` tag nothing emits; **dead flags-pane checkbox `flag-espnow-enc` sets freed bit 37** (`WebPage_Logging.h:390`); `[EVENT][BOOT]` category collapse loses 25 distinct subsystems; **~62 of ~106 emitting flags have no checkbox**; missing color keys for most real tags.
- **Behavioral trap worth knowing (§1):** `log start flags=0x…` **overwrites the global `gDebugFlags`**, and `log start` without `flags=` silently restores the last-used mask over whatever the user configured per-flag. The "log file filter" is really the device-wide emission filter for serial/web/BLE too.
- **Caveat:** emitter counts are from static grep sweeps; the multi-agent verification pass died on a usage limit, so findings were verified by direct file reads instead.

---

## 10. `docs/LLM_SETTINGS_AUDIT.md` — 2026-07-10 · **AUDIT ONLY, nothing implemented**
**Covers:** LLM settings vs reality — 37 findings (16 QUICK / 13 REAL / 2 STRUCTURAL / 6 ASK-USER).

Headline still open: the settings mostly **don't do anything** on the two main surfaces. Web chat force-sends 5 hardcoded params every message (`WebPage_LLM.h:344-355`); bare CLI `llmgenerate` uses compile-time defaults (`System_LLM.cpp:2328-2331`); `llmmaxcontext`'s only consumer is overridden by a hardcoded `max_ctx:64` (`WebPage_LLM.h:618`) so **every web-loaded model runs at half context**; bare `llmload` ignores `llmdefaultmodel`; OLED sends raw unframed prompts (`OLED_Mode_LLM.cpp:368`).

**Directly relevant to Track A/C even if you skip the LLM domain:**
- **Q3** — `temperature` and `mirostatEta` declare min=max=0 → **no validation**; `llmtemperature 99` persists and displays 99 while running 2.0 (`System_LLM.cpp:2492, :2502`).
- **R10** — **setting parse hardening across ALL modules:** `atoi`/`strtof` accept garbage, so `llmtopp abc` persists 0.0 and `llmautostart on` saves FALSE (`System_Settings.cpp:2413/:2437/:2452`). This is a general input-validation finding, not an LLM one.
- **R5** — failed sync generation returns "Generation error" with no `Error:` prefix → audit-logged as success + `OK:` stamp. Uniform-return-contract violation (Track C1).
- **Q11** — `llmstatus` embeds a stale `Error:` inside an `OK:`-stamped reply; `errorMsg` never cleared on successful load.

> ⚠️ Memory note says the model needs reconversion for the guided-input menu — LLM findings may be stale in either direction. Low priority for this run.

---

## 11. `docs/CLI_HELPTEXT_AUDIT.md` — 2026-06-28
**Covers:** CLI help-text vs behavior across 22 modules (ESP-NOW audited separately in `ESPNOW_HELPTEXT_AUDIT.md`). **122 verified, code-cited findings**; `debug` alone accounts for 79.

Cross-cutting patterns (assume all still open — no fix banner on the doc):
1. Undocumented `[temp|runtime]` arg on ~70 `debug*` commands.
2. Undocumented `json` output flag on ~25 status/read commands.
3. **~13 stored-but-never-read settings, several High** — camera auto-capture suite, OLED boot mode/duration, `thermalWebMaxFps`, `webCliHistorySize`/`oledCliHistorySize`, dead `debugi2cbus`/`debugi2cdiscovery` streams.
4. **Role/auth overclaims & gating bugs (Track B7!)** — `wifigettxpower` is a **non-admin alias of an admin setter (privilege bypass)**; `cpufreq` gates its *read* path behind admin; `setgamepadpassword` has an undocumented OLED-login precondition.
5. "Says OK but…" — `thermalread`/`sddiag` broadcast instead of returning; `eicontinuous`/`fmradiounmute`/`ledeffect` execute the wrong thing.
6. References to 5 non-existent commands (`wificonnect`, `wifiinfo`, `apdscolorstart`, `apdsproximitystart`, `apdsgesturestart`, `rtc`).

**Companion `ESPNOW_HELPTEXT_AUDIT.md`** covers the ESP-NOW module separately (e.g. `espnowkeyex`/`espnowsessionopen`/`espnowrekey` usage says `<mac>` but a device name is accepted; stale "once Phase 3.3 lands" text where 3.3 has shipped).

---

## 12. `docs/DOCS_ACCURACY_AUDIT_2026-07-26.md`
**Covers:** README / QUICKSTART / USERGUIDE against the source tree at v0.99.3 (896 registered commands across 27 modules extracted from `CommandEntry` arrays).

**Status: mostly fixed.** Still **open**: 1.1 (`HW_BOARD` never documented — flashing a XIAO by the QUICKSTART produces a FeatherS3 image, `CMakeLists.txt:47-115`), 1.4 (the whole MQTT CLI section documents commands that do not exist), 1.9 (README lists hardware with no driver), Tier 5 (build-config table drift), Tier 6 (smaller corrections).

**Registry drift sweep (reproduce with `python3 tools/command_registry.py audit`):** wiring is clean — no orphaned arrays, all 276 `SettingEntry` `cmdKey` bindings resolve. **Three duplicate registrations still open** (`registerCommand()` appends without dedup; the first wins, the second is a dead slot): `espnowenabled`, `pendinglist` (dead copy documents `json`, winner does not), `serialrequireauth`. Registry is at **897 of `MAX_COMMANDS` 1024**. `voicecancel`'s double registration is deliberate (two voice phrases) and allow-listed.

---

## 13. Plan docs describing known-open defects

| Doc | Status | What's open |
|---|---|---|
| `WIFI_RADIO_OWNERSHIP_PLAN.md` | **Plan only** | **D1** `WiFi.setAutoReconnect(false)` is called **nowhere** — after `closewifi` the driver hunts the lost AP forever, and that reconnect **scan hops channels**, knocking a pinned ESP-NOW channel off the air (`System_WiFi.cpp:397`). **D2** status surfaces read radio *mode* not *association*; `getQuickWiFiState` returns `WiFi.getMode() != WIFI_MODE_NULL` but ESP-NOW holds `WIFI_AP_STA`, so the OLED toggle is stuck ON and the `openwifi` branch is unreachable (`OLED_SettingsEditor.cpp:995-999`). **D3** `openwifi` while ESP-NOW is up downgrades AP_STA→STA and never restores. **D4** nuclear reinit calls `esp_wifi_deinit` without `esp_now_deinit` first (`System_WiFi.cpp:820-836`). **D5** no persisted "user turned WiFi off" intent. **← the single best prior-art match for pitfalls P32/P33 (A11).** |
| `AUTOSTART_NAMING_UNIFICATION_PLAN.md` | **Plan only**, written against 889f1b8 | cmdKey-fallback trap; naming inconsistency across autostart surfaces |
| `DEBUG_FLAG_XMACRO_PLAN.md` | **Plan only** | (superseded in part — memory says arcs A+B+C are complete and uncommitted, HW test pending) |
| `G2_KEYBOARD_IMPROVEMENT_PLAN.md` | **Design only** | REBUILD-reset root cause; UPDATE_TEXT compound fix HW-proven except the post-CLICK highlight probe |
| `SENSOR_READING_ENVELOPE_PLAN.md` | **Plan only, not started** | envelope standardization |
| `SENSOR_RENDERING_UNIFICATION_PLAN.md` | Level 1 executed, **Levels 2–3 plan-only** | |
| `LLM_RETRIEVAL_HYBRID_PLAN.md` | **Plan only** | |
| `DEFLOCK_CAR_DETECTOR_PLAN.md` | **Plan only** | also carries the §9 `System_RfMode` radio-arbiter design that WIFI_RADIO_OWNERSHIP defers to |
| `G2_MIC_SOURCE_PLAN.md` | Design verified, **implementation not started** | |
| `SENSORLOG_PERDAY_APPEND_PLAN.md` | Built green feathers3, uncommitted | **E12 is a shipped regression:** `WebPage_Maps.cpp:~245` and `OLED_Mode_Map.cpp:~602-616` track pickers skip subdirectories entirely, so autostart-resumed TRACK logs under dated subfolders are **invisible today**. Accepted limit: millis()-wrap breaks boot-ms rows past 49.7 unsynced days (**P42/A16 overlap**). |
| `ESPNOW_FILE_TRANSFER_INTEGRITY_PLAN.md` | Implemented, uncommitted, **HW test pending** | Round-3 review found two of its own round-2 fixes wrong, one dangerously (staging-path collision could delete/truncate a live transfer, and the receive CRC **cannot catch it** — accumulated over bytes as they arrive, never re-read from flash) |
| `ESPNOW_RELAY_RESTORE_PLAN.md` | **Phase 0 implemented** (uncommitted, built green), Phases 1+ not started | mesh is still a single-hop star; `ttl` is write-only; `espnowmeshttl`/`meshAdaptiveTTL` feed nothing |
| `R1_HEALTH_FIXES_PLAN.md` | Fixes 1–5 described; shipped in v0.99.3 | **Root cause of ring HCI `0x08` drops still open** (see `R1_AUTOCONNECT_DISCONNECT_HANDOFF_2026-07-26.md`). Two real byte-for-byte BLE replay sources at `G2_Ring.cpp:214` (retry after a false-negative `writeValue()`) and `:784` (`resetSerial()` on every reconnect makes pairAuth on link N+1 identical to link N). Also: widening TRACK-format coercion would flip an **already-open file** to CSV mid-session with no header. |
| `ESPNOW_MESH_SYSTEM.md` | Reference | `espnowsessionsend` delivers TEXT that is never executed — a command sent via sessionsend lands as chat and returns no output silently (`:9648`, `:2667`, `:14112`) |
| `STACK_TO_PSRAM_CANDIDATES.md` | **List-only, nothing changed** | ⚠️ Its central open question: most web handlers are marked `per-call-heap-spiram` **conservatively**. If `esp_http_server` here runs the IDF default single task, every web handler is non-reentrant and the whole web set collapses to cheap `static-psram`. **"Confirm the httpd task model before implementing"** — nobody has. Relevant to A7/A8 (reentrancy assumptions). |
| `COMMAND_RESULT_DELIVERY_PLAN.md` | Phase 1 shipped v0.99.3 | §4.4 **superseded** — one `Serial.write()` is already atomic (USB-CDC per-call `tx_lock`). Do not re-propose queue machinery. |
| `CMD_JSON_CONVERSION_PLAN.md` | Waves partially done | Wave 3 (action commands) open |

---

## 14. Non-doc open-defect registries (memory files — read these for Track B)

`/Users/morgan/.claude/projects/-Users-morgan-esp-hardwareone-idf/memory/`

**`project_security_review_backlog.md`** (snapshot 2026-07-20 — *re-verify, this is the B4 target*). 8 ranked still-open items:
1. **`AuthBypass` can be a real account.** First-time setup (`System_FirstTimeSetup.cpp` ~:323, ~:759) skips `isValidPublicUsername`, so the owner can be named `AuthBypass` and is written `role="superadmin"`. With `blerequireauth` off, every BLE peer is stamped that identity → super-admin. `USER_SYNC` can plant the name unvalidated too.
2. **MQTT bridge bypasses `authorizeCommand`** (`System_MQTT.cpp` ~:430-455 returns *before* `executeCommand`, calling `cmd_espnow_roomcmd`/`tagcmd`/`remote` directly — all `requiresAdmin=true`). Also `/api/gps/tracks` is guest-allowlisted and reaches `executeCommandThroughRegistry` (takes no `AuthContext`).
3. **ESP-NOW metadata stored XSS → admin RCE.** `METADATA_REQ/RESP/PUSH` dispatch with flag `0`; `deviceName` `strncpy`'d raw into `gMeshPeerMeta`, rendered unescaped into `innerHTML`. **`esc()` in `WebServer_Utils.h` is a JS-string escaper, not HTML — ~15 call sites are unsafe; the real one is `escHtml`.** No CSP anywhere; `/api/cli` same-origin reachable.
4. **No BLE link-layer security at all** — zero hits for `NimBLESecurity`/`setSecurityAuth`/`ble_sm`. Peer identity is a bare stored MAC.
5. `espnowroomcmd`/`espnowtagcmd` missing from the audit redaction table `kRules` → cleartext peer passwords in `command-audit.log` and every output lane.
6. `System_MQTT.cpp` ~:385 debug-logs the full inbound payload including the cleartext password.
7. `discardStagedMigrationRestore` clears its flag *before* the free — a window where an inbound POST frees a live buffer.
8. Restore confirm prompt shows no bundle identity.

*Fixed & holding:* restore stage-then-confirm; `REQ_SESSION_ENC` on FILE_* rows; `v4FileBondMagicAllowed`; `isAdminUser("")`/`isSuperAdminUser("")` returning **true** (fixed 2026-07-20, built green, awaiting HW). *Residual:* `pathWithinScope` is a bare `startsWith` that **would** pass `/espnow/received/<tok>/../../system/x` — `normalizeFsPath` is the single chokepoint with no redundancy, and the "defense-in-depth" comment near the FILE_START write is misleading. *Perf:* the guest gate makes a command cost up to **5 reads+parses of users.json**, each under `FsLockGuard` (corroborates HOT_PATH #2).

**`project_v0_99_1_known_gaps.md`** — 3 verified defects knowingly shipped:
1. `espnowsensorstream` has no working on-ramp on a mesh (`transmitSensorData`'s mesh branch hard-returns unless `gSensorControllerMac` is set); the web toggle reports success while nothing arrives.
2. **OLED Config > Users is dead on any `ENABLE_ONDEVICE_LLM 0` build (the documented XIAO camera build)** — `oledUserManagerModeInit()` extern + call sit inside `#if ENABLE_ONDEVICE_LLM` in `OLED_Utils.cpp` while `OLED_Mode_UserManager.cpp` compiles unconditionally. **Board-gated: harmless on FeatherS3.**
3. Web ESP-NOW user-sync role dropdown off by one (form is orphaned, zero user impact).

---

## 15. Pitfall (P1–P45) → prior-art collision map

Use this to decide whether your A-item has prior art to reconcile with, or is greenfield.

| Pitfall | Prior coverage | Verdict |
|---|---|---|
| P1, P2, P3 (IRAM ISR / PSRAM during flash op / const in flash) | none | **greenfield** |
| P4, P5 (PSRAM DMA / exec) | HEAP_OFFLOAD §4 exclusions (DMA noted as an exclusion reason) | mostly greenfield |
| P6, P7, P8 (DRAM exhaustion, fragmentation, IRAM growth) | **HOT_PATH_HEAP_AUDIT_MECHANICS** = the whole answer; also LAZY §10 coverage accounting | heavily covered — read mechanics first, then look for what it *didn't* test (it ran no build, no HW profile) |
| P9, P11 (stack DRAM, never measured) | ESPNOW_COMMENT §5 (4× myth), MECHANICS §1 (byte budget) | budget is known; **HWM instrumentation coverage is not** |
| P10, P12 (overflow detection, task returns) | none | **greenfield** |
| P13 (TWDT / no yield) | PRE_1_0 short-list #2 (thermal/ToF busy-spin) — **still open** | one known site; sweep for others |
| P14, P15 (ISR misuse) | none | **greenfield** |
| P16, P17 (task churn, priority inversion) | MECHANICS §2(b) per-UI-action G2 workers; `feedback_avoid_per_action_tasks` | partial |
| P18 (boot WDT) | none | **greenfield** |
| P19–P23 (SMP concurrency) | SETTINGS §4.5 (no `gSettings` mutex, unpinned `g2_tap_disp`); LAZY (GPSTrackManager `getPoints()` race, `sBannerQueue` and `gFrameRing` lock-free two-writer races) | scattered leads, **no systematic pass** |
| P24 (core pinning) | `feedback` memory says ALL tasks pinned, policy in `System_TaskUtils.h`; SETTINGS §4.5 names `g2_tap_disp` as **unpinned** — contradiction worth checking | **contradiction = fertile** |
| P25–P28 (I2C) | none | **greenfield** |
| P29, P30, P31 (ADC2, strapping, octal PSRAM GPIO) | none | **greenfield** |
| P32, P33 (ESP-NOW channel, power-save) | **WIFI_RADIO_OWNERSHIP_PLAN** D1–D5 | well covered, plan-only |
| P34, P35 (coex, channels 12-14) | MECHANICS §1 (coex sdkconfig verified) | partial |
| P36, P37 (NVS) | SETTINGS §Environmental (cleartext WiFi creds in NVS) | thin — **greenfield for wear/frequency** |
| P38, P39 (atomic writes, full FS) | **SETTINGS_LIFECYCLE_AUDIT §4.1–4.7** — and P38's generic form is **REFUTED** on littlefs | heavily covered; file residues only |
| P40 (flash write blocks) | HEAP_OFFLOAD §2b reasons about "snprintf'd after any LittleFS op returns" | thin |
| P41 (esp_err_t ignored) | none | **greenfield** |
| P42 (millis rollover) | SENSORLOG plan (accepted limit); HOT_PATH's unverified `nextFire()` bug | thin — **greenfield for a systematic sweep** |
| P43, P44 (buffers, snprintf truncation) | security backlog #3 (`strncpy` of `deviceName`); ESPNOW_COMMENT §3.2 (the 6143 clamp) | scattered |
| P45 (brownout) | `project_powersave_crash_loop` memory (`setCpuFrequencyMhz` vs gps_task) | one known cause; brownout config itself unaudited |

---

## 16. Refuted / corrected claims — do NOT re-file these

Filing any of these wastes the run. Each was investigated and knocked down.

1. `out.reserve(f.size()+1)` before `out = f.readString()` — **makes it strictly worse** (forces `String::move()` onto memmove).
2. `executeCommand`'s dead `String lc` / `normalizedCmd` — **gone from the tree.**
3. BLE notify path as a **30 KB/s steady-state** drain — `outble` can't survive a reboot; it's a burst cost.
4. `HardwareOne.cpp:753` 4 KB capture buffer as "all web CLI capture requests" — no in-repo client sends `capture=1`; single-threaded worker gets the same block back each time.
5. Truncate-then-write on littlefs losing data on power loss — **refuted** (copy-on-write + `LFS_F_ERRED`).
6. users.json corrupt-delete → unauthenticated device — **refuted** (wizard blocks on physical serial/OLED input before WiFi starts).
7. "Boot usually rewrites settings.json" — **wrong premise**; `setSetting` is change-gated.
8. `systemLogSettingEntries` const→flash — **not clean** (runtime `String::c_str()` forces dynamic init).
9. `gOledFileManager` "never freed" — there is a `delete` at `OLED_Mode_FileBrowser.cpp:1442`.
10. `gPeerIdentities` PSRAM/DRAM contradiction — **does not exist** (fixed; three agents confirmed).
11. Multi-fragment TEXT being reassembled device-side — **false**; `System_ESPNow.cpp:4713` excludes `ESPNOW_V4_TYPE_TEXT` from reassembly.
12. Serial output needing a queue/mutex/drain-barrier — **one `Serial.write()` is already atomic** (USB-CDC per-call `tx_lock`).
13. `dynTemp` as a zombie feature — verified fully plumbed and reachable.
14. `voicecancel` double registration — deliberate (two voice phrases), allow-listed.
15. `*_STACK_WORDS` as word counts — they are **BYTES**. Any finding whose arithmetic uses ×4 is wrong. Note the myth is *live in comments too* (`Handlers_Crypto.cpp:716`, `FsList.h:51`, `FsList.cpp:463/902`, `System_Camera_DVP.cpp:983-984`), so a comment agreeing with you is not corroboration.

---

## 17. Reusable method notes from prior runs

- **Two independent adversarial lenses** (hotness + allocation arena) is what separated the 27 CONFIRMED from the 33 disputed in HOT_PATH. One lens refuting = DISPUTED, and 24 of 33 disputes were **frequency** miscalibrations on real allocations — the mechanism was right, the tier was wrong.
- **Map indirect-usage surfaces before declaring anything dead** (ESPNOW_DEAD_CODE's method): handler tables, command tables, settings modules, web routes, OLED/G2 registries, function-pointer/deferred sites, X-macro-generated names, `#if` config gates.
- **`feedback_verify_fixes_not_just_findings`** — the security backlog got this wrong once. A broken prescription is worse than none. Every `Fix:` line in HOT_PATH is currently unverified for exactly this reason.
- **The comment-truthfulness heuristic** (ESPNOW_COMMENT §1): *these files contradict themselves, and the copy nearer the code is the true one.* Headers and section banners lie; inline comments next to the code are reliable. Anything a compiler or `static_assert` can check is scrupulously accurate.
