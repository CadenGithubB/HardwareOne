# Waveshare ESP32-P4-WIFI6-Touch-LCD-5

Bring-up: 2026-11-01, branch `codex/waveshare-p4-support`. Existing unrelated
STT/G2/OpenClaw edits were preserved. The user confirmed the **5-inch model**;
the initially pasted 4.3-inch ST7701 page is not this device.

## Device and scope

- Target USB hub port `/dev/cu.usbmodem5ABA0541421`, bridge serial `5ABA054142`,
  VID:PID `1a86:55d3`. P4 MAC `80:f1:b2:d1:c3:12`, silicon revision v1.3.
- Existing STT board `/dev/cu.usbmodem101`, serial `E8:F6:0A:E3:A7:68`, was
  not flashed or reset. Never substitute that port for this bring-up.
- esptool verified 32 MB flash; 32 MB PSRAM at 200 MHz passed the boot memory
  test. CPU is 360 MHz; UART0 TX37/RX38, console 115200 baud.
- User authorized P4/C6 installation, declined a factory backup, and explicitly
  approved erasing the new filesystem region. The P4 was flashed and verified.
- Microphone capture, camera and battery remain disabled/unqualified.
  SD is enabled and mount/read/write/remount tested (see below).
  Speaker listening was explicitly deferred by the user after the driver test.

## Configuration and wiring

Use `configurations/waveshare-p4-bringup.json` with board
`waveshare_p4_touch_lcd_5`. Build through the saved-profile tool:

```sh
source /Users/cadbecaimacmini/esp/esp-idf/export.sh
python3 tools/build_config.py build waveshare-p4-bringup
```

For direct incremental `idf.py` use, retain `HW_BOARD=waveshare_p4_touch_lcd_5`
in the environment. Otherwise a CMake regeneration drops the board selection.
Do not flash an image whose resolved board, flash size or partition table differs.

| Hardware | Configuration |
| --- | --- |
| C6 Hosted SDIO | CLK18, CMD19, D0–D3 GPIO14–17, reset54; slot1, 4-bit, 20 MHz |
| Shared I2C | SDA7, SCL8; project bus0, Arduino Wire1/I2C1 |
| HX8394 DSI panel | 720×1280, two lanes at 700 Mbps, DPI 58 MHz |
| Panel timing | H pulse/back/front 20/20/40; V pulse/back/front 4/10/24 |
| Panel reset | GPIO27, **active-high** |
| Panel power | MIPI PHY LDO3, 2.5 V |
| Backlight | GPIO26, 5 kHz, 10-bit PWM |
| ES8311 speaker | I2C address 0x18; MCLK13, BCLK12, WS10, DOUT9; amplifier GPIO53 high |

Board defaults own wiring and ordinary feature defaults. Shared services use
HAL interfaces. The EYE profile's GPIO33/35/36 companion programming/console
wiring is not available on this board; do not run its temporary programmer here.

The factory-only partition layout has an 8 MB application at `0x10000` and
LittleFS at `0x810000`, length `0x17f0000` (through `0x1FFFFFF`). It has no
recovery-OTA slot contract. The filesystem region was erased with the user's
explicit approval and Hardware One now mounts it successfully.

## Display and speaker

`HAL_Display_Waveshare` adapts the existing 128×64 software UI canvas to a
640×320 region centered on the portrait panel. It owns DSI, framebuffer,
backlight and cleanup. The UI remains the existing Hardware One interface.
Input has an independent lifecycle. DSI command draining has a deadline so an
unresponsive panel does not fill IDF 5.5's indefinitely-blocking command FIFO.

Two physical bring-up issues were addressed:

1. The board reset circuit is active-high. Leaving GPIO27 high holds the panel
   in reset and stalls DSI initialization. The backend now releases it low.
2. Arduino `ledcWrite` promotes duty 1023 to constant-high 1024. This board's
   AC-coupled backlight needs PWM edges, so that caused severe whole-screen
   strobing at default maximum brightness. An initial cap at 1022 was still
   unstable after startup power policy reapplied maximum brightness. The backend now maps logical 255 to duty 720
   (about 70%), matching the range the user confirmed stable at brightness 180
   with the earlier mapping. Boot policy can no longer exceed that range.
   The user confirmed the latest firmware is visually steady after reboot
   on 2026-11-01. Full physical brightness remains unqualified.

The backend skips unchanged canvas frames and synchronizes only the centered
UI band after drawing, reducing PSRAM bandwidth. This follows observed DSI
underruns during heavy filesystem activity in an earlier image.

`HAL_Speaker` owns TX-only I2S, ES8311 initialization and amplifier gating.
Codec accesses use the existing shared I2C transaction mechanism. Every error
path releases the I2S channel, codec and amplifier. The command
`speakertone [100..4000 Hz] [50..2000 ms] [0..60 volume]` validates arguments
before accessing hardware; defaults are 660 Hz, 400 ms, 25 volume.

On-device `speakertone 660 600 40` initialized ES8311, completed I2S writes and
returned success. **Audible output has not been confirmed**: the user was away
and asked to skip listening for now. This is tone playback support, not a media
player or microphone integration.

## Companion installation

Factory C6 firmware responds to legacy Wi-Fi RPCs but lacks current version,
application-description and bridge queries. HAL accepts either a successful
MAC query or its explicit remote `ESP_ERR_WIFI_NOT_INIT` response as transport
evidence, enabling the existing authenticated SDIO firmware update path.
Unanswered probes still fail; version/bridge qualification remains separate.

Hardware One's pinned C6 companion built successfully (1,245,888 bytes).
Serial upload completed. The first SDIO attempt began successfully but timed
out on a 4 KB write; the legacy path now uses 1 KB requests and completed.
The factory firmware rebooted automatically after OTA end, so the subsequent
new-protocol activation request timed out and the updater reported failure.
Recovery reconnected successfully: `c6status` verified Hosted 2.12.13, the
ESP-NOW bridge, and a valid running `ota_1` image. The latest P4 reboot again
confirmed those results, with zero missed heartbeats or transport errors.
The temporary uploaded image was deleted from the device after verification.

## Provisioning and verification

Normal Advanced Setup created the admin account and enabled the display and
I2C. Device name is `HardwareOne-Waveshare`. Credentials are saved privately in
ignored `work/waveshare-p4/device-admin.json` (mode 0600); do not commit them.
No Wi-Fi credentials were supplied.

- Waveshare build/link passed: application 2,370,544 bytes, 8,388,608-byte slot,
  6,018,064 bytes free. Final flash hash verified, reboot and display init passed.
- P4X-EYE `p4-glasses-test` baseline passed: 5,471,072 bytes, 1,954,976 bytes free.
  No physical EYE regression test was performed.
- 13 optional-peripheral config tests and 21 saved-config tests passed.
- Display lifecycle sanitizer checks passed including the type 5 backend boundary.
- Latest-image 65,536-byte filesystem upload/delete passed with zero reported
  display underruns or watchdog warnings; display status remained connected.
  This is a short stress check, not long-duration display qualification.
- These original checks do not qualify microphone, SD or untested radio features;
  touch verification is recorded below.

Local logs, firmware hashes, resolved profiles and baseline evidence are in
`work/waveshare-p4/`. The active Waveshare build retains matching firmware,
ELF and partition/bootloader binaries for incremental hardware work. The
completed baseline and companion build directories were removed after retaining
baseline manifests/configuration and matching C6 firmware/debug/layout files.
Final cleanup recovered 1,314,566,144 allocated bytes (about 1.22 GiB). Shared
SDK overrides (134 MiB), managed dependencies (426 MiB), and pinned companion
source caches (about 35 MB) remain for rebuilding. The active P4 build remains
for incremental display bring-up; no disposable baseline or C6 build remains.

## Single-touch input

Single-contact GT911 support is selected by `INPUT_DEVICE_TYPE=4` in the board
profile. It shares I2C bus0 (SDA7/SCL8) with the codec and probes addresses 0x5d
and 0x14 through the manager. The backend verifies its product ID and owns a
20 ms polling task. The board does not expose touch reset/interrupt GPIOs;
touch does not toggle the display reset or change its backlight policy.

`HAL_Input` owns a distinct touch snapshot and bounded Down/Move/Up/Cancel
queue. Only UI dispatch consumes events; `touchread` is a non-consuming status
command. Short taps survive between UI frames. Overflow, extra contacts,
changed tracking IDs, I/O failure and polling interruptions longer than 250 ms cancel contact.
A held contact cannot become a new press after cancellation. No multi-touch
or synthetic joystick/encoder motion is implemented.

`OLED_Touch` maps directional contact-down events and completed action-button
taps to existing navigation and logical buttons.
The visible controls below the centered 640×320 UI canvas share their hit-test
rectangles with rendering: directions, SELECT (A), BACK (B), X / OK, Y / DEL,
MENU (Start), and FN (Select/function). Action buttons allow 8 pixels of edge
movement; dragging farther outside cancels the tap. Directions emit one step
on contact, with no duplicate on release. Existing screen handlers, keyboard and authenticated command execution
remain the action owners. Touching a menu row directly is not implemented.

The normal UI and setup wizard share this adapter; changing owner/page discards
pending contacts. Display wake discards the wake gesture. Raw coordinates and
controls are in the panel's fixed portrait space; canvas rotation does not
rotate the physical control bank. Start/stop goes through the existing I2C
request queue, and `inputenabled` / `inputautostart` retain settings ownership.

Host tests: `python3 components/hardwareone/test/host/test_touch_input.py`
exercises the production queue, hit-testing and adapter with sanitizers and
checks that existing input controller numeric IDs remain unchanged.
Input-cache, setup-cancellation and optional-peripheral configuration checks
also passed (14 peripheral configurations, 24 saved-config workflow tests,
3 input-cache tests, setup cancellation under four variants, and Wi-Fi navigation
with Wi-Fi enabled/disabled).

The affected Waveshare image built at 2,394,592 bytes with 5,994,016 bytes of
application headroom and was flashed with hash verification. The P4 EYE baseline
built at 5,471,344 bytes with 1,954,704 bytes free. The initial startup attempt
exposed a missing GT911 exclusion in the no-input stub; that guard is corrected.
The physical GT911 now verifies its identity and starts successfully at boot;
`touchread` reports connected. Normal authenticated settings enabled input and
autostart persistently. Queued stop/restart also succeeded. The C6 remained
online with zero missed heartbeats or transport errors. Visual control/coordinate verification is pending the
user's response. The first contact after initialization/cancellation may be
consumed until release, preventing a held finger from becoming a new tap.

Evidence and image hashes are in `work/waveshare-p4/touch-*`. The temporary
baseline build was removed after retaining its configuration, size and hashes,
recovering 1,079,857,152 allocated file bytes. The active Waveshare build remains
for incremental hardware work; shared dependencies and compact C6 recovery
inputs remain as described above.

## Vendor sources

- [Espressif GT911 register protocol reference](https://github.com/espressif/esp-bsp/blob/master/components/lcd_touch/esp_lcd_touch_gt911/esp_lcd_touch_gt911.c)
- [Board documentation](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-5)
- [LCD-5 examples](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-5),
  revision `d144b34d8f384e84d2442e8733523560216a2f63`
- [Waveshare BSP](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/e619f5b128072b42cffe628caa9baccecd8b4d96/bsp/esp32_p4_wifi6_touch_lcd_5),
  including active-high panel reset and PWM backlight configuration
- [Schematic](https://files.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-5/ESP32-P4-WIFI6-Touch-LCD-5-Schematic.pdf)

Vendor ES8311 source retains its Apache-2.0 copyright/SPDX notices and license
in `components/hardwareone/speaker/`. The panel register table is attributed to
the vendor Arduino display profile.

## Touch responsiveness adjustment

The user reported that the initial controls felt sluggish. Touch now bypasses
mechanical gamepad debounce (100 ms) and redraws immediately when an input
handler accepts an action, bypassing the ordinary display refresh timer for
that frame. Other input backends retain their existing timing. Directional
buttons act on contact; Select/Back/other action buttons retain release-to-act
and drag-away cancellation. No hold-to-repeat is enabled.

Polling stays at 20 ms / 400 kHz. Brief shared-bus pauses preserve contacts;
I/O failure and pauses over 250 ms still cancel. The backlight duty cap and
DSI timing are unchanged. Host checks cover immediate directions, no duplicate
release actions, border tolerance, cancellation, extra fingers and UI ownership
handoff. The 2,394,624-byte image (5,993,984 bytes of application headroom) was flashed
with hash verification and rebooted with GT911 online. The three touch host
tests and display lifecycle sanitizer variants passed. On 2026-11-01 the user
confirmed that touch is more responsive. This reply did not separately confirm
screen stability with the adjusted firmware. The encoder baseline also built successfully
(5,471,344 bytes). Its temporary build was removed after
preserving configuration and hash evidence, recovering 1,079,861,248 allocated file
bytes. The active Waveshare build remains for hardware work. Evidence uses the
`work/waveshare-p4/touch-responsive-*` prefix.

## Login-popup whole-panel flashes (2026-11-01)

The user reported two whole-panel flashes on a login error, including the fixed
touch controls. No DSI underrun was captured during the reported reproduction.
The popup itself only draws into the 128x64 canvas; notification icon blinking
does not change backlight duty or panel power.

The Waveshare SDK defaults now enable `CONFIG_LCD_DSI_ISR_CACHE_SAFE`, which
also selects `CONFIG_DW_GDMA_ISR_IRAM_SAFE`. The DSI driver restarts frame DMA
from its end-of-frame interrupt; masking that interrupt during flash operations
can pause refresh. Failed credential checks write a security audit record. This
is a targeted fix for that path. On 2026-11-01, after flashing this change, the
user retried the invalid-credentials popup and confirmed: "The popup is steady now."
The existing 720/1024 maximum backlight duty is unchanged.

Validation: Waveshare image 2,395,056 bytes, 5,993,552 bytes free; flash hash
verified, ELF prefix `1dda41c94`, HX8394 and GT911 initialized. A 65,536-byte
filesystem write/delete test completed with zero display warnings. The P4 EYE
baseline built at 5,471,344 bytes with its LCD cache-safe option still disabled.
Evidence is in `work/waveshare-p4/popup-refresh-*`. The temporary baseline build
was removed after preserving its config/hash evidence, recovering 1,097,347,072
allocated bytes; the active Waveshare build remains for incremental work.

The old firmware later logged one DSI underrun around 634 seconds uptime, then
SDIO failures, C6 recovery and an instruction-access panic. This was separate
from the initially monitored popup reproduction; its preserved log is
`popup-refresh-before.log`. The short new-firmware test does not establish that
this longer-running failure is resolved.

## SD card slot (2026-11-01)

The default Waveshare profile enables the existing SDMMC HAL and `/sd` VFS.
Board wiring is CLK43, CMD44, D0–D3 GPIO39–42, slot0, four-bit, max40MHz,
LDO4. GPIO45 is the active-low card power switch, **not** card detect.
The socket has no MCU card-detect signal; `SD_MMC_DETECT_PIN=-1` skips that
GPIO and allows a mount attempt. The P4X-EYE's detect45/power46 behavior is
preserved. Slot-specific teardown leaves the C6's SDIO slot1 running.

Normal mounting never formats. On this board physical removal cannot be
reported by a detect switch: use `sdunmount` before removal and `sdmount`
after insertion. Diagnostics explicitly report the absent detect switch.

Flashed image: 2,428,704 bytes, 5,959,904 bytes of application headroom,
ELF prefix `27def58d3`; esptool verified the flash hash. The inserted card
mounted at boot and reported 117,942 MiB capacity. A unique 512-byte test file
was written and read byte-for-byte, read again after unmount/remount, then
deleted. No formatting was requested; pre-existing files were not changed by
the test. No display underrun or panic appeared during this bounded check.
Evidence: `work/waveshare-p4/sd-runtime.log` and `sd-verification.json`.

Host validation: production SD lifecycle tests with ASan/UBSan for both board
wirings, mount/LDO failures, explicit formatting, removal handling, cleanup and
disabled backend; all 15 optional-peripheral configuration tests passed.
The physical no-card case has not yet been exercised; failed probes are covered
by the host test. Long-running SD/radio/display stress remains unqualified.

Remaining board-specific work includes battery sensing (schematic BAT_ADC on
GPIO20; qualify the divider and measured voltage with the existing ADC battery
backend), ES7210 microphone capture, and an attached MIPI camera's sensor/pin
configuration. Charging and accurate charge-state reporting are separate from
voltage estimation; battery support remains disabled in this SD change.
Reference: [Waveshare LCD-5 schematic](https://files.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-5/ESP32-P4-WIFI6-Touch-LCD-5-Schematic.pdf)
and the saved vendor BSP `work/waveshare-p4/vendor/bsp.c`.

The P4 EYE baseline build also passed (5,471,344-byte image, 1,954,704 bytes
free). Its compact configuration/hash evidence is in `sd-baseline-evidence`;
removing the completed temporary build recovered 1,079,574,528 allocated bytes.
The active Waveshare build is retained for firmware/debugging and incremental
work.
