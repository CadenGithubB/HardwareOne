# feather_esp32_v2 — DRAM Exhaustion Audit

Generated 2026-08-18 against build-feather_esp32_v2 (v0.99.89, ESP-IDF v5.5.1).
70 findings raised by 6 parallel investigators; 47 survived adversarial verification, 23 refuted.

---

# DRAM Exhaustion on `feather_esp32_v2` — Engineering Report

Board: Adafruit Feather ESP32 V2 (ESP32-PICO-V3-02, dual-core LX6, 2 MB quad PSRAM, 8 MB flash)
Build: `/Users/morgan/esp/hardwareone-idf/build-feather_esp32_v2`

---

## 1. THE ARITHMETIC

### 1a. The 265,031 B headline is wrong by ~59 KB. Fix the measurement first.

The boot report prints `ESP.getHeapSize()` (`components/hardwareone/System_Utils.cpp:3720`), and Arduino defines that as `heap_caps_get_total_size(MALLOC_CAP_INTERNAL)` — **no `MALLOC_CAP_8BIT`** (`components/arduino/cores/esp32/Esp.cpp:159-165`). Two consequences:

1. It includes the **IRAM-only pool**, `_iram_end = 0x40099818` (map:182781) → `0x400A0000` = **26,600 B**. That memory carries `MALLOC_CAP_INTERNAL|EXEC|32BIT` (`esp-idf/components/heap/port/esp32/memory_layout.c:55`) but not `MALLOC_CAP_8BIT` and not `MALLOC_CAP_DEFAULT`. No `malloc()`, no task stack, no BT buffer can ever land there.
2. It **double-counts the SPIRAM DMA reserve pool**. `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768` (`build-feather_esp32_v2/sdkconfig:1449`) is `heap_caps_malloc`'d out of the DRAM heap and then re-registered as its own heap (`esp-idf/components/esp_psram/system_layer/esp_psram.c:498-524`). `heap_caps_get_total_size` sums `heap->end - heap->start` per registered heap (`esp-idf/components/heap/heap_caps.c:265-275`), so those 32,768 B are counted once as *allocated* in the parent and once as the child heap's span.

### 1b. Real budget, reconstructed from the map + IDF layout (adds up to within 5 B)

DRAM-addressable SRAM on ESP32: `0x3FFAE000 … 0x40000000` = **335,872 B**.

| Consumer | Bytes | Source |
|---|---:|---|
| ROM data reserve `0x3ffae000–0x3ffae6e0` | 1,760 | `memory_layout.c:146` |
| BT ROM data `0x3ffae6e0–0x3ffaff10` | 6,192 | `bt/controller/esp32/bt.c:451`, `esp_bt.h:23-24` |
| BT EM (BR/EDR + BLE exchange) `0x3ffb0000–0x3ffb6388` | 25,480 | `bt.c:448`, `esp_bt.h:25,34,49` |
| BT ROM bss `0x3ffb8000–0x3ffb9a20` | 6,688 | `bt.c:449`, `esp_bt.h:42-43` |
| BT ROM misc `0x3ffbdb28–0x3ffbdb5c` | 52 | `bt.c:450`, `esp_bt.h:44-45` |
| ROM PRO data `0x3ffe0000–0x3ffe0440` | 1,088 | `memory_layout.c:140` |
| ROM APP data `0x3ffe3f20–0x3ffe4350` | 1,072 | `memory_layout.c:143` |
| **Fixed ROM/BT-silicon reserves** | **42,332** | |
| **App statics** `_data_start 0x3ffbdb60 → _heap_low_start 0x3ffd32a0` | **87,872** | map:71074, map:182786 |
| **= Real 8-bit internal heap at `heap_init`** | **205,668** | 335,872 − 42,332 − 87,872 |

Cross-check against the printed number: 205,668 + 26,600 (IRAM) + 32,768 (double count) = **265,036** vs the reported **265,031**. Five bytes of region alignment. The model is correct.

Corroborating: the boot log's two D/IRAM regions "3FFE0440 14KB" and "3FFE4350 111KB" are exactly `0x3ffe0440–0x3ffe3f20` = 15,072 and `0x3ffe4350–0x40000000` = 113,840.

### 1c. Where the 87,872 B of statics go

Note this is **not** the 74,317 B figure in the brief — that parse missed `.dram1.*` (6,514 B, incl. `port_IntStack` 3,072) and DRAM-resident `.rodata` (~5,543 B) and `*fill*` (~1,498 B). By archive:

| Archive | Internal DRAM |
|---|---:|
| `libhardwareone.a` | 48,010 (54.6%) |
| `libbt.a` | 4,717 (4,380 of it `hli_vectors.S`) |
| `libfreertos.a` | 4,179 |
| `libbtdm_app.a` | 3,640 (2,048 = `vflash_mem`) |
| `libarduino.a` | 3,316 |
| `espressif__esp32-camera` | 3,100 (all of it `to_bmp.c` `work[3100]`) |
| `libhal.a` / `libphy.a` / `libpp.a` | 2,934 / 2,896 / 2,639 |

PSRAM statics are **387,828 B** (`.ext_ram.bss`, map:72525, `0x5eaf4`), not 15,937 — the runtime report is right, the earlier map parse was looking for `.dram0.*` names. 81 source files already carry `EXT_RAM_BSS_ATTR`.

### 1d. End of boot — the honest numbers

Reported: total 265,031 / used 156,060 / free 108,971.

| | Reported (`CAP_INTERNAL`) | Real (`CAP_INTERNAL|8BIT`) |
|---|---:|---:|
| Total | 265,031 | **205,668** |
| Used | 156,060 | **~123,292** (−32,768 double count) |
| Free | 108,971 | **~82,371** (−26,600 IRAM) |
| Free, reachable by plain `malloc()` | — | **~49,600** (soft; excludes the DMA reserve pool, whose caps array `{0, DMA|INTERNAL, 8BIT|32BIT}` at `esp_psram.c:516` carries no `MALLOC_CAP_DEFAULT`, so `heap_caps_malloc_default` can never be served from it — `heap_caps_base.c:147`) |

Where the ~123,292 B used at end of boot goes:

| Bucket | Bytes | Confidence |
|---|---:|---|
| FreeRTOS task stacks + TCBs | ~65,000 | high — itemized below |
| WiFi driver internal (static RX DMA ×4, mgmt, RX descriptors, netif) | ~20,000–25,000 | **soft** |
| App heap (`sEventBuf` 7,872 at `System_Automation.cpp:3962`, settings Strings, users.json, ArduinoJson pools) | ~15,000–20,000 | **soft** |
| LittleFS caches (1,152) + NVS (~2,200) + I2C/UART/SPI driver objects | ~5,000 | medium |
| LWIP + httpd control blocks (`hd_calls` 640, `hd_sd` 1,344, esp_netif ~520) | ~3,000 | high |
| Heap metadata + fragmentation | ~3,000 | soft |

Task stacks, itemized (all bytes; `portSTACK_TYPE` is `uint8_t`, `freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos/portmacro.h:88`; all stacks are pinned `MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT` by `freertos/heap_idf.c:43`):

`loopTask` 8,192 · `cmd_exec` 8,192 · `httpd` 7,680 · `wifi` ~6,656 (UNVERIFIED) · `esp_timer` 4,096 · `arduino_events` 4,096 · `debug_out` 4,096 · `sensor_queue` 4,096 · `tiT` 3,584 · `gamepad` 3,584 (only if seesaw present) · `ipc0/1` 1,536×2 · `IDLE0/1` 1,536×2 · `sys_evt` 2,816 · `Tmr Svc` 2,048 · ~14 TCBs ~2,800.

Note `main_task` (8,704) is **dead** by end of boot — this is Arduino-as-component, `app_main` creates `loopTask` and returns (`components/arduino/cores/esp32/main.cpp:113`), and the main task self-deletes (`esp-idf/components/freertos/app_startup.c:210`).

### 1e. Is 108,971 B free at boot NORMAL, or already bloated?

**Neither — the number is fictitious and the real one is worse.** Real free is ~82 KB, and only ~50 KB is reachable by `malloc()`.

Is that bloated? **No leak, but structurally over-committed.** Before a single feature runs, this firmware has spent 87,872 B on statics and ~65,000 B on task stacks = **152,872 B, or 45.5% of the chip's entire 335,872 B of data SRAM**. That is not a bug; it is 20 tasks and 48 KB of first-party statics on a part that has 205,668 B of usable heap. The firmware was designed against the FeatherS3 budget and ported down.

---

## 2. ROOT CAUSE

`g2init` consumes **76,260 B** of real 8-bit internal DRAM (82,371 free → 6,111 free at the failure; the `internal=6111` figure comes from `System_TaskUtils.cpp:168`, which uses `MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT`).

In the same window PSRAM went 1,565,848 → 1,488,820 free = **77,028 B absorbed by PSRAM**. That is the Bluedroid host, and it proves the SPIRAM-first path is already working perfectly. `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y` makes `osi_malloc_base` = `heap_caps_malloc_prefer(size, 2, DEFAULT|SPIRAM, DEFAULT|INTERNAL)` (`bt/common/osi/include/osi/allocator.h:34-35`), and combined with `CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY=y` and `dyn_mem.h:24-43` (BTU/BTM/L2C/GATT/SMP/BTA/SDP/GAP all `_DYNAMIC_MEMORY = TRUE`), essentially the entire host lives in PSRAM. `libbt.a` contributes only 8,919 B of `.dram0` total.

**What is internal-DRAM-only on ESP32 classic and cannot be moved by any config:**

| Item | Bytes | Why it cannot move |
|---|---:|---|
| BT task stacks: controller 4,096 + BTC 8,704 + BTU 4,864 + hci_host 2,560 | 20,224 | `pvPortMalloc` hard-codes `MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT` (`freertos/heap_idf.c:43`). Sizes = Kconfig + `BT_TASK_EXTRA_STACK_SIZE` 512 (`esp_task.h:33-39`; `CONFIG_NEWLIB_NANO_FORMAT` unset). |
| 4 BT TCBs + BTC/BTU work queues (both length 0 → promoted to 100 × 8 B, `osi/thread.c:62,230,27-30`) | ~3,100 | FreeRTOS objects, same pin |
| Controller per-connection state, `CONFIG_BTDM_CTRL_BLE_MAX_CONN=3` | 3,072 | "Each connection uses 1KB static DRAM" (`bt/controller/esp32/Kconfig.in:17-24`); `BLE_CONTROLLER_MALLOC_CAPS = MALLOC_CAP_8BIT|MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL` (`bt.c:103`) — `BT_ALLOCATION_FROM_SPIRAM_FIRST` does **not** apply to the controller, only the host |
| Controller blob pools: HCI buffers, ACL buffers, scan-duplicate cache (100 entries), adv-report flow control | ~20,000 (soft) | Same `BLE_CONTROLLER_MALLOC_CAPS`; radio DMA, physically cannot be cache-backed PSRAM |
| `libbtdm_app.a(vflash.o)` `.bss.vflash_mem` | 2,048 | Blob static, inside `_bt_controller_bss` (map:78007-78167) |
| G2 worker stacks created inside `initG2Client`: `g2_page_swap_w` 8,192 (`G2_Glasses.cpp:16623`) + `g2_tap_disp` 10,240 (:17061) + `r1_owner` 6,144 (`G2_Ring.cpp:3926`) + 3 TCBs | ~25,176 | Project rule + `heap_idf.c:43` |

That is ~73.6 KB of the 76.3 KB observed. There is no allocation-policy knob that touches any of it.

**Then the specific failure:** `g2SessionEnsureWorker()` is the **last** call in `initG2Client` (`G2_Glasses.cpp:12972`), asking for `kG2SessionStackBytes = 10240` (:18386) after everything above has already landed. Largest free block was 5,888 B. It fails.

Two aggravating details:

- **`esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)` is never reached.** The live path is `BLEDevice::init` → `btStart()` via the `#if defined(ARDUINO_ARCH_ESP32)` branch at `components/arduino/libraries/BLE/src/BLEDevice.cpp:373-377`. The correct release at `BLEDevice.cpp:395` is in the `#else` branch and is not compiled. `esp32-hal-bt.c:49-90` only ever calls `esp_bt_controller_mem_release(ESP_BT_MODE_BLE)`, and only when `cfg.mode == ESP_BT_MODE_CLASSIC_BT` (never — `CONFIG_BTDM_CTRL_MODE_BLE_ONLY=y`). The boot-time release at `esp32-hal-misc.c:312` is gated on `!btInUse()`, and `esp32-hal-bt.c:20-23` defines a **non-weak** `btInUse(){return true;}` for `CONFIG_IDF_TARGET_ESP32`. `components/hardwareone/BLE_IDF.cpp:673` does call it correctly, but `bleIdfInit()` has **zero callers** (confirmed by grep and by the in-tree comment at `G2_Glasses.cpp:12897-12899`); `BLE_IDF.cpp.obj` contributes 0 B to `.dram0` — linker-GC'd. So **15,448 B** of BR/EDR exchange memory (`0x3ffb2730–0x3ffb6388`) stays reserved forever on a BLE-only build.
- **The log line lies.** `System_TaskUtils.cpp:179` prints "feature unavailable this boot", but `g2SessionEnsureWorker` resets `gSessionInitState = Uninitialized` on failure (`G2_Glasses.cpp:18510`) and `g2SessionSubmit` retries at :18593. Likewise `g2-fsm` falls back to inline dispatch (`G2_HijackFsm.cpp:336`).

---

## 3. THE FIX LIST

> **Delivery warning.** `tools/build_board.sh:66` pins `-DSDKCONFIG="$BUILD_DIR/sdkconfig"`, and a saved `build-feather_esp32_v2/sdkconfig` already exists. Adding keys to `sdkconfig.defaults` **does nothing**. Put board-specific keys in `boards/feather_esp32_v2.defaults` (`components/hardwareone/CMakeLists.txt:324-390` strips and re-applies any `CONFIG_` key the board file names) — this already works in-tree: `boards/qtpy_esp32.defaults:35` has `CONFIG_BT_ENABLED=n` vs `boards/xiao_s3.defaults:53` `=y`.

### TIER 1 — config / one-line, do first (25,088 B)

| # | Change | Bytes | Risk | HW retest |
|---|---|---:|---|---|
| 1 | **`esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)`** in `main/hardwareone-idf.cpp` `app_main()`, after `otaSafetyInitEarly()` and **before** `initArduino()` (controller must be `ESP_BT_CONTROLLER_STATUS_IDLE`, `bt.c:1411`). Region is `SOC_MEM_BT_EM_BREDR_START 0x3ffb2730` → `REAL_END 0x3ffb6388` (`esp_bt.h:33,34,49`; `BR_EDR_MAX_SYNC_CONN_EFF=0`), added via `try_heap_caps_add_region` (`bt.c:1354-1405`). **Directly fixes the reported failure**: a fresh contiguous 15,448 B block satisfies the 10,240 B request that saw `largest=5888`. | **15,448** | low | **yes** |
| 2 | `CONFIG_BTDM_CTRL_HLI=n`. `libbt.a(hli_vectors.S.obj) .data` = 4,380 B (`hli_vectors.S:20,31-40`: 4,096 stack + 284 reg-save), entire file under `#if CONFIG_BTDM_CTRL_HLI` (:15); plus ~148 B of `hli_api.c` `.dram1`. Plain `bool`, `default y`, freely settable (`bt/controller/esp32/Kconfig.in:551-556`). | **4,528** | **medium** — BT controller drops from int level 4 to 3 and can be blocked while flash cache is disabled. This firmware writes LittleFS constantly. | **yes — mandatory.** Sustained log write with both temples + ring connected. |
| 3 | `CONFIG_BT_BTC_TASK_STACK_SIZE` 8192 → 5120. IDF default is **3072** (`bt/host/bluedroid/Kconfig.in:1-4`) — the in-tree comment at `sdkconfig.defaults:88-93` claiming 8192 is the default is false. The reason for the bump (tap dispatcher on BTC reaching `g2ShowText`) is gone: work is deferred via `tapDispatcherEnqueue*` (`G2_Glasses.cpp:1068-1070`, call sites 4445/4512/4541/4679/4719). Do **not** go to 3072. | **3,072** | medium | **yes** — `uxTaskGetStackHighWaterMark` on BTC after G2 hijack menus + camera stream + R1 reconnect; keep ≥1,500 B headroom |
| 4 | `CONFIG_NVS_ALLOCATE_CACHE_IN_SPIRAM=y`. Currently unset (`sdkconfig:2253`). Flips `ExceptionlessAllocatable::operator new` from `std::malloc` to `heap_caps_malloc_prefer(SPIRAM, then INTERNAL)` (`nvs_memory_management.hpp:57-63`) — PSRAM-first with automatic internal fallback, cannot fail where it succeeds today. | ~1,200 **(soft)** | low | no |
| 5 | Fix a **dead config key**: `sdkconfig.defaults:123` sets `CONFIG_BT_CTRL_SCAN_DUPL_CACHE_SIZE=30`, which does not exist on ESP32 classic. `sdkconfig:845` still reads `CONFIG_BTDM_SCAN_DUPL_CACHE_SIZE=100`, and `config/sdkconfig.h:461` confirms. Use `CONFIG_BTDM_SCAN_DUPL_CACHE_SIZE=30`. Delete line 122 (`CONFIG_BT_CTRL_BLE_MAX_ACT`, also nonexistent here) and fix the comment block at :112-121, which documents a tuning that never took effect. | ~840 **(soft — per-entry size is inside `libbtdm_app.a`, UNVERIFIED)** | low | no |
| 6 | **Zero bytes, high value:** move `g2SessionEnsureWorker()` from `G2_Glasses.cpp:12972` to **before** `BLEDevice::init` at :12873, so the 10,240 B contiguous block is claimed while DRAM is whole. The comment at :12969 already states this was the intent; the call site drifted. | 0 | low | yes |

**Running total after Tier 1: 25,088 B** (~2,040 soft). Critically, ~15.4 KB of it is a *fresh contiguous region*, not scattered fragments.

### TIER 2 — code changes, low-to-medium (28,831 B)

| # | Change | Bytes | Risk | HW retest |
|---|---|---:|---|---|
| 7 | `System_Events.h:321-327`: `SYSEVT_RING_SIZE` 48→32, `DETAIL_LEN` 80→64, `SUBJECT_LEN` 48→40, `WHO_LEN` 24→16. `sizeof(SystemEvent)` 164→132. Hits **two** buffers: `gEventRing` (`System_Events.cpp:123`, 7,872 B static) and the never-freed `heap_caps_calloc(..., MALLOC_CAP_INTERNAL)` at `System_Automation.cpp:3962` (another 7,872 B). Both must stay internal — `System_Automation.cpp:3929-3935` documents why (filled inside `taskENTER_CRITICAL`; `SYSEVT_REMOTE_CMD_RX` carries raw command text). Width-only shrink still yields 3,072 B. | **7,296** | low | light |
| 8 | `G2_Glasses.cpp:12956`: make `g2RingInit()` conditional/lazy, or split it so `bleRegisterPeer(ringPeerSpec)` (`G2_Ring.cpp:3916`) stays eager and `xTaskCreateLogged("r1_owner", 6144, …)` (`G2_Ring.cpp:3926`) moves into `ringPerformConnect()` (which already calls `g2RingInit()` at :3393/:3449). The in-source comment at :12951-12955 claiming ring init is "just a mutex + spec-publish" is wrong — it also runs two self-tests and spawns a permanent task. **This runs 16 lines before the allocation that fails.** | **6,340** | low | **yes** — verify `ringconnect` still works |
| 9 | `System_R1_Protocol.cpp:1419` `static R1ActivityDailyResult activityScratch` (2,328 B, one-shot boot self-test called only from `G2_Ring.cpp:3921`) → `MALLOC_CAP_SPIRAM` + free. `System_R1_Protocol.cpp:1253` `static … parsed` (2,328 B, debug-only annotator) → gate the caller at `G2_Ring.cpp:2483` behind `debugRingProtocolEnabled()` (it currently runs a 144-record parse on **every** ring RX and throws it away) and make the buffer transient `MALLOC_CAP_INTERNAL` — **not** PSRAM, it holds decoded health records. | **4,652** | low | light |
| 10 | esp32-camera's `static uint8_t work[3100]` (`managed_components/espressif__esp32-camera/conversions/to_bmp.c:35`) is resident on a board with no camera. `ENABLE_CAMERA_SENSOR=0` (`System_BuildConfig.h:181-188`), but map:996-999 names the puller: `G2_Glasses.cpp.obj (fmt2rgb888)` — the JPG **file** viewer at :27310 (also :26292, :26722). Replace with a direct `esp_jpeg_decode()` using `.advanced.working_buffer` from `heap_caps_malloc(3100, MALLOC_CAP_SPIRAM)` (settings per `to_bmp.c:65-79`); also drops `to_bmp.o`/`yuv.o` from the link. Cheap alternative: `EXT_RAM_BSS_ATTR` on that one line as a vendored patch. | **3,100** | low | yes — open a JPG on the lens |
| 11 | `System_UartLink.cpp:1210-1211` `sFrameBody` (1,031) + `sFrameWire` (1,039). **DONE 2026-08-19 for `sFrameBody` only.** Two corrections to the original row, from adversarial re-verification: (a) the secrets caveat was **WRONG** — CLI text never passes through these buffers (inbound lines accumulate in `sUartCLI`; outbound text goes via `uartTxLocked`/`uartWriteLine` on their own buffers); only binary AUDIO/META/EVT/LIVE frames are built here, so there is no credential exposure; (b) `sFrameWire` is memcpy'd out **inside the esp_ringbuf spinlock** (interrupts masked on the writing core) in ≤1 KB chunks — correctness-safe but it lengthens worst-case interrupt latency during voicefetch, so it was deliberately **left internal**. | **1,031** (of 2,070) | low | light |
| 12 | `gSettings` (`HardwareOne.cpp:410`, 1,732 B) → `EXT_RAM_BSS_ATTR`. The "every `DEBUG_*` macro reads it" objection is **false** — debug gates read `gDebugFlags` (`System_Debug.cpp:59`, `System_Debug.h:452-462`); the `gSettings.debug*` members are the persisted shadow (`HardwareOne.cpp:1481`), with one non-settings reader at `System_ESPSR.cpp:323-329`. Its secret members are `String`, so the bytes stay on the internal heap. | **1,732** | medium — **UNVERIFIED**: static-ctor vs PSRAM-init ordering not re-confirmed on this build | **yes** |
| 13 | `managed_components/espressif__libsodium/…/crypto_core/softaes/softaes.c:9`: add `const` to `uint32_t _aes_lut[256]`. Only use is read-only (`softaes.c:44`). Moves 1,024 B to `.flash.rodata`. Check the matching `softaes.h` extern. Vendored patch — record with `docs/arduino-local-patches`. | **1,024** | low | no |
| 14 | `System_Utils.h:102`: `size_t count;` → `const size_t* count;` and take addresses in the ~30 initializers at `System_Utils.cpp:2875-3522`. The array is `const` but lands in `.data` because `extern const size_t xxxCommandsCount` reads are not constant expressions — objdump shows `_GLOBAL__sub_I__Z19isEspNowInitializedv` (231 B) patching offset +16 with stride 28 at boot. Also removes that constructor. | **840** | low | no |
| 15 | `components/hardwareone_libs/liblc3/src/tables.h:104,114` (+5 more) and matching definitions at `tables.c:4040,4325`: add the second `const` to the pointer arrays (`extern const float * const lc3_mdct_win[…]`). In-repo vendored source, patch sticks. | **620** | low | no |
| 16 | `System_SensorStubs.cpp:228` `MeshPeerHealth gMeshPeers[16]` → `nullptr` pointer, and `System_ESPNow.h:1444` extern to match the pointer shape the real ESP-NOW build already presents. Every reader already null-guards (`System_ESPNow_Router.cpp:151`, `System_Automation.cpp:3131,3292,3307`). | **572** | low | **compile ESPNOW=1 too** |
| 17 | `System_Filesystem.cpp:66` `static uint8_t buf[512]` → `buf[64]`. Post-mount-failure path only (comment at :60-63); the scan bails on the first non-0xFF byte. | **448** | low | no |
| 18 | `G2_Page_ESPNow.cpp:13`: add `&& ENABLE_ESPNOW` to the guard, plus a matching `#if` around its `g2RegisterPage()` call. Do **not** `#if`-wrap the registry row — register a stub or renumber. | **137** | low | yes |

**Running total after Tier 2: 53,919 B.**

### TIER 3 — structural

| # | Change | Bytes | Risk |
|---|---|---:|---|
| 19 | `EXT_RAM_BSS_ATTR` sweep of the remaining `libhardwareone.a` internal statics (48,010 B total, 54.6% of all statics). 81 files already use the idiom on this board; PSRAM has 1.5 MB free. Must exclude secrets: `gSc` 512 B (`System_BleSecureChannel.cpp:60-70`, holds `ephSec/kC2D/kD2C`) and `gPsk` 32 B are hard no-moves. | **10,000–20,000 (SOFT, unenumerated)** | medium |
| 20 | Move `debug_out`'s inline `appendLineWithCap()`/LittleFS write to a small write queue drained elsewhere, then `DEBUG_OUT_STACK_WORDS` 4096→2560 (`System_TaskUtils.h:66`). **Do not trim the stack first** — it is at 74% used (3,048/4,096), below the 1.5× floor. | **1,536** | medium |
| 21 | IDF system task stacks: `CONFIG_ESP_IPC_TASK_STACK_SIZE` 1536→1024 (×2 cores), `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE` 1536→1024 (×2), `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH` 2048→1536 (**1024 is illegal — `freertos/Kconfig:219` `range 1536 32768`**). Do **not** touch `CONFIG_ESP_TIMER_TASK_STACK_SIZE`: `esp_timer` dispatches BTDM callbacks and its post-BT-init depth is unmeasured. Do not trim `CONFIG_ESP_MAIN_TASK_STACK_SIZE` — that task is already dead. | **2,560** | medium — margins were sampled **pre-BT-init**; needs a post-`g2init` HWM soak first |
| 22 | `CONFIG_FREERTOS_ISR_STACKSIZE` 1536→1024 (`port_IntStack`, map:72062, 2 cores) | 1,024 | medium — nested WiFi+BT ISR depth |
| 23 | **Bluedroid → NimBLE: REJECT.** The usual "~30 KB" assumes Bluedroid's control blocks are in internal BSS. They are not here (see §2), so the realistic gain is ~12 KB: dropping BTC 8,704 + BTU 4,864 + hci_host 2,560 and adding NimBLE's host task ~4,096, partly given back by NimBLE `os_mempool` buffers. Against that: 10 files use Bluedroid/Arduino-BLE APIs; `G2_Glasses.cpp` is 30,343 lines with 58 `BLEClient`/`BLERemote` references, `G2_Ring.cpp` 5,283 lines with 14, `Bluetooth.cpp` 3,103 lines; 22 `esp_ble_gap_` sites, 6 `esp_ble_gattc`, 7 `esp_ble_gatts`; and you would discard hardware-validated machinery with no NimBLE analogue — `BLEDevice::deinitChecked`/client quarantine (`BLEDevice.cpp:1180-1260`), the GAP hook at `G2_Glasses.cpp:12890-12900`, and the per-link MTU patches. Tier 1 items 1+2+3 give 23,048 B for a two-line config change and a one-line call. | (12,000, rejected) | high |

**Running total after Tier 3 (excluding NimBLE): ~69,000–79,000 B.**

### Zero-byte but mandatory — instrumentation is currently lying to you

| Where | Defect |
|---|---|
| `System_Utils.cpp:3720-3726` | Boot report uses `ESP.getHeapSize()/getFreeHeap()` = `MALLOC_CAP_INTERNAL`, which includes 26,600 B of unusable IRAM and double-counts the 32,768 B DMA reserve. **Switch to `MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT` and additionally print `heap_caps_get_free_size(MALLOC_CAP_DEFAULT|MALLOC_CAP_INTERNAL)`** — that last query excludes both the reserve pool (no `MALLOC_CAP_DEFAULT` in its caps) and PSRAM, and is the number that actually predicts a `malloc` failure. |
| `System_MemoryMonitor.cpp:446,452` and `System_TaskUtils.cpp:160,168,179` | Same: `INTERNAL|8BIT` silently folds in the reserve pool (`heap_caps.c:421` matches on superset). True malloc-reachable free was **lower** than the reported 963 B, and the `largest=5888` block was probably *inside* the reserve heap. |
| `System_Utils.cpp:3934,3973` | `strcmp(pcTaskName, appTasks[j].name)` — FreeRTOS truncates names to 15 chars, so `"sensor_queue_task"` (17) never matches. Use `strncmp(…, configMAX_TASK_NAME_LEN-1)` as `System_TaskUtils.cpp:113,131` already does. Better: delete the hardcoded `appTasks[]` at :3913-3923 (it lists 6 tasks compiled out on this board and omits all six G2 workers, `r1_owner`, `cm5_presence`, `mic_record`, `mic_viz`, `i2c0_begin`, `ota_probation`) and call `taskStackLookup()` instead. |
| `System_Utils.cpp:3980-3985` | System tasks print only HWM (free bytes), so `main`/`httpd`/`wifi`/`tiT` contribute **0** to every total the report prints. |
| `WebServer_Server.cpp:5622-5626, 5706-5720` | Comments assert `stack_size` is in **WORDS**. It is **BYTES** (`esp_http_server/src/port/esp32/osal.h:30` → `xTaskCreatePinnedToCoreWithCaps`). Real stacks are 11,059 B / 7,680 B, not 44 KB / 30 KB. **Landmine:** anyone "correcting" this would multiply by 4 and consume 30–44 KB. Also delete the "~18 KB measured peak" claim — impossible against a 7,680 B stack; boot HWM shows ~908 B used. If that 18 KB figure was ever real, 7,680 B is a latent overflow and must go **up**. |
| `System_TaskUtils.cpp:179` | "feature unavailable this boot" is false for `g2.session` (retries at `G2_Glasses.cpp:18593`) and `g2.fsm` (inline fallback, `G2_HijackFsm.cpp:336`). Pass a `retryable`/fallback string through `xTaskCreateLogged`. |
| `BLE_IDF.cpp` | Fully dead (`bleIdfInit()` has zero callers; 0 B in `.dram0`) yet its comment at :672-673 advertises the CLASSIC_BT release. **This is why the 15,448 B bug survived every grep-based audit.** Delete the file and `CMakeLists.txt:187`, or mark it dead at the top. |

---

## 4. THE HONEST VERDICT

**Yes, this board can run WiFi + web server + BLE-to-G2 simultaneously — but only with Tier 1 items 1, 2, 3 and 6 landed, and only as a reduced-feature target.**

The arithmetic after Tier 1:
- Free 8-bit internal DRAM at end of boot: ~82,371 → **~107,459 B**
- `g2init` cost: ~76,260 → **~68,660 B** (BTC stack −3,072, HLI −4,528, dupl cache −840, plus items 8/10 in Tier 2 remove another ~9.4 KB from that window)
- Free after `g2init`: 6,111 → **~38,800 B**, with a fresh 15,448 B contiguous region on top

That is a working device with real margin, not a device 963 B from death. **Item 1 alone converts the reported crash into a success**: the failing allocation was 10,240 B against a largest free block of 5,888 B, and the released BR/EDR region is a single contiguous 15,448 B block.

**What it cannot be:** a device that also runs the camera JPEG path, ESP-SR, the on-board LLM, a 3-link BLE mesh *and* HTTPS concurrently. There is no configuration in which that fits — §2 shows ~73.6 KB of the BT cost is physically immovable.

**Against FeatherS3.** I measured both maps rather than assuming:

| | feather_esp32_v2 | FeatherS3 (ESP32-S3) |
|---|---:|---:|
| App statics (`_data_start` → `_heap_low_start`) | 87,872 | 89,200 |
| `dram0_0_seg` window | 0x1E6A4 = 124,580 | 0x53700 = 341,760 |
| Usable 8-bit internal heap | **205,668** | **~236,000** (approx — I did not enumerate S3 ROM reserves) |

**The S3 is only ~15% roomier in internal DRAM, not 2×.** The app's static footprint is essentially identical on both. The ESP32 loses 42,332 B to fixed ROM/BR-EDR carve-outs that the S3 (no BR/EDR silicon) simply does not have — which is exactly the 15,448 B item 1 recovers plus ~27 KB the ROM genuinely needs.

This changes the conclusion in an important way: **the DRAM problem is not primarily a chip problem, it is a firmware-wide budget problem that the S3 has been masking by ~30 KB.** Every fix in §3 that is not ESP32-specific (items 7, 9, 10, 11, 12, 13, 14, 15, 17, 19, 20) pays off on FeatherS3 and XIAO too, and the S3 boards are sitting on the same ~89 KB of statics and the same ~65 KB of task stacks.

**Recommendation:** treat `feather_esp32_v2` as a **reduced-feature target** — declare it BLE + WiFi + web + G2 only, with `ENABLE_R1_HEALTH`, camera/JPEG viewer, ESP-SR and the on-board LLM off — and land Tier 1 + Tier 2 items 7–10 as the supported configuration. Do not migrate to NimBLE.

---

## 5. WHAT I DID NOT VERIFY

**Needs hardware:**
1. That `esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)` in `app_main` actually returns `ESP_OK` and yields +15,448 B. Measure `heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)` and `heap_caps_get_largest_free_block(...)` immediately before and after.
2. **BLE stability with `CONFIG_BTDM_CTRL_HLI=n`** under sustained LittleFS writes with both temples + ring connected. This is the one change I would not ship blind.
3. BTC task HWM at 5120 after exercising G2 hijack menus, camera stream, and an R1 reconnect.
4. `httpd` task real peak (`uxTaskGetStackHighWaterMark`) under `/api/settings`, file upload, and a settings write — to determine whether 7,680 B is headroom or a latent overflow.
5. `gSettings` → `EXT_RAM_BSS_ATTR`: static-constructor vs PSRAM-init ordering on this build.
6. Post-`g2init` HWM soak on `esp_timer`, `ipc0/1`, `IDLE0/1`, `Tmr Svc` before any Tier 3 item 21 trim.

**Needs a build:**
7. Per-entry size of the controller scan-duplicate cache (inside `libbtdm_app.a`) — diff internal free right after `esp_bt_controller_init` at 100 vs 30. My ~840 B is an estimate.
8. NVS cache actual size — my ~1,200 B assumes a partially-populated 4-page (0x4000) NVS.
9. `ENABLE_ESPNOW=1` compile after the `gMeshPeers` pointer change (this repo has broken ESPNOW=0/1 parity before).
10. `softaes.h`'s extern declaration may need the matching `const`.

**Soft numbers in §1:**
11. The ~20–25 KB WiFi-driver and ~15–20 KB app-heap buckets in the end-of-boot table are inference, not measurement. `wifi` task stack ~6,656 B is unconfirmed.
12. The ~24,700 B "controller blob internal pools" residual in §2 is a subtraction, not an enumeration.
13. FeatherS3's ~236,000 B usable heap is `0x3FCF0000 − _heap_low_start` and does not subtract S3 ROM reserves; treat as ±5 KB.
14. My reconstruction reproduces the reported total to within 5 B, but I did not confirm on hardware which region loses those 5 B to alignment.
