# RECON — Interrupt & Callback Context Map

Scope: `/Users/morgan/esp/hardwareone-idf` — first-party `components/hardwareone/` + `main/`,
plus the third-party code actually linked into the image (`components/arduino`,
the 14 dirs listed in `components/hardwareone_libs/CMakeLists.txt`, `managed_components/`).
Target verified from `sdkconfig`: `CONFIG_IDF_TARGET="esp32s3"`, `CONFIG_ARDUINO_VARIANT="um_feathers3"`
(FeatherS3 = the board the checked-in sdkconfig builds).

**This is a map, not a findings list.** Findings marked `⚑` are hazards handed off to
the finding agents, not conclusions.

---

## 0. HEADLINE — the shape of this codebase's interrupt surface

Definitive counts over `components/hardwareone/` + `main/` (grep, then verified by reading
every hit):

| API | count in first-party code |
|---|---|
| `IRAM_ATTR` / `DRAM_ATTR` | **0** |
| `esp_intr_alloc` / `esp_intr_alloc_intrstatus` | **0** (1 hit is a comment, see §4.1) |
| `gpio_install_isr_service` / `gpio_isr_handler_add` | **0** |
| `attachInterrupt` / `detachInterrupt` | **0** |
| `timer_isr_callback_add` / `gptimer_register_event_callbacks` | **0** |
| `portYIELD_FROM_ISR` | **0** |
| `esp_wifi_set_promiscuous` (sniffer cb) | **0** |
| `esp_event_handler_register` / `_instance_register` | **0** |
| `*FromISR` | **5**, all in one file (`System_Debug.cpp`) |
| `esp_timer_create` | 2 |
| `xTimerCreate` | 1 |
| `esp_now_register_recv_cb` / `_send_cb` | 1 each |

**The firmware installs ZERO hardware ISRs of its own and defines ZERO IRAM-resident
functions.** Every "callback" written in this project runs in **task context**. Every real
hardware ISR in the image belongs to ESP-IDF, the Arduino core, or a managed component.

**Consequence for the audit: P1/P2/P3/P8/P14/P15 have almost no first-party attack surface.**
Any finding of the form "this ISR touches PSRAM / logs / allocates" filed against
first-party code is a **false positive by construction** — verify the context first.
The genuine ISR-adjacent risk here is the *inverse* of P1: see §5.

---

## 1. TRUE HARDWARE ISRs present in the linked image

None are authored here. All are third-party. Registration flags read from source; IRAM
posture read from `sdkconfig`.

| # | ISR | Registered at | `ESP_INTR_FLAG_IRAM`? | Handler in IRAM? | Board |
|---|---|---|---|---|---|
| H1 | I2C port 0 + port 1 (`i2c_isr_handler_default`) | `~/esp/esp-idf/components/driver/i2c/i2c.c:419` via `i2c_driver_install(..., intr_alloc_flags=0)` at `components/arduino/cores/esp32/esp32-hal-i2c.c:142` | **NO** (flags literally `0`) | yes (`IRAM_ATTR` at i2c.c:562) but irrelevant without the flag | all boards with `ENABLE_I2C_SYSTEM` |
| H2 | LCD_CAM vsync (`ll_cam_vsync_isr`) | `managed_components/espressif__esp32-camera/target/esp32s3/ll_cam.c:432` | **NO** — `CAMERA_ISR_IRAM_FLAG` = `0` because `# CONFIG_LCD_CAM_ISR_IRAM_SAFE is not set` (sdkconfig:2804) | **no** — `CAMERA_ISR_IRAM_ATTR` expands to nothing (ll_cam.h:45-50) | **XIAO S3 Sense only** |
| H3 | GDMA in_suc_eof (`ll_cam_dma_isr`) | `.../esp32s3/ll_cam.c:418-426`, `ESP_INTR_FLAG_LOWMED\|ESP_INTR_FLAG_SHARED\|CAMERA_ISR_IRAM_FLAG` | **NO** (same macro = 0) | **no** | **XIAO S3 Sense only** |
| H4 | I2S0 PDM RX + its GDMA channel | `driver/i2s` via `i2s_new_channel()` @ `HAL_Audio.cpp:60` | **NO** — `# CONFIG_I2S_ISR_IRAM_SAFE is not set` (sdkconfig:1298) | no | **XIAO S3 Sense only** (`ENABLE_MICROPHONE_SENSOR`) |
| H5 | SPI2 master (SD over SDSPI) | `spi_bus_initialize(SPI2_HOST, …, SPI_DMA_CH_AUTO)` @ `System_VFS.cpp:700` | **YES** — IDF sets it because `CONFIG_SPI_MASTER_ISR_IN_IRAM=y` (sdkconfig:1351) | yes | boards with SD |
| H6 | RMT TX (NeoPixel via Arduino `rmtWrite`) | Arduino `esp32-hal-rmt.c`; NeoPixel takes the IDF5 path (`esp.c:34 #ifdef HAS_ESP_IDF_5`) | **NO** — `# CONFIG_RMT_ISR_IRAM_SAFE is not set` (sdkconfig:1337); `RMT_TX_ISR_HANDLER_IN_IRAM=y` only places the handler | handler yes, alloc no | FeatherS3 (`NEOPIXEL_PIN_DEFAULT 40`), QTPy(5), etc. — `-1` on several boards |
| H7 | UART0 | Arduino `Serial` / IDF console | **NO** — `# CONFIG_UART_ISR_IN_IRAM is not set` (sdkconfig:1382) | no | all |
| H8 | USB-Serial-JTAG (secondary console) | IDF console | n/a | — | all |
| H9 | WiFi MAC/BB | `libnet80211`/`libpp` | yes (vendor lib, always IRAM) | yes | all |
| H10 | BT controller (`btdm`) | `libbtdm_app` | yes (vendor lib) | yes | `CONFIG_BT_ENABLED=y` |
| H11 | systimer (esp_timer backend) | IDF | yes — `CONFIG_ESP_TIMER_IN_IRAM=y` (sdkconfig:1818) | yes | all |
| H12 | FreeRTOS tick / IPC / cross-core | IDF | yes — `CONFIG_FREERTOS_IN_IRAM=y` | yes | all |

**Dead IRAM code confirmed:** `Adafruit_NeoPixel/esp.c:156 ws2812_rmt_adapter` (an
`IRAM_ATTR` RMT translator) sits inside the `#else` of `#ifdef HAS_ESP_IDF_5`
(`esp.c:34` / `esp.c:123` / `esp.c:275`). On IDF 5.5.1 it is **not compiled**. Do not file
against it. Same for the `IRAM_ATTR` hits under `components/arduino/**/examples/`,
`tests/`, `variants/*/APOTA.ino`, and the unbuilt libs (`TFT_eSPI`, `WebServer_ESP32_W*`,
`NimBLE-Arduino`) — none are in `SRC_DIRS`.

`ll_cam_send_event` (`cam_hal.c:250`) IS unconditionally `IRAM_ATTR` and uses
`xQueueSendFromISR` with a propagated `HPTaskAwoken` + `portYIELD_FROM_ISR()` in the
callers — correct, but its callers (H2/H3) are non-IRAM, so the IRAM placement buys
nothing but IRAM bytes here (P8, tiny).

---

## 2. `esp_timer` callbacks — **TASK CONTEXT, not ISR**

`# CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD is not set` (sdkconfig:1828) → the
`ESP_TIMER_ISR` dispatch method **does not exist in this build**. Both call sites also
explicitly set `.dispatch_method = ESP_TIMER_TASK`. Every `esp_timer` callback here runs
on the `esp_timer` task: **stack 3584 B** (`CONFIG_ESP_TIMER_TASK_STACK_SIZE`), pinned
**CPU0** (`CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0=y`), high priority (IDF default 22).

| # | Callback | File:line | Body | Alloc? | Log? | Block? |
|---|---|---|---|---|---|---|
| T1 | `notifyClearTimerCb` | `G2_Glasses.cpp:13464` (created 13495-13501) | builds a `NotifySpec` + `LensUiJob`, `g2EnqueueLensJob` | **YES — two `new (std::nothrow)`** | `DEBUG_G2F` on enqueue-fail only | `g2EnqueueLensJob` does `xQueueSend(..., pdMS_TO_TICKS(50))` — **can block 50 ms on the esp_timer task** (`G2_Glasses.cpp:11305`) |
| T2 | `factoryreset_doRestart` | `System_Utils.cpp:2162` (created 2195-2203) | `ESP.restart()` | no | no | terminal |

⚑ **T1**: heap `new` + a 50 ms blocking queue send on the shared, high-priority, 3584 B
`esp_timer` task. Legal, but it stalls *every other* esp_timer in the firmware (including
IDF-internal ones) for up to 50 ms when the lens queue is full. Author's own comment
("small stack and is shared with every other esp_timer") acknowledges the stack but not
the blocking send.
⚑ **T2**: the timer handle is deliberately leaked (documented). Fine — reboot follows.

---

## 3. FreeRTOS software timer — **TASK CONTEXT (`Tmr Svc`)**

`Tmr Svc`: **2048 B** stack (`CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=2048`, priority 1,
**no affinity** — `CONFIG_FREERTOS_TIMER_TASK_NO_AFFINITY=y`, queue length 10).

| # | Callback | File:line | Body |
|---|---|---|---|
| S1 | `heartbeatTimerCallback` | `G2_Glasses.cpp:8116` (created 8166) | `if (gBeatSem) xSemaphoreGive(gBeatSem);` — nothing else |

Textbook-correct: no logging, no alloc, no BLE, no blocking. The real work lives in
`heartbeatWorkerTask` (`g2_hb_worker`, 2048–3072 B, prio 5, pinned `APP_CORE`,
`G2_Glasses.cpp:8154`), gated on internal-DRAM headroom before creation. **No finding.**

---

## 4. Radio-stack callbacks — all TASK CONTEXT

### 4.1 ESP-NOW (WiFi task)

Registered at `System_ESPNow.cpp:9825-9826`, immediately after the RX ring is allocated.

| # | Callback | File:line | Context | Notes |
|---|---|---|---|---|
| E1 | `onEspNowDataReceived` | `System_ESPNow.cpp:1092-1111` | **WiFi task** | ~20 lines: bounds-check, `memcpy` into `gEspNowRxRing`, advance head. No log, no alloc, no lock, no block. `gEspNowRxHead/Tail` are `volatile uint8_t` (decl 394-395); drain is in `espnow_task` (8488-8514). Ring is `MALLOC_CAP_INTERNAL` and never freed (comment 9803-9808 explains why). **Clean.** |
| E2 | `onEspNowDataSent` | `System_ESPNow.cpp:6105-6121` | **WiFi task** | two `++` on `gEspNow->routerMetrics.messagesSent/Failed`. |

⚑ **Comment defect, E2**: the doxygen block at `System_ESPNow.cpp:6098` says
`@note Called in interrupt context - keep processing minimal`. **False.** ESP-NOW
recv/send callbacks run on the WiFi task in IDF v5. The header at `System_ESPNow.cpp:398`
gets it right ("runs on the WiFi task"), so the file contradicts itself. A finding agent
reading only line 6098 will file a bogus ISR finding.
⚑ **E2 minor**: `routerMetrics.messagesSent++` is a non-atomic RMW on the WiFi task read
concurrently by `espnowstats` on cmd_exec (P21) — cosmetic, stats-only.
Note for §5: E1 reads a function-local `static const uint8_t kBcastMac[6]` (line 1107) —
flash-resident DROM. **Harmless**, because this is a task, not an ISR. Do not file P3.

### 4.2 BLE — Bluedroid (`CONFIG_BT_BLUEDROID_ENABLED=y`, NimBLE **off**)

All of these run on **BTC_TASK**: `CONFIG_BT_BTC_TASK_STACK_SIZE=8192` (sdkconfig:781,
matching `sdkconfig.defaults:90`), pinned **core 0** (`BT_BLUEDROID_PINNED_TO_CORE=0`).
BTU task is 4352 B.

| # | Callback | File:line | Registered | What it does |
|---|---|---|---|---|
| B1 | `ServerCallbacks::onConnect` | `Bluetooth.cpp:371` | `setCallbacks` @ 966 | slot alloc, `bleScReset`, `systemEventPost` (~164 B local + spinlock), `BLEDevice::stopAdvertising()`, `BLE_DEBUGF` |
| B2 | `ServerCallbacks::onDisconnect` | `Bluetooth.cpp:440` | 966 | `bleClearConnectionByConnId`, `bleScReset`, `systemEventPost`, **`startBLEAdvertising()`** (re-enters Bluedroid from a Bluedroid callback), `BLE_DEBUGF` |
| B3 | `CmdRequestCallbacks::onWrite` | `Bluetooth.cpp:489` | 1003 | zero-copy `getData()/getLength()`, then `processIncomingBLECommand()` → routes heavy work to `cmd_exec` |
| B4 | `CmdStatusCallbacks::onRead` | `Bluetooth.cpp:520` | 1018 | 128 B `snprintf` + `setValue` |
| B5 | `CmdResponseCallbacks::onStatus` | ~`Bluetooth.cpp:1011` | 1011 | notify backpressure bookkeeping |
| B6 | `G2ClientCallbacks::onConnect/onDisconnect` | `G2_Glasses.cpp:1357/1361` | per-temple | teardown: `g2MicOnLeftDisconnect`, `resetHijackListSwapCache`, `g2LensSetHijackActive`, `BROADCAST_PRINTF`, `systemEventPost`, `blePeerNoteLinkLost`, `stopHeartbeatTimer` (which does `vTaskDelay(20 ms)` — **blocks BTC**), `g2PushStatusEvent` |
| B7 | `notifyThunkL/R` → `handleNotify` | `G2_Glasses.cpp:1439/1443` → `2072` | `registerForNotify` @ 8484 | 128 B hex buffer, envelope reassembly into `t.rxBuf`, `g2ParseEnvelope`, `g2statsRecordRx`, `handleEnvelope`, several `DEBUG_G2F` + `BROADCAST_PRINTF` |
| B8 | `audioNotifyThunkL/R` → `handleAudioNotify` | `G2_Glasses.cpp:1998/2002` | `registerForNotify` @ 2021 | **`int16_t pcm[800]` = 1600 B stack local**, 5× `lc3_decode`, WAV write under `gMicWavMutex`, AFE ring push under `gMicAfeMutex`, `DEBUG_G2F` every frame for the first 5 then every 50 |
| B9 | `ringNotifyThunk` → `ringDumpFrame` | `G2_Ring.cpp:749` → `658` | `registerForNotify` @ 1137 | `R1Decoded d` (contains a 256 B payload buffer), `char pbuf[196]`, `char abuf[256]`, `char buf[196]`, `r1Decode`, `r1AnnotatePayload`, up to 4 `DEBUG_G2F` |
| B10 | `g2GapEventHandler` | `G2_Glasses.cpp:9173` | `BLEDevice::setCustomGapHandler` @ 8642 | conn-param classification + 2 `DEBUG_G2F`; comment at 9172 correctly says "runs on the Bluedroid callback task" |
| B11 | `RingClientCallbacks::onConnect/onDisconnect` | `G2_Ring.cpp:851/854` | — | pointer teardown + `DEBUG_G2F` |

**BTC-stack budget, worst single frame (B9 ring notify):** `R1Decoded` (~300 B) + 196 +
256 + 196 = ~950 B of locals, plus each `DEBUG_G2F` adds a 256 B `line[]` +
`vsnprintf` frame inside `debugQueuePrintf` (`System_Debug.cpp:775-800`). **B8** adds
1600 B in one frame. Against 8192 B this is survivable but not roomy — and Bluedroid's
own GATT frames sit underneath it.

⚑ **Comment/config drift — appears at ≥6 sites and will mislead agents.** Every BTC
comment in the tree still quotes the *old* stack size:
- `Bluetooth.cpp:485` "runs on BTC_TASK with limited stack (**~3KB**)"
- `G2_Glasses.cpp:702` "BTC_TASK has a small stack (**~3-4 KB**)"
- `G2_Glasses.cpp:11348` "overflows BTC_TASK's **4 KB** stack"
- `G2_Glasses.cpp:2994` "**4 KB** BTC stack"
Actual: **8192 B** in both `sdkconfig` and `sdkconfig.defaults`. Note `sdkconfig.esp32s3:708`
still carries the old `4096` — that file is a stale artifact, not what's built.
⚑ **Terminology defect**: `Bluetooth.cpp` repeatedly labels BTC-task code "ISR-safe"
(`:373`, `:429`, `:443`, `:490`) and logs `"ISR connect …"` / `"ISR disconnect …"`
(`:429`, `:470`). BTC is a **task**. This is the single most likely source of false
"ISR does X" findings in this codebase.

**Cross-context hazard reachable from BTC:** `BROADCAST_PRINTF` from B6/B7 lands in
`broadcastOutputCore` (`System_Debug.cpp:901`). Step 6 just enqueues (good — the LittleFS
write happens in the *consumer* task at `System_Debug.cpp:254`), but **step 7**
(`:966-972`) calls `sendEspNowStreamMessage(String(text))` inline whenever
`gCurrentStreamCmdId != 0` — a `String` heap allocation plus `espnowtx::sendAead`
(which `malloc`s the payload, `System_ESPNow_Tx.cpp`) **on the BTC task**. Only live while
an ESP-NOW command stream is open, but it is exactly the "allocate on the BLE callback
stack" class the tap-dispatcher was built to avoid.

### 4.3 Deferral machinery (the pattern that keeps BTC safe)

- `tapDispatcherEnqueue` / `tapDispatcherEnqueueExit` — `G2_Glasses.cpp:11456` / `11499`.
  Producer is deliberately tiny (struct + zero-tick `xQueueSend`, drop-on-full with an
  `__atomic_add_fetch` counter). Worker `g2_tap_disp` @ `11420`.
- `g2EnqueueLensJob` / page-swap worker — `G2_Glasses.cpp:11290-11310`.
- `bleConnectInit` / `g2_ble_connect` worker — `G2_Glasses.cpp:8905`.
- WiFi-event snapshot → `wifiEventLogDrain()` — `System_WiFi.cpp:1647-1692`.
- `espnowNoteWifiChannelMayHaveChanged()` — `System_ESPNow.cpp:994` — one volatile store,
  drained on `espnow_task`.

⚑ **Residual hole in the tap dispatcher**: `tapDispatcherEnqueue` (11457-11459) and
`tapDispatcherEnqueueExit` (11500-11502) both call `tapDispatcherInit()` **on the BTC
stack** when the queue doesn't exist yet — and `tapDispatcherInit` does `xQueueCreate` +
`xTaskCreatePinnedToCore` (heap allocation + scheduler spinlocks), which is precisely the
hazard the header comment at `G2_Glasses.cpp:11313-11325` says the dispatcher exists to
prevent. It only fires on the very first tap of a session. Both also emit `DEBUG_G2F`
(vsnprintf) on the failure path, contradicting the "no vsnprintf here" note at 11453.

### 4.4 WiFi / MQTT

| # | Callback | File:line | Context | Notes |
|---|---|---|---|---|
| W1 | `wifiEventLogger` | `System_WiFi.cpp:1658`, registered 1716-1717 | **`arduino_events` task**, core 1 (`CONFIG_ARDUINO_EVENT_RUNNING_CORE=1`) | Snapshot-only into `sWifiDiscPending` + 4 statics; heavy logging deferred to `wifiEventLogDrain()` on the main loop. Comment at 1636-1647 documents the prior stack overflow. **KNOWN — fixed** (memory: `project_wifirm_persist_fix`). |
| W2 | `mqtt_event_handler` | `System_MQTT.cpp:891`, registered 1073 | **esp-mqtt client task** | `MQTT_EVENT_DATA` → `handleMQTTCommand()` + `updateExternalSensor()` inline. Not an ISR. Gated off: `ENABLE_MQTT 0` in `System_BuildConfig.h:104`. |

No `esp_event_handler_register`, no promiscuous/sniffer callback, no SNTP sync callback
(`configTime()` only, `System_Utils.cpp:2296`), no `esp_register_shutdown_handler`.

---

## 5. The real cache-disable story (P1/P2/P40) — inverted from the checklist

`# CONFIG_SPI_FLASH_AUTO_SUSPEND is not set` (sdkconfig:2560). Every LittleFS/NVS write
and erase therefore runs a full cache-off window with the other core parked.

**Correct model for ESP-IDF (the pitfall doc's P1 wording is slightly off):** interrupts
allocated *without* `ESP_INTR_FLAG_IRAM` are **automatically masked** by
`esp_intr_noniram_disable()` for the duration of the flash op. They cannot fault. The
crash risk is only for interrupts allocated **with** the flag that then touch flash/PSRAM.

So the exposure here is **missed/lost interrupts, not Cache Errors**:

| ISR | consequence of a multi-ms flash erase |
|---|---|
| H1 I2C (non-IRAM) | transaction stalls → `ESP_ERR_TIMEOUT` on the sensor/OLED/gamepad bus. Interacts with the documented bus-wedge work in `System_I2C_Manager.cpp:202-220`. |
| H4 I2S PDM (non-IRAM) | DMA descriptor servicing delayed → dropped mic samples / ring overrun. XIAO only. |
| H2/H3 camera (non-IRAM) | dropped VSYNC/EOF → torn or lost frames. XIAO only. |
| H7 UART0 (non-IRAM) | RX FIFO overrun → lost serial CLI input. |
| H6 RMT (non-IRAM alloc) | NeoPixel glitch. Cosmetic. |
| H5 SPI2 (**IRAM=yes**) | keeps running — so SDSPI descriptors/buffers must be internal-DRAM. `spi_bus_initialize` with `SPI_DMA_CH_AUTO` at `System_VFS.cpp:700`; IDF enforces internal DMA buffers, but this is the **one place where a PSRAM buffer would actually be a P2 crash**, and it's worth a targeted check by whoever owns A4. |

⚑ Handoff: the single highest-value cross-check in this area is **H5 + PSRAM**, not the
non-IRAM ISRs.

Also relevant: `# CONFIG_ESP_WIFI_IRAM_OPT`, `# CONFIG_ESP_WIFI_RX_IRAM_OPT`,
`# CONFIG_LWIP_IRAM_OPTIMIZATION` are all **disabled** (sdkconfig:1857-1859, 2131-2132)
— non-default, presumably to reclaim IRAM (`MEMORY_LAYOUT.md:146` notes "IRAM goes to
~100 % full with BT on"). Costs WiFi throughput; documented tradeoff, not a bug.

---

## 6. `*FromISR` usage — one file, and it is defensive, not reachable from first-party ISRs

`System_Debug.cpp` `enqueueChunk()` (`:735-772`) branches on `xPortInIsrContext()`:
- `uxQueueMessagesWaitingFromISR` (744) / `xQueueReceiveFromISR` (752) /
  `xQueueSendFromISR` (764, 767).
- Pool growth is correctly refused in ISR context (`:415-420`) — it sets
  `gDebugPoolGrowthRequested` and lets the consumer task do the `heap_caps_malloc`.
- `idfLogVprintf` (`:1075`) bypasses the queue entirely in ISR context and falls back to
  the previous `vprintf` handler.

Reachability: since first-party code installs no ISRs, the ISR branch is only reachable if
an **IDF/vendor** ISR calls `ESP_LOG*` (which routes through `idfLogVprintf` — and that
path explicitly bails before `enqueueChunk`). So the `FromISR` branch is, in practice,
**defensive dead code**. Correct and cheap; leave it.

⚑ **P15 nit, if any agent wants it**: all three `*FromISR` calls pass `NULL` for
`pxHigherPriorityTaskWoken` and there is no `portYIELD_FROM_ISR()`. If the branch ever
*did* execute, the consumer wake would be deferred to the next tick. Low value given
unreachability — mention, don't headline.

---

## 7. Spinlock (`portMUX`) inventory — all task-context, none ISR-shared

11 files. Every one is a task↔task guard; **no `portMUX` in this codebase is shared with
an ISR**, so `taskENTER_CRITICAL` (not `...FROM_ISR`) is the right primitive everywhere.

| Mux | File | Guards | Longest critical section |
|---|---|---|---|
| `gEventMux` | `System_Events.cpp:125` | 48-slot `SystemEvent` ring (`:122`) | one ~164 B struct copy (`:250`); `systemEventFetchSince` copies up to `maxOut` structs (`:302-306`) — the longer one |
| `gDebugPoolMux` | `System_Debug.cpp:66` | debug pool growth state | scalar writes |
| `sToastMux` | `OLED_UI.cpp:16` | `gOledToast` (`char message[64]`) | `strncpy` ≤64 B (`:154`) |
| `gRingTxQMux` | `G2_Ring.cpp:111` | 8-slot `RingTxPending` TX queue | ⚑ `ringTxQEnqueue`/`Pop`/`PushFront` shift up to 7 slots of `bytes[R1_MAX_FRAME=273]` ≈ **1.9 KB of memcpy with interrupts off** (`:145-155`, `:172`, `:184`). Int WDT is 1500 ms so it cannot trip, but it is by far the longest critical section in the tree |
| `sStatsLock` | `System_ESPNow_Tx.cpp:50` | TX stats; lock-free HWM pre-check at `:66` | scalar / one `Stats` copy |
| `sSwapSpinlock` | `OLED_Mode_Map.cpp:83` | front/back framebuffer pointer swap + render snapshot | `memcpy(subtypeVisibility)` (`:990`) then pointer swap only (`:1019-1023`) — correct design |
| `sPauseMux` | `System_PollPause.cpp:19` | poll-pause depth counters + mirror | scalars; `pollPaused()` reads lock-free on purpose (`:48-55`) |
| `sConnPriMux` | `G2_Glasses.cpp:9362` | BLE conn-priority depth (APP_CORE vs cmd_exec) | scalars |
| `s_netScanMux` | `G2_Page_Network.cpp:739` | `s_netScanRunning` single-flight | one bool |
| `gSensorBcastTaskMux` | `System_ESPNow_Sensors.cpp:73` | `claimBroadcasterForDelete()` — double-`vTaskDelete` guard | one pointer swap |
| — | `System_Automation.cpp:3683` | (comment only) explains why the event drain buffer must **not** be PSRAM under `gEventMux` | — |

`gEventRing` (`System_Events.cpp:122`) is plain `static` → internal BSS, **not**
`EXT_RAM_BSS_ATTR`. 48 × ~164 B ≈ 7.9 KB internal. Deliberate, and the reasoning is
written down at `System_Automation.cpp:3683-3685`. Do not "optimize" it to PSRAM.

`System_Events.h:317` correctly documents `systemEventPost` as "from any task (**not ISR**)".

---

## 8. Non-callback peripheral paths (for completeness — no callback registered)

- **PDM mic**: `HAL_Audio.cpp` uses the new `driver/i2s_pdm.h` API but registers **no**
  event callback — `i2s_channel_read()` is polled with a 100 ms timeout (`:108`, `:279`).
  No `i2s_channel_register_event_callback` anywhere.
- **Camera**: `esp_camera_fb_get()` polled from task context (`System_Camera_DVP.cpp:518`,
  `610`, `715`, `728`). `fb_location = CAMERA_FB_IN_PSRAM` (`:345`, `:406`),
  `# CONFIG_CAMERA_PSRAM_DMA is not set` (sdkconfig:2800) → DMA lands in internal DRAM
  line buffers and the **cam task** (`CONFIG_CAMERA_TASK_STACK_SIZE=4096`, core 0) copies
  to the PSRAM framebuffer. The PSRAM touch is in a task, not the ISR. **P4/P2 clean here.**
- **Input (buttons/encoder)**: `HAL_Input.cpp` (345 lines) contains **no** GPIO API at all
  — input is I2C-polled (seesaw gamepad / ANO encoder, `i2csensor_seesaw.cpp`).
  `INPUT_DEVICE_TYPE 1` (`System_BuildConfig.h:164`).
- **I2C affinity**: `beginBusOnCpu1()` (`System_I2C_Manager.cpp:237`) deliberately runs
  `TwoWire::begin()` on a short-lived CPU1 task so the legacy driver's `esp_intr_alloc`
  pins H1 to **CPU1**, off the BLE controller's core. Rationale documented at `:202-220`
  (Int WDT storm under RF coexistence). This is the one place first-party code
  *intentionally* controls an ISR — worth knowing before anyone "simplifies" it.
- **Deep sleep**: `esp_sleep_enable_timer_wakeup` only (`System_Utils.cpp:1451`); no
  ext0/ext1/GPIO wake sources, so no RTC-domain ISR concerns.

---

## 9. ⚑ OUT-OF-SCOPE HANDOFF — words-vs-bytes regression (belongs to A1)

Found while tracing callback→worker deferral. The project's own rule is documented
(`*_STACK_WORDS` are byte counts; `xTaskCreate`'s depth arg is **bytes** on ESP-IDF), but
**six comment sites now assert the opposite** and at least two stacks appear to have been
*shrunk* on the wrong belief:

| Site | Comment claims | Actual value passed | Actual size (bytes) | Comment's own "observed peak" |
|---|---|---|---|---|
| `G2_Glasses.cpp:11424-11431` | "6400 words = 25 KB" | `6400` | **6.25 KB** | **~11.5 KB** ⚠ |
| `G2_Glasses.cpp:8916-8921` | "5120 words = 20 KB" | `5120` | **5 KB** | **~9.4 KB** ⚠ |
| `WebServer_Server.cpp:5291-5305` | "7680 words = 30 KB" | `config.stack_size = 7680` | **7.5 KB** | **~18 KB** ⚠ |
| `WebServer_Server.cpp:5207-5212` | "11059 words = 44 KB" | `sslConfig.httpd.stack_size = 11059` | **10.8 KB** | **~30 KB** ⚠ |
| `System_Camera_DVP.cpp:988-991` | "10240 … is 40 KB" | `10240` | 10 KB | ~2.5 KB (safe) |
| `System_ESPNow_Tx.cpp:105-131` | logs "stack=%u words" | `ESPNOW_TX_STACK_WORDS` | (arithmetic still correct — `uxTaskGetStackHighWaterMark` also returns bytes on IDF) | — |

The first four are candidate stack overflows on the four deepest-call-chain workers in the
firmware (tap dispatcher, BLE connect worker, HTTP server, HTTPS server). Note the
`/*stack bytes*/` inline label at `G2_Glasses.cpp:8917` and `:11437` **contradicts the
prose two lines above it**, which is how the regression hid. Not my assignment — handing
to whoever owns A1/task stacks. Worth verifying against a live `uxTaskGetStackHighWaterMark`
readout before anyone acts.

---

## 10. Quick answer key for finding agents

- "Is this an ISR?" — in `components/hardwareone/`, **no**. Always. Check §1 first.
- BTC_TASK = task, 8192 B, core 0. Not an ISR, not 3–4 KB.
- esp_timer callbacks = task, 3584 B, core 0, prio ~22. ISR dispatch is compiled out.
- `Tmr Svc` = task, **2048 B**, prio 1, **unpinned**.
- ESP-NOW recv/send = WiFi task. The `@note Called in interrupt context` at
  `System_ESPNow.cpp:6098` is wrong.
- `arduino_events` = task, core 1, small stack — already hardened, don't re-file.
- Non-IRAM ISRs here are *masked*, not *faulted*, during flash ops. The only
  IRAM-flagged ISR the app causes to exist is **SPI2/SDSPI** (H5).
