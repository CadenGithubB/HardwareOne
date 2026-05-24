# Memory footprint report — hardwareone-idf

This document summarizes **flash**, **partition layout**, **runtime RAM policy**, and **task stack budget** for the current tree. Values reference repository sources and the built artifact `build/hardwareone-idf.bin` (May 3, 2026 build).

---

## 1. Target and build identity

| Item | Value |
|------|--------|
| Chip | ESP32-S3 |
| Board variant | `CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3"` (`sdkconfig.defaults.esp32s3`) |
| IDF (typical) | v5.3.x (see serial `boot:` line on device) |
| Compiler | `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`, stack check enabled (`sdkconfig`) |
| Bluetooth | Enabled (`CONFIG_BT_ENABLED=y`, Bluedroid, controller on) — **differs** from `sdkconfig.defaults.esp32s3` line 42 (`n`) if that file is not fully applied |
| PSRAM | Octal @ 80 MHz, `CONFIG_SPIRAM=y` |

Feature gates that affect **flash** and **RAM** are in `components/hardwareone/System_BuildConfig.h` (e.g. `ENABLE_ESP_SR`, `ENABLE_G2_GLASSES`, `ENABLE_BLUETOOTH`, sensor `ENABLE_*` flags). The **partition table** CMake logic copies either `partitions_sr.csv` or `partitions_no_sr.csv` into `partitions.csv` at configure time (`CMakeLists.txt`).

---

## 2. Flash footprint

### 2.1 Application binary

| Metric | Value |
|--------|--------|
| `build/hardwareone-idf.bin` size | **4,638,144 bytes (~4.42 MiB)** |

This is the padded image written to the `factory` partition; it must fit under that partition’s **Size** in `partitions.csv` (see §2.2).

### 2.2 Partition table (as committed: ESP-SR layout)

`partitions.csv` in the repo currently matches the **with ESP-SR** layout (see header comments):

| Partition | Offset | Size (hex) | Size (decimal) | Purpose |
|-----------|--------|------------|----------------|---------|
| `nvs` | 0x9000 | 0x6000 | 24 KiB | WiFi/BLE NVS |
| `phy_init` | 0xf000 | 0x1000 | 4 KiB | PHY calibration |
| `factory` | 0x10000 | **0x4E0000** | **5,046,272 B (~4.81 MiB)** | Application |
| `model` | 0x4F0000 | 0x2F0000 | 3,080,192 B (~2.94 MiB) | SPIFFS wake/word models (`ENABLE_ESP_SR`) |
| `littlefs` | 0x7E0000 | 0x20000 | 131,072 B (128 KiB) | LittleFS |

**Total flash span** used by these rows: from `0x9000` through `littlefs` end ≈ **8 MiB** (typical 8 MB flash).

When **`ENABLE_ESP_SR=0`**, CMake switches to `partitions_no_sr.csv`: same ballpark **factory** size, **no `model` partition**, and a **much larger `littlefs`** (~3.1 MiB) — see file comments there.

### 2.3 Observed bootloader segments (reference)

On boot, the ROM loader logs mapped segments (example from field logs): large **flash-mapped** `.text`/`.rodata` segments (~2.1 MiB + ~2.2 MiB), plus internal RAM loads — useful when correlating with **map**/**size** tools locally (`idf.py size`).

---

## 3. Runtime RAM: internal DRAM vs PSRAM

### 3.1 Hardware caps (typical ESP32-S3 + 8 MB octal PSRAM)

From a representative boot log:

- **Internal heap** (Arduino `ESP.getHeapSize()`): ~**274 KiB** total for general allocator view.
- **PSRAM** added to heap: ~**8 MiB** (`esp_psram: Found 8MB PSRAM device`).

**Important:** Many objects still allocate from **internal** DRAM (task stacks by default, WiFi/BT internal buffers, DMA-adjacent pools). **`ESP.getFreeHeap()` mixing PSRAM** can hide **internal** starvation; use `heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` for internal truth.

### 3.2 sdkconfig policy (SPIRAM)

Relevant settings (`sdkconfig`):

| Symbol | Value | Effect |
|--------|-------|--------|
| `CONFIG_SPIRAM_USE_MALLOC` | `y` | Standard `malloc` can use PSRAM |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | **16384** | Allocations ≤ this size prefer internal RAM |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` | **32768** | Reserve **32 KiB** internal for DMA / critical allocs |
| `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | `y` | Prefer moving WiFi/LwIP buffers toward PSRAM when possible |
| `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` | `y` | Allows tasks that opt in to place stack in PSRAM (project mostly uses **internal** stacks unless explicitly static/stack-in-PSRAM) |
| `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` | `y` | `.bss` may use external memory for eligible symbols |

### 3.3 Fixed system stacks (`sdkconfig`)

| Setting | Bytes (ESP-IDF convention) |
|---------|----------------------------|
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | **8192** |
| `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` | **2304** |

Bluetooth (when enabled):

| Setting | Value |
|---------|--------|
| `CONFIG_BT_BTC_TASK_STACK_SIZE` | 4096 |
| `CONFIG_BT_BTU_TASK_STACK_SIZE` | 4352 |

---

## 4. Task stack budget (application)

There are **two conventions** in this codebase:

1. **`System_TaskUtils.h` — `*_STACK_WORDS`**: sizes are **FreeRTOS words** (4 bytes each on Xtensa). Used with `xTaskCreate` patterns that pass **words** (e.g. `CMD_EXEC_STACK_WORDS`).
2. **`G2_Glasses.cpp` / some paths**: comments state **`xTaskCreate` stack in bytes** (ESP-IDF / Arduino style on this target). Example: heartbeat worker uses **2048–3072** bytes; tap dispatcher **20480** bytes.

Always check the call site comment before converting.

### 4.1 Centralized word-based stacks (`components/hardwareone/System_TaskUtils.h`)

| Constant | Words | ~Bytes (×4) |
|----------|-------|-------------|
| `CMD_EXEC_STACK_WORDS` | 6144 | ~24 KiB |
| `ESPNOW_HB_STACK_WORDS` | 5530 | ~22 KiB |
| `SENSOR_QUEUE_STACK_WORDS` | 2765 | ~11 KiB |
| `THERMAL_STACK_WORDS` / `IMU_STACK_WORDS` / `RTC_STACK_WORDS` | 4096 each | ~16 KiB |
| `TOF_STACK_WORDS` / `APDS_*` / `GPS_*` / `PRESENCE_*` | 3072 each | ~12 KiB |
| `GAMEPAD_STACK_WORDS` | 3584 | ~14 KiB |
| `DEBUG_OUT_STACK_WORDS` | 3584 | ~14 KiB |
| `FMRADIO_STACK_WORDS` | 4608 | ~18 KiB |
| `MIC_*` / `SR_SNIP_*` | 4096 | ~16 KiB |
| `SR_STACK_WORDS` / `EI_CONTINUOUS_STACK_WORDS` / `MAP_RENDER_STACK_WORDS` | 8192 each | ~32 KiB |

Sensor tasks are often **disabled** in minimal builds (`System_BuildConfig.h`); stubs still exist.

### 4.2 G2 / camera / ring (representative; stack arg = **bytes** where noted)

| Task name | Stack parameter | Notes |
|-----------|-----------------|--------|
| `g2_tap_disp` | **20480** | Hijack tap path — largest routine worker |
| `g2_connect` / `g2_reconnect` | **6144** each | BLE connect pipeline |
| `g2_page_swap_w` | **4096** | Serializes lens page swaps |
| `g2_hijack` | **4096** | Per-hijack worker |
| `g2_hb_worker` | **2048–3072** | Adaptive by internal heap |
| `g2-fsm` | **3072** (`G2_HijackFsm.cpp`) | FSM worker |
| `g2_cam_view` / `g2_cam_stream` / `g2_bmp_view` | **6144** | Image/stream workers |
| `g2_bmp_full` | **8192** | Full BMP viewer |
| `g2_live_page` / `g2_live_text` | **4096** | Live UI workers |
| `g2_notify_clear` | **3072** | Notification timer worker |
| `cam_pwr` | **10240** (`System_Camera_DVP.cpp`) | Camera power/async init queue |
| `ring_connect` / `ring_reconnect` / `ring_connect_mac` | **5120** (`G2_Ring.cpp`) | Ring BLE |

### 4.3 Camera DMA / PSRAM (runtime, not task stack)

From driver logs (`cam_hal`): frame buffers are typically **~62 KiB** each in PSRAM (e.g. JPEG pipeline), **two** frames common — **order ~125 KiB PSRAM** for buffers alone when streaming, independent of flash size.

---

## 5. Boot-time diagnostics in firmware

`printMemoryReport()` in `components/hardwareone/System_Utils.cpp` prints:

- DRAM vs PSRAM totals and **heap_caps** internal/DMA largest-free-block (fragmentation).
- Per-task stack usage for known app tasks (words × 4).
- Rough estimates for WiFi (~32 KiB), FreeRTOS (~8 KiB), and static module state.

Use this on-device after boot for **actual** RAM pressure; numbers vary with WiFi/BLE connection state.

---

## 6. Regenerating precise flash breakdown

On a machine with ESP-IDF exported in the shell:

```bash
idf.py size
idf.py size-components   # optional: per-component flash
```

This repo’s `build/hardwareone-idf.map` is ~20 MB (full linker map); `idf.py size` distills **flash/RAM by object** without manually parsing it.

---

## 7. Revision history

| Date | Notes |
|------|--------|
| 2026-05-03 | Initial report from `partitions.csv`, `sdkconfig`, `System_TaskUtils.h`, `G2_Glasses.cpp`, `System_Camera_DVP.cpp`, `hardwareone-idf.bin` size |
