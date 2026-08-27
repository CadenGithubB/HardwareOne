# DRAM long-tail verification — 2026-08-19

Per-symbol adversarial verification of the remaining internal-DRAM statics >=96 B on
feather_esp32_v2 (post v0.99.91.1 + sTaskStackReg). Six subsystem verifiers, six
disqualifiers each (ISR/WiFi-callback, DMA, pre-PSRAM-init, spinlock, cache-off,
secrets/health) plus zero-init and downstream-consumer analysis. ELF/link-map ground
truth from build 37ca624e3.

Pool correction: the earlier "~22 KB / ~60 rows" estimate counted IDF/BT-ROM internals
(~19 KB, untouchable) and already-refused symbols. Real app pool >=96 B: ~17.0 KB in 49
symbols; minus prior refusals/deferrals (~6.7 KB) = ~10.3 KB fresh, verified below.
The 32-95 B app tail is only 9 symbols / 461 B — not worth per-row verification.

## Totals by verdict
- SAFE_PSRAM: 5,628 B (21 symbols; 3,708 B app + 1,920 B vendored-Arduino)
- CONST_TO_FLASH: 336 B (1 symbol, needs build-time string)
- RESTRUCTURE: 1,132 B (5 symbols, small fixes each)
- NOT_WORTH_IT: 532 B (4 symbols)
- UNSAFE: 4,376 B (15 symbols — spinlocks and secrets)

## REGRESSION FOUND (batch 1, committed in v0.99.91.1 8842ccc7)
sTransactionHistory (G2_Ring.cpp:283, EXT_RAM_BSS_ATTR since batch 1) IS scanned inside
portENTER_CRITICAL(&sTransportMux) via ringFindTransactionLocked / ringAllocateTransactionLocked /
ringUpdateTransaction (:565-618) and ringMarkGenerationDisconnected (:714-723) — violating the
file's own ":276-279 no PSRAM byte under sTransportMux" invariant. Latency hazard (PSRAM scan
under masked interrupts), not correctness. Fix options: (a) revert to internal (-672 B), or
(b) store the slot index in G2RingTransactionHandle so the locked touch is one entry.

## Symbol identity corrections vs the nm/map attribution
- gR / gL = G2Temple RIGHT/LEFT connection state in G2_Glasses.cpp (NOT SensorStubs/OLED_Utils)
- gSc = BLE secure-channel key table in System_BleSecureChannel.cpp (NOT G2_Ring)
- sDeferred = System_ESPNow_FsList.cpp:102 (NOT G2_Health.cpp)
- gControlStatus (200 B) = G2_Glasses.cpp:1154 (G2_Ring's sControlStatus is a different, smaller symbol)

## Full verdicts
### bus — 1064 B — **SAFE_PSRAM**
Definition: components/arduino/cores/esp32/esp32-hal-i2c-ng.c:44 (the live file on IDF 5.5.1; esp32-hal-i2c.c:66 is compiled out by its ESP_IDF_VERSION < 5.4.0 guard at line 19)

All six disqualifiers pass. A: every mutating access sits between xSemaphoreTake/Give on bus[n].lock (i2cInit:87, i2cDeinit:177, i2cWrite:249, i2cRead:303, i2cWriteReadNonStop:351, i2cSetClock:396) - a blocking mutex take proves task context; lock-free reads (i2cBusHandle:63, i2cIsInit:70, i2cGetClock:446-450) are called only from Wire.cpp and firmware System_I2C code, task context; i2cDetachBus:46-57 is the periman deinit callback, invoked only from perimanSetPinBus during pin reattach (task context). No ISR or WiFi-task caller (firmware has zero attachInterrupt users; System_ESPNow.cpp 'Wire' hits are the V3 wire-schema naming, not TwoWire). B: struct holds only i2c_master handles + scalars; data buffers are caller-owned; classic-ESP32 I2C is FIFO/interrupt, no DMA retention of this array. C: first touch is Wire.begin() from app_main-descended init; even a static-ctor touch runs at do_global_ctors (startup.c:207), after esp_psram_bss_init (cpu_start.c:729). D: mutexes only, no portENTER_CRITICAL in the file. E: no panic/shutdown/OTA cache-off path touches I2C. F: handles and pin numbers only. Zero-init confirmed (no initializer; nm 'b' .bss, 0x428=1064 B - the dev_handles[128] per bus x SOC_I2C_NUM=2 dominates). Patch: one-line EXT_RAM_BSS_ATTR (esp_attr.h already included at :26) under the local-patch convention - biggest single yield in this batch, worth the marker + verify_patches.sh refresh.

- Init: Zero-init .bss (no initializer; nm section 'b')
- Contexts: Task: Wire methods via i2cInit/i2cWrite/i2cRead/i2cWriteReadNonStop/i2cSetClock under per-bus mutex (esp32-hal-i2c-ng.c:87,249,303,351,396). Task: periman deinit callback i2cDetachBus via perimanSetPinBus pin-reattach (esp32-hal-i2c-ng.c:46, esp32-hal-periman.c:150). Task: lock-free reads i2cIsInit/i2cBusHandle/i2cGetClock from Wire.cpp and System_I2C.cpp. No ISR, no WiFi-task, no DMA contexts found.
- Downstream: Consumers: every I2C sensor driver (i2csensor_*.cpp via the i2cDeviceTransaction wrapper) and the OLED push in HAL_Display.cpp. Per-transaction cost is a handful of field reads (~1-2 us of PSRAM cache misses worst case) against 100 us - 25 ms of bus time per transaction - unmeasurable. No consumer sees a behavior change.

### pins — 640 B — **SAFE_PSRAM**
Definition: components/arduino/cores/esp32/esp32-hal-periman.c:20

Passes all six today, with one documented latent caveat. A: the only ISR-adjacent readers are __digitalWrite (esp32-hal-gpio.c:168-183) and __digitalRead (:185-196), which call perimanGetPinBus and are marked ARDUINO_ISR_ATTR - but CONFIG_ARDUINO_ISR_IRAM is not set (sdkconfig.esp32:416, sdkconfig:627), so the attr expands empty (esp32-hal.h:48-52), the functions live in flash and cannot run from a cache-off ISR at all; the firmware has ZERO attachInterrupt or gpio_isr_handler_add users (grep of components/hardwareone + main). The NeoPixel critical-section+digitalWrite hit (components/hardwareone_libs/Adafruit_NeoPixel/Adafruit_NeoPixel.cpp:2393) is the nRF52 path, not compiled - ESP32 uses RMT espShow. All writers (perimanSetPinBus:118-162, perimanSetPinBusExtraType:164) run from driver attach/detach in task context. B: bookkeeping table, never handed to a driver. C: earliest touch is a ctor-called pinMode at do_global_ctors (startup.c:207), after esp_psram_bss_init (cpu_start.c:729). D: no critical sections in periman.c. E: no panic-path GPIO in firmware. F: pin/bus bookkeeping only. Latent caveat for the patch marker: any FUTURE GPIO ISR calling digitalRead/digitalWrite would take PSRAM cache misses in a cache-on ISR - jitter, not corruption. Bundle in the same local-patch hunk as deinit_functions to amortize the vendored-file burden.

- Init: Zero-init .bss (no initializer; nm 'b'; 40 pins x 16 B packed = 640 B on classic ESP32)
- Contexts: Task: driver attach/detach writers - perimanSetPinBus from pinMode (esp32-hal-gpio.c:157) and uart/i2c/spi/ledc/adc/rmt HAL init/deinit. Task (ISR-capable by design but no ISR caller exists in this firmware): perimanGetPinBus from __digitalWrite:178 and __digitalRead:192 (ARDUINO_ISR_ATTR empty, CONFIG_ARDUINO_ISR_IRAM unset, zero attachInterrupt users). Task: boot-time chip-debug-report.cpp dump.
- Downstream: Consumers: digitalWrite/digitalRead (23 firmware call sites - I2C recovery toggles in System_I2C_Manager.cpp, battery/NeoPixel power pins, display resets; none in tight loops), pinMode, analogRead, every peripheral attach. Adds at most one PSRAM cache-line miss per digital IO call - negligible at this firmware's call rates.

### sPeerOwner — 608 B — **SAFE_PSRAM**
Definition: components/hardwareone/BLE_Peers.cpp:274

All six pass. (A/D) All four access sites are under the PeerOwnerGuard recursive FreeRTOS mutex in task context: peerOwnerPublish (325+), blePeerOwnerSessionSnapshot (492-495), blePeerOwnerSessionIsCurrent (506-509), blePeerOwnerSessionClearIfCurrent (521-533). The only spinlock in the neighborhood (sPeerOwnerInitMux, 277) guards lazy creation of the mutex handle (281-287) and never touches sPeerOwner itself. The design comment (263-267) confirms: one mutex, never held across filesystem I/O (peerOwnerPersistAfterUnlock runs after the guard drops, 535). (B) No DMA. (C) Plain struct, zero-init (kNoTransportSessionEpoch = 0 per System_User.h:62; char user[]={}; generation=0), .bss confirmed (nm 'b'), no ctor. (E) None. (F) Contents = public username (kPublicUsernameMaxLen), generation counter, transport epoch. The username is an authority/identity marker, not a credential — it is already persisted un-encrypted in the settings JSON on LittleFS ('pairedByUser', BLE_Peers.cpp:1568) — so PSRAM residue reveals nothing the flash doesn't.

- Init: PeerOwnerAuthority default member inits are all-zero (kNoTransportSessionEpoch = 0), so .bss — EXT_RAM_BSS_ATTR-compatible as-is.
- Contexts: Writers: peerOwnerPublish on pairing/login/logout (CLI, settings load, G2 session paths — task context). Readers: System_User.cpp:40/56/610 (session teardown/lookup on cmd_exec), G2_HijackCmd.cpp:83/105/246 (per-G2-command owner-authority check on the G2 dispatcher/cmd_exec), blePeersWriteJson snapshot (settings persist). Per-command, NOT per-BLE-notify.
- Downstream: The G2 hijack per-command auth check (G2_HijackCmd.cpp:246 blePeerOwnerSessionIsCurrent) takes a mutex and reads ~76 B — a PSRAM cache miss adds microseconds to a millisecond-scale command execution. No consumer breaks. Best byte-payoff of the safe set: 608 B.

### sSetupOwner (RingSetupOwner) — 308 B — **SAFE_PSRAM**
Definition: /Users/morgan/esp/hardwareone-idf/components/hardwareone/G2_Ring.cpp:318 (.dram0.bss 0x134, confirmed in ELF)

All six disqualifiers pass. (A) r1_owner task only: ringBeginOwnedGeneration :2689-2693, ringFinishSetup :1919-1920, ringAdvanceSetup :1933-1936, ringHandleSetupRx :2057-2098 (reached via ringProcessRxFrame :2534, called only from ringOwnerTask :3240), Ring1Error match :2438-2440, ringServiceSetup :2808-2901, dark-probe :2774-2788/2802-2803, drop :2715-2719, loop gate :3258 — the BLE notify callback (ringNotifyThunk :2584, BTC task) never touches it; it only fills sRxSlabs. (B) frame.bytes handed to ringOwnerWrite :2882-2883 → BLERemoteCharacteristic::writeValue (:813), where Bluedroid memcpys into its own message — no DMA retention. (C) owner task created at :3997 post-app_main; first touch on connect. (D) never accessed under a spinlock — the portENTER_CRITICAL sections inside its functions touch only sControlStatus (:1910-1915, :2857-2859). (E) not in any panic/OTA path. (F) frame contents are pairAuth probe [0x01] (System_R1_Protocol.cpp:214-221, a fixed public byte — no key), deviceInfo query, syncTime epoch+tz, advStart temple MACs — no secrets, no health records.

- Init: Zero-init .bss (G2_RING_SETUP_IDLE=0, R1Frame{} zero) — EXT_RAM_BSS_ATTR compatible as-is.
- Contexts: r1_owner task exclusively; the setup ritual runs once per connect plus one .active flag read per 20 ms loop lap.
- Downstream: Consumers are the setup state machine itself; external observers read sControlStatus (DRAM) instead. Latency impact: a handful of PSRAM cache lines during the connect-time ritual and one flag check per lap — negligible next to the existing per-frame PSRAM slab reads. Clean 308 B win.

### sActivePacketAck (RingActivePacketAck) — 300 B — **SAFE_PSRAM**
Definition: /Users/morgan/esp/hardwareone-idf/components/hardwareone/G2_Ring.cpp:321 (.dram0.bss 0x12c, confirmed in ELF)

All six pass. (A) r1_owner only: dup-check in ringQueuePacketAck :2010-2018 (via ringProcessRxFrame, owner ctx), ringServicePacketAck :2723-2756, resets :2695/:2713/:2734, loop reads :3216-3218/:3247. (B) frame.bytes → ringOwnerWrite :2746-2747 → writeValue, host-stack copy, no retention. (C) post-connect only. (D) no spinlock touches it (sRxSlabMux/sTransportMux sections never reference it). (E) no cache-off path. (F) contents are a generation + R1PacketAckDescriptor (module/cmd/subCmd/serial — static_assert 12 B at :244) and the outbound packet-ACK frame — pure protocol flow-control metadata, no health values, no secrets.

- Init: Zero-init .bss (valid=false, pending{} POD-zero, R1Frame{} zero).
- Contexts: r1_owner task exclusively.
- Downstream: The .valid flag is read every 20 ms owner lap and after each RX frame (:3216/:3247); the lane is only actively exercised during health-daily sync bursts, when the ring is already streaming fragments out of PSRAM sRxSlabs. One extra cache line per lap is noise. 300 B win; batch it with sSetupOwner.

### gLoopPerf — 296 B — **SAFE_PSRAM**
Definition: components/hardwareone/HardwareOne.cpp:1093

All six disqualifiers pass. A: no ISR/timer-callback access — perfMarkSection() (HardwareOne.cpp:1103) and loopHealthTick() (1110) are called only from loop() (HardwareOne.cpp:2560, 2593-2990) on the Arduino loopTask; loopHealthGetSnapshot (1242-1247) runs from the perftop command (cmd_exec task, System_TaskUtils.h:311). B: never handed to a driver. C: zero-init .bss, first touch is in loop(), long after PSRAM init. D: no portENTER_CRITICAL anywhere in the access set. E: not read or written in panic/cache-off paths (crash path uses its own RTC records). F: pure timing counters. Honest latency assessment: ~7 struct accesses per lap (6 perfMarkSection stores + the loopHealthTick read-modify block); the 296 B span ~10 32-byte cache lines that are re-touched every lap so they stay warm; absolute worst case (all-miss, classic-ESP32 quad PSRAM ~1-2 us per line fill) is ~10-20 us/lap, i.e. <=1-2% of one core even at 1 kHz lap rates and unmeasurable against the 16 ms histogram floor and 200 ms stall threshold. Self-measurement skew: a miss on the store lands AFTER esp_timer_get_time() is captured, so section attribution shifts by single-digit us on ms-scale sections — noise. This is the hottest of the approved candidates; it is safe, just not free.

- Init: zero-init .bss (plain static struct, no initializer — comment at :1093 confirms)
- Contexts: writer: loop() on loopTask via loopHealthTick (HardwareOne.cpp:2560) and perfMarkSection x6 (2593,2635,2697,2708,2737,2990); readers: same task (5 s Tier-2 report, 1175-1203) + perftop snapshot accessor 1210-1247 on cmd_exec task
- Downstream: perftop command and the 5 s [LOOPHEALTH]/[PERF] prints consume it; both are diagnostic, low-rate, task-context. No consumer breaks; worst-lap timing numbers gain us-level jitter only.

### sRxFingerprints (RingRxFingerprint[16]) — 256 B — **SAFE_PSRAM**
Definition: /Users/morgan/esp/hardwareone-idf/components/hardwareone/G2_Ring.cpp:322 (.dram0.bss 0x100, confirmed in ELF)

All six pass. (A) r1_owner only: ringRememberRx :1982-1994 (reached solely from ringProcessRxFrame :2529, owner task), memset on generation begin :2697. The BTC-task notify thunk never sees it. (B) never handed to a driver. (C) post-connect. (D) not under any spinlock. (E) none. (F) fields are generation/crc32/serial/module/cmd/subCmd/statusByte (:254-262) — duplicate-suppression metadata about frames, never the decoded health payloads themselves (those flow R1Decoded-on-stack → typed caches), so the health-records policy is not triggered.

- Init: Zero-init .bss; code additionally memsets it per generation (:2697), so the zeroed boot image is exactly the expected state.
- Contexts: r1_owner task exclusively.
- Downstream: Hot-ish: a 16-entry × 16 B linear scan per accepted RX frame (:1987-1988). But each such frame was just memcpy-read out of a PSRAM slab (:3236-3240), so ~4 additional PSRAM cache lines per frame at BLE-notify rates (bursty during health sync, sparse otherwise) is well inside budget on the classic ESP32. No consumer breaks. 256 B win.

### deinit_functions — 216 B — **SAFE_PSRAM**
Definition: components/arduino/cores/esp32/esp32-hal-periman.c:19

Same profile as pins[] but strictly quieter. Written only by perimanSetBusDeinit (:225-237) and perimanClearBusDeinit (:245-253) during driver init - task context; read in perimanSetPinBus:146-150 (attach/detach, task) and perimanGetBusDeinit:255-264 (uartDetachPins-style paths, task). Never touched by the digitalWrite/digitalRead fast path (that only reads pins[]). Zero-init '= {NULL}' -> .bss (nm 'b'). No ISR, no DMA, no pre-PSRAM touch (do_global_ctors runs after esp_psram_bss_init), no spinlocks, no cache-off path, no secrets. Standalone it is only 216 B - not worth its own vendored patch; worth it ONLY as a rider in the same esp32-hal-periman.c hunk as pins[] (one marker, one verify-count bump, 856 B combined).

- Init: Zero-init .bss ('= {NULL}' - all-zero image; nm 'b')
- Contexts: Task: perimanSetBusDeinit writers from every HAL driver init (e.g. i2cInit at esp32-hal-i2c-ng.c:104-105, uart, spi, ledc, adc). Task: reads in perimanSetPinBus:146,150 during pin reattach and perimanGetBusDeinit:255. No ISR/DMA/WiFi-task contexts.
- Downstream: Consumed only during pin attach/detach and driver teardown - cold paths (init, Wire.end, uartDetachPins). Zero hot-path latency impact; no consumer breaks.

### sDummyResult — 208 B — **SAFE_PSRAM**
Definition: components/hardwareone/OLED_SetupWizard.cpp:529 (type SetupWizardResult, System_SetupWizard.h:32)

All six disqualifiers pass, including the credential check: although SetupWizardResult contains String wifiPassword/mqttPassword members, sDummyResult NEVER receives any data. Its only uses are wizardNextPage(sDummyResult) from the FEATURES page (:540) and handleTogglePageInput(...,sDummyResult) for SENSORS/NETWORK pages (:548, :552), dispatched strictly per-page by System_SetupWizardMode.cpp:903-909 (subMode is derived from getWizardCurrentPage()). wizardNextPage writes result fields ONLY when leaving the SYSTEM page (System_SetupWizard.cpp:825-848, tz/ntp/led strings), and the SYSTEM page handler receives the caller's real result — never sDummyResult. Credential writes (result.wifiPassword OLED_SetupWizard.cpp:437, result.mqttPassword :843) happen in renderWiFiPage/handleOLEDMQTTPage, which are never passed sDummyResult (verified: only refs are lines 529/540/548/552). So it is a pure signature filler: written never, read never. Main-loop context, zero-init .bss (nm 'b' 0xd0); String default-ctors run at static-init AFTER PSRAM init (IDF zeroes .ext_ram.bss and brings PSRAM up in do_core_init, before global ctors), matching existing EXT_RAM_BSS_ATTR precedents.

- Init: zero-init .bss; String members' trivial-empty ctors run post-PSRAM-init — safe
- Contexts: Main loop task only, via the OLED wizard input dispatch (System_SetupWizardMode.cpp:903-909 -> OLED_SetupWizard.cpp handleFeaturesInput/handleSensorsInput/handleNetworkInput).
- Downstream: No real consumers — nothing ever reads a value out of it. Even better than a PSRAM move: refactor the toggle-page signatures to drop the unused parameter and delete it entirely (208 B + no PSRAM). As a straight EXT_RAM_BSS_ATTR move it is trivially safe.

### reply (function-local static in ringConfirmRawSet) — 180 B — **SAFE_PSRAM**
Definition: /Users/morgan/esp/hardwareone-idf/components/hardwareone/G2_Ring.cpp:4684 (ELF _ZZL17ringConfirmRawSetPvE5reply, .dram0.bss 0xb4)

All six pass. (A) ringConfirmRawSet is the accept-callback registered with cliRequestConfirm (:4908-4910); confirm answers are typed by the user and execute on the command-execution path (cmd_exec_task — BLE ingress is explicitly barred from answering interactive CLI modes, Bluetooth.cpp:1096-1097 COMMAND_CONTEXT_MODE_INDEPENDENT), never ISR/BTC. (B) the returned const char* is only formatted/printed by the CLI framework — no driver retention. (C) user-interactive, long after PSRAM init. (D) no spinlock. (E) none. (F) contents are 'RING: raw SET queued (tx=N gen=N)…' — transaction ids only; the raw payload itself already lives in PSRAM by existing design (sPendingRawSet :4675). Plain zero-init char[180], so no guard-variable or constructor concern. Direct in-file precedent: EXT_RAM_BSS_ATTR on function-local static char buffers at :4621 (cmd_ringconnect) and :4726 (cmd_ringquery) — apply the identical one-token change: `EXT_RAM_BSS_ATTR static char reply[180];`.

- Init: Zero-init .bss.
- Contexts: cmd_exec_task (confirm-mode accept callback), one write + one read per admin-confirmed raw SET — the rarest path in the file.
- Downstream: Consumer is the CLI reply pipeline reading the buffer once per confirmation; PSRAM latency is irrelevant at human-interaction frequency. 180 B win.

### sPickerReq — 172 B — **SAFE_PSRAM**
Definition: components/hardwareone/OLED_Mode_FileBrowser.cpp:219 (struct FilePickerRequest, OLED_Display.h:528)

All six disqualifiers pass. (A) no ISR/callback access — every reference is in FileBrowser mode functions on the main loop: push (oledFilePickerPush :249, called only from the peer file-send flow :625-631), title accessor for the breadcrumb (:246, read by OLED_Utils.cpp render), resolve (firePickerCallback :270-274, called from prepareFileBrowserData's pending-action processing per its own comment), session reset (:227, from the OLED session watcher). The espnow_task FS-list reply callback (onPeerListReply :416) never touches it. (B) no DMA — never handed to a driver. (C) touched only after UI is up. (D) no spinlocks in these paths. (E) not in any panic/OTA path. (F) contents are a title[32], startPath[128], filter/onPicked function pointers, requesterMode — no credentials. Zero-init `= {}` -> .bss confirmed (nm 'b' 0xac). POD struct, no String ctor concerns.

- Init: zero-init .bss (= {}, nm 'b')
- Contexts: Main loop task only: OLED_Mode_FileBrowser.cpp :227 (session reset), :246 (per-frame title read while picker active), :251 (push), :270-274 (resolve), :671-672 (filter/startPath application in prepareFileBrowserData).
- Downstream: Consumers: file browser render (per-frame title read while a picker is open) and the one-shot callback resolution — PSRAM latency negligible. No consumer breaks. Clean EXT_RAM_BSS_ATTR candidate, 172 B.

### gRecFloorWin — 160 B — **SAFE_PSRAM**
Definition: components/hardwareone/System_Microphone.cpp:570

All six disqualifiers pass. A: no ISR/timer/DMA-callback/WiFi-task access — recordingTask is a normal task; the G2 BLE mic path feeds PCM through audioReadPcm into a separate 'buffer' (1009), and the floor window is only touched by the task loop. B: never handed to any driver — it stores computed int32 chunk averages, not sample data; the DMA/I2S targets are HAL-internal buffers plus the local 'buffer'. C: touched only mid-capture, long after app_main/PSRAM init. D: no spinlock encloses any access (verified lines 900-1110 and 1632). E: never on a flash-cache-off path. F: contents are per-128ms amplitude averages (a coarse loudness envelope) — not decodable audio, not a health record, no credentials. File precedent exists: gMicCmdBuffer at line 663 is already EXT_RAM_BSS_ATTR.

- Init: No initializer — plain .bss, zero-init. Indices gRecFloorWinIdx/gRecFloorWinCount (571-572) are reset at capture start (1632), so EXT_RAM_BSS_ATTR zeroing semantics are identical to today.
- Contexts: Exactly two contexts, verified repo-wide (no references outside System_Microphone.cpp; symbol is static). (1) recordingTask (System_Microphone.cpp:846, ordinary FreeRTOS task, TASK_PRIORITY_HIGH pinned to core 1 via xTaskCreatePinnedToCore at 1697-1705): write + windowed-min scan at 1064-1069, once per 128 ms audio chunk. No portENTER_CRITICAL anywhere in lines 900-1110 — the access is plain task code. (2) The start-recording function (caller task, e.g. cmd_exec_task/EvenAI admission) resets the indices at 1632 BEFORE the task is created at 1697 — strictly sequenced, no new concurrency.
- Downstream: Sole consumer is the VAD ambient-floor computation feeding gRecFloorAvg (1067-1069), which drives auto-stop/trim in the same task. Latency cost: 1 write + up to 40 int32 reads (a few cache lines) per 128 ms chunk — microseconds against a 100 ms audioReadPcm timeout and per-chunk LittleFS writes in the same loop. No consumer breaks; auto-stop timing is unaffected. Payoff is only 160 B, but the move is clean.

### gV4FragAckWait — 160 B — **SAFE_PSRAM**
Definition: components/hardwareone/System_ESPNow.cpp:352

All six disqualifiers pass. A: the WiFi-task RX callback never touches this array (onEspNowDataReceived is ring-push-only, 1305-1326); the ACK scan runs on espnow_task, an ordinary drain task. B: pure bookkeeping (msgId/fragIndex/dstMac/flags) — the frames handed to esp_now_send are separate stack buffers (2577) via v4_radio_emit. C: touched only during ESP-NOW operation, long after PSRAM init. D: EspNowTxGuard is a mutex, not portENTER_CRITICAL — no spinlocked access anywhere (all 12 reference lines audited). E: no flash-cache-off context. F: peer MACs and message IDs — on-air plaintext, not secrets. Non-blocking note: sentMs is written (366, 2609) but never read — dead field, so no timer/sweeper context exists. The lock-free volatile ->acked cross-task poll pattern (10 ms granularity) is unchanged by PSRAM placement; this file already shares EXT_RAM_BSS_ATTR state across these same tasks (gTopoStreams:613, gV4Dedup:1424, gSessionPrewarm:3453).

- Init: No initializer — .bss, zero-init (8 slots x ~20 B V4FragAckWait, V4_FRAG_ACK_WAIT_MAX=8 at line 262). All-zero start state (active=false) is exactly what the alloc scan expects.
- Contexts: Three contexts, all ordinary tasks, none the WiFi task. (1) v4_frag_ack_alloc (354-373): slot claim under EspNowTxGuard — a FreeRTOS mutex (gEspNowSessionTxMutex, System_Mutex.h:250-276), NOT a spinlock — called from the encrypted chunked sender at 2603; per the comment at 356-358 fragmented sends run OFF espnow_task, on cmd_exec_task / SENSOR_BCAST_TASK (context provably blockable: vTaskDelay at 2618/2631). (2) Sender lifecycle at 2608-2633: sentMs/acked writes, lock-free 10 ms poll of ->acked, active=false free under the same mutex. (3) ACK-mark scan (6398-6408) under the same mutex in v4_try_handle_incoming — which runs ONLY on espnow_task: verified structurally, not just by its comment — the registered RX callback onEspNowDataReceived (esp_now_register_recv_cb at 11059; body 1305-1326) only memcpys into gEspNowRxRing and returns; the ring drain in processMeshHeartbeats (9532-9558) rebuilds recv_info and calls onEspNowRawRecv (7058) -> v4_try_handle_incoming (7072) on espnow_task. The relay-unwrap recursion (5354) stays on the same task. The stale 'Wi-Fi callback path' comment at 7047 belongs to captureEspNowFrame (which never touches this array).
- Downstream: Consumers: the fragmented encrypted sender (per-fragment 10 ms ->acked poll — one PSRAM cache line per poll, invisible against a 200 ms ACK timeout) and the espnow_task ACK-RX scan (8 slots ~160 B per received ACK). The identical RX dispatch path already does per-frame PSRAM work that dwarfs this: EXT_RAM_BSS_ATTR plainBuf AEAD scratch in the very same function (6150) and the gV4Dedup PSRAM scan (1424). No consumer breaks; espnow_task drain latency change is noise relative to established in-file precedent.

### cmd_dictate::status (function-local static) — 160 B — **SAFE_PSRAM**
Definition: components/hardwareone/System_Dictation.cpp:425

All six pass. The snprintf at :434-437 runs OUTSIDE any critical section — dictationSnapshotNow() copies gDict to a stack snapshot under the mux (:262-266) and the static buffer is formatted afterward from that stack copy. A: cmd_dictate is UART-transport-gated (:415-419) and runs on the serialized command-executor task — no ISR/callback path reaches it. B: the returned pointer is consumed by the command-reply path which frames/copies it for UART TX (uart driver copies into its own ring; classic-ESP32 UART has no DMA). C: zero-init .bss static, first touch on first `dictate status`. D: no lock held during access. E: no panic-path use. F: contents are state name, source label ('PDM'/'G2'), elapsed ms, and fixed failure literals ('no mic', 'host did not answer', ... :142-151, :187, :207, :293) — never transcript text, never credentials. Access is once per host status poll — cold. Note the caveat: EXT_RAM_BSS_ATTR does not apply syntactically to a function-local static; it must be hoisted to file scope (or use a one-line wrapper) to take the attribute — trivial.

- Init: zero-init .bss
- Contexts: single context: cmd_dictate on the command-executor task (UART transport only, :415-417)
- Downstream: reply text delivered to the CM5 over the UART link; per-command, no hot path

### sActiveDesc — 156 B — **SAFE_PSRAM**
Definition: components/hardwareone/System_LLMBackend.cpp:32

All six pass. A: no ISR/WiFi-task access — writers are llmBackendSelect (:206, :216) and llmBackendUnload (:237), called from OLED_Mode_LLM.cpp:343/946 (display task), System_LLMCommands.cpp:229/254 (cmd_exec), WebPage_LLM.cpp:269/291 (httpd worker); reader llmBackendActiveModel (:244) called from G2_Glasses.cpp:7488 (G2 menu/tap dispatcher task) and web — every context is a traced ordinary task. B: no driver hand-off (struct is memcpy'd to caller-owned `out`). C: zero-init `= {}`, first touch is a user model-select, far past app_main. D: no spinlock — the file deliberately relies on plain stores (comment :27-29). E: no panic-path access. F: contents are model id/name/path/size (:71-77) — filenames, not secrets. Latency: written per model select (seconds-long operation anyway), read per settings/picker render — cold. Note: the pre-existing, code-acknowledged descriptor/kind pair race (:200-205) is unchanged by the move; sActiveKind (enum, 4 B) stays in DRAM and remains the dispatch gate, so the failure mode is identical to today.

- Init: zero-init (.bss, `= {}` at :32)
- Contexts: write: llmBackendSelect/:206,:216 and llmBackendUnload/:237 from cmd_exec, httpd, OLED display tasks; read: llmBackendActiveModel/:244 from G2_Glasses.cpp:7488 and WebPage_LLM.cpp — all plain task contexts
- Downstream: G2 LLM picker row highlight and web LLM page status; per-render read of a 156 B copy — no hot path, no consumer breaks

### sDeferred — 152 B — **SAFE_PSRAM**
Definition: components/hardwareone/System_ESPNow_FsList.cpp:102

LOCATION CORRECTION: sDeferred (152 B, _ZL9sDeferred) is in System_ESPNow_FsList.cpp:102, NOT G2_Health.cpp (that file only has the unrelated 18 B sDeferredHistoryPeerId at G2_Health.cpp:108). All six disqualifiers pass: (A) writers run on espnow_task — a regular drain task, not the WiFi task; the actual esp_now RX callback (onEspNowDataReceived, System_ESPNow.cpp:1305-1326) only memcpys into gEspNowRxRing and never touches sDeferred (comment System_ESPNow.cpp:549 confirms the WiFi-task boundary). (B) No DMA: request payloads are memcpy'd in (FsList.cpp:443-457) and replies are built into separate buffers on cmd_exec; sDeferred is never handed to esp_now_send. (C) Zero-init .bss (nm 'b'); fsListInit (memset, FsList.cpp:133) is called from HardwareOne.cpp:2288 at setup time, well after PSRAM init. (D) All access is under sMutex, a FreeRTOS xSemaphoreCreateMutex (FsList.cpp:109-118, ScopedLock) — a task-level mutex, never a portENTER_CRITICAL section. (E) No flash-cache-off contexts. (F) Contents are an op tag, source MAC, and fs request structs (paths/reqIds) — no credentials, keys, or decoded health records.

- Init: static DeferredReply sDeferred = {} — zero-init, .bss, EXT_RAM_BSS_ATTR-compatible as-is.
- Contexts: Writers: captureDeferred on espnow_task under sMutex (FsList.cpp:432-461; mutex comment at 105-107 names espnow_task RX handlers, cmd_exec_task, and the ESP-NOW tick as the three contexts). Reader/consumer: processDeferredLocked on cmd_exec_task (FsList.cpp:555+ — job explicitly moved OFF espnow_task per comment at 462-470), which snapshots the union then releases the slot. sendBusyReply reads only the incoming payload, not sDeferred.
- Downstream: Consumers: ESP-NOW file-browser RPC (list/stat/get) peers. Access is one small struct write per incoming fs request and one read per deferred job — user-driven file browsing, not per-frame/per-loop. A PSRAM cache miss adds microseconds to an operation that then does LittleFS directory scans (milliseconds). No consumer breaks. 152 B reclaimed.

### gOledDialog — 144 B — **SAFE_PSRAM**
Definition: components/hardwareone/OLED_UI.cpp:23 (struct OledDialog)

All six disqualifiers pass — but the honest finding is that this is a DEAD feature: the only activation API (oledDialogOK/oledDialogYesNo/oledDialogCustom, OLED_UI.h:101-107) has ZERO callers anywhere in the repo (verified repo-wide grep incl. main/); only OLED_UI.cpp's own layer pump references it (input :870-871, render :890, active :916), all gated on gOledDialog.active which nothing ever sets. Deleting the dialog subsystem frees the same 144 B DRAM plus flash and is strictly better than EXT_RAM_BSS_ATTR. If kept: aggregate initializer is all-zero (NONE/nullptr/false/0) so it already lands in .bss (nm 'B' 0x90), access is main-loop only, no Strings, no secrets, no DMA/ISR/spinlock.

- Init: aggregate initializer is all-zero -> .bss (nm 'B'); EXT_RAM_BSS_ATTR compatible as-is (drop the redundant initializer for clarity)
- Contexts: Only OLED_UI.cpp internal: oledDialogShow*/Close (:229-306, unreachable — no external callers), oledDialogHandleInput (:314, from oledUIHandleInput :870), oledDialogRender (:358, from oledUIRender :890) — main loop task via OLED_Utils.cpp:4206.
- Downstream: None — no code can ever activate it. Recommend deletion over relocation; as a move it is trivially safe with zero latency consequence.

### bleMessageHistory — 128 B — **SAFE_PSRAM**
Definition: components/hardwareone/Bluetooth.cpp:898

All six pass, though the payoff (128 B, and only when ENABLE_OLED_DISPLAY) is marginal. Secrets check (the flagged risk): the RX write (Bluetooth.cpp:982) stores 'auditLine', which is redacted at source — BLE 'login <user> <password>' is masked to 'login <user> ********' including malformed grammars (Bluetooth.cpp:956-973, comment explicitly about not leaking the credential tail 'into debug/OLED history'), and every other command goes through redactCmdForAudit (Bluetooth.cpp:966/973). So typed credentials over BLE do NOT reach this buffer. The TX writes (1698, 1750) store the first 28 chars of outbound notifies; the only credential-display path found — settings reads — masks isSecret values as '********' (System_Settings.cpp:2731, 2880, 3271 'never expose secrets'), and no command was found that echoes a raw password (grep of Network_WiFi/System_Settings output paths came back clean). Residual caveat: a FUTURE command leading its BLE response with a credential would leave a 28-byte prefix in probeable PSRAM. Contexts: (A) RX writer runs on cmd_exec_task (processBleCommandLine, comment at Bluetooth.cpp:922-923), TX writers run on whatever task sends the notify — all task context, no ISR. (B) No DMA — strncpy copies; the BLE stack is handed 'data', never this buffer. (C) Plain char array, .bss, no ctor. (D) No lock at all around it (pre-existing benign race), so no spinlock. (E) None. (F) Redacted as above.

- Init: static char[4][32], zero .bss (nm 'b'), no initializer — EXT_RAM_BSS_ATTR-compatible as-is.
- Contexts: Writers: bleAddMessageToHistory (Bluetooth.cpp:2725-2732) called from cmd_exec_task RX (982) and per-notify TX success paths (1698 in sendBLEResponseToSession retry loop, 1750 in bleRawNotify). Reader: bleGetRecentMessages (2737-2745) returns POINTERS into the array, consumed by the OLED Bluetooth status screen render loop (OLED_Mode_Bluetooth.cpp).
- Downstream: Per-notify cost: one <=31-byte strncpy to PSRAM on the BLE TX path — negligible next to the Bluedroid send itself, but it IS on the per-notify hot path during streamed CLI output, so expect a few extra microseconds per notify. OLED reads happen per render frame only while the BLE status screen is showing. Nothing breaks (pointers into PSRAM are fine for the OLED text renderer). Honest assessment: safe, but 128 B is barely worth the patch.

### cmd_voicefetch::sReply (function-local static) — 128 B — **SAFE_PSRAM**
Definition: components/hardwareone/System_UartLink.cpp:1380

All six pass. A: cmd_voicefetch is UART-transport-gated (:1383-1386) and 'streams synchronously on cmd_exec_task' per its own comment (:1432-1433) — a fully traced ordinary task. B: sReply is never handed to a driver; the audio bytes stream from a separate ps_alloc'd buffer (:1443), and the reply text goes through uartLinkWriteFrameForSession which frames into the UART driver (no DMA on classic-ESP32 UART; driver copies). C: zero-init .bss, first touch on first voicefetch. D: written at :1476-1477 with no lock held. E: no panic path. F: contents are byte count, frame count, and CRC16 of a voice recording (:1476) — the CRC of user speech audio is not extractable auth material. One write per voicefetch (a multi-second bulk transfer) — cold. Caveat as with the other locals: must be hoisted to file scope for EXT_RAM_BSS_ATTR.

- Init: zero-init .bss
- Contexts: single context: cmd_voicefetch on cmd_exec_task (comment :1432)
- Downstream: end-of-stream reply the CM5 uses as its transfer-complete + CRC verification signal; per-transfer, no hot path

### sPacketAckQueue (RingPacketAckPending[8]) — 96 B — **SAFE_PSRAM**
Definition: /Users/morgan/esp/hardwareone-idf/components/hardwareone/G2_Ring.cpp:319 (.dram0.bss 0x60, confirmed in ELF)

All six pass, same profile as sActivePacketAck: r1_owner only (dup scan :2002-2008, append :2024-2025, shift-down :2726-2730, reset :2696/:2714), no spinlock, no ISR, no DMA, no cache-off path, contents are 12-byte ack descriptors (metadata, no health data). Zero-init .bss.

- Init: Zero-init .bss.
- Contexts: r1_owner task exclusively; companion counter sPacketAckCount (uint8_t, :320) stays in DRAM so the every-lap emptiness checks (:3216/:3247) stay internal.
- Downstream: 96 B is marginal on its own — take it only as part of the same one-commit batch with sSetupOwner/sActivePacketAck/sRxFingerprints (adjacent declarations, identical ownership); as a standalone change it would not be worth a flash cycle. Latency: shift-down of at most 8×12 B per serviced ACK, only during health-daily bursts.

### dictDeliver::reply (function-local static) — 96 B — **SAFE_PSRAM**
Definition: components/hardwareone/System_Dictation.cpp:370

All six pass. Written by snprintf at :405-406 AFTER the critical section closes at :399 — content is only "OK: dictation delivered (N chars)", a byte count, never the transcript itself (the transcript goes into gDict.text under the mux, a separate symbol ruled UNSAFE above). A: same single context as cmd_dictate — command-executor task, UART transport only. B/C/D/E: no driver hand-off, zero-init .bss, no lock held at the write, no panic path. F: no secret can reach it — the format string is fixed and takes only strlen(text). One access per completed dictation — as cold as code gets. Same syntactic caveat: hoist to file scope to attach EXT_RAM_BSS_ATTR. Honestly, 96 B is near the noise floor of worth-doing.

- Init: zero-init .bss
- Contexts: dictDeliver called only from cmd_dictate `result` branch (:453) on the command-executor task
- Downstream: command reply to the CM5 host; per-dictation, no latency concern

### systemLogSettingEntries — 336 B — **CONST_TO_FLASH**
Definition: components/hardwareone/System_Debug.cpp:2660

Already declared `static const` and has zero writers (only refs in the tree: definition :2660 and the module descriptor :2673-2674) — it sits in .data only because one initializer is not a compile-time constant: the systemLogFlags row's `options` field is kSystemLogFlagsBitmaskOptions (System_Debug.cpp:2665-2666), a `static const char*` filled at C++ dynamic-init from gSystemLogFlagsBitmaskOptions.c_str() (:2657-2658), a String built at static-init by buildSystemLogFlagsBitmaskOptions() (:2641-2652) from the DBG_FLAG_LIST X-macro. That one field drags the whole 336 B array into runtime-initialized .data. What needs const: replace that initializer with a link-time constant — either (a) generate the "bitmask:0x1|Label,..." string at build time (the repo already has X-macro codegen precedent; bit -> hex text is what blocks doing it in the preprocessor directly), or (b) store a sentinel and have the web settings renderer resolve systemLogFlags options dynamically. Either way the array's other fields are already constant expressions (&gSettings.* member addresses are link-time constants, System_Settings.h:1358-1379 fields are PODs/pointers). Bonus: the String gSystemLogFlagsBitmaskOptions (~5 KB reserve, :2643) and its heap copy can then be dropped too — a far bigger win than the 336 B. Do NOT use EXT_RAM_BSS_ATTR here: const-to-.rodata is strictly better (zero RAM).

- Init: .data via C++ dynamic initializer (one runtime pointer field); after the fix: .rodata, zero RAM
- Contexts: read-only consumers: settings framework iterating systemLogSettingsModule (registered module table) — settings load/persist, CLI per-setting commands, web Settings renderer; all task context, cold paths
- Downstream: settings load/save and the web BitmaskField grid; .rodata reads through flash cache on cold paths — no latency concern, no consumer breaks

### sActiveTransaction (RingActiveTransaction) — 332 B — **RESTRUCTURE**
Definition: /Users/morgan/esp/hardwareone-idf/components/hardwareone/G2_Ring.cpp:317 (.dram0.data 0x14c, confirmed in ELF)

Task-context checks all pass: every access is on the r1_owner task's call tree (ringCompleteActive :1944-1972, ringHandleActiveRx :2134-2182, payload handler :2228-2233, Ring1Error path :2442-2447, ringBeginOwnedGeneration :2694, ringDropOwnedGeneration :2704, ringServiceNormalTransaction :2983-3060 — all reached only from ringOwnerTask :3192-3273). Crucially it is NOT touched inside any critical section — ringCompleteActive copies intent BEFORE portENTER_CRITICAL (:1945 vs :1948) and resets the struct AFTER portEXIT (:1972). No ISR, no DMA (frame.bytes go to writeValue :2887→796-819; Bluedroid copies), no secrets/health (holds only OUTBOUND frames; RX payloads live in sRxSlabs/R1Decoded; even raw-SET payload bytes already transit PSRAM today via sPendingRawSet :4675 and sRawPayloadBytes :281-282). The sole blocker is the non-zero .data initializer from RingIntent's NSDMIs (payloadSlot=0xFF, timeoutMs=5000): .ext_ram.bss is zeroed, so the boot image would lose those defaults. All readers gate on valid==false and every activation fully reassigns (:2993-2996 `sActiveTransaction = RingActiveTransaction{}` then field init), so the zeroed image is dead in practice — but zeroed payloadSlot=0 aliases a valid slot, a latent trap. Restructure: make the zero image the documented idle state (drop/adjust the NSDMIs for the static or re-init explicitly in ringBeginOwnedGeneration, which already assigns RingActiveTransaction{} at :2694), then EXT_RAM_BSS_ATTR is safe.

- Init: NON-ZERO .data (RingIntent NSDMIs payloadSlot=0xFF, timeoutMs=5000 embedded in the intent member). Must be restructured to a zero-equivalent boot image before the attribute can be applied.
- Contexts: r1_owner task exclusively (created at :3997, well after PSRAM init). Writers/readers enumerated above; no other task reads it (external status queries go through sControlStatus/sTransactionHistory under sTransportMux instead).
- Downstream: Touched per RX frame and per 20 ms owner-loop lap (:3272). The same loop already reads whole frames from PSRAM sRxSlabs (:3236-3240), so the added cache misses are in-family; no consumer breaks. Benefit after restructure: 332 B.

### gBlePeerData — 288 B — **RESTRUCTURE**
Definition: components/hardwareone/BLE_Peers.cpp:43

One disqualifier-D access, trivially fixable: in peerConfigApply, gBlePeerData[kind].autoReconnect is READ inside portENTER_CRITICAL(&sReconnectMux) — BLE_Peers.cpp:200 ('state.autoReconnect != data.autoReconnect'), :201 (assignment source), :205 ('!data.autoReconnect') within the critical section spanning 177-231. Every other access is under the PeerDataGuard recursive mutex in task context: peerConfigApply String writes at 159-176 and rollback at 185-187 (rollback happens AFTER portEXIT_CRITICAL at 184), blePeerCommitLearnedTargetIfCurrent (819-846 — String rollback at 845-846 is likewise after the exit at 844), and settings JSON read/write (1555-1557, 1593-1599) under PeerDataGuard. REQUIRED CHANGE: hoist 'const bool cfgAuto = data.autoReconnect;' before portENTER_CRITICAL at line 177 and use the local at 200/201/205. After that the move is clean.

- Init: Zero-state ('= {}', nm 'b' .bss) but the struct holds two Arduino Strings, so it has a dynamic (global-ctor) initializer. On this ESP-IDF build, esp_psram init and .ext_ram.bss zeroing precede do_global_ctors, so the ctor writes land after PSRAM is up — acceptable, but worth a boot-test since it would be a ctor-bearing EXT_RAM_BSS object.
- Contexts: Writers: peerConfigApply (settings/pairing changes, CLI/web/settings-load tasks), blePeerCommitLearnedTargetIfCurrent (scan-learned targets, BLE coordinator task), blePeersReadJson (settings load at boot, app_main-era task). Readers: blePeersWriteJson (settings persist), the disqualified spinlock reads at 200-205. No ISR, no DMA, no flash-cache-off. Comment at 47-50 states the Strings are only the settings-file mirror; runtime scheduling uses the fixed sReconnect mirror.
- Downstream: MAC text (17 chars, exceeds String SSO so heap-allocated regardless of where the object lives) is broadcast over the air — not a secret; autoReconnect is a bool policy. All consumers are rare paths (pairing, settings I/O), no hot path. Payoff is modest: 288 B is mostly String object headers; the character data is already on the heap.

### sTargetLoginThrottle — 228 B — **RESTRUCTURE**
Definition: components/hardwareone/System_Utils.cpp:5797

Blocked today by disqualifier (4): non-zero .data initializer. TargetLoginThrottle has default member init `source = SOURCE_INTERNAL` (System_Utils.cpp:5792) and SOURCE_INTERNAL = 2 (System_User.h:33), so the 3-element array carries a non-zero image — EXT_RAM_BSS_ATTR would silently zero it. The restructure is trivial and behaviorally equivalent: drop the non-zero default (or reset slots explicitly), because targetLoginThrottleFor() (5799-5809) already resets any slot whose source/user mismatch the caller; the only caller that a zeroed slot 'matches' (SOURCE_WEB=0 with empty user) sees failures=0/lockedUntilMs=0 — identical to a freshly reset slot. After that change all six disqualifiers pass: accesses only from cmd_login (5884 Allows, 5900 Record) on the serialized command-exec path, rare by definition (human login attempts); no ISR/DMA/spinlock/panic access; holds usernames + fail counters, not credentials — no password ever stored (5806 copies caller.user only). Two honest caveats: (a) this is brute-force-lockout state, and external PSRAM is physically writable by a bus interposer — but with flash encryption OFF that attacker already reads password hashes from flash, so no net loss; (b) it is 228 B — the bytes barely justify touching auth code.

- Init: currently .data (non-zero: source=SOURCE_INTERNAL=2 per slot); must become zero-init — equivalence argued above
- Contexts: targetedLoginThrottleAllows (System_Utils.cpp:5884) and targetedLoginThrottleRecord (5900), both inside cmd_login only — command-executor task context, no other reader/writer in the tree
- Downstream: only the targeted-login throttle path consumes it; latency irrelevant (per human login attempt). State never needed to survive reboot — .ext_ram.bss zeroing at boot matches current DRAM lifetime.

### gOledPairingRibbon — 156 B — **RESTRUCTURE**
Definition: components/hardwareone/OLED_UI.cpp:493 (struct OLED_UI.h:191)

Blocked only by check (4): it sits in .data (nm 'D') because of a non-zero aggregate initializer {..., 3000 /*visibleDurationMs*/, -20 /*animY*/, ...}. Both non-zero fields are DEAD values: oledPairingRibbonShow (OLED_UI.cpp:554-582) rewrites every field (message, icon, visibleDurationMs, iconBlink, blinkCount, state, stateStartMs, animY, scrollOffset, lastScrollMs) before the ribbon leaves HIDDEN, and update/render early-return while HIDDEN (OLED_UI.cpp:601-604, :786). HIDDEN==0 and LINK==0 (OLED_UI.h:172, :180), so zero-init is a valid hidden state. Fix: delete the initializer (plain `OledPairingRibbon gOledPairingRibbon;`), verify it moves to .bss, then add EXT_RAM_BSS_ATTR. All six disqualifiers then pass — and the identical caller set already writes the EXT_RAM_BSS_ATTR sBannerQueue (OLED_UI.cpp:521) whenever the ribbon is busy, so PSRAM writes from these exact contexts are established precedent in this file.

- Init: non-zero .data initializer (3000, -20) — must be dropped; both values proven dead (rewritten by Show before first visible use)
- Contexts: Writers: main-loop render pump (oledPairingRibbonUpdate/Render via oledUIRender, OLED_Utils.cpp:4206); oledNotificationBannerShow (OLED_UI.cpp:732-744) called from cmd_exec (System_ESPNow.cpp:15844 espnowremote handler), ESP-SR task (System_ESPSR.cpp:1108/:1189), notifications pump (System_Notifications.cpp:914), radio sensor path (i2csensor_rda5807.cpp:736). No ISR/esp_timer/IRAM_ATTR in OLED_UI.cpp or OLED_Utils.cpp; no spinlocks; no DMA; message text is status/command-name strings, no credentials.
- Downstream: Readers: per-frame ribbon animation (~10 Hz scroll of a 128 B message) on the main loop — PSRAM read cost negligible. No consumer breaks.

### sPeerPath — 128 B — **RESTRUCTURE**
Definition: components/hardwareone/OLED_Mode_FileBrowser.cpp:378

Blocked only by check (4): `static char sPeerPath[128] = "/"` has a non-zero initializer -> .data (nm 'd' 0x80). The initializer is redundant: every entry into the PEER browsing source resets it (`strlcpy(sPeerPath, "/", ...)` at :704 under the sSourceChangePending entry gate), and peerNavigateTo already normalizes empty->"/" (:513 `if (sPeerPath[0]=='\0') strlcpy(sPeerPath, "/", ...)`). Fix: drop the `= "/"` initializer (zero-init), then EXT_RAM_BSS_ATTR. All six disqualifiers pass afterwards: main-loop-only access (navigation :508-545, entry activation :559-565, render :982, reset :704); the espnow_task reply callback (onPeerListReply :416) never reads it; fsListSendRequest strlcpy's the path into a stack-local V4 payload before the synchronous sendAeadSync (System_ESPNow_FsList.cpp:200-205) — no pointer retention, no DMA; no secrets (an FS path on a paired peer); no spinlocks; no pre-init access.

- Init: non-zero .data init ("/") — must be dropped; runtime reset at :704 plus the empty-string guard at :513 already cover it
- Contexts: Main loop task only: peerNavigateTo/:508, peerNavigateUp/:535, entry join :559-562, requestPeerList send :498, render :982, peer-mode entry reset :704. RX-side callback writes only sPeerEntries/sPeerStatus (and sPeerEntries is ALREADY PSRAM via ps_alloc :465).
- Downstream: Read per-frame in the peer-browser breadcrumb render (:982) and on navigation — negligible PSRAM cost. 128 B, marginal but free once the initializer is dropped.

### Wire / Wire1 — 200 B — **NOT_WORTH_IT**
Definition: components/arduino/libraries/Wire/src/Wire.cpp:653 (Wire) and :656 (Wire1), 100 B each, nm 'B' .bss

Technically SAFE_PSRAM on the merits, but 200 B does not justify a standalone vendored patch in a third file. The ctor-ordering question resolves cleanly: .ext_ram.bss zeroing happens in esp_psram_bss_init() (esp-idf/components/esp_psram/system_layer/esp_psram.c:618-624), called from call_start_cpu0 at esp_system/port/cpu_start.c:729, while global C++ ctors run later in start_cpu0_default via do_global_ctors (esp_system/startup.c:207) - so a C++ object with a runtime constructor CAN live in EXT_RAM_BSS on this IDF (5.5.1): its static image is all-zero (.bss today, nm 'B'), zeroing precedes the ctor, and the ctor's non-zero writes (num, sda=-1, _timeOutMillis=50, bufferSize=128; Wire.cpp:41-54) land after PSRAM is mapped. ISR check: the only ISR-adjacent path is I2C slave mode, whose ISR touches _i2c_bus_array (not the TwoWire object) and whose user callbacks dispatch from a task - and the firmware never uses slave mode (zero slave-begin callers; all 40+ uses are master-mode via the i2cDeviceTransaction mutex wrapper in System_I2C.h, task context). rx/txBuffer are heap pointers unaffected by moving the object. Verdict driver: 200 B for a marked hunk in Wire.cpp (which currently carries no local patches), plus patch refresh and verify_patches.sh bumps - fold it in only if the i2c-ng.c bus[] patch is being made anyway; standalone, skip.

- Init: Zero static image (.bss, nm 'B'); non-zero values written by the runtime ctor at do_global_ctors - AFTER esp_psram_bss_init zeroing (verified ordering: cpu_start.c:729 vs startup.c:207), so EXT_RAM_BSS placement is legal for ctor'd C++ globals
- Contexts: Startup: ctor at do_global_ctors (post-PSRAM-init). Task: all master-mode calls via i2cDeviceTransaction (System_I2C.h:57) from ~17 sensor drivers and HAL_Display, serialized by the firmware mutex plus TwoWire's own lock. None: slave-mode ISR/callback paths compiled in but unreachable (no slave begin callers in firmware).
- Downstream: Consumers: i2csensor_*.cpp drivers, HAL_Display OLED, System_I2C manager - Wire1 is the primary sensor bus (bus 0) per System_I2C.h:42. Object field reads per transaction; latency impact negligible vs bus time. No consumer breaks.

### sReason — 128 B — **NOT_WORTH_IT**
Definition: components/hardwareone/System_CrashRecord.cpp:74

The anticipated disqualifier E does NOT hit sReason, and the evidence matters: the panic-context writer is crashPanicHook (:131-179), which writes rtcPanicReason — a separate RTC_NOINIT_ATTR symbol (:50, written via IRAM_ATTR crashCopyStr at :169) that cannot and must not move. sReason is only the boot-time DECODED copy: written solely in crashRecordBootConsume (:245, else-cleared :256), which is called from the top of setup() (HardwareOne.cpp:1398) on loopTask — after PSRAM init and after .ext_ram.bss zeroing, so C passes too. Readers (crashRecordEmitEarly :318 via esp_rom_printf pre-Serial, crashRecordSummary :357, crashRecordDetail :386, crashRecordReasonText :344 consumed at System_Utils.cpp:2743/2762) all run in normal task context with caches on. F: assert/panic text, not secrets. So all six pass mechanically — and I still refuse. This is the diagnostics-of-last-resort module: its decode and pre-Serial emit exist precisely to survive setup()-phase boot loops (HardwareOne.cpp:1409-1414). Failing PSRAM on this board manifests EXACTLY as the random crashes this module reports; putting the decoded reason in PSRAM makes the crash reporter read through the component under suspicion, converting a diagnosable loop into a deeper one. All accesses are once-per-boot or per-command, so there is zero latency benefit to weigh against that — 128 B does not buy the risk.

- Init: zero-init .bss ({0} at :74)
- Contexts: writer: crashRecordBootConsume on loopTask at setup() top (HardwareOne.cpp:1398); readers: crashRecordEmitEarly (same, pre-Serial), summary/detail/reason accessors from boot logging (HardwareOne.cpp:1586,1596) and diagnostics commands (System_Utils.cpp:2743,2762)
- Downstream: boot-time crash banner, crash-history persist, and the crash JSON/detail commands; all cold. Note for the sweep: the genuinely panic-written state (rtcPanic*) is RTC memory and out of scope by construction

### _uart_bus_array — 108 B — **NOT_WORTH_IT**
Definition: components/arduino/cores/esp32/esp32-hal-uart.c:103

Hard-blocked by the non-zero-initializer test (disqualifier 4) and the fix is not worth 108 B in a vendored file. Initialized '{NULL, 0, ...}, {NULL, 1, ...}, {NULL, 2, ...}' with num=0/1/2 and all pins = -1 (lines 103-120); nm confirms section 'd' (.data). EXT_RAM_BSS_ATTR would silently zero it: every num field becomes 0 (UART1/UART2 would drive the UART0 IDF driver - uartBegin at :501 and write paths index the IDF API by uart->num) and the -1 unassigned-pin sentinels become 0 (a valid GPIO), breaking detach bookkeeping at :621-:625. RESTRUCTURE would mean a runtime-init function patched into the vendored file plus a call-before-first-use guarantee - multi-hunk patch with ordering risk, for 108 B. It is also on a warm path: log_printf takes _uart_bus_array[s_uart_debug_nr].lock on EVERY core debug log line (:1457-:1468), and this classic-ESP32 board uses UART0 as its serial console, so a PSRAM move would tax every log line. Leave it.

- Init: NON-ZERO .data initializer (nm 'd'): uart nums 0..2 and -1 pin sentinels baked into the image - EXT_RAM_BSS zeroing would corrupt both
- Contexts: Task: uartBegin/uartEnd/uartWrite/uartRead under UART_MUTEX_LOCK (xSemaphoreTake macro :95-101 proves task context). Task: log_printf per log line takes the debug UART's lock (:1457). No ISR access: the IDF uart driver ISR uses its own driver state; the Arduino uart_t is task-side bookkeeping only.
- Downstream: Consumers: HardwareSerial (Serial console on UART0 for this board) and log_printf on every debug line. A PSRAM move would add cache-miss latency to every log line, and the zeroed initializer would misroute UART1/2 onto the UART0 driver. Nobody benefits from 108 B.

### cmd_blesecret::buf (function-local static) — 96 B — **NOT_WORTH_IT**
Definition: components/hardwareone/Bluetooth.cpp:2182

The expected F hit does NOT materialize on the evidence, and I verified it adversarially: the only write is snprintf(buf, ..., "Error: passphrase must %s (...)", need) at :2183, where `need` comes from blePassphrasePolicyError (:2149-2161) which returns ONLY fixed string literals ('be at least 10 characters', 'include an uppercase letter', ...). The passphrase itself lives in the stack String `a` (:2165) and in gSettings.bleSecureChannelSecret (:2186) — it never touches buf, not even via String SSO (buf is a char array written by one fixed-format snprintf). Context is fine too: cmd_blesecret is a super-admin command (:2669) on cmd_exec, BLE transport refused (:2173-2175). So mechanically this would be SAFE_PSRAM — but I refuse it on cost/benefit: 96 B of savings for planting a PSRAM buffer in the middle of the secure-channel provisioning function, where any future edit that echoes the rejected passphrase into the error message (a very natural UX change) would silently violate the no-secrets-in-PSRAM policy with no compiler to catch it. The bytes do not pay for that standing hazard.

- Init: zero-init .bss
- Contexts: cmd_blesecret only, cmd_exec task, super-admin, non-BLE transports only (:2173)
- Downstream: policy-error reply text; per-command. No consumer would break — the refusal is hazard-proximity, not mechanics

### gMicRecordingControl — 608 B — **UNSAFE**
Definition: components/hardwareone/System_Microphone.cpp:137

Disqualifier D, compounded by A. D: every read/write is under the gMicRecordingMux spinlock (portENTER_CRITICAL), and the file's own comment at System_Microphone.cpp:352-354 says hold time is deliberately minimized 'because status and source-loss callbacks read/write the same control object on both cores' — moving the 608 B object to PSRAM turns each ~100 ns critical section into multi-microsecond PSRAM cache-miss windows with interrupts masked, contended cross-core. A: one writer runs on the Bluedroid BLE host task (G2_Glasses.cpp:3198-3227), whose comment explicitly warns that stalling there stalls the BLE stack; a PSRAM miss inside a spinlock on that task is exactly the stall being avoided. Not RESTRUCTURE-worthy: the spinlock cannot become a mutex because the BLE-host caller must be non-blocking (G2_Glasses.cpp:3211 'BLE host context cannot take the drain mutex'), and deferring the source-lost publication reopens the ordering race documented at System_Microphone.cpp:700-704 (SOURCE_LOST must be recorded before gMicRunning drops). Not CONST_TO_FLASH: mutated on every capture. F note: shadowAuth is LiveAudioRecorderAuthorization = bools + controller/exchange IDs + epoch (System_LiveAudio.h:54-60), and results[].path holds WAV file paths — no credentials/health payload, so F alone would pass; D+A refuse it regardless.

- Init: Zero-init in effect: initializer at lines 137-149 is all-zero values (MicRecordingState::IDLE=0 per System_Microphone.h:18, MIC_RECORDING_OWNER_MANUAL=0 per System_Microphone.h:30, rest false/{}/0), so the object already lands in .bss. Init is not the blocker.
- Contexts: ALL 18 access sites are inside portENTER_CRITICAL(&gMicRecordingMux) spinlock sections: getMicRecordingState (169-171), micRecordingStopRequested (195-198), micRecordingSourceLost (202-204), micRecordingClaimStart (227-240), micRecordingRequestStop (249-269), micRecordingRequestStopOwned (285-313), micRecordingEnterCapturing (321-329), micRecordingEnterFinalizing (334-339), micRecordingPublishIdle (368-385), micRecordingLastResult (390-397), getRecordingResultOwned (410-431), micRecordingSealDisposition (447-453), micRecordingShadowSnapshot (464-467), micRecordingDiscardCompleted (1792-1805). Caller tasks: recorder task (recordingTask, per-chunk poll at 1000-1001), cmd_exec_task command paths, EvenAI live-audio path, AND the BLE host (BTC) task: G2_Glasses.cpp:3203 g2MicOnLeftDisconnect ('Runs in the BLE host task context' per its comment at 3198-3200) calls microphoneNotifySourceLost (G2_Glasses.cpp:3227) -> micRecordingRequestStop(sourceLost=true) (System_Microphone.cpp:704) -> spinlocked writes at 249-269.
- Downstream: recordingTask polls micRecordingCapturing()/micRecordingStopRequested() every audio chunk (~128 ms, System_Microphone.cpp:1000-1001); micrecord status/stop commands and EvenAI stopid/discard/result queries read it; the G2 disconnect path writes it from the BLE host task. No consumer would functionally break, but every one of these would pay PSRAM-miss latency inside interrupt-masked windows on both cores, and the BLE host task would absorb that latency during disconnect handling. 608 B of DRAM is not worth degrading the sync primitive the module was designed around.

### gSc (ScConn[BLE_MAX_CONNECTIONS]) — 512 B — **UNSAFE**
Definition: /Users/morgan/esp/hardwareone-idf/components/hardwareone/System_BleSecureChannel.cpp:71 (NOT G2_Ring.cpp — ELF symbol _ZL3gSc, .dram0.bss 0x200)

Disqualifier F, categorical. gSc is the BLE secure-channel per-connection table; each ScConn holds ephSec[32] (X25519 ephemeral secret, System_BleSecureChannel.cpp:64), kC2D[32] and kD2C[32] (live ChaCha20-Poly1305 session keys, :65-66). Keys are written during the handshake (:354, :363) and read on every encrypted frame in both directions (:238 scBuildData, :380/:409 decrypt). Repo policy is explicit: flash encryption is OFF and PSRAM is a probeable external chip — no secret may ever reside there, even transiently. bleScReset (:209) sodium_memzero's the struct on disconnect, which itself proves these bytes are treated as secret residue. No other disqualifier even needs testing.

- Init: Zero-init, .dram0.bss — the zeroing requirement is satisfied, but irrelevant given F.
- Contexts: Writers: cmd_exec_task via bleScDeferredInbound → bleScHandleInbound (Bluetooth.cpp:1136-1160 — handshake crypto is deliberately deferred OFF BTC_TASK); readers/writers: cmd_exec_task and debug_out task via bleScSendEncrypted (System_BleSecureChannel.cpp:267-323, serialized by gScTxMutex per the comment at :76-79); bleScReset on disconnect (:207). No ISR, no spinlock, no DMA target (bleRawNotify → Bluedroid copies).
- Downstream: Every encrypted CLI reply, console mirror, and BLE-OTA frame does AEAD with keys read from this table (per-BLE-notify hot path), so a PSRAM move would also add cache-miss latency to every secure notify — but the secrets policy alone is the refusal. Note the ciphertext staging buffer (BleScDeferred, Bluetooth.cpp ps_alloc PreferPSRAM 'ble.sc.rx') is already in PSRAM by design; that is fine because it is ciphertext — the keys must stay internal.

### sReconnect — 512 B — **UNSAFE**
Definition: components/hardwareone/BLE_Peers.cpp:96

Disqualifier D, definitive and pervasive: sReconnect exists solely to be accessed inside portENTER_CRITICAL(&sReconnectMux) — that is its documented design (comment at 115-117: 'callback/main-loop scheduling never reads them concurrently after this publication' via the spinlock-published fixed mirror). Locked access sites: peerConfigApply 177-231 (reads+writes the whole PeerReconnectRuntime including the 100+ byte savedTarget struct), reconnectPublishOwnerAuthority 243-260, scheduler snapshot reads 399-412, 418-420, 428-432, 444-447, intent writes 457-482, blePeerCommitLearnedTargetIfCurrent 838-877 (savedTarget struct assignment at 863 under the lock). The BLE reconnect scheduler polls these under the spinlock from the coordinator loop — recurring PSRAM cache misses with interrupts masked. Restructuring would mean redesigning the publish/snapshot pattern around a mutex, defeating its purpose (it exists to be cheap enough for frequent scheduler polls).

- Init: PeerReconnectRuntime default member inits are all zero/false, .bss (nm 'b') — moot given D.
- Contexts: Every reader and writer holds sReconnectMux (spinlock): settings/pairing writers on cmd_exec/settings tasks, the reconnect scheduler on the BLE coordinator loop, owner-authority publication. No access exists OUTSIDE a critical section.
- Downstream: The reconnect scheduler reads it per scheduling pass; moving it to PSRAM converts every pass into potential cache-miss stalls under masked interrupts on both the scheduler and any writer racing it. Refuse.

### sIntentQueue (RingIntent[12]) — 432 B — **UNSAFE**
Definition: /Users/morgan/esp/hardwareone-idf/components/hardwareone/G2_Ring.cpp:273 (.dram0.data 0x1b0, confirmed in ELF)

Two independent disqualifiers. (D) Every single access is inside portENTER_CRITICAL(&sTransportMux): ringEnqueueIntent coalesce-scan + entry copy (G2_Ring.cpp:659-693), ringPopIntent 36-byte struct copy under the lock (:699-708), and ringMarkGenerationDisconnected's full-queue compaction loop under the lock (:714-755). The file's own design comment (:276-279) states the invariant 'No PSRAM byte is touched while sTransportMux is held'. (4/init) It sits in .dram0.data because RingIntent has non-zero NSDMIs (payloadSlot=0xFF at :201, timeoutMs=RING_TRANSACTION_TIMEOUT_MS=5000 at :205/:152); .ext_ram.bss is NOLOAD-zeroed, so the initial image would be silently discarded — zeroed payloadSlot=0 is a VALID slot index, a real footgun. Restructuring both (move all accesses out of the spinlock AND kill the NSDMIs) contradicts the module's documented locking design for 432 bytes — refuse.

- Init: NON-ZERO .data initializer: 12 copies of RingIntent defaults {payloadSlot=0xFF, timeoutMs=5000}. Blocks EXT_RAM_BSS_ATTR outright.
- Contexts: Producers on multiple tasks — cmd_exec_task (ringquery/g2RingSubmitRawTransaction :869-975), main-loop health/time-sync ticks, and the r1_owner task itself (history coordinator) — all funneled through ringEnqueueIntent under sTransportMux. Consumer: r1_owner via ringPopIntent (:698) and ringMarkGenerationDisconnected (:712), also under the mux.
- Downstream: Consumed by ringServiceNormalTransaction (:2983+) to build every outbound ring frame. Related pre-existing issue worth flagging to the parent: sTransactionHistory at :283 already carries EXT_RAM_BSS_ATTR yet IS accessed under sTransportMux (ringFindTransactionLocked/ringAllocateTransactionLocked/ringUpdateTransaction :565-618, ringMarkGenerationDisconnected :714-723), violating the :276-279 comment — an existing PSRAM-under-spinlock latency hazard, not introduced by this audit.

### sCatalog — 288 B — **UNSAFE**
Definition: components/hardwareone/System_LLMCm5.cpp:38

Disqualifier D: every access is inside portENTER_CRITICAL(&sMux) where sMux is a portMUX_TYPE spinlock (System_LLMCm5.cpp:31): catalog row writes on the UART drain task at 217-225, a memcpy of up to the FULL 288 B array under the spinlock in the model-list snapshot at 390-395 ('memcpy(snap, sCatalog, sizeof(Cm5Model) * n)'), and the strcasecmp lookup loop under the lock at 419-425. Whole-array copies and string compares in PSRAM with interrupts masked is the refusal pattern. Decisive corroboration: this file already made the DRAM/PSRAM split deliberately — the author's comment at lines 26-30 says the 2 KB sResult went to PSRAM via EXT_RAM_BSS_ATTR (line 51) precisely because it is NOT under the critical sections, while 'everything else is small enough to stay in DRAM where the critical sections are cheap'. Moving sCatalog would reverse an explicit, correct design decision. A restructure (convert sMux to a FreeRTOS mutex, or double-buffer the catalog) is possible but touches every field the spinlock guards (sActiveName, sSelected, sHostReady, etc.) for 288 B — not justified.

- Init: Zero .bss (nm 'b', anonymous namespace) — moot given D.
- Contexts: Writer: cm5 llm model push handler on the UART drain task (217-225, file comment at 25-27). Readers: model-list snapshot (390-395) and find-by-name (419-425) polled from OLED/web/BLE surface tasks. All under the sMux spinlock.
- Downstream: Model picker surfaces (OLED/web) snapshot it on open; the UART drain task writes rows during catalog push. Both would stall under masked interrupts on PSRAM misses; the UART drain task is latency-sensitive (it also carries live STT/LLM streams). Refuse.

### gMeshDerivedKeys — 272 B — **UNSAFE**
Definition: components/hardwareone/System_ESPNow_MeshKeys.cpp:18

Disqualifier F (secrets) — categorical refusal. Located: anonymous namespace, System_ESPNow_MeshKeys.cpp:18, 'MeshDerivedKeys gMeshDerivedKeys[Settings::N_MESHES] = {}' (4 slots x 68 B). The struct IS key material: System_ESPNow_MeshKeys.h:45-46 — 'uint8_t bootstrapKey[32]; // KDF subkey for KEY_EX HMAC' and 'uint8_t groupKey[32]; // KDF subkey for mesh broadcast AEAD'. These are the live derived mesh keys (KDF subkeys of the PBKDF2-stretched passphrase, MeshKeys.cpp:96-100). With flash encryption OFF and PSRAM being a probeable external chip, moving live AEAD/HMAC keys off-die is exactly what the policy forbids — the code even practices key hygiene already (sodium_memzero in meshKeysInvalidate, MeshKeys.cpp:129-131). Secondary reasons that would each also disqualify: meshKeysGet/meshKeysFindByFingerprint (135-150) return const POINTERS into the array that feed per-frame AEAD encrypt/decrypt on espnow_task — a hot per-packet path; and the file's own comment (line 17) already settled the tradeoff: 'Tiny (~70 B x 4 = 280 B) so just static BSS.'

- Init: Zero .bss — irrelevant; secrets refusal is terminal.
- Contexts: Writers: meshKeysDerive/meshKeysInitAll (boot + mesh config changes, task context) and meshKeysInvalidate (sodium_memzero). Readers: meshKeysGet + meshKeysFindByFingerprint consumed by the ESP-NOW v4 crypto layer on espnow_task for every encrypted mesh frame.
- Downstream: Every AEAD-protected ESP-NOW frame derives its protection from these 64 bytes per mesh. Moving them to PSRAM = physically probeable master material for the whole mesh auth/RCE channel, plus per-frame PSRAM reads on the RX drain path. NEVER move.

### sWizard — 240 B — **UNSAFE**
Definition: components/hardwareone/System_SetupWizardMode.cpp:108 (NOT System_SetupWizard.cpp — anonymous struct { WizardSubMode; SetupWizardResult result; ...; String wifiPendingSsid; })

Disqualifier F (secrets). sWizard.result is a SetupWizardResult carrying String wifiPassword and String mqttPassword (System_SetupWizard.h:37, :50), and the CLI wizard mode writes real credentials into it: `sWizard.result.wifiPassword = raw` at System_SetupWizardMode.cpp:738 (WIFI_PASSWORD sub-mode) and `sWizard.result.mqttPassword = raw` at :660. Arduino String SSO stores short strings INLINE in the String object — a typical <=~14-char WiFi/MQTT password would physically reside inside sWizard's 240 bytes, i.e. in PSRAM if moved. The credentials persist in the struct at least until wizard finalize copies them onward (:615 gSettings.mqttPassword, :831 upsertWiFiNetwork(sWizard.result.wifiSSID, sWizard.result.wifiPassword,...)), and there is no secure wipe of sWizard afterwards. Flash encryption OFF + probeable PSRAM chip -> refusal per repo policy, exactly like gSettings. Longer passwords heap-allocate, but the SSO case alone is disqualifying. Other checks would pass (ordinary CLI-mode task context, zero-init .bss nm 'b' 0xf0) — F is decisive. A restructure (splitting credentials out into an internal-DRAM sub-struct) is possible but not worth it for 240 B.

- Init: zero-init .bss; String ctors post-PSRAM-init — moot given F
- Contexts: CLI wizard mode handler (cmd_exec / console command path — CLI_MODE_HANDLED returns at :660/:738) and wizard OLED input dispatch (:903-909, main loop). No ISR/spinlock/DMA — but the password residue stands regardless of context.
- Downstream: wizardFinalize/apply paths read result to write gSettings and the WiFi store (:615, :831). Keep in internal DRAM; if DRAM pressure demands it, restructure by moving ONLY non-credential members (subMode, counters, flags) out, leaving the SetupWizardResult internal.

### gOledKeyboardState — 224 B — **UNSAFE**
Definition: components/hardwareone/OLED_Utils.cpp:1570 (struct at OLED_Utils.h:266)

Disqualifier F (secrets). The keyboard's char text[65] buffer is the live entry buffer for credentials: OLED login password (OLED_Mode_Auth.cpp:230 oledKeyboardInit("Enter Password:"...), :153-157 oledKeyboardGetText() -> passwordBuffer), change-password current/new/confirm (OLED_Mode_ChangePassword.cpp), and the first-time-setup WiFi password via getOLEDTextInput (OLED_FirstTimeSetup.cpp:91 oledKeyboardInit(prompt,...) called from OLED_SetupWizard.cpp:437 result.wifiPassword = getOLEDTextInput("WiFi Password:", true, ...)). The module itself treats the state as sensitive (secureClearString(gOledKeyboardState.title) at OLED_Utils.cpp:1615). Flash encryption is OFF and PSRAM is externally probeable; even transient residue during typing is a refusal per repo policy. All other disqualifiers pass (main-loop only, zero-init .bss confirmed by nm 'B' 0xe0), but F alone is decisive.

- Init: zero-init .bss (nm 'B'); String title member's ctor runs at static-init after PSRAM init — would be fine if not for F
- Contexts: Writers/readers all on the main loop task: oledKeyboardInit/Reset/HandleInput/Display (OLED_Utils.cpp:1570-1700+), called from OLED mode input handlers (OLED_Mode_Auth.cpp, OLED_Mode_ChangePassword.cpp, OLED_FirstTimeSetup.cpp:91, OLED_SettingsEditor.cpp, OLED_ESPNow.cpp, etc.). No ISR, no timer callback, no spinlock, no DMA.
- Downstream: Keyboard render reads per frame while active — would be latency-fine; irrelevant given the secrets refusal. Keep in internal DRAM.

### gR — 220 B — **UNSAFE**
Definition: components/hardwareone/G2_Glasses.cpp:409

First, a correction the sweep needs: gR is NOT in System_SensorStubs.cpp — no symbol named gR exists anywhere in that file (verified by grep over the whole component; the stubs file holds only gRtcCache/gRadioInitialized/etc.). The only gR in the tree is `static G2Temple gR` at G2_Glasses.cpp:409 — the RIGHT-temple BLE connection object (client/char pointers, RX reassembly state, conn-params bookkeeping, heartbeat/tx watchdog counters; struct at :222-369). The size report's file attribution is a tooling slip; do not patch the stubs file. Verdict on the real symbol: UNSAFE, two disqualifiers. (A) It is read on the BLE notify callback context on every incoming notification: notifyThunkR (:2483-2486) -> handleNotify (:3910-3916) -> g2RxPacketEnqueue(t,...) (:3740) runs on the Bluedroid callback task (the code itself latches that task handle at :3911), and that path carries heartbeat acks, every protocol envelope, and — when the audio path is up — per-LC3-frame mic notifications; PSRAM misses there stall the BT stack on a link whose audio cadence is already marginal. (D) Inside that same path, t.connectionGeneration and t.side are read UNDER portENTER_CRITICAL(&gRxPacketMux) (:3751-3766) — PSRAM reads with interrupts masked, per notification. Additional hazard: the GAP event handler attributes events via gR.peerBda specifically because templeReset() may be concurrently deleting on the other core (:321-324) — more BT-stack-context access. Nothing about zero-init or secrets even needs reaching; A+D kill it.

- Init: zero-init .bss (no initializer; String members are dynamically constructed but empty)
- Contexts: BLE notify callback task per-notification (:2483->:3910->:3740, generation/side read under gRxPacketMux :3751-3766); Bluedroid GAP callback (peerBda attribution, :321-324); heartbeat path on gBeatTaskHandle (:10063); connect/disconnect lifecycle on worker + user tasks; cross-core concurrent with templeReset
- Downstream: every G2 feature — text/display pushes, hijack FSM, EvenAI, mic audio. A PSRAM miss per BLE notify lands directly on the stack's callback thread during audio streaming; refuse

### gL — 220 B — **UNSAFE**
Definition: components/hardwareone/G2_Glasses.cpp:408 (NOT OLED_Utils.cpp — task attribution was wrong; ELF local .bss symbol 'gL' at 0x3ffc6fb0)

gL is `static G2Temple gL` — the G2 glasses LEFT-temple BLE connection state (client/char pointers, rx reassembly, conn params, counters). Two disqualifiers: (D) read inside portENTER_CRITICAL spinlock sections — G2_Glasses.cpp:464-468 reads gL.connected/connectionGeneration under gTopologyMux; :1200-1245 reads gR/gL generations under gRxPacketMux; :3728-3734 memcpy(out->firmwareLeft, gL.firmwareVersion, ...) under gRxPacketMux — a PSRAM cache miss with interrupts masked on classic ESP32 is exactly the refused pattern. (A/hot-path) written on every BLE notify from the Bluedroid BTC task: notifyThunkL (G2_Glasses.cpp:2479-2482) -> handleNotify(gL,...) does per-notify rx-reassembly writes, including the LC3 mic audio notify path — a documented fragile hot path in this repo (mic half-rate regimes are timing-sensitive). 220 B is not worth either risk.

- Init: zero-init .bss (nm 'b'); String members ctor at static-init — moot given D/A
- Contexts: BTC task (Bluedroid notify callbacks via notifyThunkL:2479, connect/disconnect callbacks ~:2440-2468), main/worker tasks for g2 commands (arm-selection reads :15683/:15772/:16421 etc., 111 references total), and spinlock-guarded readers at :464, :1200, :3728.
- Downstream: Consumed by the whole G2 command/mic/hijack stack; PSRAM would add cache-miss latency per BLE notify and risk deadlock/IRQ-latency inside gRxPacketMux/gTopologyMux critical sections. Keep internal.

### gControlStatus — 200 B — **UNSAFE**
Definition: components/hardwareone/G2_Glasses.cpp:1154

Disqualifier D (spinlock access). Identity confirmed: nm shows _ZL14gControlStatus 0xc8 (200 B, .bss) from G2_Glasses.cpp:1154 'static G2ControlStatus gControlStatus{}'; the G2_Ring.cpp:325 symbol is a DIFFERENT, smaller one (static G2RingControlStatus sControlStatus, _ZL14sControlStatus, 44 B). Every access is inside portENTER_CRITICAL(&gControlStateMux), frequently nested under gRxPacketMux: writes in g2ControlNoteEcho (G2_Glasses.cpp:1200-1231, enters gRxPacketMux at 1198 then gControlStateMux at 1205) and g2ControlNoteAck (1236-1251); generation-reset writes in g2AdvanceGeneration (3706-3715, nested inside both muxes); a FULL 200-byte struct copy '*out = gControlStatus' in g2ControlStatusSnapshot (3729) under both spinlocks; reads in g2ControlReconcileTick (10593-10597) and writes (10801-10806) under gControlStateMux. A PSRAM cache miss — worse, a 200 B struct copy — with interrupts masked on classic ESP32 is exactly the pattern the audit refuses.

- Init: Zero-init ('static G2ControlStatus gControlStatus{}'), .bss confirmed by nm flag 'b' — but moot given D.
- Contexts: Writers: G2 settings-echo/ack parse path (g2ControlNoteEcho/NoteAck, G2_Glasses.cpp:1197-1251 — fires per settings-echo notify from the glasses), g2AdvanceGeneration on reconnect (3698-3720), g2ControlReconcileTick on the G2 control worker (10593, 10801-10806). Readers: g2ControlStatusSnapshot (3722-3738) called by web/OLED status surfaces. All task context, but 100% of accesses are inside portENTER_CRITICAL sections.
- Downstream: Snapshot consumers (web/OLED status pages) would each pay a masked-interrupt PSRAM copy of 200 B; the echo/ack writers sit on the G2 BLE notify parse path (hot per-notify when echoes arrive). Could in principle be restructured by copying to locals outside the critical sections, but the lock-ordering contract (RX-then-control, comment at 3724-3727) makes that a risky invasive rework for 200 B — refuse.

### gDict — 200 B — **UNSAFE**
Definition: components/hardwareone/System_Dictation.cpp:45

Disqualifier D, decisively: essentially EVERY access is inside portENTER_CRITICAL(&gDictMux) (mux at :44). Evidence: dictationReleaseMicIfDue :56-68, dictFail :129-135, dictationBegin :169-179 and :196-202, dictationRequestStop :220-222, dictationCancel :231-241, dictationResetForSessionBoundary :253-255, dictationSnapshotNow :262-266, dictationTick :277-281, dictationTakeText :302-312 (snprintf copying up to 129 B OUT of gDict.text under the mux), dictationOnCaptureClosed :323-329, dictDeliver :374-379 and :391-399 (snprintf writing up to DICTATION_MAX_TEXT=128 B INTO gDict.text under the mux), cmd_dictate fail path :465-467. A PSRAM cache miss inside a portMUX critical section stalls with interrupts masked on that core, and the two snprintf's make it a multi-line burst, not one miss. Contexts span three tasks (OLED display task per :51-53, recorder task per :320-321, cmd_exec for the UART-delivered `dictate result`), so the mux cannot simply be removed. Secondary F concern: gDict.text holds whatever the wearer dictated into an OLED keyboard field (drained at OLED_Utils.cpp:2360-2365); if a text field ever accepts dictation for a passphrase, spoken-secret residue would sit in probeable PSRAM. D alone is disqualifying.

- Init: zero-init in effect (IDLE=0, kNoTransportSessionEpoch=0 per System_Dictation.h:49-54 and System_User.h:62) — lands in .bss, but that does not rescue it
- Contexts: OLED display task (tick/cancel/take), mic recorder task (dictationOnCaptureClosed :317), cmd_exec task for UART `dictate result/fail` — all under the gDictMux spinlock
- Downstream: OLED MIC-keyboard mode; a PSRAM miss under the mux would add masked-interrupt stalls on the display frame path and the recorder's terminal hook

### gEvenAiPendingCancels — 192 B — **UNSAFE**
Definition: components/hardwareone/G2_Glasses.cpp:11060

Disqualifier D (spinlock access). Its dedicated spinlock gEvenAiCancelMux is defined immediately after it (G2_Glasses.cpp:11062) and EVERY access is inside portENTER_CRITICAL(&gEvenAiCancelMux): g2EvenAiQueueCancelRetry scans and writes the whole 4-slot array under the lock (11217-11241, including a 48 B struct assignment at 11239), g2EvenAiCancelRetryPending scans under the lock (11255-11263), g2EvenAiCancelRetryTick does a full-array sweep with struct copies under the lock (11275-11295) plus a second locked section (11304-11312). Full-array PSRAM sweeps under masked interrupts = refusal.

- Init: Zero-init ('...gEvenAiPendingCancels[G2_EVENAI_CANCEL_DEPTH]{}'), .bss (nm 'b') — moot given D.
- Contexts: Writers/readers: EvenAI cancel queue/retry machinery on the G2 worker task (g2EvenAiCancelRetryTick runs per owner lap of the BLE control worker, comment at 11266-11267) and the session-terminate path (g2EvenAiQueueCancelRetry from g2EvenAiTerminate). No ISR/DMA. Contents are exchange ids + retry bookkeeping + a short reason string — no secrets.
- Downstream: The retry tick runs every worker lap while the EvenAI feature is live; putting the array in PSRAM would add cache-miss stalls inside a critical section on that loop. The struct copies could be hoisted, but the whole design is scan-under-lock; not worth restructuring for 192 B — refuse.

### writeSessionCookie::cookieBuf (function-local static) — 160 B — **UNSAFE**
Definition: components/hardwareone/WebServer_Server.cpp:471

Disqualifier F — this is the clearest secrets refusal in the set. The buffer is formatted as "session=%s; Path=/; HttpOnly; ..." with sid.c_str() (:473-476), i.e. the full web session identifier — a bearer token that grants the authenticated session to anyone who replays it. Because the buffer is static (deliberately: httpd_resp_set_hdr stores the pointer and the header is serialized later, per the lifetime comment :455-463), the LAST ISSUED SESSION TOKEN persists in the buffer indefinitely — it is not even transient residue, it is a resident copy that survives until the next login/logout overwrites it. With flash encryption OFF and PSRAM being an externally probeable die, moving this to EXT_RAM_BSS_ATTR would park a live auth token where a logic probe can read it (per the project's own no-secrets-in-PSRAM policy). The logout path (:478-480, empty sid) coincidentally scrubs it, but only when a logout happens. Everything else passes (httpd worker task only, single-task handler model per :460-463; zero-init; no lock; no panic path) — irrelevant, F is absolute.

- Init: zero-init .bss — not the issue
- Contexts: writeSessionCookie on the httpd worker task only (:460-463); called from login (:573), logout (:565, :713), session-expiry (:3126)
- Downstream: httpd_resp_set_hdr holds the pointer until response serialization — moving it also makes the header send read PSRAM, harmless; the refusal is purely the token residue

### _i2c_bus_array — 96 B — **UNSAFE**
Definition: components/arduino/cores/esp32/esp32-hal-i2c-slave.c:124

Two independent hard disqualifiers. Non-zero .data initializer: '{&I2C0, 0, -1, -1, ...}' bakes hardware register-block pointers (&I2C0/&I2C1) and -1 pin sentinels into the image (lines 124-139; nm 'd' confirms .data) - EXT_RAM_BSS zeroing would null the dev pointer, so the first i2c_ll_* call through i2c->dev would dereference NULL. Disqualifier A: i2c_slave_isr_handler (:749, installed via esp_intr_alloc at :385/:392 with ESP_INTR_FLAG_LOWMED|SHARED) receives &_i2c_bus_array[n] as its arg and dereferences the struct from interrupt context throughout - FromISR queue/ringbuffer ops on handles read from the struct at :695, :707, :728, :739, :791, plus direct field reads and portYIELD_FROM_ISR at :824. Mitigating reality: the firmware NEVER initializes I2C slave mode (zero i2cSlaveInit callers in components/hardwareone and main), so the ISR never installs and the array is 96 B of dead .data pulled in via Wire.cpp's SOC_I2C_SUPPORT_SLAVE references. The correct lever is elimination, not relocation - and carving slave support out of vendored Wire.cpp/CMake for 96 B is also not worth the patch burden. Refuse the move.

- Init: NON-ZERO .data initializer (nm 'd'): {&I2C0, 0, -1, -1, ...} - hardware register pointers and pin sentinels; zeroing = NULL dev-pointer crash on first slave use
- Contexts: ISR: i2c_slave_isr_handler (esp32-hal-i2c-slave.c:749, alloc at :385/:392) reads/writes the struct and its queue handles from interrupt context (:695,:707,:728,:739,:791,:824). Task: i2cSlaveInit/i2cSlaveWrite/i2cSlaveDeinit (:239,:274,:913) under struct lock. In practice: unreachable - firmware has zero slave-mode callers.
- Downstream: No firmware consumer exists (slave mode unused). The 96 B is dead weight retained by Wire.cpp's slave API references; recovering it would require patching slave support out of the vendored Wire/CMake - burden far exceeds the bytes.

