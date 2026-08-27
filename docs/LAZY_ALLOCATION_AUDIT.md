# Lazy Allocation Audit

> **STATUS 2026-07-16 (later):** the four items marked ✅ below are **implemented and building green** (feathers3/esp32s3), awaiting HW validation. `.dram0.bss` measured **106,329 → 73,589 B — 32.7 KB of internal DRAM freed**; `.ext_ram.bss` +2,112 B (two deliberate PSRAM moves). See §9 for what shipped, what changed vs the recommendation, and the two rule violations caught during implementation.
>
> **STATUS 2026-07-25:** additional lazy gates landed for the web JSON/mirror buffers, ESP-NOW remote sensor cache, adaptive debug message pool, OLED CLI history, automation due-check cache, BLE output reserve, and G2 first-use workers/RX buffers. These changes passed `git diff --check`; firmware build was not run in this shell because `idf.py` was unavailable on PATH. See §9b for the delta.

**Date:** 2026-07-16
**Method:** 118-agent multi-pass audit: one auditor per subsystem (LLM, Automations, Maps, ESP-NOW, Bluetooth/G2/Ring, Web, boot-task inventory, misc/common), every finding ≥256 B adversarially verified against source + the linker map from the 2026-07-16 build (`build/hardwareone-idf.map`), a dedicated re-verification pass for first-use "ratchet" allocations, a completeness-critic pass whose 4 flagged gaps were audited+verified in a second round, and a whole-map coverage accounting pass (result: **zero unaudited application DRAM symbols ≥512 B** — see §9).
**Scope:** allocations that are paid **eagerly** (at boot, or at feature-init long before use) that could instead be allocated on first use / feature enable, and ideally freed on disable. Internal DRAM first; PSRAM second.

Static baseline from the linker map: `.dram0.bss` **103.8 KB** + `.dram0.data` **31.7 KB** internal DRAM; `.ext_ram.bss` **171.5 KB** PSRAM statics.

> **Cross-cutting discovery (re-confirmed):** the words-vs-bytes stack footgun is *live in the comments, not just the names*. Auditors trusting in-code "stack in WORDS" comments produced 4×-inflated sizes for `g2_tap_disp`, `g2_ble_connect`, `g2_page_swap_w`, and `mapRender`; verifiers refuted each one against `portSTACK_TYPE=uint8_t` (IDF v5.5.x: `xTaskCreate` depth is **bytes**). Every task-stack number below is the verified byte value.

---



## 1. Executive summary

The codebase is already structurally lazy in the right places: **LLM weights/KV/tokenizer** (PSRAM, freed on unload), **camera framebuffers** (on `cameraon`), **mic/ESP-SR/EdgeImpulse**, **MQTT client**, **sensor pollers** (setting + hardware double-gate, self-delete on stop), **httpd** (start/stop frees everything), **maps tile cache** (on map load), and **all i2csensor driver objects** (on I2C detection). The eager remainder falls into three families:

1. **Boot-unconditional statics/allocs for optional features** — originally ~55–65 KB internal DRAM plus large PSRAM buffers existed even when their feature was never used. Several top items are now implemented: LLM worker stack, automation event buffer, OLED console/history, automation due-check cache, BLE output reserve, ESP-NOW remote sensor cache, and the web JSON/mirror buffers.
2. **First-use ratchets** — lazily created but never torn down: ~~1.35 MB PSRAM tile cache pinned by merely browsing past OLED Map mode, **~~867 KB PSRAM ESP-NOW working set retained after `closeespnow`** (dominated by `peerMessageHistories`), 120 KB GPS track buffer, the `mapRender` task (8.2 KB DRAM + 10 KB PSRAM buffers), the cam_pwr worker, `gLLMResultBuf` (8 KB PSRAM surviving `llmunload`).
3. **Missing free-on-disable** — features gated correctly at init but retaining workers after use: G2 page/tap/FSM workers are now lazy but still persist once created, the `espnow_tx` dispatcher (~6.5 KB DRAM) survives `closeespnow`, HTTPS PEM strings survive `closehttp`.

Two pieces of outright **dead weight**: `gAutoMemoId`/`gAutoMemoNextAt` (1,536 B PSRAM allocated every boot, provably never read — delete) and `BLE_IDF.cpp` (linker-GC'd, delete for hygiene).

**Framework gap that blocks several teardowns:** `OLEDModeEntry` has `onEnterFunc` but **no** `onExit` **hook** — Map-mode/ESPNow-mode/Automations-mode teardown all need one added at the `requestOLEDMode()` chokepoint (`OLED_Utils.cpp:2840`).

**Non-POD warning:** there is **zero precedent** in the codebase for `EXT_RAM_BSS_ATTR` on String-bearing/non-trivially-constructed objects (every existing use is POD). Findings proposing PSRAM moves for `gOledEspNowState`, `gBlePeerData`, AuthContext owners etc. must either verify global-ctor-vs-PSRAM-init ordering once, or convert to pointer + placement-new instead.

**Secrets rule interactions** (flash encryption is OFF → PSRAM is probeable plaintext): do **not** PSRAM-move `gSc` (X25519 secrets), `gMeshDerivedKeys`/`gIdentity`, the HTTPS private key, or `gOledEspNowState` as a whole (holds a typed remote password — exclude those String members). `gSessions` (ESP-NOW AEAD session keys) is *already* PSRAM heap in violation — noted below.

---



## 2. Internal DRAM — boot-unconditional, ranked

"Trigger" = when the memory is consumed. All confirmed by adversarial verification unless marked ⏳.


| #   | Symbol                                                         | Bytes  | File                                                      | Fix shape                                                                                                                                                                                                                                                                                                                          |
| --- | -------------------------------------------------------------- | ------ | --------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | ✅ `gLLMWorkerStack`+TCB                                        | 12,664 | `System_LLM.cpp:170`                                      | **DONE** — lazy heap stack in `llmEnsureWorker()`, retained (never freed), worker never deleted. §6.1's "redesign the handshake" was superseded: the handshake was *removed*. See §9                                                                                                                                               |
| 2   | ✅ `sEventBuf` (automation scheduler tick)                      | 7,872  | `System_Automation.cpp:3619`                              | **DONE 2026-07-16** — internal heap allocation on first scheduler tick, with the security/spinlock caveats in §9                                                                                                                                                                                                                   |
| 3   | `gEventRing` (typed event ring)                                | 7,872  | `System_Events.cpp:121`                                   | Can't be lazy (boot event record) but nothing pins it internal: `EXT_RAM_BSS_ATTR`; optionally shrink `SYSEVT_DETAIL_LEN` 80→48 / `SUBJECT` 48→32 (~2.3 KB, also shrinks #2 twin)                                                                                                                                                  |
| 4   | ✅ `gOledConsole`                                               | 6,808  | `OLED_Utils.cpp` / `OLED_ConsoleBuffer.h`                 | **DONE 2026-07-25** — internal-DRAM ring/mutex are allocated only after OLED display init succeeds (`earlyOLEDInit` / `initOLEDDisplay`). Devices with OLED disabled or absent keep the ring unallocated. Kept internal because echoed OLED CLI commands can contain credentials                                                   |
| 5   | `sensor_queue_task` stack                                      | 4,472  | `HardwareOne.cpp:1515`                                    | Delete boot creation; `processAutoStartSensors` already has an identical late-init guard, add same ensure-create (mutex-guarded) in `enqueueDeviceStart()`                                                                                                                                                                         |
| 6   | `commandRegistry`                                              | 4,096  | `System_Command.cpp:27`                                   | `EXT_RAM_BSS_ATTR` (sibling `registeredModules` already is); optionally trim `MAX_COMMANDS` toward real count                                                                                                                                                                                                                      |
| 7   | ESP-NOW OLED UI statics (`gOledEspNowState` + refresh bufs)    | 3,312  | `OLED_ESPNow.cpp:38`                                      | Pointer + placement-new in `oledEspNowInit()` — **not** blanket PSRAM-attr: holds typed remote credentials (secrets rule) and is non-POD                                                                                                                                                                                           |
| 8   | `scratchData` (MLX90640 vendored lib)                          | 3,072  | `hardwareone_libs/.../MLX90640_API.cpp:22`                | Pure init-time scratch for `ExtractParameters`; ps_alloc/free around extraction, or one-line `EXT_RAM_BSS_ATTR`                                                                                                                                                                                                                    |
| 9   | G2 CLI handler static ret/out/err bufs (~19)                   | 2,784  | `G2_Glasses.cpp`                                          | `EXT_RAM_BSS_ATTR` sweep — stragglers of the already-applied G2 Group A pattern                                                                                                                                                                                                                                                    |
| 10  | ✅ `initDebugSystem` eager heap (queues + BLE reserve)          | 2,784  | `System_Debug.cpp`                                        | **BLE reserve DONE 2026-07-25** — queues remain core; `gBLEOutputBuffer.reserve(1024)` moved out of debug init and runs on first authenticated BLE-routed output. Also reduced the debug message pool's initial allocation to 96 slots with one-time PSRAM growth before saturation                                                |
| 11  | ✅ `gAllocTracker`                                              | 2,560  | `HardwareOne.cpp`                                         | **DONE 2026-07-25** — diagnostics table moved from internal `.bss` to PSRAM `.bss` with `EXT_RAM_BSS_ATTR`; allocation hook/reporting behavior is unchanged                                                                                                                                                                        |
| 12  | `gEspNowRxRing`                                                | 2,112  | `System_ESPNow.cpp:354`                                   | Pointer + internal-heap alloc in `initEspNow` before recv-cb registration, free in `deinitEspNow` after unregister+quiesce. **Must** replace both `sizeof(array)`-derived ring-size computations (`:965`, `:7757`) — as a pointer they become `% 0` in the WiFi-task callback                                                      |
| 13  | `g2ProbeImageQ*` static result bufs (~11)                      | 2,068  | `G2_Glasses.cpp`                                          | `EXT_RAM_BSS_ATTR` sweep (same as #9)                                                                                                                                                                                                                                                                                              |
| 14  | OLED map UI scratch (8 statics)                                | 1,960  | `OLED_Mode_Map.cpp`                                       | `EXT_RAM_BSS_ATTR` + hoist 3 function-locals to file scope; keep `sRenderSnapshot` (92 B) internal (render-task handshake)                                                                                                                                                                                                         |
| 15  | `~115 static httpd_uri_t` registration structs                 | 1,696  | `WebServer_*.cpp`, `WebPage_*.cpp`                        | **Add** `const` → moves to flash rodata, zero RAM; httpd deep-copies. Cleanest single win in the audit                                                                                                                                                                                                                             |
| 16  | ✅ `externalSensors[]` (MQTT, String ctors)                     | 1,664  | `System_MQTT.cpp`                                         | **DONE 2026-07-25** — converted from fixed `.bss` storage to a PSRAM heap block constructed on MQTT start and destructed/freed on `closemqtt`; reader/writer paths re-check pointer/count under the mutex during teardown                                                                                                          |
| 17  | ✅ `gAutoCache[64]`                                             | 1,536  | `System_Automation.cpp`                                   | **DONE 2026-07-25** — converted to internal heap pointer, allocated when the automation scheduler starts or first rebuilds, released by `stopAutomationScheduler()` when the system is disabled                                                                                                                                    |
| 18  | espnow small DRAM statics (7 symbols)                          | 1,432  | `System_ESPNow.cpp`                                       | Fold into one heap `EspNowRuntime` block (init/deinit pair). Members hold Strings → placement-new/destruct, not memcpy. See §6.2 quiesce caveat                                                                                                                                                                                    |
| 19  | `gBroadcastTrackers`                                           | 1,216  | `System_ESPNow.cpp:1075`                                  | Into `EspNowRuntime`; add null gate incl. `cmd_espnow_broadcaststats` (guarded only by `gEspNow` today)                                                                                                                                                                                                                            |
| 20  | ✅ `gFrameRing` (G2 post-mortem ring)                           | 1,152  | `G2_Glasses.cpp`                                          | **DONE 2026-07-25** — moved from internal `.bss` to PSRAM `.bss` with `EXT_RAM_BSS_ATTR`; no heap behavior change                                                                                                                                                                                                                  |
| 21  | G2 misc caches (excl. `gL`/`gR` shells)                        | ~944   | `G2_Glasses.cpp`                                          | `EXT_RAM_BSS_ATTR` for POD parts; AuthContext owners are non-POD — pointer-convert or leave. Keep `gL`/`gR` internal (BTC notify hot path)                                                                                                                                                                                         |
| 22  | `gPeerIdentities`                                              | 896    | `System_ESPNow_Identity.cpp:293`                          | `EXT_RAM_BSS_ATTR` — public keys only, no secret. Overrides an explicit hot-path DRAM comment (`:288-291`); update both comments                                                                                                                                                                                                   |
| 23  | Maps core statics                                              | 785    | `System_Maps.cpp`                                         | `EXT_RAM_BSS_ATTR`; two `.data` items become zero-init + set defaults in code                                                                                                                                                                                                                                                      |
| 24  | G2 Automations page statics (`rows`, `gAutos`, results)        | 1,392  | `G2_Page_Automations.cpp`                                 | `rows` → stack-local or PSRAM (deep-copied by `g2ShowListPage`); `gAutos`/`gRunResult`/`backRow` must outlive calls → `EXT_RAM_BSS_ATTR`                                                                                                                                                                                           |
| 25  | `sNotifView`                                                   | 704    | `OLED_Utils.cpp:202`                                      | `EXT_RAM_BSS_ATTR` (rebuildable view over the ring)                                                                                                                                                                                                                                                                                |
| 26  | OLED registries (`oledModeRegistry` + modules)                 | 512    | `OLED_Utils.cpp`                                          | `EXT_RAM_BSS_ATTR` the two registry arrays (ctor-timing verified safe); `registeredOLEDModules` deletable after boot summary. `**gOledKeyboardState` (128 B) is EXCLUDED** — its `text[33]` holds the typed device password / unlock pattern / change-password entry (secrets rule)                                                |
| 27  | `G2_Ring` cmd static bufs                                      | 600    | `G2_Ring.cpp`                                             | `EXT_RAM_BSS_ATTR` (stragglers of in-file sweep)                                                                                                                                                                                                                                                                                   |
| 28  | automation small statics (2× errorBuf, evt-fire pair, log ctx) | 536    | `System_Automation.cpp`                                   | Consolidate/relocate. NB: `executeConditionalCommand` runs on the **main loop task**, not cmd_exec — the two errorBufs *can* be live "simultaneously" across tasks; don't merge them into one buffer without checking                                                                                                              |
| 29  | `sKindLastShownMs`                                             | 540    | `System_Notifications.cpp:568`                            | Hoist to file scope + `EXT_RAM_BSS_ATTR`                                                                                                                                                                                                                                                                                           |
| 30  | `gSc` secure-channel table                                     | 512    | `System_BleSecureChannel.cpp:71`                          | Lazy **internal**-heap alloc in `bleScInit()`, `sodium_memzero`+free in `deinitBluetooth` under `gScTxMutex`. **Never PSRAM** (X25519 secrets)                                                                                                                                                                                     |
| 31  | `gBlePeerData[8]`                                              | 416    | `BLE_Peers.cpp:36`                                        | Can't be lazy (read at boot for mode selection); PSRAM move only via non-POD-safe route                                                                                                                                                                                                                                            |
| 32  | ✅ LLM small statics (`sTurns`, `sRenderLines`; diag reviewed)  | ~1,020 | `System_LLMChat.cpp`/`OLED_Mode_LLM.cpp`/`System_LLM.cpp` | **VERIFIED 2026-07-25** — `sTurns`, `sFramedPrompt`, and `sRenderLines` are already `EXT_RAM_BSS_ATTR`; downstream consumers use direct statics, not pointers, so no null-guard fallout. Remaining tiny profiler/prompt-diagnostic structs (`gProf`, `gPromptDiag`) are hot-path-adjacent and not worth moving without measurement |
| 33  | web bridges + cookie/debug-IP statics                          | ~616   | `WebPage_Bond.cpp`, `WebServer_Server.cpp`                | Bridges → ps_alloc in `webFsBridgeEnsureInit()` (+ null guards in ESP-NOW reply callbacks); `cookieBuf` holds live SIDs — keep internal in HTTPS mode (secrets rule)                                                                                                                                                               |
| 34  | `fsListInit` boot call                                         | 320    | `System_ESPNow_FsList.cpp` + `HardwareOne.cpp:1956`       | Move call into `initEspNow()` — every entry point already no-ops on null mutex                                                                                                                                                                                                                                                     |
| 35  | Bluetooth.cpp small statics                                    | 297    | `Bluetooth.cpp`                                           | `EXT_RAM_BSS_ATTR` batch with #9/#13                                                                                                                                                                                                                                                                                               |
| 36  | 6 of 8 boot mutexes (espnow×4, maps, mic)                      | ~530   | `System_Mutex.cpp:30`                                     | Only opportunistically; `gMapCacheMutex` has two independent consumer families → needs lazy-create-on-first-lock wrapper, not a single ensureInit                                                                                                                                                                                  |




**Realistic total: ~75–85 KB of internal DRAM** currently consumed at boot for features that may never run, recoverable via lazy-alloc or PSRAM relocation. (The two single largest levers elsewhere remain: Bluetooth controller+Bluedroid ~60 KB internal heap, paid because `bluetoothAutoStart` defaults true — flipping that default is a bigger lever than any item above; and `cmd_exec`/`debug_out`/`loopTask` core stacks, which are keep-as-is.)

## 3. Boot-gated but never freed on disable (toggle-off leaks)


| Symbol                                                                                                                               | Bytes        | Type               | Gap                                                                                                                                                                                                                                                                                                                                                               |
| ------------------------------------------------------------------------------------------------------------------------------------ | ------------ | ------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| ESP-NOW PSRAM working set (`peerMessageHistories` dominant, + `gEspNow` 15.8K, streamQueue 18.9K, deferred-resp 6.1K, reasm 6.4K, …) | **~867,500** | PSRAM heap         | `deinitEspNow` deliberately keeps everything (`:9299`). `PeerMessageHistory` = 162,016 B **each** (250 msgs/device); initial 5-slot block = 810,080 B. Full teardown is safe per re-init-from-null design, but 18 TUs deref `gEspNow` — audit `initialized`-only checks first. Also consider just shrinking `MESSAGES_PER_DEVICE` — 250 is the SPIRAM-build value |
| G2 client workers: `g2-fsm` (11,520) + `g2_tap_disp` (6,752) + `g2_page_swap_w` (3,616)                                              | ~21,900      | DRAM stacks+queues | **PARTIAL DONE 2026-07-25** — no longer spawned by plain `initG2Client`; FSM initializes on first dispatch, page worker on first page/lens job, tap worker on first tap dispatch. Shutdown after use is still open; see §6.3 blockers                                                                                                                             |
| `gL`/`gR` rxBuf reassembly                                                                                                           | 16,384       | PSRAM heap         | **PARTIAL DONE 2026-07-25** — moved from `initG2Client` to `connectTemple`, so G2 client/ring-only setup does not allocate temple RX buffers. Normal `closeg2` still retains them with the existing fast-reconnect cache policy; `closeg2 full`/deinit frees via `templeReset`                                                                                    |
| `espnow_tx` dispatcher stack + queue                                                                                                 | 6,484        | DRAM               | No `espnowtx::stop()` exists. Cooperative shutdown (poison job), wake `submitSync` waiters with FAIL, drain-free payloads; `vQueueDelete` while producers blocked is forbidden — quiesce first                                                                                                                                                                    |
| ✅ `g2_ble_connect` worker                                                                                                            | 5,136        | DRAM               | **DONE 2026-07-25** — now created by `g2SubmitBleConnect()` on first connect/reconnect submission and still freed on `deinitG2Client`                                                                                                                                                                                                                             |
| HTTPS PEM Strings                                                                                                                    | ~2,500       | DRAM heap          | `cmd_httpstop` never clears them; add `String()` move-assign clears after `httpd_stop`. Cert may move to PSRAM; **key stays internal**                                                                                                                                                                                                                            |
| `gBroadcasterBuf` (sensor bcast)                                                                                                     | ~1,000       | PSRAM              | Task is torn down correctly; buffer kept — free in `stopSensorBroadcaster` (mind cross-core delete race)                                                                                                                                                                                                                                                          |




## 4. First-use ratchets (lazy create, no teardown)


| Symbol                                               | Bytes                                                                                                                     | Trigger                                                                                                       | Fix                                                                                                                                                                                                                                                                                               |
| ---------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Map tile cache (`cachePool`+slots+names+tileDir)     | ~1,384,448                                                                                                                | **Browsing past OLED Map mode** auto-loads first `.hwmap` (`OLED_Mode_Map.cpp:1079-1087`)                     | `unloadMap()` exists and frees cleanly — call it from a new OLED Map exit hook with a cross-surface last-user check (G2 page + web share the map); or make auto-load opt-in                                                                                                                       |
| `GPSTrackManager::_points`                           | 120,000                                                                                                                   | Track load / live tracking; kept on `setLiveTracking(false)` **by design** (viewable/saveable)                | Free on Map-exit after auto-save prompt; **add sync first** — httpd reader iterates `getPoints()` unsynchronized vs GPS-task `appendPoint`                                                                                                                                                        |
| cam_pwr worker task + queue                          | **10,700** (claimed 41 KB — the 4× footgun again; the in-code comment at `System_Camera_DVP.cpp:983-984` is itself wrong) | First camera power request — **pre-warmed by merely opening the G2 camera page** (`G2_Page_Sensors.cpp:1521`) | Self-delete after power-off + idle queue; `cameraPwrSend` is check-then-send → needs a lifecycle mutex, and sync callers block on task-notify up to 60 s → drain notify-carrying messages before delete                                                                                           |
| `mapRender` task + `sMapBufA/B/ShadeBuf` + semaphore | 8,192 DRAM + 10,240 PSRAM                                                                                                 | First OLED map frame                                                                                          | Teardown in same Map-exit hook. **Never** `vTaskDelete` **from outside** — task takes `MapCacheGuard`; use quit-flag + self-delete after frame                                                                                                                                                    |
| `gLLMResultBuf`                                      | 8,192                                                                                                                     | First async generation; survives `llmunload`                                                                  | Free in `llmStopWorker` after confirmed worker exit; gate on the force-null timeout path (`:275`) — a wedged worker still writes via the token callback                                                                                                                                           |
| `WaypointManager::_waypoints`                        | 8,832                                                                                                                     | PSRAM static (boot)                                                                                           | Pointer + ps_alloc on first waypoint load; free-in-unloadMap is a UAF as proposed (httpd/OLED hold raw `getWaypoint()` pointers without `MapCacheGuard`) — allocate-lazy yes, free only with guard discipline                                                                                     |
| `gMLX90640` driver object                            | 4,748 (compiled `sizeof` with project toolchain)                                                                          | Thermal detect                                                                                                | Copy the GPS driver's ps_alloc + placement-new pattern (`i2csensor_pa1010d.cpp:136-141`). Two delete sites to convert (`:531` failure path AND `:1461` teardown); `begin()` plain-news an `Adafruit_I2CDevice` that the existing delete **already leaks** each stop/start cycle — fix while there |




## 5. PSRAM boot-eager (second priority)

Confirmed items worth acting on when touching the same files:

- ✅ `**gJsonResponseBuffer` 65,536 B** (`HardwareOne.cpp` / `WebServer_Server.cpp`) — **DONE 2026-07-25**: removed boot allocation; `startHttpServer()` now calls a shared web-runtime ensure path before handlers can use the buffer.
- ✅ `**gRemoteSensorCache` 20,224 B** (`System_ESPNow_Sensors.cpp`) — **DONE 2026-07-25**: converted from PSRAM `.bss` array to `ps_alloc` during `initEspNow()` via `initRemoteSensorSystem()`, with null guards added to external consumers.
- **ESP-NOW scratch statics ~11,978 B** — deliberate stack-avoidance buffers; several returned as `const char`* results, and `meshesJsonBuf` must work pre-init by design. Leave unless a PSRAM crunch appears.
- **G2_Glasses ext statics ~11,688 B** — intentional G2-Group-A output; leave (static-return-buffer idiom, BTC crash history on `gRows`).
- **SSE/system JSON statics 12,352 B** (`WebServer_Events.cpp:223`, `WebServer_Server.cpp:3029`) — per-request ps_alloc/free, or single-buffer build (drops 4 KB outright).
- ✅ `**gWebMirror.buf` 8,192 B** (formerly via `initDebugSystem`) — **DONE 2026-07-25**: initialized from the web-runtime ensure path instead of debug init. Tradeoff accepted: web CLI history no longer captures output before HTTP starts.
- **OLED_ESPNow ext statics 8,224 B** — fold into the same UI-state struct as #7 above if that lands.
- `**gHelpTail` 3,840 B** — transient help-render scratch; alloc at help-entry, free after flush; two concurrent writers → alloc at single entry point or CAS.
- `**gSessions` (ESP-NOW) 3,328 B** (`Sessions.cpp:26`) — delete the boot call at `HardwareOne.cpp:1946`; `sessionAllocate()` already lazy-inits and readers null-check. Add a lock: `sessionsInit` itself is unprotected and first-touch can race once lazy. **Holds live AEAD keys in PSRAM — pre-existing secrets-rule violation; consider PreferInternal while touching.**
- **web security state** — `sIpBans` 1,984 B (alloc in `loadIpBans`, accessors must keep working server-off), `sLoginAttempts` 416 B (**not web-scoped**: serial console auth gate uses it from the main loop — allocate on first record, not at server start), web `gSessions`+`gLogoutReasons` 2,088 B (move into `startHttpServer`; 4 unguarded consumers reachable server-off need guards, e.g. `buildSystemInfoJson` at `System_Utils.cpp:1644`).
- `**gPreInitEvents` 2,304 B** — dead weight seconds after boot; free in `flushPreInitEvents()` (null-before-free).
- `**gAutoMemoId`/`gAutoMemoNextAt` 1,536 B — DEAD. Delete** (`System_Automation.cpp:906-912`, externs in the header; zero readers anywhere).
- `**gFileSlots`+mutex 1,016 B** — move `fileSlotsInit()` into `initEspNow()`; keep `fileSlotsBootCleanup()` at boot (needs no table). Free-on-disable blocked by deferred cmd_exec jobs holding raw slot pointers.
- `**emb_a`/`emb_b` 4,096 B** (`System_LLM.cpp:1413`) — debug-only pre-forward analysis; gate on the `DEBUG_LLM_GENERATE` flag (also saves per-generation CPU).



## 6. Implementation blockers the verifiers found (must-read before coding)



### 6.1 LLM worker stack (top DRAM item)

The static stack exists **because of a documented fragmentation bug** (dynamic 16 KB alloc failed with model + G2 viewer up; comments `System_LLM.cpp:162-169`, `:247-253`). The lazy version allocates at the same clean-heap moment (model load) so it inherits the timing that worked — but the free path is the hard part: the worker **self-deletes** (`vTaskDelete(nullptr)` at `:196-198`) and `llmStopWorker` has a **force-null timeout path** (`:271-275`) that returns while the worker may still be running on the stack. Freeing there is a UAF; a self-deleted static-stack task also sits on the FreeRTOS termination list until **core 1's idle task** runs. Safe pattern: worker parks/suspends as its final act → `llmStopWorker` calls `vTaskDelete(handle)` on the non-running task → then free stack+TCB; leak-don't-free on the timeout path. Do **not** move this stack to PSRAM (generation is PSRAM-bus-bound; flash-op windows).

### 6.2 ESP-NOW runtime block frees

`deinitEspNow` ordering only quiesces `espnow_task`. Producers that survive it: `v4_broadcast*` on cmd_exec/web (write tracker fields after a single `initialized` check), `v4_send_encrypted_chunked` polls `ackWait->acked` **deliberately without the Tx guard** (`:196-200`, `:2026`) from cmd_exec/sensor_bcast — freeing the runtime block mid-chunked-send is a UAF a null gate cannot fix. Needs an in-flight-sender quiesce (busy-count or generation gate) before free. Struct members hold `String`s (TopologyStream, BufferedPeerMessage, MeshRetryEntry) → placement-new/destructors, not raw malloc/free.

### 6.3 G2 worker shutdowns

- `postEvent` treats null `gFsmQueue` as "apply inline on caller's stack" — fine pre-init, wrong post-shutdown (stragglers mutate FSM state from arbitrary tasks); shutdown must flip a state, not just delete the queue.
- `G2_Page_Network.cpp:1624/:1659` call `deinitG2Client()` **on the g2_tap_disp worker itself** — a tap-dispatcher shutdown inside deinit would `vTaskDelete` the calling task mid-function. Needs current-task detection + deferred teardown.
- Page-swap queue jobs carry per-kind heap payloads (PageSwapArgs/NotifySpec/CustomSpec) — a drain must free by kind.
- Producers (`tapDispatcherEnqueue*`) run on BTC_TASK — they already null-check and drop; keep that contract.



### 6.4 Sessions/EXT_RAM/misc

- `sessionsInit` (ESP-NOW) unprotected → add once-lock when lazy.
- `gEspNowRxRing` pointer conversion: fix both `sizeof`-based ring-size computations (`:965` → `% 0` crash in WiFi callback).
- ✅ `gOledConsole` lazy: `initOLEDDisplay()` runtime paths (`oledstart`, `oledenabled 1`) now ensure-init after display init succeeds.
- Anything `EXT_RAM_BSS_ATTR` + non-POD: no precedent in codebase — verify ctor ordering once or use pointer + placement-new.



## 6b. Completeness-pass additions (critic-flagged, audited + verified)

The completeness critic found four areas the subsystem auditors missed; all were audited and adversarially verified in a second round.

### OLED mode UI scratch (~4.8 KB DRAM statics + ~6.3 KB unfreed internal heap)

Sibling files got the earlier `EXT_RAM_BSS_ATTR` sweep; these were skipped because each file is individually small:


| Symbol                                                                   | Bytes | File                                    | Fix                                                                                                                                                                      |
| ------------------------------------------------------------------------ | ----- | --------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| animation buffers (`waveform` 512 + `stars` 480 + `fire` 256)            | 1,248 | `OLED_Mode_Animations.cpp`              | `EXT_RAM_BSS_ATTR` — exact in-file precedent exists (Life `grid` at line 207). Bonus: `waveform` is `int[128]` holding 0-255 values → `uint8_t` shrinks it 4× regardless |
| `gOledFileManager` (heap, never freed)                                   | 5,024 | `OLED_Mode_FileBrowser.cpp`             | Lazy alloc already correct; delete it + invalidate render data in a Files-mode `onExit` (same hook as §6.4)                                                              |
| `bondPickerScroll` + `pickerLabels` (heap, never freed)                  | 1,332 | `OLED_Mode_Remote.cpp`                  | Free on Remote-mode exit / after successful bond (picker never shown again)                                                                                              |
| `sWifiScanSSIDs[16][33]`                                                 | 528   | `OLED_Mode_Network.cpp`                 | `EXT_RAM_BSS_ATTR` — its literal twin `sWifiScanLabels` on the next line already has it; pure omission                                                                   |
| `sWizard` + 2 static prompt bufs                                         | 496   | `System_SetupWizardMode.cpp`            | **NOT PSRAM** — `sWizard.result` holds `String wifiPassword`/`mqttPassword`. Heap-pointer on wizard entry, zero+free on completion                                       |
| `fileBrowserRenderData`                                                  | 452   | `OLED_Mode_FileBrowser.cpp`             | `EXT_RAM_BSS_ATTR` on the defining declaration (extern reader unaffected)                                                                                                |
| `gOledDialog`/`gOledToast`/`gOledPairingRibbon`                          | 372   | `OLED_UI.cpp`                           | `EXT_RAM_BSS_ATTR`; ribbon's nonzero defaults → set in code                                                                                                              |
| `sConfirm`                                                               | 364   | `System_CLIConfirm.cpp`                 | `EXT_RAM_BSS_ATTR` (all-zero initializer → drop it)                                                                                                                      |
| `sPickerReq` + `sPeerPath`                                               | 300   | `OLED_Mode_FileBrowser.cpp`             | `EXT_RAM_BSS_ATTR`; `sPeerPath = "/"` initializer moves to browser reset                                                                                                 |
| `sBtActions`/`sNetworkMainActions` (int[32] holding enums <20)           | 256   | `OLED_Mode_Bluetooth.cpp`/`Network.cpp` | Type-shrink to `uint8_t[32]` (256→64 B, zero placement questions). **Mutable runtime state — cannot ride the** `gCommandModules` **const-ify pass**                      |
| misc (`sCfgBuf`, `sCatLabels`, `g2TextInputBuffer`, `sDummyResult` etc.) | ~840  | various                                 | `EXT_RAM_BSS_ATTR` batch; `sDummyResult` → eliminate via nullable pointer param (holds password Strings — don't relocate)                                                |
| ~14 static String globals incl. password buffers                         | 224   | various                                 | **Leave** — several hold credentials (secrets rule); headers are 16 B each                                                                                               |




### Filesystem / WiFi static-String reply holders (never audited, ~11 KB internal heap after first use)

Function-local `static String` command-reply holders retain their **peak-ever** size in internal heap forever:

- `filesListingJsonForApp::s_listJson` — up to ~4,096 B after one large `files json` listing (`System_Filesystem.cpp:542`)
- `cmd_fileread::s_readJson` — ~4,032 B after one base64 fileread chunk (`System_Filesystem.cpp:785`)
- `cmd_wifiscan::jsonResult` — ~2,800 B after a dense scan (`System_WiFi.cpp:410`)
- 4 small `respBuf` statics — 768 B aggregate

Fix (codebase-norm, cf. `controls.json` in `System_Settings.cpp:2843`): build in a **local** String, copy into one lazily ps_alloc'd PreferPSRAM 4 KB scratch (replies are dispatcher-truncated at 4,096 anyway), return the scratch. Or route the small ones through the existing `ensureDebugBuffer()`/`getDebugBuffer()` convention.

### esp_wifi driver retention (~32 KB internal heap after `closewifi`)

`ensureWiFiInitialized()` is correctly lazy, but nothing ever calls `esp_wifi_deinit()` — after `closewifi` the driver + STA netif keep ~32 KB of internal heap forever. Feasible (partial): in `cmd_wifidisconnect`, when ESP-NOW is not active, `WiFi.mode(WIFI_OFF)` → `esp_wifi_deinit()`. Must respect the ESP-NOW dependency (shared radio) — gate on `gEspNow` inactive, and re-init is already the lazy path. Biggest single toggle-off DRAM lever outside Bluetooth.

### Codec-layer placement bugs (zero-cost fixes)

- `r1Crc32EnsureTable()::table` — 1,024 B DRAM for a **fixed** CRC-32C table: replace the lazy-fill with a `constexpr` compile-time table in flash `.rodata`. 1 KB DRAM → 0, and the ensure-call disappears.
- `gSidStats` — 576 B → `EXT_RAM_BSS_ATTR` (diagnostics, task-context).
- `gCommandModules` — 840 B sitting in `.dram0.data` **despite being** `static const`: 28 of 30 `count` initializers are `extern const` values from other TUs, defeating constant-initialization. Make the counts constexpr-visible (or numeric literals) and the whole table drops to flash.
- 4 private static response buffers (472 B) → consolidate through one shared per-TU scratch.



## 7. Verified-fine (no action)

Already-lazy model citizens confirmed end-to-end: LLM engine heap side (weights/KV/tokenizer/chat, freed on unload); sensor pollers (double-gated, self-deleting); `sensor_bcast` (create-on-first-stream, delete-on-last — the reference pattern, minus the buffer free noted above); httpd start/stop; MQTT open/close; camera/video buffers; mic/ESP-SR/EdgeImpulse; i2c driver objects (on-detect `new`). Keep-as-is core: `cmd_exec` (8.6 KB, core-0 pin is load-bearing), `debug_out` (16 KB — LittleFS writes on-stack; trim was tried and reverted 2026-06-11), `loopTask`, `gSettings`, `gLoopPerf`, `gMeshDerivedKeys`/`gIdentity` (secrets, boot-recovery path), `espnow_task` (correctly created/freed), BLE controller init (correctly gated; the lever is the `bluetoothAutoStart=true` **default**, not the code).

## 8. Suggested sequencing

1. **Zero-risk sweep (one session):** `const httpd_uri_t` (#15) + `EXT_RAM_BSS_ATTR` POD batch (#3, #6, #9, #11, #13, #14, #20, #22, #23, #25, #26, #27, #29, #32, #35) + delete dead `gAutoMemo`* + delete `BLE_IDF.cpp`. ≈ **25–30 KB DRAM** back, no lifecycle logic touched.
2. **Boot-call moves:** `sessionsInit`/`fileSlotsInit`/`fsListInit` into `initEspNow`; web `gSessions` into `startHttpServer`; remaining web security state as appropriate. ✅ `gJsonResponseBuffer`, `gWebMirror`, and `gBLEOutputBuffer.reserve` are done.
3. **Lazy conversions with free-on-disable:** `sensor_queue_task`, MLX scratch, `gSc`, `externalSensors`. ✅ LLM worker stack, automation `sEventBuf`, automation `gAutoCache`, and `gOledConsole` are done.
4. **Teardown work (hardest):** OLED `onExit` hook → Map-mode teardown (task, buffers, tile cache, track w/ sync fix); G2 worker shutdowns after first use (§6.3); `espnowtx::stop()`; full `deinitEspNow` free (§6.2) or at minimum shrink `MESSAGES_PER_DEVICE`.



## 9. Implemented 2026-07-16 (building green, awaiting HW flash)

Four items landed. Measured against the linker map, not estimated:


| Item                                   | Symbol removed from `.dram0.bss`   | Bytes      |
| -------------------------------------- | ---------------------------------- | ---------- |
| ✅ LLM worker stack + TCB               | `gLLMWorkerStack`, `gLLMWorkerTcb` | 12,664     |
| ✅ Automation event drain buffer        | `schedulerTickMinute::sEventBuf`   | 7,872      |
| ✅ OLED console ring                    | `gOledConsole` (arrays)            | 6,808      |
| ✅ ESP-NOW RX ring                      | `gEspNowRxRing`                    | 2,112      |
| ✅ ESP-NOW broadcast trackers           | `gBroadcastTrackers`               | 1,216      |
| ✅ ESP-NOW peer identities → PSRAM      | `gPeerIdentities`                  | 896        |
| ✅ OLED ESP-NOW refresh scratch → PSRAM | `line1Bufs`/`line2Bufs`/`entries`  | 1,216      |
|                                        | **total removed**                  | **32,784** |


**Governing decision: allocate lazily, do NOT free on disable.** Every free-on-disable path in these subsystems races an unguarded cross-task reader (§6). Allocate-on-first-use-and-keep captures the whole win for devices that never enable the feature, with zero teardown risk. This is why `llmunload` still does not reclaim the LLM stack — it never did (the stack was `.bss`), and the block is deliberately retained so a re-created worker wins the same contiguous 12 KB.

**Net DRAM is conditional, and the 32.7 KB headline is the link-time reservation, not steady state.** The freed `.bss` becomes heap; features then draw from it only when used:


| Device profile                                          | Net internal DRAM freed |
| ------------------------------------------------------- | ----------------------- |
| Nothing enabled (no model, no ESP-NOW, automations off) | ~29.3 KB                |
| Default (automations on, no model loaded, ESP-NOW off)  | ~21.5 KB                |
| Everything on (model loaded + ESP-NOW up)               | ~5.5 KB                 |


The "everything on" figure is small by design — a feature in use *should* pay. The win is that unused features no longer do.

### Deviations from the audit's recommendation (and why)

- **§2 #3 / #4 said "move to PSRAM". Both were wrong — reverted to internal DRAM mid-implementation.** Two rule violations were caught by review and independently confirmed against source:
  - **OLED console** (#4) holds **verbatim CLI command echoes** (`OLED_Mode_CLIInput.cpp:150-152`), and `login <user> <pass>` explicitly supports the `display` transport (`System_Utils.cpp:4896`) — it is *designed* to be typed on the OLED keyboard. PSRAM would put plaintext passwords on a probeable chip. Kept internal; the win became right-sizing only (allocate the latched `oledCliHistorySize`, default 50, instead of the 100-line ceiling) ≈ 3.4 KB.
  - **Automation** `sEventBuf` (#2) is filled by `systemEventFetchSince` from **inside** `taskENTER_CRITICAL(&gEventMux)` (`System_Events.cpp:254-280`) — a PSRAM destination would put ~7.9 KB of cache-missing writes under a global spinlock with interrupts off. Independently, `SYSEVT_REMOTE_CMD_RX` carries raw remote command text in `detail[]`, so an inbound `login`/`wifiadd` would land in PSRAM. Kept internal.
- **Both use explicit** `heap_caps_calloc(MALLOC_CAP_INTERNAL)`**, not** `ps_alloc(AllocPref::PreferInternal)`**.** `PreferInternal` merely skips the PSRAM attempt and falls through to plain `malloc()`, which returns PSRAM once an allocation exceeds `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (16384 here). Both buffers are under that today, so they land internal *incidentally*. For placements whose justification is "must never reach PSRAM," the requirement is now stated rather than inherited from a tunable.
- **ESP-NOW RX ring is allocated immediately before** `esp_now_register_recv_cb`**,** not at the top of `initEspNow` — latest point that still precedes the callback (which must never allocate), so a bring-up that fails at `esp_now_init` doesn't commit the DRAM. `ESPNOW_RX_RING_SLOTS` replaces both `sizeof(array)/sizeof(elem)` computations; as a pointer those would have evaluated to 0 and produced a `**% 0` divide-by-zero inside the WiFi callback**.
- `**gOledEspNowState` deliberately NOT moved** (audit §2 #7): holds `remoteUsername`/`remotePassword`, and Arduino `String` keeps short strings inline via SSO. Only the credential-free refresh scratch moved.



### The LLM worker is now never deleted (this also removed a latent crash)

`llmStopWorker()` is **gone**, along with the `gLLMWorkerExit` flag and the worker's exit branch. `cmd_llm_unload` now just calls `llmUnload()`, which already stops a live generation and waits for it before freeing the weights — the retire path was adding nothing it didn't already do.

The reasoning: once the stack/TCB are deliberately retained, `vTaskDelete` buys **nothing** — the task is the only thing that would go away, and it costs ~0 parked on `portMAX_DELAY`. Meanwhile retiring it has no safe form, and a clean-baseline review found three concrete defects all rooted in it:

- `llmStopWorker`'s force-null timeout nulled `gLLMTask` after ~500 ms even when the worker never acked, after which `llmEnsureWorker` would `xTaskCreateStaticPinnedToCore` **on the stack and TCB a live task was still running on** — and its `gLLMWorkerExit = false` reset would make the old worker resume generating instead of exiting. Two tasks, one stack. (This one predated the lazy conversion — the static `.bss` version had the identical hazard.)
- The worker published `gLLMTask = nullptr` *before* `vTaskDelete(nullptr)`, so re-creating on that `StaticTask_t` would memset a TCB still linked into `xTasksWaitingTermination`, and IDLE would later pop a corrupted entry.
- `llmStartAsync` dereferenced `gLLMTask` at `xTaskNotifyGive` outside `gLLM.mutex` while `llmStopWorker` nulled it under no lock — `xTaskNotifyGive(nullptr)` is an assert-abort.

All three are structurally impossible now: the task is created exactly once per boot (guarded by `if (!gLLMTask)` under `gLLM.mutex`) and parks forever between generations. That is what the "persistent worker" comments always described; the retire path was the part that didn't fit.

### Known-accepted risks (need HW validation)

1. **LLM worker creation is now fallible.** Link-time `.bss` could not fail; a 12 KB contiguous internal `heap_caps_malloc` can. Mitigated by claiming it at the identical moment as before (model load, cleanest heap) and because both callers already treat failure as retry-later — `llmLoadModel:957` logs "create deferred… will retry at first generate" and `llmStartAsync` re-enters next generation. Worst case is "generation unavailable until reboot," not a crash.
2. `**llmunload` no longer stops the worker task** (it never reclaimed its stack anyway). The task stays parked and idle; the next `llmload` reuses it. Behaviourally this makes CLI `llmunload` and `POST /api/llm/unload` identical, which they were not before.
3. Alloc-failure degrade paths (automation event drain skipped w/ throttled warn; OLED history disabled w/ error; ESP-NOW refuses to start on ring failure, non-fatal warn on tracker failure) are all coded but unexercised on HW.



### Pre-existing bugs found during review — NOT fixed here

- `**OLED_Mode_CLIInput.cpp:155/164**`: the "did this command produce output?" check compares `getLineCount()` across the submit, but `count` **saturates at** `capacity`. Once the user has generated 50 lines (the latched default), both reads return 50 forever, so the equality is always true and a fallback feedback line is appended even when the command's own output already landed — permanently duplicating output on the CLI Output page. Needs a monotonic `totalAppends` counter to express "something was appended". Fully pre-existing: `capacity` was already latched to `oledCliHistorySize` at `HEAD` and `append` already wrapped on `% capacity` — this conversion changed the *allocation* size, not the saturation point.



### Homoglyph cleanup (also done)

`System_ESPNow.cpp:8926-8929` declared and used `reasмSize` with a **Cyrillic** `м` **(U+043C)** in place of ASCII `m`. It compiled only because all three occurrences carried the same bad byte — GCC accepts UTF-8 identifiers — so it read as `reasmSize` while being a different symbol entirely. Renamed to ASCII; the built binary is byte-identical in size, confirming a pure identifier change.

A whole-codebase scan (comments and string literals stripped, so only *code* is considered) found **no other homoglyphs in first-party source**. The remaining non-ASCII-in-code hits are all legitimate and were left alone:

- `WebPage_*.h` — em-dashes, bullets, and check/ballot marks inside `R"(...)"` raw strings; these are embedded HTML/JS UI text, and `WebPage_ESPNow.h:2342`'s `→` is a JS regex deliberately matching literal arrow output.
- `hardwareone_libs/Adafruit_EPD/*`, `hardwareone_libs/Audio_-_Adafruit_Fork/*` — U+FEFF BOMs at file start in vendored third-party code.

Scanner kept at `scratchpad/homoglyph_scan.py` if this is ever worth re-running.

## 9b. Implemented 2026-07-25 (code-level checked, build not run here)

This follow-up pass implemented the items that had become clear after the first audit and review discussion:


| Item                              | Previous eager cost                                    | New behavior                                                                                                                                                                                                                          |
| --------------------------------- | ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| ✅ ESP-NOW remote sensor cache     | ~20 KB PSRAM static                                    | `gRemoteSensorCache` is now a pointer allocated by `initRemoteSensorSystem()` only after `initEspNow()` is actually starting. MQTT, G2 ESP-NOW page, OLED network page, and JSON/list helpers have null guards for the pre-init state |
| ✅ HTTP JSON response buffer       | 65,536 B PSRAM heap at boot                            | Boot allocation removed. `startHttpServer()` now claims the shared JSON buffer through the web-runtime ensure path before any handler can run                                                                                         |
| ✅ Web CLI mirror                  | 8,192 B PSRAM heap at debug init                       | Moved into the same web-runtime ensure path; mirror allocation failure only disables live web CLI history, not the whole web server                                                                                                   |
| ✅ Debug message pool              | 192 `DebugMessage` objects up front on PSRAM builds    | Queue capacity stays 192, but the pool starts at 96 messages and grows once to 192 when the free-list crosses a low-water mark. ISR producers request growth; task context performs the allocation                                    |
| ✅ OLED CLI history                | ~3.4 KB internal by default, paid even OLED-off/absent | `gOledConsole.init()` is no longer called from `initDebugSystem()`. The internal ring/mutex are allocated only after OLED display initialization succeeds                                                                             |
| ✅ Automation due-check cache      | 1,536 B internal BSS                                   | `gAutoCache` is now internal heap, allocated when the scheduler starts or first rebuilds, and freed by `stopAutomationScheduler()` on disable                                                                                         |
| ✅ BLE output buffer reserve       | 1,024 B heap reserve during debug init                 | `gBLEOutputBuffer.reserve()` runs on first authenticated BLE-routed output, after BLE is connected and the BLE output route is active                                                                                                 |
| ✅ G2 connect/page/tap/FSM workers | ~17.7 KB DRAM stacks+queues at G2 client init          | `bleConnect` worker starts on first connect submission; page worker starts on first page/lens job; tap worker starts on first tap dispatch; FSM worker starts on first FSM dispatch                                                   |
| ✅ G2 temple RX buffers            | 16,384 B PSRAM at G2 client init                       | Per-temple RX buffer/write mutex are allocated in `connectTemple()`, not in `initG2Client()`                                                                                                                                          |
| ✅ MQTT external sensor table      | 1,664 B internal BSS in MQTT-enabled builds            | `externalSensors` is now a lazy PSRAM heap block: `startMQTT()` constructs it only after MQTT config validation, `stopMQTT()` clears the pointer/count under the mutex and destructs/frees the table                                  |
| ✅ G2 frame post-mortem ring       | 1,152 B internal BSS                                   | `gFrameRing` is now `EXT_RAM_BSS_ATTR`, removing it from internal DRAM while keeping the same always-available diagnostic behavior                                                                                                    |
| ✅ Allocation tracker table        | 2,560 B internal BSS                                   | `gAllocTracker` is now `EXT_RAM_BSS_ATTR`; the diagnostic allocation hook still records into the same always-present table, just backed by PSRAM `.bss`                                                                               |


Validation performed in this session:

- `git diff --check` passes.
- Targeted source searches confirmed the old eager calls/statics are gone (`gOledConsole.init()` from debug init, `gBLEOutputBuffer.reserve()` from debug init, G2 worker init calls from `initG2Client`, fixed-array `gAutoCache[64]`, fixed-array MQTT `externalSensors[]`, internal-DRAM `gFrameRing`).
- Firmware build was not run because `idf.py` was not available in the shell PATH.

Still open after this pass:

- G2 page/tap/FSM workers are lazy now, but once used they still persist until reboot/deinit behavior is redesigned. Full shutdown remains blocked by §6.3's queue-drain/self-delete/current-task hazards.
- Normal `closeg2` still retains temple RX buffers as part of the existing fast-reconnect cache policy; `closeg2 full`/deinit frees them.
- Full `deinitEspNow` heap teardown remains separate from the remote-sensor-cache boot-footprint fix.



## 10. Coverage accounting (proof of completeness)

The accounting pass classified **every** symbol ≥512 B in `.dram0.bss` + `.dram0.data` (80,911 B across 37 symbols/entries of the 138,799 B combined section total; the remainder is the <512 B tail, itself largely inside the small-statics aggregate findings):

- **Covered by findings:** 37,488 B / 18 symbols
- **Core-keep (explicitly judged always-on):** 24,824 B / 7 symbols (`gEventRing`*,* `gOledConsole`, `commandRegistry`*, MLX* `scratchData`, `gSettings`, `sNotifView`*,* `sKindLastShownMs`) — *starred items additionally have PSRAM-relocation findings in §2
- **IDF/toolchain-internal:** 18,599 B / 12 entries (freertos, phy, cache_hal, libsodium LUT, arduino periman/i2c…)
- **Unaudited application symbols: 0 B / 0 symbols.**

One borderline flag inside the IDF bucket: the 3,100 B TJpgDec `work` buffer in `managed_components/espressif__esp32-camera/conversions/to_bmp.c:35` (static "for legacy reasons") is only linked because the app uses `fmt2rgb888` for the G2/web JPG viewers — a component patch could heap/PSRAM it per-decode if 3.1 KB of internal DRAM is ever needed.