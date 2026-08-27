# Session handoff — 2026-08-22/23

**Nothing is committed. Nothing is hardware-tested except where noted.** HEAD is `8842ccc7`.

## Modified / added files
```
 M boards/xiao_s3.defaults
 M components/hardwareone/BLE_Peers.cpp
 M components/hardwareone/Bluetooth.cpp
 M components/hardwareone/CMakeLists.txt
 M components/hardwareone/G2_Glasses.cpp
 M components/hardwareone/G2_Glasses.h
 M components/hardwareone/G2_Health.cpp
 M components/hardwareone/G2_Health.h
 M components/hardwareone/G2_HijackCmd.h
 M components/hardwareone/G2_HijackFsm.cpp
 M components/hardwareone/G2_Page_ESPNow.cpp
 M components/hardwareone/G2_Page_Files.cpp
 M components/hardwareone/G2_Page_Network.cpp
 M components/hardwareone/G2_Page_Settings.cpp
 M components/hardwareone/G2_Page_TestSuite.cpp
 M components/hardwareone/G2_Page_TestSuite.h
 M components/hardwareone/G2_Page_TextEntry.cpp
 M components/hardwareone/G2_Page_TextEntry.h
 M components/hardwareone/G2_Page_Users.cpp
 M components/hardwareone/G2_Page_Users.h
D  components/hardwareone/G2_Pet.cpp
D  components/hardwareone/G2_Pet.h
 M components/hardwareone/G2_Ring.cpp
 M components/hardwareone/G2_Ring.h
 M components/hardwareone/HardwareOne.cpp
 M components/hardwareone/OLED_ESPNow.cpp
 M components/hardwareone/OLED_ESPNow.h
 M components/hardwareone/OLED_FirstTimeSetup.cpp
 M components/hardwareone/OLED_Mode_Auth.cpp
 M components/hardwareone/OLED_Mode_Automations.cpp
 M components/hardwareone/OLED_Mode_CLIInput.cpp
 M components/hardwareone/OLED_Mode_ChangePassword.cpp
 M components/hardwareone/OLED_Mode_FileBrowser.cpp
 M components/hardwareone/OLED_Mode_LLM.cpp
 M components/hardwareone/OLED_Mode_Map.cpp
 M components/hardwareone/OLED_Mode_Network.cpp
 M components/hardwareone/OLED_Mode_R1_Health.cpp
 M components/hardwareone/OLED_Mode_SetPattern.cpp
 M components/hardwareone/OLED_Mode_UserManager.cpp
 M components/hardwareone/OLED_SettingsEditor.cpp
 M components/hardwareone/OLED_SetupWizard.cpp
 M components/hardwareone/OLED_UI.cpp
 M components/hardwareone/OLED_Utils.cpp
 M components/hardwareone/OLED_Utils.h
 M components/hardwareone/R1_HealthHistoryStore.cpp
 M components/hardwareone/R1_HealthHistoryStore.h
 M components/hardwareone/System_Automation.cpp
 M components/hardwareone/System_BuildConfig.h
 M components/hardwareone/System_CLI.cpp
 M components/hardwareone/System_Camera_Video.cpp
 M components/hardwareone/System_CaptureCrypto.cpp
 M components/hardwareone/System_Command.cpp
 M components/hardwareone/System_Debug.cpp
 M components/hardwareone/System_Dictation.cpp
 M components/hardwareone/System_Dictation.h
 M components/hardwareone/System_ESPNow.cpp
 M components/hardwareone/System_ESPNow.h
 M components/hardwareone/System_ESPNow_FsList.cpp
 M components/hardwareone/System_ESPSR.cpp
 M components/hardwareone/System_EdgeImpulse.cpp
 M components/hardwareone/System_Events.h
 M components/hardwareone/System_FeatureRegistry.cpp
 M components/hardwareone/System_FeatureRegistry.h
 M components/hardwareone/System_Filesystem.cpp
 M components/hardwareone/System_FirstTimeSetup.cpp
 M components/hardwareone/System_G2_Protocol.cpp
 M components/hardwareone/System_G2_Protocol.h
 M components/hardwareone/System_I2C.cpp
 M components/hardwareone/System_I2C.h
 M components/hardwareone/System_ImageManager.cpp
 M components/hardwareone/System_LLMBackend.cpp
 M components/hardwareone/System_LLM_Model.cpp
 M components/hardwareone/System_LiveAudio.cpp
 M components/hardwareone/System_LiveAudio.h
 M components/hardwareone/System_Maps.cpp
 M components/hardwareone/System_Maps.h
 M components/hardwareone/System_MemUtil.h
 M components/hardwareone/System_MemoryMonitor.cpp
 M components/hardwareone/System_Microphone.cpp
 M components/hardwareone/System_Microphone.h
 M components/hardwareone/System_Notifications.cpp
 M components/hardwareone/System_OTASafety.cpp
 M components/hardwareone/System_OTASafety.h
 M components/hardwareone/System_R1_Protocol.cpp
 M components/hardwareone/System_R1_Protocol.h
 M components/hardwareone/System_RamFlush.cpp
 M components/hardwareone/System_SensorLogging.cpp
 M components/hardwareone/System_SensorLogging.h
 M components/hardwareone/System_SensorStubs.cpp
 M components/hardwareone/System_SensorStubs.h
 M components/hardwareone/System_Settings.cpp
 M components/hardwareone/System_Settings.h
 M components/hardwareone/System_TaskUtils.cpp
 M components/hardwareone/System_TimeAnchors.cpp
 M components/hardwareone/System_UartLink.cpp
 M components/hardwareone/System_UartLink.h
 M components/hardwareone/System_User.cpp
 M components/hardwareone/System_Utils.cpp
 M components/hardwareone/System_Utils.h
 M components/hardwareone/System_WiFi.cpp
 M components/hardwareone/System_WiFi.h
 M components/hardwareone/WebPage_R1_Health.cpp
 M components/hardwareone/WebPage_Sensors.cpp
 M components/hardwareone/WebServer_Events.cpp
 M components/hardwareone/WebServer_Handle.h
 M components/hardwareone/WebServer_MigrationTool.cpp
 M components/hardwareone/WebServer_Server.cpp
 M components/hardwareone/i2csensor_apds9960.cpp
 M components/hardwareone/i2csensor_bno055.cpp
 M components/hardwareone/i2csensor_ds3231.cpp
 M components/hardwareone/i2csensor_mlx90640.cpp
 M components/hardwareone/i2csensor_pa1010d.cpp
 M components/hardwareone/i2csensor_pca9685.cpp
 M components/hardwareone/i2csensor_rda5807.cpp
 M components/hardwareone/i2csensor_sths34pf80.cpp
 M components/hardwareone/i2csensor_vl53l4cx.cpp
 M docs/arduino-local-patches/README.md
 M docs/arduino-local-patches/arduino-local-patches.patch
 M docs/arduino-local-patches/verify_patches.sh
?? components/hardwareone/System_ConfigLoad.h
?? components/hardwareone/System_DictationPolicy.h
?? components/hardwareone/System_MapViewportCore.h
?? components/hardwareone/System_MemTracker.cpp
?? components/hardwareone/System_MemTracker.h
?? components/hardwareone/System_MemTrackerCore.h
?? components/hardwareone/System_MemUtil.cpp
?? components/hardwareone/test/
?? tools/btsnoop/
?? tools/build_coverage.sh
?? tools/build_memory_coverage.sh
?? tools/check_raw_allocations.py
?? tools/map_attribute.py
?? tools/map_live_sections.py
?? tools/model/
?? tools/webui/
```

## Read these, in order

1. `docs/BUILD_GATING_GAP_2026-08-22.md` — the master record. §1-§28. Every change, every
   measurement, every correction. **§13, §19, §25 are corrections to my own earlier claims** — read
   them or you will re-litigate settled questions.
2. `docs/DRAM_UNACCOUNTED_CENSUS_2026-08-23.md` — validated DRAM census.
3. `docs/MEMORY_REPORT_FIX_PLAN.md` — the analysis; `docs/MEMORY_REPORT_FIX_RESULT.md` — what was applied.

## Current tree config

**MINIMAL / serial-only.** Every feature flag 0, `CONFIG_BT_ENABLED=n` in `boards/xiao_s3.defaults`.
Image 1,086,336 B, 82% of the factory partition free. Flashed and booting on the bench XIAO.

Config snapshots (restore points) are in the session scratchpad:
`/private/tmp/claude-501/-Users-morgan-esp-hardwareone-idf/7ca86bd9-be5a-4e84-aa61-e3c198c74f4a/scratchpad/`
— `BuildConfig.SNAPSHOT.h` (the original carrier build), `xiao_s3.defaults.SNAPSHOT`,
`sdkconfig.xiao.SNAPSHOT`. **The carrier config predates the new `ENABLE_UART_HOST_LINK` flag**, so
restoring it wholesale removes that flag and the derived rules — merge, do not overwrite.

## What shipped this session

**Flash: 5,978,496 -> 1,086,336** on the XIAO (mostly the minimal strip; ~167 KB of that was real
gating work applicable to any config).

- Two build flags added: `ENABLE_G2_TESTSUITE` (default **1**), `ENABLE_UART_HOST_LINK` (default
  **1**). Both default ON so no existing build changes behaviour.
- `ENABLE_G2_PET` added then **removed** — the Pet feature was deleted outright at the owner's
  request.
- `mesh_peers.json` persistence **deleted**; replaced by `meshBootstrapSlotsFromRegistry()` seeded
  from `devices.json`.
- liblc3 trimmed to baseline (`LC3_PLUS=0 LC3_PLUS_HR=0`), -33,792 B.
- Nine "load-failure wipe" fixes (see below).
- `features json` `toggleable`/`heapKB`/`category` corrected + a compile-time overflow tripwire.
- Sensor `<sensor>bus` commands relocated into their gated sensor files.

## Open queue

### 1. Memory report correctness — APPLIED 2026-08-23 (later session), HW-test pending
`docs/MEMORY_REPORT_FIX_RESULT.md` is the record: what landed, the deliberate deviation from
`MEMORY_REPORT_FIX_STEPS.md` (one `heap_caps_walk` instead of per-pointer
`heap_caps_get_allocated_size`), the three-config build matrix (all green), and the six hardware
checks still open — the `alloc_meta` band check is the one that matters most.

### 2. Load-failure wipes: 6 steps left, all need hardware
Full plan: session scratchpad `wipe.md` (15 steps; 1, 6, 9-15 are DONE).
Remaining: **STEP 2** `writeTextAtomic`, **STEP 3 + 7** `devices.json` load/save,
**STEP 4 + 8** `ip_bans.json`, **STEP 5** (superseded — mesh peers already deleted).
These change commit mechanisms and the per-request auth path; the plan lists per-site hardware
checks.

### 3. Hardware checks outstanding on work already landed
- **mesh_peers deletion** — reboot node A, confirm node B receives a `MSG_SYSTEM_EVENT` with the
  right boot counter and non-zero timestamp. **This is the one thing the deletion breaks and the
  seed repairs — check it first.** Then: mesh comes up within ~5 s with no traffic; same in DIRECT
  mode.
- **`ENABLE_G2_TESTSUITE` / lens menus** — the Tests page under System.
- **LC3 baseline** — the G2 mic decode path (10 ms / 16 kHz).
- **The "Not active:" help line** — needs a build with sensors compiled in but *unplugged*. The
  minimal build cannot exercise it.

### 4. Known pre-existing breaks found but NOT fixed
- `I2C_FEATURE_LEVEL > 0` with `INPUT_DEVICE_TYPE 0` **does not link** — four OLED files reference
  `gNavEvents`, `gDataSource`, `updateInputState()`, `getNewlyPressedButtons()`, `getJoystickDelta()`
  with no stub.
- `ENABLE_ESP_SR 1` with `ENABLE_MICROPHONE 0` **does not compile** — `System_ESPSR.cpp:2173-2204`
  uses `audioGetSource()` / `AUDIO_SRC_G2_LEFT` ungated.
- `ENABLE_ESPNOW=0` has never been compiled (pre-existing; `System_MQTT.cpp:36` gated include,
  ungated use at `:1180-1182`).
- `System_Utils.cpp:4012` `system_tasks_total` declared and never used (pre-existing).
- Four bus settings (`thermalBus`, `tofBus`, `imuBus`, `apdsBus`) are **write-only** — 2 refs each
  vs `gpsBus` 7 / `rtcBus` 8. `thermalbus 1` persists, says success, changes nothing. Owner decision
  pending: migrate the 4 drivers to explicit-bus overloads, or retire the settings.
- `getFeaturesByCategory()` declaration removed (was defined nowhere, unimplementable as declared —
  categories are not contiguous in `featureRegistry[]`).

### 5. Confirmed on hardware, needs a fix decision
**The per-command INPUT stall is NOT liveaudio-specific.** Measured 216-776 ms stalls on a build
with no CM5, no UART link and LiveAudio compiled out — one after every interactive command. The
cost is the always-on command audit path
(`executeCommand -> logCommandExecution -> appendLineWithCap -> refreshLittleFsFreeCached ->
LittleFS.usedBytes()` full-FS scan). The FS was near-empty, so those figures are a **floor**.
See the memory note `project_cmd_audit_input_stall` — upgraded to HIGH confidence.

## Traps this session cost real time on — do not rediscover

1. **The link map lists gc-DISCARDED sections at load address 0x0.** Filter `addr != 0` or stripped
   code reappears in your report as if it shipped. `tools/map_live_sections.py` does this.
2. **`esp_idf_size --archives` cannot be trusted for rodata** (SHF_MERGE string folding; sums to
   1.87x truth).
3. **Green on one board proves only that board.** `System_Maps.cpp`, `WebServer_MigrationTool.cpp`
   and `System_ESPSR.cpp` are each compiled OUT of the minimal build. Three configs were needed to
   cover this session's edits.
4. **Check the caller graph before pricing a gate.** 40 `g2Probe*` functions (~42 KB) vanished for
   free when their only caller — the TestSuite dispatch table — was gated out.
5. **`probe*`-prefixed helpers in `G2_Glasses.cpp` are NOT diagnostics.** They are the shared
   image-ack plumbing used by the BMP/JPG viewers, camera, Maps and Health. Never gate on the name.
6. **Relocating a command changes its VISIBILITY policy**, not just its compilation —
   `System_CLI.cpp:126-133` hides a whole `CMD_MODULE_SENSOR` module from `help` when its
   `isConnected()` predicate is false.
7. **`#define XIAO_ESP32S3_SENSE_ENABLED 0` used to disable nothing** — the gates tested
   `defined(...)`, not the value. Fixed; worth 297 KB when it was.
8. **`settings.json` already implements sticky-note semantics** (merge-read at
   `System_Settings.cpp:1074-1080`). **Confirmed on hardware.** The comment at `:1034-1039` reads
   like a denial but is scoped to the failed-load case. 245 of 294 SettingEntry rows sit behind a
   build gate; the merge-read is what makes that safe.
9. **`debug.json` does NOT merge-read** — it rebuilds. Safe today only because
   `debugSettingEntries` contains no `#if`.
10. **A comment stalled a decision twice.** `System_ESPNow.cpp:14039` claimed `loadMeshPeers()`
    re-registers peers; it never touched the radio. Verify a load-bearing comment before trusting it.

## Build commands

```bash
source $HOME/esp/esp-idf/export.sh
tools/build_board.sh xiao_s3 build
tools/build_board.sh xiao_s3 -p /dev/cu.usbmodem2101 flash monitor
```

Plain `flash` is correct for xiao_s3 — it is the **factory** layout, not the OTA layout. The
`ota0-flash` rule applies only to `HW_OTA_LAYOUT=1` on the FeatherS3.

**Serialize board builds.** The root `partitions.csv` is generated per-configure and shared; the
last configure wins, and FeatherS3 (16 MB, factory `0x615000`) and XIAO (8 MB, `0x5B5000`) disagree.
Run `tools/build_board.sh <board> reconfigure` before building if another board built last.
