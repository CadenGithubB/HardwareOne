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
- Microphone capture and camera remain disabled/unqualified.
  Battery ADC support is enabled; only USB-powered/no-cell behavior is tested.
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

`HAL_Speaker_Waveshare` implements the shared speaker API and owns TX-only I2S, ES8311 initialization and amplifier gating.
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
application-description and bridge queries. During an explicit authenticated
`c6update`, HAL accepts either a successful MAC query or its explicit remote
`ESP_ERR_WIFI_NOT_INIT` response as update-transport evidence. Normal startup
and recovery require the version query to succeed. Legacy update-only sessions
report normal radio availability as false; unanswered probes still fail.

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
in `components/hardwareone/third_party/es8311/`. The panel register table is attributed to
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

## Battery voltage sensing (2026-11-01)

The Waveshare board header now selects the shared calibrated ADC backend:
GPIO20, R12=200k from BAT and R15=100k to ground, gain3, 6dB attenuation.
These values were visually checked in the official LCD-5 schematic (local
reference `work/waveshare-p4/vendor/schematic.pdf`). The board default enables
battery monitoring; saved configurations can disable it independently.
The EYE's GPIO49/divider configuration is unchanged.

The charger STAT signal terminates at TP1 rather than a P4 GPIO. ADC voltage
alone cannot establish cell presence, charging or USB state; the existing
shared policy continues to report those as unknown and marks percentage as a
voltage estimate. A powered charger can produce BAT-node voltage with no cell
connected. No charger or power-button behavior is changed by this feature.

The user has no battery connected. USB-only checks can qualify ADC startup,
calibrated sampling and honest validity metadata, but cannot establish cell
voltage accuracy, runtime, charging behavior or state-of-charge accuracy.
Those require an appropriate battery and comparison with a meter. Waveshare
specifies a 3.7V lithium battery and MX1.25 two-pin main BAT connector; verify
connector polarity and do not substitute the separate RTC battery header.

Host battery policy/backend/fault/OTA tests passed with ASan/UBSan, including
the Waveshare board's pin/divider/attenuation selection. All 16 optional
peripheral configuration tests passed, including disabling battery monitoring
while retaining SD and touch support.

Flashed image: 2,448,608 bytes, 5,940,000 bytes free, ELF prefix `936507ce9`;
flash hash verified. USB-only sampling passed with calibrated voltage valid,
raw ADC 2680–2681, measured BAT-node voltage 4.18875–4.19025V, and cell presence,
charging and USB state all unknown. The voltage-derived ~99% indication is not
proof of a battery: the user explicitly confirmed no cell is connected.
The SD card remained mounted; HX8394/GT911 initialized and no display underrun
or panic was logged during the bounded test. Evidence is under
`work/waveshare-p4/battery-*`.

The P4 EYE baseline built successfully at 5,471,344 bytes, 1,954,704 bytes free.
After preserving config/hash evidence, its temporary build was removed,
recovering 1,085,513,728 allocated bytes. The active Waveshare build is retained
for firmware/debugging. The temporary schematic render was removed; the
official PDF reference is retained.

## Optional landscape UI — local integration, 2026-11-01

The Display/OLED settings module now has **Landscape UI (90 degrees)**,
stored as `oledLandscape` under its `display` group. It defaults off on existing
and fresh settings. `oledlandscape 1` enables the layout, `oledlandscape 0`
returns to portrait, and `oledlandscape` reads the saved choice. It uses the
shared settings validation/persistence and authorized command path, so the
local settings editor and remote command/settings surfaces share one value.
The setting and command are registered only for the Waveshare display backend.

Landscape rotates the physical surface 90 degrees clockwise. Its 1280x720
surface has a 1024x704 UI viewport on the left (8x integer scaling, 8-pixel
vertical margins) and a 256-pixel strip on the right for the ten touch controls.
The underlying page-packed UI is 128x88: it retains 128-pixel text wrapping and
adds 24 logical rows compared with the previous 128x64 canvas. Portrait also
uses the taller canvas, scaled 5x into 640x440 at (40,380); the original portrait
control positions remain below it. Other display backends retain their canvas
sizes. `oledflip` remains the independent 180-degree logical-UI option.

The native panel/framebuffer and GT911 diagnostic coordinates stay 720x1280.
The HAL owns the surface transform; touch hit-testing applies its inverse, and
the UI still owns the drawn buttons. Display-owner frame boundaries apply
layout changes before input, erase old geometry, repaint the controls, and
discard held/pending contacts. A failed control submission is retried. Logical
frame caching includes all 1408 canvas bytes, including the extra bottom pages.
Wake brightness gating and the running-scanout full-speed clock requirement
remain in force in both layouts.

Host checks cover both pixel transforms, every output pixel, framebuffer bounds,
control bounds/separation, all ten rotated actions, gesture cancellation across
layout changes, control painting, bottom-page cache changes and wake sequencing.
Affected settings/command tests and P4-EYE/disabled-display checks also pass.
Firmware build: Waveshare 2,504,128 bytes, 5,884,480 bytes app headroom.
P4-EYE phone-pipe baseline build passed: 6,125,808-byte application,
1,300,240 bytes headroom. All 19 affected host suites passed. Physical landscape
testing is pending; the user has requested flashing and enabling the setting.

## Glasses/phone Bluetooth and readable errors — 2026-11-01

The saved Waveshare profile now includes `ENABLE_G2_GLASSES=1` and
`ENABLE_BLE_PHONE_PIPE=1`, as requested. The shared client host supports the
Glasses and Phone pipe menu roles; the app-console server remains selectable.
Existing accounts, layout choice and network settings are preserved by an
application-only update. Pairing/enrollment remain explicit user operations.

A reproduced Bluetooth ON failure came from a menu action that always ran
`openble`, including on the earlier server-only profile. Its central setup
worker was compiled out, so it misleadingly reported unavailable worker memory.
The menu now starts `openconsole` on server-only builds and `openble` where
client devices are built. Host checks exercise all four glasses/pipe selections
at 64-, 88- and 128-pixel canvas heights (12 builds, 14,869 checks). Direct
hardware testing confirmed the old client refusal and working server advertising.

Display failures now use the shared error style: an error icon, word-wrapped
text, an eight-second minimum duration, and automatic return to the underlying
screen. The reply and toast buffers retain up to 191 characters; the available
screen area shows up to six lines (four on a 64-pixel OLED), with an ellipsis
when text exceeds that space. Error prefixes are normalized consistently. Toast
expiry invalidates an otherwise static screen, handles timer wrap, and rendering
uses a locked snapshot without holding the lock through graphics operations.
The Add Wi-Fi flow uses the same error/success severity convention.

Wi-Fi connect/radio commands now preserve known causes in their returned reply,
including disabled Wi-Fi and companion startup refusal. Empty saved-network
lists return `No saved Wi-Fi networks. Add one in Wi-Fi settings.` instead of a
bare `ERROR`; a failed list snapshot is reported separately. Connection failures
suggest checking signal and password without claiming an unobserved cause.

Verification: both profile builds passed; 26 affected host suites passed across
the initial run and the icon-harness dependency retest, plus final Add Wi-Fi and
command-feedback reruns. Toast tests execute production rendering/lifetime on
64/88/128-pixel canvases; command tests execute the real Wi-Fi refusal paths.
Waveshare application: 3,369,712 bytes, 5,018,896 bytes app headroom.
Installed application-only with hash verification. Selected the client role,
closed the previously running app-console server, and verified shared client
stack startup. Landscape remained enabled. The live `openwifi` command returned
the explicit no-saved-network error, with the original Wi-Fi enable setting
preserved; C6 transport remained online with zero errors/timeouts. Serial was
logged out and closed. Physical pairing/link qualification awaits the user;
P4-EYE was build-checked but has not been reflashed with these shared changes.

Cleanup removed approximately 23 MiB of temporary host build. The single normal
Waveshare build cache (approximately 957 MiB) is retained for this active board
test, alongside the pre-existing P4-EYE cache (approximately 1.1 GiB) for its
pending application-only update. Matching firmware/debug and compact evidence
are retained in `.scratch/waveshare-feedback` (approximately 21 MiB). Remove
Waveshare compiler objects once qualification completes without further edits.

## Local transcription parity (2026-11-01)

The saved Waveshare profile now enables `ENABLE_LOCAL_STT` and
`ENABLE_STT_MODEL_15X5_GEN2`, matching the model selection of `p4-phone-pipe`.
The other saved P4X-EYE profiles already inherit local transcription from their
existing feature profile. The Waveshare uses the connected G2 microphone; its
onboard microphone remains disabled. No UART host or Wi-Fi is required.

The selected model is `/sd/STT Models/quartznet15x5-gen2-25397.p4.stt`.
The user supplied the matching 20,112,224-byte model; its envelope, frontend
identity and payload SHA-256 were verified against the generated identity.
The model file SHA-256 is
`dccef3a8cead069d97d780e0d08ddfc65d9f0870c0af4673b29327f00158f048`.

Readiness now distinguishes a missing microphone, microphone contention,
unmounted model card, inaccessible model, wrong model size, disabled engine,
and CM5 session/service failures, with a next action. The local readiness
worker retains the backend's static refusal instead of replacing it with
“local model unavailable.” Its cheap check verifies presence/size; inference
still verifies the pinned model identity before use.

The shared transcription screen wraps status text while retaining menu space.
Explicit failed actions use the shared eight-second error toast, and setting
failures preserve the actual refusal. Polling does not repeatedly show toasts.
Larger bounded message buffers preserve the model path and next action.

Verification: eight affected host suites passed, with sanitizer coverage where
supported: local dictation (both providers plus disabled stubs), local backend
LM/readiness (including the SD gen2 selection), OLED transcription at
64/88/128 pixels, command adapter, HTTP/JavaScript, OLED/G2 dictation consumers,
G2 transcription, and activity policy. Both final firmware builds passed:
Waveshare 3,720,672 bytes with 4,667,936 bytes app headroom; P4X-EYE
6,128,864 bytes with 1,297,184 bytes headroom. P4X-EYE is build-checked only;
its updated application flash and user verification remain pending.

Deployment/model-installation evidence is in `.scratch/waveshare-local-stt`.
The existing normal build caches are retained for active hardware qualification:
Waveshare approximately 981 MiB and P4X-EYE approximately 1.1 GiB. Host suites
remove their temporary build directories automatically. Retain only compact
firmware/debug evidence once hardware iteration ends.

The Waveshare application-only update passed esptool hash verification. Live
status confirms `continuous:true` and the new actionable missing-microphone
message while the glasses are disconnected; landscape remains enabled and C6
transport reports zero errors/timeouts. The supplied model was copied to
`STT Models` on the Waveshare's FAT32 card, its complete SHA-256 read back and
verified, and the card safely ejected. Device model readiness and a spoken
transcription test await reinsertion/reconnection and user qualification.
Compact matching firmware/debug evidence is approximately 23 MiB.
