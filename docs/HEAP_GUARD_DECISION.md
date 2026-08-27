# Heap Guard Correction — Decision Document

Generated 2026-08-18. 20 guard analyses, each adversarially verified (8 confirmed, 12 disputed
and corrected). NO CODE CHANGED — this is the pre-change sweep.

---

# DECISION DOCUMENT — Honest heap queries for the 11 memory guards
**Board:** feather_esp32_v2 / ESP32-PICO-V3-02, 2 MB quad PSRAM · **Repo:** `/Users/morgan/esp/hardwareone-idf` · **Status:** proposal, no code changed

---

## 0. THE FINDING THAT SETTLES THE HELPER QUESTION

Before any per-guard verdict, one fact resolves a disagreement running through every sub-analysis: **`hw1MallocableInternalBytes()` is the wrong helper for all eleven guards on this board.**

`/Users/morgan/esp/esp-idf/components/heap/heap_caps.c:107-134` (`heap_caps_malloc_default`, and the identical `heap_caps_realloc_default` at :140-167):

```c
if (size <= (size_t)malloc_alwaysinternal_limit)
    r = heap_caps_malloc_base(size, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
else
    r = heap_caps_malloc_base(size, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM);
if (r == NULL && size > 0)
    r = heap_caps_malloc_base(size, MALLOC_CAP_DEFAULT);   // "less picky"
```

`build-feather_esp32_v2/sdkconfig:1445-1449` — `SPIRAM_USE_MALLOC=y`, `ALWAYSINTERNAL=16384`, `RESERVE_INTERNAL=32768`. `esp-idf/components/heap/port/esp32/memory_layout.c:56` registers SPIRAM as `{MALLOC_CAP_SPIRAM|MALLOC_CAP_DEFAULT, 0, 8BIT|32BIT}`.

**Consequence:** `ALWAYSINTERNAL` sets a *preference*, not a constraint. Every plain `malloc`/`new`/`String`/ArduinoJson allocation on this board falls back to bare `MALLOC_CAP_DEFAULT`, which matches PSRAM. With ~1.53 MB PSRAM free, **plain `malloc`/`new`/`String` cannot fail on this board**, regardless of internal free. `hw1MallocableInternalBytes()` measures the pool malloc *prefers* and is *allowed to abandon*. Guarding on it manufactures refusals for work that would have succeeded — failure mode (b), directly.

**What CAN actually fail internal, with no PSRAM fallback:**

| Consumer | Cap set | Can use the 32,768 B DMA reserve? |
|---|---|---|
| FreeRTOS task stacks / TCBs / queues | `INTERNAL\|8BIT` (`esp-idf/components/freertos/heap_idf.c:43`) | **Yes** (last-resort prio) |
| Explicit `heap_caps_malloc(INTERNAL\|8BIT)` — ESP-NOW RX ring `System_ESPNow.cpp:10996`, broadcast trackers `:11095`, TX lifecycle `System_ESPNow_Tx.cpp:376` | `INTERNAL\|8BIT` | Yes |
| BT controller / Bluedroid internal + DMA | `INTERNAL`, `DMA\|INTERNAL` | Yes (DMA prefers it *first*) |

`hw1InternalFreeBytes()` = `free(INTERNAL|8BIT)` is **exactly** that cap set. It is the correct helper for every guard in this set.

Two follow-on consequences:
- The "unmeasured reserve-pool split" anxiety in several sub-analyses **evaporates** — we never query a metric that excludes the reserve.
- The header comment at `components/hardwareone/System_MemUtil.h:56-58` ("What a plain malloc()/new/String can actually be served from — **use for GUARDS**") is **wrong for this config** and must be amended, or a future sweep will re-introduce the error. `hw1MallocableInternalBytes()` is correct only for allocations that are internal-only *and* cannot fall back — of which this set contains none.

Also confirmed: `memory_layout.c:54` — `SOC_MEMORY_TYPE_IRAM = {MALLOC_CAP_INTERNAL|MALLOC_CAP_EXEC|MALLOC_CAP_32BIT, 0, 0}`, no `8BIT`. So the ~25,864 B phantom is excluded by `INTERNAL|8BIT` exactly as the brief states. `esp_psram.c:516` — reserve caps `{0, DMA|INTERNAL, 8BIT|32BIT}`, no `DEFAULT`.

---

## 1. VERDICT PER GUARD

`hw1IFB` = `hw1InternalFreeBytes()` (`INTERNAL|8BIT`). `hw1ILB` = `hw1InternalLargestBlock()`.

| # | Guard (file:line) | Change? | Helper | Threshold | Risk |
|---|---|---|---|---|---|
| 1 | `G2_Glasses.cpp:24465` g2ShowBmpFile | change | hw1IFB | **16384 → 6144** (+ contiguity clause) | low |
| 2 | `G2_Glasses.cpp:26376` g2ShowCameraViewer | change (no-op here) | hw1IFB | **16384 → 6144** (+ contiguity) | none on this board |
| 3 | `G2_Glasses.cpp:26840` g2ShowCameraStream | change (no-op here) | hw1IFB | **16384 → 6144** (+ contiguity) | none on this board |
| 4 | `G2_Glasses.cpp:27145` g2ShowBmpFileFullScreen | change | hw1IFB | **16384 → 6144** (+ contiguity) | low |
| 5 | `G2_Glasses.cpp:27434` g2ShowJpgFile | change | hw1IFB | **16384 → 6144** (+ contiguity) | low |
| 6 | `G2_Glasses.cpp:27559` g2ShowJpgFileFullScreen | change | hw1IFB | **16384 → 6144** (+ contiguity) | low |
| 7 | `G2_Ring.cpp:4529` ringconnect reconnect | change + **relocate** | hw1IFB | **16384 → 10240** | med (coverage) |
| 8 | `System_MemoryMonitor.cpp:113` shared query | change **+ retune all 12 rows same commit** | hw1IFB | see 8a–8f | **HIGH** |
| 8a | `:85` bluetooth | change | hw1IFB | **61440 → 36864** | **HIGH — brick risk** |
| 8b | `:76` thermal | change | hw1IFB | **49152 → 12288** | low |
| 8c | `:77` imu | change | hw1IFB | **24576 → 10240** | low |
| 8d | `:78,80,81,82` tof / presence / apds / gps | change | hw1IFB | **16384 → 12288 (tof) / 8192 (other 3)** | low |
| 8e | `:74,75,79,83` gamepad / ano / fmradio / rtc | change | hw1IFB | **20480 → 8192 / 8192 / 10240 / 10240** | low |
| 8f | `:84` espnow | query only | hw1IFB | **KEEP 20480** | med — new refusal |
| — | `:84` espnow PSRAM leg | **leave** | `ESP.getFreePsram()` | 327680 | none |
| 9 | `System_SensorLogging.cpp:1265` cmd_sensorlog | change + move up | hw1IFB | **8192 → 4096** | none |
| 10 | `System_ESPNow.cpp:7706` sendBondSettings | change | hw1IFB | **20000 → 8192** (→ 6144 after stream fix) | med if left at 20000 |
| 11 | `System_ESPNow.cpp:7832` sendBondSchema | change | hw1IFB | **20000 → 8192** (→ 6144 after stream fix) | med if left at 20000 |
| — | `Bluetooth.cpp:1257` + `:1525` leak accounting | convert **both or neither** | hw1IFB | n/a (delta) | none if paired |

### Numeric justification

**Guards 1–6 (G2 viewers), 16384 → 6144.** Every large buffer is PSRAM, verified: `readBmpFromVfs` → `ps_alloc(PreferPSRAM,"g2.bmp.fileLoad")` (`G2_Glasses.cpp:24100`); LZ4 dst `ps_alloc("g2.img.lz4")` (`:23223`); LZ4 hash table `ps_calloc("lz4.table")` (`System_Lz4.cpp:136`); `pbBuf` `ps_alloc` (`:23350`); tile scratch `g2SessionScratchAcquire` → `ps_alloc("g2.session.scratch")` (`:18409`), **cached** across invocations; JPEG decoder workspace is the 3,100 B static BSS `work[]` in `managed_components/espressif__esp32-camera/conversions/to_bmp.c:35`, passed as `advanced.working_buffer`, so `jpeg_decoder.c:86`'s heap branch never runs.
Honest internal need: ~250 B caller-side (`new *ViewerArgs` + `strdup` ≤160 B + `new G2SessionJob`) + ~1–2 KB of `String`/VFS-open traffic in the worker = **~3–4 KB**. 6144 is ~1.7×. Clears the ~19,000 B post-g2init estimate by ~13 KB and would still have cleared the *measured* pre-BR/EDR-fix 6,111 B point. **Do not keep 16384**: at the only operating point where these are reachable (lens connected ⇒ post-g2init) it leaves ~2.6 KB of margin against an unmeasured number.

Contiguity clause — the one thing free-size cannot express. `kG2SessionStackBytes = 10240` (`G2_Glasses.cpp:18399`) is a contiguous `INTERNAL|8BIT` allocation. Normally paid eagerly at `initG2Client` (`:12985`), but the comment at `:12976-12984` explicitly anticipates that create failing on this board and being retried lazily inside `g2SessionSubmit`. Add:
```c
if (hw1InternalFreeBytes() < 6 * 1024 ||
    (gSessionInitState != G2WorkerInitState::Ready && hw1InternalLargestBlock() < 11 * 1024))
```
Fold all six into one `g2ViewerHeapOk(const char* what)` so they cannot drift, and **convert the `DEBUG_G2F` line inside each body too** — it currently calls `ESP.getFreeHeap()` a second time and would print the phantom number next to an honest decision.

Guards 2 and 3 are **not compiled** on this board: `ENABLE_CAMERA_SENSOR` defaults 0 for non-XIAO (`System_BuildConfig.h:185-192`), `#else` stub returns false at `:26856`. Change them for consistency; the numbers must be validated on xiao_s3 separately. Note for that board: `build-xiao_s3/sdkconfig` has `ESP_SYSTEM_MEMPROT_FEATURE=y`, which compiles out the S3's only 32-bit-only region — so the IRAM bias there is **zero**, but `RESERVE_INTERNAL=32768` still makes `ESP.getFreeHeap()` overstate mallocable-internal by up to 32,768 B. These guards are decorative on the XIAO too.

**Guard 7 (ring reconnect), 16384 → 10240.** The reconnect's own allocations are `new` (`BLEDevice::createClient()`, `G2_Ring.cpp:3644`, plus the GATT cache) — PSRAM-fallback-capable, cannot fail. `r1_owner` (6144) already exists. The *net* internal delta is near zero: the stale-client retire branch (`:3610-3641`) deletes the old client and frees ~10–14 KB **before** `createClient()`. So this guard is a **system-health floor**, not an allocation predicate — it exists so the device does not open a BLE session while the BT controller and task stacks are starving. 10240 sits ~9 KB clear of the post-g2init estimate and blocks the genuine death spiral (963 B min-ever measured).

**Guard 8a (bluetooth), 61440 → 36864.** Today's *effective* floor is `61440 − 25864 = 35,576` real bytes. 36864 reproduces it to within 1,288 B — **behavior-preserving by construction**, which is the entire answer to "does fixing this break BLE". Corroborated bottom-up: with `BT_ALLOCATION_FROM_SPIRAM_FIRST=y` and `BT_BLE_DYNAMIC_ENV_MEMORY=y` (`build-feather_esp32_v2/sdkconfig:792-793`) the Bluedroid host heap is PSRAM, leaving controller + `BTC_TASK_STACK_SIZE=8192` + BTU 4352 ≈ 28–34 KB internal. 36864 covers that with headroom.

**Guard 8b–8e.** Real stacks (`System_TaskUtils.h`): THERMAL 6144, IMU 4096, TOF/PRESENCE/APDS/GPS 3072, INPUT 3584, FMRADIO 4608, RTC 4096. The registry's own comment (`System_MemoryMonitor.cpp:66-71`) admits the rows were derived on a 4×-wrong words-vs-bytes assumption and says "re-tune them against measured usage if a feature is ever wrongly refused". Rule applied: **stack + ~4–6 KB slack for TCB, mutex, driver object, I2C transients.** tof gets 12288 rather than 8192 as insurance: `new VL53L4CX()` (`i2csensor_vl53l4cx.cpp:338`) embeds a `VL53L4CX_Dev_t` of ~9,480 B. It falls back to PSRAM like any other `new`, so it cannot fail — but 12288 costs nothing at either operating point and covers it if PSRAM is ever bypassed.

**Guard 8f (espnow), KEEP 20480.** Identified internal-only demand in `initEspNow`: `espnow_task` 6656 + `espnow_tx` 5120 (started *inside* `initEspNow` at `:11061`) + RX ring `heap_caps_calloc(8 × sizeof(InboundRxItem), INTERNAL|8BIT)` at `:10996` ≈ 2,112 B contiguous + broadcast trackers `INTERNAL|8BIT` at `:11095` ≈ 1,216 B + TX job queue/lifecycle `INTERNAL|8BIT` at `System_ESPNow_Tx.cpp:376` + 2 TCBs — **≈ 16–17 KB before** `WiFi.mode(WIFI_AP_STA)` (`:10893`) brings up a second netif. None of it can fall back to PSRAM. The file's own error string says "need ~40KB DRAM" (`:11288`). Lowering this to 12288 was proposed and is **rejected**.

**Guards 10/11, 20000 → 8192.** Both build a `String` via `serializeJson` into a plain Arduino `String` — PSRAM-fallback-capable, cannot fail. Empirical corroboration in-tree: `computeBondLocalSettingsHash` (`System_ESPNow.cpp:7538`) builds the *identical* ~10 KB String with **no guard at all** and runs after every settings write, routinely post-g2init, without failing. What genuinely needs internal RAM here is the FS write path and ESP-NOW TX machinery — a few KB. 8192 is a system floor; keeping 20000 with an honest query kills bond settings/schema sync whenever BLE is up, for a failure that cannot occur.

---

## 2. WHAT ACTUALLY CHANGES ON HARDWARE

**On a healthy device: nothing visible.** All eleven guards today sit below the ~25,864 B phantom and have never fired on this board except the 49152 and 61440 tiers. With the recommended numbers, at boot-complete (95,835 B measured) every guard passes with 6–10× margin, and at the estimated ~19,000 B post-g2init point every guard except espnow still passes.

Three things do change:

**(a) The BLE leak guard becomes live — this is the point of the exercise.** `checkMemoryAvailable("bluetooth")` at 36864 against honest free will now fire after roughly 2–4 `openble`/`closeble` cycles (10–14 KB leaked per cycle, `Bluetooth.cpp:1527`) instead of never. The user sees `[BLE] Insufficient memory for Bluetooth (need ~60KB DRAM)` + `Reboot to recover.` That is the message's intended behavior; today it is unreachable. **Soft:** the exact cycle count depends on the post-teardown starting figure, which is derived (52–58 KB), not measured.

**(b) `openespnow` starts declining post-g2init.** With 20480 honest, ESP-NOW init is refused at ~19,000 B free. Boot autostart (`HardwareOne.cpp:2296`) runs before g2init at ~95,835 B and is unaffected. This is a **new refusal of something that "works" today** — but what happens today is that a ~17 KB internal init plus a `WIFI_AP_STA` netif bring-up proceeds at ~19 KB free, and every failure past `:10893` leaves the radio in AP+STA with power-save off and no ESP-NOW running. Refusing is the safer outcome. Verify with measurement (§5).

**(c) Six G2 viewer decline paths become reachable for the first time.** Under the recommended 6144 they will only fire in a genuine death spiral, but they have **never executed**, and four of six give the user no visible reason (silent chooser dismissal + a `DEBUG_G2F` line), while two callers ignore the return value entirely (`G2_Page_Sensors.cpp:1604`, `:1618`). Add a lens-visible line as part of the change.

Everything else — sensor starts, sensorlog, bond sync, ring reconnect — behaves identically until the device is genuinely tight, at which point it declines instead of failing an allocation deep inside a half-mutated subsystem.

---

## 3. THE REGRESSION TRAPS, RANKED

### T1 — HEADLINE. Converting `checkMemoryAvailable`'s query without retuning the `bluetooth` row bricks BLE, destructively.
`System_MemoryMonitor.cpp:113` is one line and feeds all 12 tiers. If it becomes honest while `:85` stays 61440:

The check at `Bluetooth.cpp:1246` runs **after** `deinitG2Client()` (`:1214`) and **after** `BLEDevice::deinitChecked(false)` (`:1233`), so it measures a post-teardown heap. `g2UiWorkersQuiesce()` (`G2_Glasses.cpp:17222`) drains queues and waits on a generation barrier — **it never `vTaskDelete`s** — so `g2_page_swap_w` (8192), `g2_tap_disp` (10240) and `r1_owner` (6144) survive. Derived post-teardown free: **~52–58 KB < 61440**. `openble` after `g2init` would be **refused where it succeeds today**.

And the refusal is destructive. `cmd_blestart` (`Bluetooth.cpp:2013-2032`) has already done `setSetting(gSettings.bleMode, BLE_MODE_SERVER)` — persisted to NVS — plus `g2Disconnect(userInitiated=true)` and `g2RingDisconnect(userInitiated=true)` **before** calling `initBluetooth()`, which then destroys the G2 client and only then hits the check. Result: no BLE role running, G2 client gone, nothing restores it, and NVS says SERVER across reboot. **Mitigation: 61440 → 36864 in the same commit. Non-negotiable.**

### T2 — Do NOT move the bluetooth check above `deinitG2Client()`.
This was proposed as a fix for T1's destructiveness. It would evaluate the guard with the G2 client and BLE stack still resident — the ~19,000 B point. `19,000 < 36,864`, and `19,000 + 25,864 = 44,864 < 61,440` too, so it fails on the *current* query as well. It makes `openble`-after-`g2init` permanently impossible. Fix the destructiveness the other way (§4, B2).

### T3 — Using `hw1MallocableInternalBytes()` anywhere in this set.
Over-strict by up to 32,768 B against a split nobody has measured. At the ~19,000 B post-g2init point the mallocable figure is **unknown and could be near zero** — the estimate is already below the 32,768 B reserve span, so the reserve is provably partly drained, but by how much is unmeasured. Any threshold set against it is an estimate stacked on an unmeasured partition. §0 shows it also models the wrong failure. **Rule: this helper is for reporting/diagnostics in this codebase, not guards.** Amend `System_MemUtil.h:56-58`.

### T4 — espnow: honest 20480 introduces a real refusal (§2b). Accepted deliberately, but verify against a measured post-g2init figure.

### T5 — Keeping 16384 on the six G2 viewers with an honest query.
Leaves ~2.6 KB of margin against the ~19,000 B *estimate*, at exactly the operating point where the feature is reachable. The threshold was sized against buffers that live in PSRAM.

### T6 — Half-converting the BLE leak accounting.
`sBLEHeapBeforeInit = ESP.getFreeHeap()` (`Bluetooth.cpp:1257`) is differenced against `heapAfterDeinit` (`:1525`). The bias is invariant and cancels exactly, so the printed leak is **already correct**. Converting one side injects a spurious ~26 KB and makes BLE look like it leaks 36 KB/cycle. **Convert both in one edit** (preferred over a comment — a comment only helps if the next sweeper reads it) and add a paired comment at each site.

### T7 — thermal at 49152 honest refuses post-g2init.
Currently masked: `CUSTOM_ENABLE_THERMAL 0` (`System_BuildConfig.h:138`), so it is not compiled. Latent for whenever it is re-enabled.

### T8 — `-fcallgraph` of the bias is not a constant.
The IRAM-only heap serves `MALLOC_CAP_EXEC`/`32BIT` requests, so anything allocating from it shrinks the 25,864 B figure. "These tiers can never fire" is true *today*, not invariant. Do not encode it as an assumption.

---

## 4. BUGS FOUND THAT ARE NOT ABOUT THE QUERY

Ranked by severity. Several of these matter more than the threshold work.

**B1 — Bond file sends mark themselves "sent" on *submit*, not on completion.** `System_ESPNow.cpp:10152-10171` (settings) clears `bondNeedsSettingsResponse` and sets `bondSettingsSent = true` on `cmd_exec` **submit success**, before `sendBondSettings` ever runs. `:10172-10181` does the same for schema. **Any** early return inside those functions — heap, debounce, in-progress, file-open failure, `sendFileToMac` failure — leaves the worker believing it responded, with no self-re-arm. Structural, not heap-specific. Settings is partly rescued by the master's retry budget (`bondSyncInFlight`/`bondSyncRetryCount`, `:10054-10082`); **schema is not in that state machine at all** — it covers CAP/MANIFEST/SETTINGS only. Fix: set the flags to reflect actual completion, or have the senders re-arm `bondNeeds*Response` on every early return. *(Note: schema has no automatic requester — only `System_BondedPeer.cpp:180` and `cmd_bond_requestschema`, and `System_BondedPeer.cpp:190` does surface "Timed out waiting for schema sync" to the caller. So this is a wasted request + a stuck advisory, not a silent permanent disable. Do not add retry machinery for a manual operation.)*

**B2 — `initBluetooth` refusal is destructive and unrecoverable.** Detailed in T1. Fix: on the `checkMemoryAvailable` failure at `Bluetooth.cpp:1246`, restore the prior role (`initG2Client()` + revert `setSetting(gSettings.bleMode, BLE_MODE_G2_CLIENT)`); or add a cheap pre-flight in `cmd_blestart` **before** the `setSetting`/`g2Disconnect` at `:2022`, sized against the pre-teardown heap (12–16 KB).

**B3 — `computeBondLocalSettingsHash` (`System_ESPNow.cpp:7538`) is unguarded and can hash a truncated buffer.** `String::concat` fails silently (`WString.cpp:329` returns false without invalidating), `ArduinoStringWriter::flush` then returns non-zero, `serializeJson`'s return is discarded → `esp_crc32_le` hashes a short buffer. Runs after **every** settings save. Best fix: eliminate the String — a ~15-line `Print`-derived CRC sink, `serializeJson(doc, sink)`, commit only if `sink.n == measureJson(doc)`. **Do not** "leave the hash unchanged on failure": both callers (`System_Settings.cpp:1196`, `:1281`) are post-save, so the retained value is stale by definition, converting a loud false-dirty into a silent false-clean. If a fallback is needed, set a `bondLocalSettingsHashStale` retry flag.

**B4 — Bond payloads are built as internal Arduino Strings, then written to a file.** Both senders already open a temp file; they should stream `serializeJson(doc, f)` (ArduinoJson ships `PrintWriter.hpp`; `fs::File` derives from `Print`; the pattern is already used at `System_ESPNow_Identity.cpp:110`, `System_Maps.cpp:2428`, `System_User.cpp:2117`). Removes ~320 reallocs, the transient 2× peak, and the silent-truncation path in one move. **Trade-off:** serialization moves inside `FsLockGuard`. Acceptable, but measure the hold; if too long, build into a `measureJson()`-sized PSRAM buffer and do one `fwrite` under the lock.

**B5 — Schema payload is ~4× the documented size.** `System_ESPNow.cpp:7799` and `:7890` say "~8 KB schema"; `docs/WEB_API_INVENTORY.md:1768` says "~8 KB typical". `WebServer_Server.h:20-36` records that `buildSettingsSchemaJson` — the same helper — serialized to **exactly 32,768 B** and overflowed the web buffer, forcing `JSON_RESPONSE_SIZE` to 65,536. **These stale comments are what mis-set `SCHEMA_MIN_HEAP`.** Fix all three.

**B6 — `serializeJson`'s return value is discarded in both bond senders.** Add `if (len != measureJson(doc)) fail;` (or compare the streamed byte count).

**B7 — `OLED_Mode_Map.cpp:1531-1543` latches the UI on an unchecked command result.** Sets `GPSTrackManager::setLiveTracking(true)` **before** `executeOLEDCommand("sensorlog start ...")` and discards the result. Live tracking shows ON with nothing being written. Already reachable today via the `sensorLogEnabled` master switch (`System_SensorLogging.cpp:1177`), the FS-space check (`:1290`), and sealing-key failure (`:1238`). Fix regardless of the heap work.

**B8 — `G2_Page_CameraSettings.cpp:641-648` clears `g2CamStreamSettingsExitRelaunch = false` before a call that can fail, then returns unconditionally.** Reachable **today** on shipping XIAO firmware: `g2ShowCameraStream`'s first statement is `if (!gCameraRunning) return false;` (`G2_Glasses.cpp:26835`), and `closecamera` is a live CLI command (`System_Camera_DVP.cpp:2541`) and settings mutator (`System_Settings.cpp:2925`). Back tap vanishes. Fix: clear the flag only on success, else fall through to `g2ReshowSensorsDetail()`.

**B9 — Ring guard covers 1 of 4 entry points.** `G2_Page_Network.cpp:1710`: `const char* line = ringUp ? "ringconnect reconnect" : "ringconnect";` — the guarded spelling is chosen only when the ring is **already connected**. Bare `ringconnect`, `ringconnect <mac>` (`OLED_Mode_Bluetooth.cpp:441`) and `g2RingConnectSaved()` are unguarded, and all converge on the same `BLEClient` allocation and the same leak. **Fix: put the guard inside `ringPerformConnect()`**, immediately before the `if (!gRing.client)` / `BLEDevice::createClient()` block (~`G2_Ring.cpp:3643`) — that one site covers RING_SCAN/RING_SAVED/RING_MAC and every CLI spelling, and samples on the same task at the same moment as the allocation. Keep the `cmd_ringconnect` check too (it can return an actionable string). `g2RingConnectMarkComplete()` runs unconditionally in all four dispatch cases (`G2_Glasses.cpp:13726-13781`), so a deep decline does not latch — but emit a rate-limited `WARN_RINGF` since the guard now sits on the silent auto-reconnect path.

**B10 — `bleStampPairedByIfBlank(BLE_PEER_R1_RING)` runs before the guard** (`G2_Ring.cpp:4520`). In CASE C it performs `peerOwnerPersistAfterUnlock` — a durable settings write — on a command that is about to be refused. Move it below the guard.

**B11 — Refusals do not arm the debounce.** `sLastSettingsSendMs = now` (`System_ESPNow.cpp:7714`) and `sLastSchemaSendMs = now` (`:7841`) are set *after* the heap check, so every master retry re-runs the whole path and re-emits the WARN. Set them on the decline path.

**B12 — `taskStackWords` in `gMemoryRequirements[]` is dead data.** `MemoryRequirement.taskStackWords` (`System_MemoryMonitor.h:14`) is populated on every row and **never read** by `checkMemoryAvailable` (`:113-135` reads only `minHeapBytes` and `minPsramBytes`). A task stack must be **one contiguous** `INTERNAL|8BIT` block; free-size cannot detect fragmentation. Add: when `req->taskStackWords > 0`, also require `hw1InternalLargestBlock() >= req->taskStackWords + 1024`. Gating on `> 0` leaves the bluetooth row unaffected. On today's barely-fragmented heap (95,835/94,208 and 6,111/5,888 — ~2% gap) this never binds, so it adds no refusal; it closes the case where the gate passes and `xTaskCreateLogged` fails anyway (`System_TaskUtils.cpp:314`). Relevant: `i2csensor_seesaw.cpp:154-166` and `i2csensor_ano_encoder.cpp:248` set `gInputRunning = true` **before** `createInputTask()`, so that failure latches "running" with no task.

**B13 — `cmd_sensorlog`'s guard sits after the mkdir walk, key load and file create + header write** (`System_SensorLogging.cpp:1247-1265`). A decline leaves a zero-row log file on disk. **Cosmetic, not an integrity bug** — the header is complete and valid, so `resolveSessionTarget` (`:272-337`) matches and appends next time; the sibling `removeGuarded` at `:1256` exists for the different case of a *truncated* header. Worth moving above the FS work anyway (return a string literal there, not `getDebugBuffer()`, which may be unallocated).

**B14 — Cosmetic/doc.** `Bluetooth.cpp:1249/1253` "need ~60KB DRAM" → update to match 36864. `System_MemoryMonitor.cpp:66-71` registry comment. `G2_Glasses.cpp:18273` says "persistent g2_session_w (8 KB)" vs `kG2SessionStackBytes = 10240`. `G2_Glasses.cpp:24464` comment "BMP decode buffer can be tens of KB" is factually wrong (it's PSRAM). `G2_Glasses.cpp:27174-27179`'s 322 KB memory-profile table is misleading without its "all PSRAM" line. `G2_Glasses.cpp:27308`'s 1600 px JPEG cap is justified as "comfortably under PSRAM headroom" — against ~1.53 MB free with a ≤128 KB file copy held alongside, the real safe square bound is ~660×660; derive it at runtime from `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` rather than hardcoding. `G2_Glasses.cpp:27519-27530` uses `continue` on tile-build failure, leaving a partial 3-of-4 image held for 60 s.

---

## 5. RECOMMENDED SEQUENCING

### Step 0 — MEASURE FIRST. Blocking for Steps 2, 4, 5.

Add a `heapdiag` CLI command printing six numbers on one line:
```
hw1InternalFreeBytes  hw1InternalMinFreeBytes  hw1InternalLargestBlock
hw1MallocableInternalBytes  ESP.getFreeHeap()  heap_caps_get_largest_free_block(DEFAULT|INTERNAL)
```
Capture at five points:
1. Boot complete, WiFi + HTTP up, BLE not started — *validates the 95,835 B baseline*
2. **Immediately after `g2init`** — the single most important number in this document
3. At `Bluetooth.cpp:1246` on an `openble`-after-`g2init` (post-teardown) — *validates T1*
4. During an active bond settings sync
5. After ring reconnects 1, 2, 3 — *validates the leak-per-cycle figure*

**Numbers that are SOFT and must not be shipped against:**

| Figure | Status |
|---|---|
| **~19,000 B post-g2init free** | **ESTIMATE** — `6,111 + ~12,716`. g2init has not been re-run since the BR/EDR fix. Gates guards 1–7, 8b–8f, 10, 11. |
| **~52–58 KB post-teardown at `Bluetooth.cpp:1246`** | **DERIVED** from surviving-task arithmetic. Gates 8a — the brick risk. |
| 25,864 B bias | Derived, and **not constant** (IRAM heap serves EXEC/32BIT) |
| ~28–34 KB BLE server-mode internal cost | Derived by subtraction from the ~76,800 B g2init figure, which itself rests on (1) |
| ~16–17 KB espnow init internal | Identified allocations only; excludes `WIFI_AP_STA` netif + `esp_now_init` peer tables |
| ~32 KB schema / ~10 KB settings | Measured on the *web* path of the same helper, not the ESP-NOW path |
| 9,480 B `VL53L4CX_Dev_t` | Computed from vendored headers, not measured |
| 10–14 KB BLE leak/cycle | From the existing delta print, which is already correct |

### Step 1 — Land now, zero threshold risk (independent of the memory work)
B1, B2, B3, B6, B7, B8, B10, B11, B14. Plus convert **both** BLE leak-accounting sites together (T6) with paired comments. Plus amend `System_MemUtil.h:56-58` per §0. None of these change a threshold; several fix bugs that are reachable on current firmware.

### Step 2 — `checkMemoryAvailable`, one atomic commit (after measurement 2 and 3)
`:113` → `hw1InternalFreeBytes()`, **all 12 rows retuned per §1**, plus the B12 contiguity clause, plus the `Bluetooth.cpp:1249/1253` text. The query and the rows **must** move together or T1 fires.

### Step 3 — G2 viewers
Introduce `g2ViewerHeapOk()`, convert all six sites plus their inner `DEBUG_G2F` calls, add the contiguity clause, fix the stale comments, add a lens-visible decline reason.

### Step 4 — Ring guard (after measurement 5)
Relocate into `ringPerformConnect()` per B9, apply 10240, keep the CLI-level check, add the rate-limited warn. If the deep guard is deferred, **say so explicitly in the commit message** so the coverage gap is on record rather than assumed closed.

### Step 5 — Bond senders
B4 (stream `serializeJson(doc, f)`) + B5 (fix the stale ~8 KB comments) first, then drop guards 10/11 to 6144. Doing the stream fix first means the threshold stops mattering — the right order is *remove the demand*, then *lower the floor*.

### Step 6 — XIAO S3 Sense, separately
Build, run `g2init`, capture the same six numbers, then validate guards 2/3. Nothing about the feather measurements transfers: the S3's IRAM bias is zero but its reserve bias is 32,768 B.

### Do not do
- Move the bluetooth check above `deinitG2Client()` (T2).
- Use `hw1MallocableInternalBytes()` in any guard (T3).
- Convert `System_MemoryMonitor.cpp:113` without the rows (T1).
- Convert only one side of the leak accounting (T6).
- Touch `ESP.getFreePsram()` at `System_MemoryMonitor.cpp:114` or the 327680 espnow PSRAM leg — Arduino defines it as `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`, already correct.
- Add SCHEMA to the sync-tick retry state machine — schema has no automatic requester by design.
