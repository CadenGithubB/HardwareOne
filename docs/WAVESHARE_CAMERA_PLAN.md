# Waveshare camera support plan

Status: planned; no camera code, configuration or firmware changed by this plan.

## Objective and scope

Add the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 camera to the existing camera
service, so its commands, settings, web/G2 interfaces, snapshots and recording
use the same public contract as P4X-EYE and XIAO Sense. Keep current P4X-EYE,
S3 DVP and camera-disabled behavior compatible.

The attached sensor has not been confirmed. OV5647 is the initial candidate
because it is documented by Waveshare and its driver is already in the checked
`esp_cam_sensor` dependency. Confirm the actual module, cable orientation and
clock/reset/power wiring before selecting sensor defaults or testing hardware.
Other attached sensors require their own supported driver and format selection.

## Existing architecture to extend

- `HAL_Camera.h`: `CameraBackend`, `CameraConfig`, owned JPEG `CameraFrame`,
  supported resolution/control masks, and backend information.
- `HAL_Camera.cpp`: selects the P4 CSI or S3 DVP backend. Persisted frame-size
  IDs and CLI control units are already shared.
- `System_Camera_DVP.cpp`: despite its historical name, this is the common
  serialized camera service used by both drivers. It owns runtime state,
  settings/command integration, capture validation and recovery.
- `HAL_Camera_P4.cpp`: CSI/ISP capture, RGB565 processing and hardware JPEG.
- `HAL_Camera_DVP.cpp`: existing S3/esp32-camera implementation.
- Web/G2 settings, image consumers and video recording already use these
  interfaces and capability reporting to varying degrees.

Waveshare and P4X-EYE should use the same P4 capture/encode implementation,
parameterized by board wiring and sensor/native-format selection. SDK/DMA
buffers remain private to the backend. Consumers continue to receive bounded,
owned JPEG frames with explicit release semantics.

## Implementation sequence

### 1. Confirm the hardware and shared-bus solution

Record the attached sensor identity and verify the LCD-5 schematic/BSP wiring.
Camera SCCB is on GPIO7/8, shared with touch, ES7210 and ES8311. The current
P4X-EYE backend owns a dedicated I2C0 and hardcodes its board's power/reset/XCLK
pins; those assumptions cannot be applied to Waveshare.

First prove a bounded SCCB integration with the existing I2C owner. The checked
`esp_video` API accepts an existing IDF master-bus handle, while this project's
manager currently exposes Arduino `TwoWire`. A native handle alone does not
provide the manager's locking, recovery and lifetime guarantees. Inspect the
checked SDK/Wire implementation and use the smallest supported integration
that provides all of the following:

- One controller/pin owner for the physical shared bus.
- Camera SCCB operations serialized with touch and audio transactions.
- Bus recovery/shutdown coordinated with active camera clients.
- Camera stop releases its client resources without deleting a borrowed bus.
- No I2C lock held while waiting for a video frame or encoding JPEG.

If this requires a shared-bus lease or SCCB adapter, add it in the existing I2C
owner and cover it with actual implementation tests. Do not migrate unrelated
I2C devices or dependencies as a side effect. This milestone must pass before
enabling camera support in the normal Waveshare profile.

### 2. Parameterize the existing P4 backend

Move physical power/reset/XCLK/SCCB definitions out of P4X-EYE constants into
board definitions. Represent absent pins explicitly. Preserve P4X-EYE's current
rail handling, dedicated bus and qualified OV2710 mode.

Select the verified Waveshare sensor through board SDK defaults and the saved
configuration workflow. Use the existing OV5647 component if that is the
attached sensor; do not introduce another camera stack.

Remove the P4 driver's fixed 1280x720 layout assumption for Waveshare. Read and
validate the selected native format, dimensions, stride, buffer capacity and
ISP output. Size buffers and crop/resize from those checked values. Retain the
qualified P4X-EYE format and existing application frame-size IDs. Request a
backend-supported format and advertise only output sizes the implementation
can actually produce. Identify the detected sensor accurately in status.

### 3. Keep commands and settings standardized

Audit every camera entry point against the existing common service:

- Startup/shutdown, capture, source status and recovery.
- Resolution, JPEG quality, mirror/flip and other sensor controls.
- Web/G2 settings, previews, snapshots, image capture and recording.

Use one capability result for availability across these surfaces. An unsupported
control is hidden or disabled in a UI and explicitly refused by commands. A
failed change leaves the previous effective configuration intact. Report the
saved request separately from an effective runtime fallback; do not overwrite
saved settings just because a different sensor cannot apply them.

Preserve command names, settings keys, persisted IDs, CLI units, permissions,
transport identity and frame ownership. Keep ranges/choices in their existing
shared owner. Add capability metadata only if a real new sensor restriction
cannot be expressed by the current contract. Native sensor register access
remains optional and permission guarded.

Use actionable service errors through the existing display feedback path:
missing sensor, unavailable shared bus, incompatible sensor format, capture
failure, or insufficient buffer memory should identify the observed reason and
next action. Logs retain stage/driver detail.

### 4. Qualify resources and recovery

Account for CSI buffers, ISP, JPEG workspaces, the 720x1280 display, audio and
local STT within the board's actual memory budget. Start with a conservative
capture configuration. Check actual sensor/ISP frame-rate choices; advertise
supported values rather than promising full sensor resolution or recording
speed in advance.

Start failure must unwind only camera-owned resources. Repeated start/stop,
absent sensor, capture timeout and recovery must preserve touch, audio, SD,
display and radio. Memory admission failure must be explicit, without silently
turning off unrelated features. Camera startup remains opt-in and an absent
module must not cause boot failures or restart loops.

### 5. Verify and deploy

Host verification should execute production code at the relevant boundaries:

- Both P4 board configurations and camera-disabled builds.
- Dedicated versus shared SCCB ownership and failed-start cleanup.
- Native format/stride validation and JPEG bounds/release behavior.
- Common command/settings validation, capability visibility and refusal behavior.
- Existing DVP consumers and recording lifecycle.

Build the Waveshare profile, P4X-EYE baseline and an affected S3 Sense camera
profile, sequentially where the shared dependency registry is mutable. Check
compiled sensor choices, image headroom and buffer budgets. Reuse normal build
directories; retain compact matching firmware/debug evidence and remove
experiment-only output.

On hardware, verify real preview/snapshot, supported controls, JPEG validity,
repeated start/stop and a short recording. Exercise touch and onboard audio
alongside camera activity, then check STT coexistence and resource refusal.
Compare P4X-EYE behavior after its application-only update and test S3 camera
hardware if available; otherwise label S3 qualification as host/build only.

Follow the existing deployment agreement: Waveshare application-only flash,
user verification, P4X-EYE application-only flash preserving setup, user
verification, then push. Do not erase data or change startup preferences as part
of adding the backend.

## Completion criteria

The Waveshare's confirmed camera works through the existing camera commands,
settings, snapshots and recording consumers. Every UI reports the same supported
choices as the command service. P4X-EYE and S3 retain their existing contracts.
Missing/unsupported hardware fails cleanly, shared peripherals remain usable,
and documented hardware results are distinguished from build-only checks.
