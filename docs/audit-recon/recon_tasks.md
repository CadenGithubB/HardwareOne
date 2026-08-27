# RECON — Complete FreeRTOS Task Map (HardwareOne firmware)

Repo: `/Users/morgan/esp/hardwareone-idf` · Source: `components/hardwareone/`
Target: ESP32-S3, ESP-IDF v5.5.1, dual-core, `CONFIG_FREERTOS_HZ=1000`, `configMAX_PRIORITIES=25`.
Read-only recon. Every row below was opened and read, not grepped.

## 0. Method / scope

Search pattern covered `xTaskCreate`, `xTaskCreatePinnedToCore`, `xTaskCreateStatic`,
`xTaskCreateStaticPinnedToCore`, `xTaskCreateLogged`, `xTaskCreateUniversal`, plus
`pthread_create` / `std::thread` / `esp_pthread` (**zero** hits — no non-FreeRTOS threads).

- **41 task-creation call sites** in `components/hardwareone/`
- **38 distinct task names** (3 names have two create sites each — see §7)
- `main/hardwareone-idf.cpp` creates **no** tasks.
- **1** software timer only (`xTimerCreate("g2_hb")`, `G2_Glasses.cpp:8166`) → one `Tmr Svc` user.

**UNITS.** Every stack number in this document is **BYTES**. On this port
`portSTACK_TYPE == uint8_t`, so `StackType_t` is 1 byte and ESP-IDF's `usStackDepth`
is a byte count (`System_TaskUtils.h:9-29`). The `*_STACK_WORDS` constants are byte
counts with a misnamed identifier. `uxTaskGetStackHighWaterMark()` likewise returns bytes.

**BOARD.** `System_BuildConfig.h` currently selects **FeatherS3** (`I2C_FEATURE_LEVEL 4`
CUSTOM: OLED + Seesaw gamepad only; `INPUT_DEVICE_TYPE 1`). Live flags:
`ENABLE_WIFI 1`, `ENABLE_ESPNOW 1`, `ENABLE_BLUETOOTH 1`, `ENABLE_G2_GLASSES 1`,
`ENABLE_MAPS 1`, `ENABLE_MICROPHONE 1` (derived, via G2 BLE audio) —
`ENABLE_CAMERA_SENSOR 0`, `ENABLE_MICROPHONE_SENSOR 0`, `ENABLE_ESP_SR 0`,
`ENABLE_EDGE_IMPULSE 0`, `ENABLE_ONDEVICE_LLM 0`, and all nine I2C sensors except
gamepad are `0`. The "Board" column names which builds each task exists in.

---

## 1. Documented core-split policy (`System_TaskUtils.h:132-171`)

```
PRO_CORE = 0   APP_CORE = 1   I2C_SENSOR_CORE = APP_CORE
TASK_PRIORITY_LOW = 1 · NORMAL = 3 · HIGH = 5
```

Verbatim rules as written in the header:

| Class | Placement | Rationale as documented |
|---|---|---|
| **Anything touching shared I2C/Wire** | **Core 1 (APP)** — `I2C_SENSOR_CORE`. Header calls this "the hard rule — no exceptions." | An unpinned I2C task that floats onto saturated Core 0 and is preempted mid-transaction storms the bus → `panic(4)`. Blamed for the FeatherS3 out-and-about crash loop (`docs/NewCapture` 2026-07-22). |
| **CPU-bound compute / render** (SR inference, map/image raster) | Core 1 (APP) | Keep off the radio core so it can't crowd Wi-Fi/BLE. |
| **BLE-reply / command I/O (`cmd_exec`), serial/log I/O, ESP-NOW real-time path** | Core 0 (PRO) | "They belong with the radio + I/O they serve." |
| **`tskNO_AFFINITY`** | Allowed **only** for short-lived workers touching no shared bus, not latency-sensitive, **and only as a documented choice**. | — |

Verification tool named by the policy: `perftop` (IDLE0/IDLE1 rows ≈ per-core headroom).

**Compliance:** every one of the 41 firmware create sites passes an explicit core. There is
**zero** use of `tskNO_AFFINITY` in `components/hardwareone/`. Policy is fully honoured by
firmware-authored tasks. (The `httpd` server task is the one exception, and it is
IDF-created — see §6.)

---

## 2. Task Watchdog (TWDT) status — **no firmware task is subscribed**

`sdkconfig`: `CONFIG_ESP_TASK_WDT_EN=y`, `CONFIG_ESP_TASK_WDT_INIT=y`,
`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`, `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y`,
`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y`, **`CONFIG_ESP_TASK_WDT_PANIC` is NOT set**.

- `esp_task_wdt_add` / `_reset` / `_delete` / `_status`: **0 occurrences** anywhere in
  `components/hardwareone/` or `main/`.
- `enableLoopWDT()` / `disableCore0WDT()` / etc.: **0 occurrences** in firmware.
- Arduino's `app_main` sets `loopTaskWDTEnabled = false`
  (`components/arduino/cores/esp32/main.cpp:111`) and nothing ever flips it, so `loopTask`'s
  `esp_task_wdt_reset()` at `main.cpp:79-81` is dead — **`loopTask` is not subscribed either**.

**⇒ The only TWDT subscribers on this device are `IDLE0` and `IDLE1`.** The TWDT column in
the table below is therefore uniformly **No**, and it is a property of the config, not of any
individual task. Because `PANIC` is off, a trip prints a warning and does **not** reset.

---

## 3. Master task table — firmware (`components/hardwareone/`)

Priority column: numeric value (constant name where one is used). Core: 0 = PRO, 1 = APP.
"Returns?" = can the entry function fall off its end (P12 hazard).

### 3a. Boot / always-on infrastructure

| # | Task name | Entry fn | Create site | Body site | Stack (B) | Prio | Core | TWDT | Returns? | Board |
|---|---|---|---|---|--:|--:|--:|---|---|---|
| 1 | `cmd_exec_task` | `commandExecTask` | `HardwareOne.cpp:1514` | `HardwareOne.cpp:682` | 8192 | 1 `LOW` | 0 | No | **Never** — `for(;;)`, no exit path | all |
| 2 | `sensor_queue_tas`* | `sensorQueueProcessorTask` | `HardwareOne.cpp:1611` **and** `System_I2C.cpp:3052` | `System_I2C.cpp:2682` | 4096 | 1 `LOW` | 1 `I2C_SENSOR_CORE` | No | **Never** — `while(true)`; early `vTaskDelete(nullptr)` if I2C mgr null | all w/ `ENABLE_I2C_SYSTEM` |
| 3 | `debug_out` | `debugOutputTask` | `System_Debug.cpp:660` | `System_Debug.cpp:193` | 4096 | 1 `LOW` | 0 `PRO_CORE` | No | **Never** — `while(true)`, no `vTaskDelete` | all |

\* name is `"sensor_queue_task"` (17 chars) but `CONFIG_FREERTOS_MAX_TASK_NAME_LEN=16` truncates it — see §8-A.

### 3b. I2C sensor pollers — all pinned to `I2C_SENSOR_CORE` (1), all `TASK_PRIORITY_LOW`

All nine are created by `create*Task()` helpers in `System_TaskUtils.cpp`, each guarded by
an `eTaskGetState()` stale-handle check. All exit via `SENSOR_TASK_EXIT(x)` (= log +
`vTaskDelete(nullptr)`) at the **top** of the loop; the stack-safety bailout deliberately
uses `continue`, not `break`, so a near-overflowed frame never returns out of the task
(comment repeated verbatim in every driver).

| # | Task name | Entry fn | Create site | Body site | Stack (B) | Returns? | Board |
|---|---|---|---|---|--:|---|---|
| 4 | `gamepad_task` / `ano_task` (`INPUT_TASK_NAME`) | `inputTask` | `System_TaskUtils.cpp:192` (prio literal `1`, core literal `1`) | `i2csensor_seesaw.cpp:438` **or** `i2csensor_ano_encoder.cpp:447` | 3584 | Never — `SENSOR_TASK_EXIT(INPUT)` | **FeatherS3** (seesaw variant; CMake picks one driver) |
| 5 | `thermal_task` | `thermalTask` | `System_TaskUtils.cpp:233` | `i2csensor_mlx90640.cpp:1459` | 6144 | Never — `SENSOR_TASK_EXIT(THERMAL)` @1483 | full-I2C builds only |
| 6 | `imu_task` | `imuTask` | `:250` | `i2csensor_bno055.cpp:1113` | 4096 | Never — `SENSOR_TASK_EXIT(IMU)` @1137 | full-I2C only |
| 7 | `tof_task` | `tofTask` | `:268` | `i2csensor_vl53l4cx.cpp:709` | 3072 | Never — `SENSOR_TASK_EXIT(TOF)` @732 | full-I2C only |
| 8 | `fmradio_task` | `fmRadioTask` | `:286` | `i2csensor_rda5807.cpp:234` | 4608 | Never — bare `vTaskDelete(nullptr)` @295 (**only sensor not using `SENSOR_TASK_EXIT`**) | full-I2C only |
| 9 | `apds_task` | `apdsTask` | `:305` | `i2csensor_apds9960.cpp:473` | 3072 | Never — `SENSOR_TASK_EXIT(APDS)` @494 | full-I2C only |
| 10 | `presence_task` | `presenceTask` | `:323` | `i2csensor_sths34pf80.cpp:549` | 3072 | Never — `SENSOR_TASK_EXIT(PRESENCE)` @566 | full-I2C only |
| 11 | `gps_task` | `gpsTask` | `:341` | `i2csensor_pa1010d.cpp:414` | 3072 | Never — `SENSOR_TASK_EXIT(GPS)` @431 | full-I2C only |
| 12 | `rtc_task` | `rtcTask` | `:359` | `i2csensor_ds3231.cpp:442` | 4096 | Never — `while(gRtcRunning)` then `vTaskDelete(nullptr)` @514 | full-I2C only |

### 3c. Radio / ESP-NOW

| # | Task name | Entry fn | Create site | Body site | Stack (B) | Prio | Core | Returns? | Board |
|---|---|---|---|---|--:|--:|--:|---|---|
| 13 | `espnow_task` | `espnowHeartbeatTaskFn` | `System_ESPNow.cpp:9404` | `System_ESPNow.cpp:9385` | 6656 | 5 `HIGH` | 0 (literal) | **Never** — `for(;;) { processMeshHeartbeats(); vTaskDelay(10ms); }` | `ENABLE_ESPNOW` |
| 14 | `espnow_tx` | `txTask` | `System_ESPNow_Tx.cpp:156` | `System_ESPNow_Tx.cpp:105` | 5120 | 5 `HIGH` | 0 `PRO_CORE` | **Never** — `for(;;)` on queue | `ENABLE_ESPNOW` |
| 15 | `sensor_bcast` | `sensorBroadcasterTask` | `System_ESPNow_Sensors.cpp:790` | `System_ESPNow_Sensors.cpp:665` | 4096 | 5 `HIGH` | 1 (literal) | **Never** — `for(;;)`; self-teardown via `claimBroadcasterForDelete()` → `vTaskDelete(nullptr)` @760, else parks `for(;;) vTaskDelay(portMAX_DELAY)` @762 | `ENABLE_ESPNOW` |

### 3d. Media / inference (compiled out on the current FeatherS3 config unless noted)

| # | Task name | Entry fn | Create site | Body site | Stack (B) | Prio | Core | Returns? | Board |
|---|---|---|---|---|--:|--:|--:|---|---|
| 16 | `sr_task` | `srTask` | `System_ESPSR.cpp:2632` | `System_ESPSR.cpp:1917` | 8192 | 5 `SR_TASK_PRIORITY_LEVEL` | 1 (literal) | Never — `vTaskDelete(nullptr)` @2001 (alloc fail) and @2540 (clean stop) | `ENABLE_ESP_SR` (**0** now) |
| 17 | `sr_snip_wr` | `srSnipWriterTask` | `System_ESPSR.cpp:1348` | `System_ESPSR.cpp:1172` | 4096 | 3 `NORMAL` | 0 (literal) | **Never** — `while(true)`; killed externally by `vTaskDelete(gSrSnipWriterTask)` @1367 | `ENABLE_ESP_SR` (**0**) |
| 18 | `ei_continuous` | `continuousInferenceTask` | `System_EdgeImpulse.cpp:1880` | `System_EdgeImpulse.cpp:1802` | 8192 | 1 (literal) | 1 `APP_CORE` | Never — `while(gEIContinuousRunning)` → `vTaskDelete(nullptr)` @1856 | `ENABLE_EDGE_IMPULSE` (**0**) |
| 19 | `mic_record` | `recordingTask` | `System_Microphone.cpp:406` | `System_Microphone.cpp:243` | 4096 | 5 `HIGH` | 1 (literal) | Never — `vTaskDelete(NULL)` @258 (buf fail) and @343 | `ENABLE_MICROPHONE` (**1** on FeatherS3 via G2 audio) |
| 20 | `mic_viz` | `micVisualizerTaskFunc` | `System_Microphone.cpp:1213` | `System_Microphone.cpp:1125` | 4096 | 3 `NORMAL` | 1 `APP_CORE` | Never — `vTaskDelete(nullptr)` @1131 and @1193 | `ENABLE_MICROPHONE` (**1**) |
| 21 | `cam_record` | `recordingTask` | `System_Camera_Video.cpp:421` | `System_Camera_Video.cpp:299` | 6144 (`VIDEO_REC_STACK_WORDS`, local const @`:65`) | 5 (literal) | 1 (literal) | Never — `vTaskDelete(nullptr)` @340 | `ENABLE_CAMERA_SENSOR` (**0**) |
| 22 | `cam_pwr` | `cameraPwrWorker` | `System_Camera_DVP.cpp:1003` | `System_Camera_DVP.cpp:973` | **10240** (largest dynamic stack) | 2 (`tskIDLE_PRIORITY+2`) | 1 `I2C_SENSOR_CORE` | **Never** — `for(;;)` on queue | `ENABLE_CAMERA_SENSOR` (**0**) |
| 23 | `llm_gen` | `llmWorkerTask` | `System_LLM.cpp:275` (**`xTaskCreateStaticPinnedToCore`**) | `System_LLM.cpp:196` | **12288** (`LLM_TASK_STACK_SIZE`, `System_LLM.h:84`); stack+TCB `heap_caps_malloc(MALLOC_CAP_INTERNAL)` | 2 `LLM_TASK_PRIORITY` | 1 (literal) | **Never** — `for(;;)` parked on `ulTaskNotifyTake`; comment: "This task never exits" | `ENABLE_ONDEVICE_LLM` (**0** now) |

### 3e. OLED / I2C helpers

| # | Task name | Entry fn | Create site | Body site | Stack (B) | Prio | Core | Returns? | Board |
|---|---|---|---|---|--:|--:|--:|---|---|
| 24 | `mapRender` | `mapRenderTask` | `OLED_Mode_Map.cpp:949` | `OLED_Mode_Map.cpp:972` | 8192 | 1 `LOW` | 1 `APP_CORE` | **Never** — `for(;;)` on semaphore | `ENABLE_MAPS` + OLED |
| 25 | `i2c0_begin` | `busBeginTask` | `System_I2C_Manager.cpp:244` | `System_I2C_Manager.cpp:229` | 4096 | **10** (literal) | 1 (literal) | Never — `xSemaphoreGive` then `vTaskDelete(nullptr)` | all w/ I2C |

### 3f. G2 glasses / R1 ring (all `APP_CORE`)

| # | Task name | Entry fn | Create site | Body site | Stack (B) | Prio | Returns? |
|---|---|---|---|---|--:|--:|---|
| 26 | `g2_hb_worker` | `heartbeatWorkerTask` | `G2_Glasses.cpp:8154` | `G2_Glasses.cpp:8073` | **3072 / 2560 / 2048 — adaptive**, chosen from `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`; refuses to start below `stack+1200` | 5 | Never — `while(!gBeatTaskStop)` → `gBeatTaskHandle=nullptr; vTaskDelete(nullptr)` @8113 |
| 27 | `g2_ble_connect` | `bleConnectWorkerLoop` | `G2_Glasses.cpp:8921` | `G2_Glasses.cpp:8846` | 5120 | 5 | **Never** — `for(;;)`; killed externally @8940 |
| 28 | `g2_page_swap_w` | `pageSwapWorkerLoop` | `G2_Glasses.cpp:11233` | `G2_Glasses.cpp:11115` | 3584 | 2 (`tskIDLE+2`) | **Never** — `for(;;)` on queue |
| 29 | `g2_tap_disp` | `tapDispatcherWorkerLoop` | `G2_Glasses.cpp:11432` | `G2_Glasses.cpp:11366` | 6400 | 2 (`tskIDLE+2`) | **Never** — `for(;;)` on queue |
| 30 | `g2_session_w` | `g2SessionWorkerLoop` | `G2_Glasses.cpp:12563` | `G2_Glasses.cpp:12537` | 8192 (`kG2SessionStackBytes` @12471) | 2 (`tskIDLE+2`) | **Never** — `for(;;)`; killed externally @12588 |
| 31 | `g2-fsm` | `fsmWorkerTask` | `G2_HijackFsm.cpp:334` | `G2_HijackFsm.cpp:280` | **2560** (smallest) | 5 | **Never** — `for(;;)` on queue |
| 32 | `g2_net_scan` | `networkScanWorker` | `G2_Page_Network.cpp:818` | `G2_Page_Network.cpp:799` | 4096 | 5 | Never — 3-line body ending `vTaskDelete(nullptr)` @802 |
| 33 | `g2_wifi_pending` | `wifiPendingWatchdogTask` | `G2_Page_Network.cpp:1134` **and** `:1360` | `G2_Page_Network.cpp:866` | 4096 | 5 | Never — `vTaskDelete(nullptr)` @895 |
| 34 | `g2_ring_pending` | `ringPendingWatchdogTask` | `G2_Page_Network.cpp:1665` | `G2_Page_Network.cpp:1030` | 4096 | 5 | Never — `vTaskDelete(nullptr)` @1050 |
| 35 | `g2_ai_test` | `aiWorker` | `G2_Page_TestSuite.cpp:522` | `G2_Page_TestSuite.cpp:477` | 4096 | 5 | Never — `vTaskDelete(nullptr)` @479 (null arg) and @495 |
| 36 | `g2_img_probe` | `imgProbeWorkerInternalStack` (primary) | `G2_Page_TestSuite.cpp:1094` | `G2_Page_TestSuite.cpp:1096` | 8192 (`kStackBytes/sizeof(StackType_t)` = 8192) | 5 | Never — `vTaskDelete(nullptr)` @1062 |
| 36b | `g2_img_probe` | `imgProbeWorker` (**PSRAM-stack fallback**) | `G2_Page_TestSuite.cpp:1114` (`xTaskCreateStaticPinnedToCore`) | `G2_Page_TestSuite.cpp:1049` | 8192, stack from `heap_caps_malloc(MALLOC_CAP_SPIRAM)` | 5 | Never — frees own stack/TCB then `vTaskDelete(nullptr)` @1055 — **see §8-C** |
| 37 | `ring_spoof` | `ringSpoofTaskBody` | `G2_Ring.cpp:1528` | `G2_Ring.cpp:1466` | 4096 | 4 (literal) | Never — `while(gSpoofEnabled)` → `vTaskDelete(nullptr)` @1519 |
| 38 | `ring_bridge_hb` | `ringBridgeHeartbeatBody` | `G2_Ring.cpp:2015` | `G2_Ring.cpp:1987` | 3072 | 3 (literal) | Never — `while(gBridgeHbEnabled)` → `vTaskDelete(nullptr)` @2009 |

**P12 verdict: clean.** Not one of the 38 entry functions can fall off its end. Every one
either loops forever or terminates in `vTaskDelete`. Several drivers carry an explicit
comment explaining why the stack-safety bailout uses `continue` instead of `break`
precisely to avoid an `IllegalInstruction` panic on return.

---

## 4. Static stack footprint

Sum of all 38 distinct firmware task stacks, if every task were alive simultaneously:

| Group | Tasks | Bytes |
|---|--:|--:|
| Boot infrastructure (`cmd_exec`, `sensor_queue`, `debug_out`) | 3 | 16,384 |
| I2C sensor pollers + input | 9 | 34,816 |
| ESP-NOW (`espnow_task`, `espnow_tx`, `sensor_bcast`) | 3 | 15,872 |
| Media / inference (SR ×2, EI, mic ×2, cam ×2, LLM) | 8 | 57,344 |
| OLED / I2C helpers (`mapRender`, `i2c0_begin`) | 2 | 12,288 |
| G2 / R1 | 13 | 60,672 |
| **TOTAL (all-tasks-alive upper bound)** | **38** | **197,376 B ≈ 192.8 KB** |

**As compiled on the current FeatherS3 config:** subtract the eight disabled I2C sensors
(31,232 B), SR ×2 (12,288 B), EI (8,192 B), camera ×2 (16,384 B) and LLM (12,288 B)
= 80,384 B removed → **116,992 B ≈ 114.3 KB** of firmware task stack, and that is still a
*ceiling* (only ~8 of those are actually resident at idle: `cmd_exec`, `sensor_queue`,
`debug_out`, `gamepad`, `espnow_task`, `espnow_tx`, plus lazily-spawned G2 workers).

**Prior doc is stale.** `docs/STACK_TO_PSRAM_CANDIDATES.md:199` says *"Tasks (46 tasks,
~248 KB)"*. That inventory lists `g2_cam_view`, `g2_live_page`, `g2_live_text`,
`g2_bmp_view`, `g2_jpg_view`, `g2_llm_page`, `g2_map_page` as separate tasks — **none of
those task names exist any more**; they were consolidated into `g2_page_swap_w` /
`g2_session_w`. Only stale *comments* referencing those names survive
(`G2_Glasses.cpp:6186`, `:17793`, `:19275`). The correct current figures are 38 tasks /
197,376 B. Mark the doc's headline as superseded. (Its "⛔ PSRAM task stacks are RULED OUT"
decision and the flash-cache-disable reasoning remain valid and unchanged.)

Largest individual stacks, descending: `llm_gen` 12288 · `cam_pwr` 10240 ·
`cmd_exec_task` / `sr_task` / `ei_continuous` / `mapRender` / `g2_session_w` /
`g2_img_probe` 8192 · `espnow_task` 6656 · `g2_tap_disp` 6400 ·
`thermal_task` / `cam_record` 6144.

---

## 5. Tasks created outside init time (pitfall P16)

Only **three** firmware tasks are created during `setup()`:
`cmd_exec_task` (`HardwareOne.cpp:1514`), `sensor_queue_task` (`:1611`),
`debug_out` (via `initDebugSystem`). Everything else is lazy or user-triggered.

**(a) One-shot lazy singletons** — created on first use, then persistent forever. These are
the *good* pattern and are all idempotent-guarded (`if (queue) return;`):
`espnow_task`, `espnow_tx`, `sensor_bcast`, `mapRender`, `cam_pwr`, `llm_gen`,
`g2_page_swap_w`, `g2_tap_disp`, `g2_ble_connect`, `g2_session_w`, `g2-fsm`, `sr_snip_wr`.

**(b) Per-user-action / repeatedly created + destroyed** — the actual P16 exposure:

| Task | Trigger | Site |
|---|---|---|
| `g2_net_scan` | every WiFi-scan tap on the glasses | `G2_Page_Network.cpp:818` |
| `g2_wifi_pending` | every WiFi toggle / connect tap (2 sites) | `G2_Page_Network.cpp:1134`, `:1360` |
| `g2_ring_pending` | every R1 connect tap | `G2_Page_Network.cpp:1665` |
| `g2_ai_test` | every AI-card test tap | `G2_Page_TestSuite.cpp:522` |
| `g2_img_probe` | every image-probe run | `G2_Page_TestSuite.cpp:1094` / `:1114` |
| `i2c0_begin` | every `wire->begin()` (bus init / re-init) | `System_I2C_Manager.cpp:244` |
| `mic_record`, `mic_viz`, `cam_record` | each recording / visualiser / video start | mic `:406`, `:1213`; video `:421` |
| `sr_task` | each `srstart` | `System_ESPSR.cpp:2632` |
| `ei_continuous` | each continuous-inference start | `System_EdgeImpulse.cpp:1880` |
| `ring_spoof`, `ring_bridge_hb` | CLI on/off | `G2_Ring.cpp:1528`, `:2015` |

The four `g2_*` UI ones are 4 KB or 8 KB internal-DRAM allocations taken on a *tap*, which
is exactly the pattern the project preference ("avoid spawning a worker task per UI action")
targets. They are guarded against *concurrent* duplicates (`s_netScanRunning`,
`gWifiPendingTaskActive`, `gRingPendingTaskActive`) but not against repeated
create/destroy fragmentation.

**(c) Created from the BLE notify (BTC) task stack.** `tapDispatcherInit()` — which does
`xQueueCreate` + a 6400-byte `xTaskCreatePinnedToCore` — runs lazily from
`tapDispatcherEnqueue()` / `tapDispatcherEnqueueExit()` (`G2_Glasses.cpp:11458`, `:11500`),
both of which are documented as running on the Bluedroid notify stack.
`pageSwapInit()` (`:11265`, `:11299`) has the same shape. Task creation on the
`BTC_TASK` stack is a known-tight budget on this project.

---

## 6. Platform / library tasks (not firmware-authored, for completeness)

| Task | Source | Stack (B) | Prio | Core | TWDT |
|---|---|--:|--:|--:|---|
| `loopTask` | `arduino/cores/esp32/main.cpp:113` | 8192 (`CONFIG_ARDUINO_LOOP_STACK_SIZE`) | 1 | 1 (`CONFIG_ARDUINO_RUNNING_CORE=1`) | No (`loopTaskWDTEnabled=false`) |
| `arduino_events` | `arduino/libraries/Network/src/NetworkEvents.cpp:76` | 4096 | 19 (`ESP_TASKD_EVENT_PRIO-1`) | 1 (`CONFIG_ARDUINO_EVENT_RUNNING_CORE=1`) | No |
| `httpd` | IDF `esp_http_server`, config at `WebServer_Server.cpp:5305` (HTTP) / `:5211` (HTTPS) | **7680** HTTP / **11059** HTTPS | 5 (`tskIDLE+5`) | **`tskNO_AFFINITY`** (IDF default, never overridden) | No |
| `IDLE0` / `IDLE1` | FreeRTOS | 1536 each | 0 | 0 / 1 | **Yes — the only subscribers** |
| `esp_timer` | IDF | 3584 | 22 | 0 (`CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0`) | No |
| `Tmr Svc` | FreeRTOS | 2048 | 1 | no affinity (`0x7FFFFFFF`) | No |
| `tiT` (lwIP) | IDF | 3072 | 18 | no affinity (`0x7FFFFFFF`) | No |
| `sys_evt` | IDF | 2304 | 20 | — | No |
| `ipc0` / `ipc1` | IDF | 1024 each | 24 | 0 / 1 | No |
| `BTC_TASK` | Bluedroid | 8192 (`CONFIG_BT_BTC_TASK_STACK_SIZE`) | — | 0 (`CONFIG_BT_BLUEDROID_PINNED_TO_CORE=0`) | No |
| `BTU_TASK` | Bluedroid | 4352 | — | 0 | No |
| BT controller | — | — | 23 | 0 (`CONFIG_BT_CTRL_PINNED_TO_CORE=0`) | No |
| `wifi` | IDF | — | 23 | 0 (`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0`) | No |
| `main` (`app_main`) | IDF | 8192 | 1 | 0 (`CONFIG_ESP_MAIN_TASK_AFFINITY=0`) | No — exits after `app_main` |

BLE stack is **Bluedroid** (`CONFIG_BT_BLUEDROID_ENABLED=y`, NimBLE not set), so the
NimBLE-Arduino `nimble_host`/`ll` tasks in `hardwareone_libs` are not built.

---

## 7. Duplicate create sites (same task name, >1 site)

| Name | Sites | Why |
|---|---|---|
| `sensor_queue_task` | `HardwareOne.cpp:1611` (setup) + `System_I2C.cpp:3052` (`processAutoStartSensors` late-init) | Both guard on `!queueProcessorTask`, so no double-create. |
| `g2_wifi_pending` | `G2_Page_Network.cpp:1134` (WiFi toggle) + `:1360` (connect-saved) | Both guard on `!gWifiPendingTaskActive`. |
| `g2_img_probe` | `G2_Page_TestSuite.cpp:1094` (internal stack) + `:1114` (PSRAM-stack fallback) | Mutually exclusive: `:1114` only runs when `:1094` returns `!= pdPASS`. |

---

## 8. Anomalies surfaced during recon (recon notes — not filed findings)

**A. `sensor_queue_task` name is truncated by FreeRTOS, breaking the name-keyed report. CONFIRMED.**
`CONFIG_FREERTOS_MAX_TASK_NAME_LEN=16` → 15 chars + NUL. `"sensor_queue_task"` is 17 chars,
so `pcTaskName` is stored as `"sensor_queue_ta"`. `System_Utils.cpp:3626` puts the literal
`"sensor_queue_task"` in its `appTasks[]` table and matches with
`strcmp(taskStatusArray[i].pcTaskName, appTasks[j].name)` at `:3645` — that comparison can
never be true, so the task is silently excluded from the "Application Task Stacks" report
and its 4096 B lands in the `system_tasks_total` bucket instead.
It is the **only** one of the 38 names longer than 15 chars (`g2_wifi_pending` and
`g2_ring_pending` are exactly 15 and fit). The other two reporters
(`System_TaskUtils.cpp:536` `reportAllTaskStacks`, `System_MemoryMonitor.cpp:277`) key on
`TaskHandle_t` and are unaffected.

**B. Three "stack arg is in WORDS" comments survive and are 4× wrong. CONFIRMED.**
The `System_TaskUtils.h` header exists specifically to kill this misreading, but three
sites still assert the opposite in prose:
- `G2_Glasses.cpp:11424` — *"xTaskCreate's third parameter is in WORDS (4 bytes), not bytes,
  despite what older comments throughout this file claim… 6400 words = 25 KB"*. Real value: **6400 B**.
- `System_Camera_DVP.cpp:988` — *"stack arg is in WORDS… 10240 here is 40 KB"*. Real: **10240 B**.
- `G2_Glasses.cpp:8917` — *"Stack in WORDS (4 bytes)… 5120 words = 20 KB"*. Real: **5120 B**.

Same class, in the web server: `WebServer_Server.cpp:5291-5293` and `:5207-5210` claim
`httpd_config_t.stack_size` is in words (*"11059 words = 44 KB"*, *"7680 words = 30 KB"*).
Traced to IDF: `httpd_main.c:530-535` passes `hd->config.stack_size` straight to
`httpd_os_thread_create` → `xTaskCreatePinnedToCore`, i.e. **bytes**. The HTTPS server task
really gets **10.8 KB**, the HTTP one **7.5 KB** — not 44/30 KB. Every headroom figure in
those two comment blocks ("~14 KB headroom", "~12 KB headroom, 40%") is therefore fiction;
the HTTP path's own stated ~18 KB measured peak would not even fit in 7680 B.

**C. `g2_img_probe`'s PSRAM-stack fallback frees the stack it is running on. CONFIRMED (code-read).**
`G2_Page_TestSuite.cpp:1049-1056`: `imgProbeWorker` calls `heap_caps_free(ctx->stack)` and
`heap_caps_free(ctx->tcb)` **while still executing on that stack**, then `vTaskDelete(nullptr)`.
The frees return through the freed stack and the scheduler then touches the freed TCB.
Also relevant: this path deliberately places a task stack in PSRAM
(`MALLOC_CAP_SPIRAM`, `:1108`), which `docs/STACK_TO_PSRAM_CANDIDATES.md:208-210` already
calls out as *"not a precedent to copy — a fallback that only fires when the internal
allocation fails."* Board: any build with `ENABLE_BLUETOOTH && ENABLE_G2_GLASSES` (i.e.
FeatherS3 today). Only reachable when the internal 8 KB create fails.

**D. `LLM_VIEW_STACK_WORDS` is dead. CONFIRMED.**
`System_TaskUtils.h:117` defines `LLM_VIEW_STACK_WORDS = 6144` with a long sizing narrative.
It is referenced by **zero** call sites — the G2 LLM viewer no longer spawns its own worker
(`OLED_Mode_LLM.cpp:21`: *"No private xTaskCreate'd worker"*). Similarly `MIC_VIZ`,
`SR_SNIP`, `MAP_RENDER` etc. are live, but `LLM_VIEW` is orphaned.

**E. `httpd` is the only unpinned task in the system, and undocumented as such. CONFIRMED.**
`HTTPD_DEFAULT_CONFIG()` sets `.core_id = tskNO_AFFINITY` (`esp_http_server.h:56`) and
neither `WebServer_Server.cpp:5287` nor `:5200` overrides it. The core-split policy permits
`tskNO_AFFINITY` only "as a *documented* choice"; there is no such note at either site.
A 7.5–10.8 KB, priority-5 web-request task floating onto Core 1 is the one placement the
policy's own reasoning would want decided deliberately.

**F. `ENABLE_ONDEVICE_LLM` value and comment disagree. CONFIRMED (working tree).**
`System_BuildConfig.h:302-306`: the comment says *"CURRENT: on for the FeatherS3"* while the
line reads `#define ENABLE_ONDEVICE_LLM 0`. Since `CMakeLists.txt:423` regex-greps this exact
line to decide whether to compile `System_LLM*.cpp`, the LLM subsystem (and the 12 KB
`llm_gen` stack) is currently **out of the build**. Note `System_BuildConfig.h` is one of the
files listed as modified in `git status`, so this may be an in-progress edit.

**G. `fmRadioTask` is the only sensor poller not using `SENSOR_TASK_EXIT`.**
`i2csensor_rda5807.cpp:295` calls bare `vTaskDelete(nullptr)`. Functionally equivalent, but
it skips the `INFO_FMRADIO_LIFECYCLEF("Task disabled…")` line the macro emits, so the
per-sensor Lifecycle debug toggle does not cover FM-radio task exit. Cosmetic/consistency.

**H. `g2_hb_worker` is the one task whose stack size is chosen at runtime.**
`G2_Glasses.cpp:8137-8154` steps 3072 → 2560 → 2048 based on
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)`, and below
`stackBytes + 1200` it declines to start at all, logging *"heartbeats will not run."*
This is a real feature-loss-under-DRAM-pressure gate and the only such gate in the codebase;
worth knowing when reading any heartbeat-stopped-working report.
