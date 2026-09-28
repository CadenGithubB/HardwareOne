# Shared camera qualification — 2026-09-28

Status: camera and cross-board qualification passed on the attached P4X-EYE
and XIAO ESP32-S3 Sense. Final applications are flashed; original preferences
and runtime network state were restored.

## Scope and architecture

The shared camera lifecycle, settings, commands, image saving, web page and G2
settings now use `HAL_Camera`. The P4X-EYE backend captures OV2710 CSI/ISP RGB565
and encodes JPEGs using the P4 hardware. The XIAO ESP32-S3 Sense backend keeps
esp32-camera and its sensor-produced JPEGs. Existing resolution IDs are retained.
Capabilities describe each backend's supported sizes and controls; unsupported
live changes are rejected without persisting a false success.

P4 smaller sizes use a center crop and software resize. This milestone does not
add video recording, hardware scaling, autofocus, or general-purpose JPEG
encoding outside the camera backend. The existing shared JPEG decoder remains.

The S3 backend now validates the JPEG entropy stream through the shared JPEG HAL
before handing a frame to consumers. The validator discards decoded MCU output
and uses a per-call decoder workspace instead of allocating a full RGB image.
Malformed native frames are released and retried at most three times. No
esp32-camera vendor fork is introduced by this change.

## Build environment

- Isolated branch: `codex/jpeg-portable`, based on battery milestone `8d9562e`.
- ESP-IDF 5.5.5, commit `b774170ff46c393eeb5e495ea37936038d3f4f4f`.
- Existing qualified Bluetooth and P4 JPEG SDK overlays are retained.
- P4: pinned esp_video 2.2.0, OV2710, 32 MiB PSRAM, 16 MiB flash.
- S3: pinned esp32-camera 2.1.4, OV3660, 8 MiB PSRAM, 8 MiB flash.
- S3 qualification profile uses I2C0, 128 KiB camera JPEG buffers, and disables
  direct PSRAM DMA. The primary Wire1 sensor bus remains available.
- Both profiles enable the shared camera and Sensors web page. SD and microphone
  are disabled for the boards-only tests. Existing radio, power and BLE choices
  are retained; P4 display/input code remains available when accessories return.

## Installed final application images

Both app-only writes were verified against the firmware bytes. The partition
tables match the pre-task flash backups.

| Board | Bytes | SHA-256 |
| --- | ---: | --- |
| P4 | 4,871,584 | `49d40621f699554a37f46b9f6bd30e07cf2b9e6e2c726287edc479ea9c3be140` |
| S3 | 4,956,416 | `61ac0efbb5f51ea1c837497f8122e854dcc6a1f9c8231ada2b73a726bc24306d` |

## Physical evidence

The corrected P4 camera backend passed run `20260928T151919Z-da04ef`:
17 checks, 12 saved JPEGs, all strictly decoded without libjpeg warnings.
All ten advertised resolutions passed: 96×96, 160×120, 176×144, 240×176,
240×240, 320×240, 400×296, 640×480, 800×600 and 1280×720.
Both quality endpoints (0 and 63), temporary tiny-capture restoration, three
start/stop/capture cycles and unsupported-control rejection passed. QVGA, VGA,
SVGA and HD photographs were visually checked. Camera settings were restored
and all test photographs removed from the board.

That sweep used P4 image SHA-256
`76c6d668d612fea8ad5a9c7f09ca3516aa4e1c1c2bc2fcaf85700617d83a0d12`.
The final image also includes the stopped-camera web selection fix, complete
Sensors page markup and the shared software entropy-validator API; its camera
backend is unchanged.

The guarded S3 camera code passed run `20260928T153900Z-4e2588`:
13 checks and nine saved JPEGs, all strictly decoded without warnings.
Its seven supported resolutions passed: 160×120, 176×144, 240×176, 240×240,
320×240, 400×296 and 640×480. Both quality endpoints, temporary tiny-capture
restoration, and three restart/capture cycles passed. The VGA photograph was
visually checked; settings were restored and all test files removed. This run
needed no USB rereads and observed no rejected native frames.

Earlier S3 testing exposed the malformed QCIF frame that prompted the shared
validation guard. Five consecutive steady-QCIF captures passed in preliminary
run `20260928T151904Z-f750be`; the final full sweep above qualifies the guarded
firmware. The final image adds only the Sensors HTML close to this tested
configuration; the optional AI module is disabled in both profiles.

The final S3 ROM decoder passed a direct fixture check in
`s3-validation-153716.json`: the retained damaged QCIF image was rejected in all
five trials, and good QCIF/VGA images were accepted in all five trials each.
All three uploaded files were independently read back byte-for-byte before the
checks, then removed. Median validation times were 3.841 ms for the damaged
QCIF image, 3.559 ms for good QCIF, and 30.241 ms for good VGA at 240 MHz. These
are sample-image measurements in the running app, not a universal frame-rate
or worst-case latency claim. The S3 ROM validator uses a 3,100-byte workspace.

The final flashed images passed 12 cross-board checks in run
`20260928T155108Z-df3f8a`. Each board saved a strictly valid JPEG; encrypted
messages were delivered in both directions with cameras running. S3 then joined
P4's isolated AP and verified:

- Unauthenticated frame request: HTTP 401.
- Login: HTTP 303 with a session cookie.
- Sensors page: HTTP 200, 108,223 bytes, complete transport and closing HTML.
- Camera status: HTTP 200, with hardware JPEG reported on P4.
- Frame: HTTP 200, 1,089-byte 160×120 JPEG, fully decoded without warnings.
- S3 capture while its Wi-Fi client was connected.
- Logout followed by frame request: HTTP 401.

Camera settings and test files were restored/removed. One missing USB file-read
reply was safely reread at the same offset; no malformed chunk was accepted.
The temporary web session was logged out, S3's cookie cleared, and P4's HTTP
server stopped before the final reboot check.

## Issues found during qualification

- The streamed Sensors page omitted its closing page markup. A complete HTTP
  response alone did not prove complete markup; the board-to-board HTML check
  caught this. The page now closes its shell after all sensor scripts, before
  the outer response terminator.
- Optional Edge Impulse input used a VGA-sized RGB buffer without bounding the
  decoded JPEG. Both camera and file conversion now inspect dimensions and
  capacity first, reject larger P4 images, and resize using actual dimensions.
  Its focused sanitizer regression covers VGA, HD, insufficient buffers and
  malformed inputs; no extra RGB allocation is introduced.
- P4 DMA output corruption: dirty cache lines from allocating the JPEG output
  could overwrite the encoder's output. Cleaning and invalidating the aligned
  output allocation before DMA fixed the reproduced zero-filled blocks. The
  backend regression covers ordering, sync failure, cleanup and retry. The
  earlier `20260928T145606Z-de7c0c` sweep is explicitly rejected despite structural
  decoding initially accepting its damaged images.
- S3 direct PSRAM DMA caused capture timeouts in this profile. Disabling it
  restored the previously proven buffered capture path.
- A S3 QCIF image had valid markers but an incomplete final entropy block.
  The bounded shared software validator rejects this retained failure fixture.
  Stale DMA-tail bytes in the camera driver are a plausible cause, not a proven
  root cause. The guard prevents that reproduced malformed stream being accepted.
- Existing USB diagnostic output can lose a file-read chunk or completion reply.
  The harness checks offsets, lengths, base64 and EOF, rereads malformed chunks,
  and fails on an unknown completion barrier. It never treats an interrupted
  download as a passed image. A diagnostic reread of a failing saved JPEG retains
  both copies and byte equality; the original qualification remains failed.
  The interrupted P4 run `20260928T151355Z-60364f` was recovered by reopening the
  console, restoring camera settings, and deleting only its owned test file.

## Host coverage

Address/undefined-behavior sanitizer checks cover shared lifecycle and recovery,
owned frame bounds, temporary settings restoration, concurrent capture/stop,
DVP allocation/geometry/readiness/retry, P4 resource/cache lifetimes, crop/resize,
G2 settings and controller ownership. I2C reservation covers 18 combinations of
chip family, camera enabled/disabled and SDK SCCB controller selection.

The actual software JPEG codec is tested with FASTDECODE 0/1/2 and scaling
on/off, including entropy truncation with intact headers/end markers, marker
normalization, unsupported formats, allocation failure and the private observed
failure image. Existing decoder parity/concurrency tests still pass. Web tests
cover actual embedded JavaScript and stopped/running resolution selection.

## Preservation and limits

Full flash backups were verified before app-only writes at `0x10000`. Physical
chip identities and unchanged partition tables were checked. Private photos,
USB logs, credentials, backups and detailed results remain ignored; no captured
photographs are committed.

No SD cards, local display/input accessories, G2 glasses, R1 ring or Android app
were present for these camera tests. G2 menu behavior is covered by host tests,
not a new physical glasses test. SD persistence, long-duration video/streaming,
battery-powered operation, physical AI inference and throughput/power benchmarks
remain unqualified.
The temporary HTTP network uses P4 as AP and S3 as client, with no workstation
Wi-Fi/Bluetooth changes. Both boards are rebooted and their preserved settings
checked after that test.

Final reboot/state check `postcheck-155224.json` passed for both boards. Existing
accounts still authenticated; encrypted peers/mesh fingerprints, Bluetooth
preferences, power profiles and camera preferences matched the baseline. Both
were disconnected from the temporary Wi-Fi network, HTTP and cameras were
stopped as before, and P4 battery voltage telemetry remained valid.

Saved storage preference remains SD (`cameraStorageLocation=1`) even though no
card is fitted. To save new photos internally, explicitly choose
`camerastoragelocation 0`. The tests used internal storage temporarily and did
not change that saved preference permanently.
