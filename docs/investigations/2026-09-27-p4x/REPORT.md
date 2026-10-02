**HardwareOne -> ESP32-P4X-EYE investigation - 27 September 2026**

**Recommendation:** retain one application, add a P4X-EYE board profile and hardware backends, and start on ESP-IDF **5.5.5**. Treat IDF 6 as a separate migration. The principal feasibility question is preserving HardwareOne's ESP-NOW mesh through the board's C6 radio coprocessor.

Created local branch `codex/investigate-esp32-p4x-eye` at `6ba1c6c`. No commits or pushes were made. The original checkout had 16 modified/deleted tracked paths on entry; those were preserved. Application source changes and dependency restoration occurred only in a disposable copy. Neither board was flashed or erased; chip queries reset them and serial output was read.

**What was physically verified**

| Device | USB serial port during investigation | Observed hardware and firmware |
|---|---|---|
| ESP32-P4X-EYE | `/dev/cu.usbmodem2101` | ESP32-P4 revision **3.2**, 400 MHz, **16 MB flash**; factory boot detects **32 MB PSRAM** and **OV2710** camera. Running `factory_demo` 1.1.0, built February 5, 2026. |
| XIAO ESP32-S3 Sense control | `/dev/cu.usbmodem1101` | ESP32-S3 revision **0.2**, **8 MB flash**, **8 MB embedded PSRAM**; existing firmware prints the Seeed XIAO Sense greeting. |

Both USB links and existing firmware responded. This does not validate HardwareOne on either device. The P4 factory image identifies itself as `v5.5.2-712-g4297f22f8ab-dirty`: a vendor development snapshot after 5.5.2, not evidence that stock 5.5.2 supports this silicon. The onboard C6's firmware version and Hosted configuration were not established.

**Why IDF 6 is unnecessary for initial support**

Your repository records **IDF 5.5.1** and documents patched **Arduino-ESP32 3.3.5**. Espressif requires **IDF 5.5.3+ or 6.0+** for P4 revision 3.x and requires `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=n`. New and old P4 revisions cannot share an image. The connected board is definitely new revision 3.x. [Official revision guide](https://documentation.espressif.com/esp32-p4-chip-revision-v3.x_user_guide_en.html).

The official EYE examples recommend the 5.5 release line. Arduino 3.3.5 already lists P4 as a target; it need not be removed simply because the CPU is RISC-V. For an updated candidate pairing, Arduino **3.3.12** is released on **IDF 5.5.5**. Its broad manifest version constraint is not proof that every allowed combination works with HardwareOne. Rebase and verify your Arduino patches before upgrading. [EYE examples](https://github.com/espressif/esp-dev-kits/blob/master/examples/esp32-p4-eye/README.md), [Arduino 3.3.12 release](https://github.com/espressif/arduino-esp32/releases/tag/3.3.12).

IDF 6 would additionally require work on legacy peripheral APIs, moved JSON/MQTT components, Mbed TLS 4/PSA crypto, and Arduino integration. Settings encryption, authentication and OTA compatibility need regression checks. Those changes do not solve the P4 camera or coprocessor integration.

**What changes, and what can stay shared**

| Area | Assessment |
|---|---|
| CLI, web UI, settings, automation, application protocols | Substantially reusable C/C++. No actual Xtensa assembly was found in the application audit. Hardware-specific dependencies still need to be removed from shared interfaces. |
| Wi-Fi and BLE | P4 has no native radios; the onboard **ESP32-C6-MINI-1U** supplies them. Integrate ESP-Hosted/Wi-Fi Remote with correct board wiring, reset/init sequence, compatible C6 firmware, and recovery behavior. Standard Wi-Fi API compatibility helps preserve application code. BLE/G2/R1 behavior still needs dedicated tests. |
| ESP-NOW mesh | **Largest feature-parity risk.** Stock ESP-Hosted lacks the needed ESP-NOW RPC implementation. Ordinary Wi-Fi success does not imply mesh success. See the bridge milestone below. |
| Camera | Existing `esp32-camera` capture is ESP32/S2/S3-specific. Add a P4 `esp_video`/MIPI-CSI backend behind neutral frame/resolution APIs. Keep capture commands, storage and streaming shared; explicitly account for JPEG encoding and buffer ownership. |
| Display and audio | Better reuse than a new CPU might suggest: the EYE has a **240×240 SPI ST7789** display and **PDM microphone**. Those technologies already exist in your HALs. Supply correct pins, panel geometry, initialization and resource ownership; register the existing ST7789 driver in CMake and replace remaining SSD1306-specific UI calls/framebuffer assumptions; a wholesale UI/audio rewrite is unnecessary. |
| SD card | Add SDMMC mounting and board power configuration below the existing VFS; current code assumes SPI SD. |
| Power, tasks, memory, identity | Replace hardcoded CPU frequencies, revisit task placement, validate DMA/cache constraints and RISC-V crash handling. Device identity must work before the C6 radio is ready. |
| OTA/recovery | The separate recovery application also needs Hosted networking. Add target/board/revision identity, partitions and compatible coprocessor firmware policy to the release contract. Main-firmware support alone is insufficient. |

Board facts come from the [official board guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html) and [BSP definitions](https://raw.githubusercontent.com/espressif/esp-dev-kits/master/examples/esp32-p4-eye/examples/common_components/esp32_p4_eye/include/bsp/esp32_p4_eye.h). Code locations and dependency details are in the accompanying code-audit appendix.

**The ESP-NOW feasibility milestone**

Current upstream ESP-Hosted 3.0.9 source has no ESP-NOW RPC implementation; Wi-Fi Remote's injected `esp_now.h` declarations do not supply an implementation. Espressif's request remains open. [Tracking issue](https://github.com/espressif/esp-hosted-mcu/issues/19), [version-pinned RPC reference](https://github.com/espressif/esp-hosted-mcu/blob/95e70156a3904fa172b9bb50888a4d7dd23d2f3a/docs/design/rpc-reference.md).

ESPHome demonstrates a plausible route using CustomRpc with matching host and C6 firmware. It is useful reference code, not drop-in compatibility: peer-query functions return unsupported, peer addition/deletion use asynchronous requests and local caches, and some receive metadata is synthesized. HardwareOne uses peer queries and checks peer-add errors, so those semantics matter. [ESPHome companion firmware](https://github.com/esphome/esp-hosted-firmware), [version-pinned host shim](https://github.com/esphome/esphome/blob/97f362a718effa2c476490c5bc8b204c6de64913/esphome/components/esp32_hosted/esp_now_hosted.cpp).

The first product-level experiment should send HardwareOne-compatible packets between **P4+C6 and the S3 control**: discovery, encrypted peer setup, delivery callbacks, loss/retry behavior, channel changes, larger transfers, and C6 reset/reconnect. Preserve the existing mesh protocol above a native/hosted transport boundary. Expect to maintain a small companion adapter unless upstream gains complete support. This firmware pair is the main extra maintenance cost to investigate.

**Architecture and implementation sequence**

Keep four explicit inputs: **SoC capabilities**, **board wiring**, **deployment features**, and **selected hardware backends**. Extend existing board/deployment files and HALs rather than creating a parallel application. Derive CMake dependencies and C++ feature availability from the same configuration. A P4's lack of native Wi-Fi must not automatically hide networking when a C6 supplies it.

1. Make the existing build reproducible: pin Arduino, capture the complete board variant and patches, and reconcile dependency manifests. Keep S3 as the control.
2. Build a minimal P4 profile with serial, memory, settings and filesystem; verify it on hardware in a subsequent authorized flashing experiment.
3. Prove C6 Hosted networking and the ESP-NOW bridge early, before investing heavily in camera/UI feature parity.
4. Add SDMMC, existing PDM/ST7789 integration, buttons/encoder, and the P4 camera backend incrementally. Then qualify BLE/G2/R1 and concurrent workloads.
5. Add recovery OTA and regression builds/tests for ESP32, S3 and P4. Evaluate IDF 6 separately.

One source tree is realistic within the ESP-IDF family. This does not imply the current application is portable to arbitrary non-Espressif microcontrollers. One binary across Xtensa/RISC-V CPUs or incompatible P4 silicon revisions is not. Hardware backends and build configurations are necessary; duplicate application implementations are avoidable.

**Experiment record**

ESP-IDF 5.5.5, RISC-V/S3 compilers and isolated Python tooling were installed under `/private/tmp/hw1-p4-investigation-20260927/`. No global shell configuration was changed. Disposable directories may be removed by macOS; the report and selected evidence are saved separately as durable outputs.

The official hello-world source compiled and linked successfully for both chips using the same IDF checkout. P4 configuration selected RISC-V and revision >=3.1, with the older-revision option disabled. These are build controls, not on-device tests of the newly compiled images.

The HardwareOne experiment copied the **current tracked working-tree content**, including existing edits/deletions, into a throwaway tree. It restored Arduino 3.3.5 and successfully applied the documented patch. **S3 application result:** configuration succeeded using the existing lock, but compilation stopped in the vendored Adafruit GPS library on `-Werror=stringop-truncation` under the updated toolchain. **P4 application result:** fresh dependency resolution failed because HardwareOne requests ESP-SR `^1.4.0` while the patched Arduino manifest requests `^2.1.5`. This validates the need to reconcile dependencies before a meaningful P4 application build. Neither failure is a successful application port. More detail is recorded in `experiment-results.txt`.

Detailed code findings: [code-audit.md](code-audit.md). Verification results: [experiment-results.txt](experiment-results.md).
