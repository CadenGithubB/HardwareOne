# Camera preview on the shared OLED-style UI

The shared UI now has a Camera mode under Sensors. It uses the existing camera
service and JPEG decoder, so camera commands, backend selection, settings, web
capture and color photos retain their existing owners. Open it through the
sensor menu or `oledmode camera` after signing in on the display.

## Controls and presentation

| Input | Action |
| --- | --- |
| A | Hold/resume the displayed preview |
| X | Start/stop the camera through the authorized command executor |
| Y | Save a new full-color photo through `camerasave` |
| Up/down | Increase/decrease preview contrast, from -2 through +2 |
| Left/right | Hard threshold / ordered dithering |
| B | Standard back navigation |

Holding freezes the displayed bitmap only. Y takes a new photo, including when
the preview is held; it does not save that held frame. The preview drains its
own capture/decode operation before submitting the photo command, then restores
the previous held/live state. Presentation choices are session-local and do not
change persisted camera resolution, quality, mirror or photo storage settings.

The image preserves aspect ratio inside the actual content rectangle. The
128-pixel-wide canvas leaves room for the normal header/footer plus an eight
pixel status line: image viewports are 128×34 on a 64-pixel OLED, 128×58 on the
Waveshare's 88-pixel logical landscape canvas, and 128×98 on the EYE canvas.
These are shared UI coordinates, not the physical P4 panel resolution.

## Rendering and lifetime

`CameraMonochrome.h` area-downsamples RGB luminance before conversion to an
MSB-first one-bit bitmap. Bounded histogram contrast avoids amplifying nearly
flat dark scenes; smoothed tone limits reduce pumping. A fixed destination-
anchored 4×4 Bayer pattern represents gray as dot density without temporal
flashing. Hard threshold is available for high-contrast subjects.

`OLED_Mode_Camera` owns a low-priority worker with a 400 ms delay between
capture/decode passes. It uses owned camera frames, bounded shared JPEG decoding
and PSRAM-preferred scratch. The UI copies a completed bitmap under a short lock
and draws after releasing it. `cameraStateSnapshot` supplies nonblocking owned
service status; camera contention is reported as busy rather than blocking the
UI. `oledcameraread` returns bounded metadata, timings and worker stack headroom,
without exporting cached pixels.

The worker runs only while the signed-in, non-Guest display session is awake
and showing Camera. View generations, setting revisions and transport epochs
discard late results after navigation, hold or identity replacement. Leaving,
display sleep and logout clear cached pixels and retire the worker. Camera
power remains with the common service and explicit X/start/stop commands.
Preview failures use the shared error toast with an eight-second minimum.

## What the thermal viewer teaches us

`i2csensor_mlx90640_oled.h` already demonstrates mode registration, snapshot
ownership, unavailable states and authorized start/stop commands. Its current
image renderer uses three temperature bands: off, a checkerboard of whole
sensor cells, and on. It ignores `oledThermalColorMode` and clips drawing to
the literal height 64. Its per-frame min/max mapping can also change contrast
as a hot/cold extreme enters the scene. Reuse its integration pattern, rather
than copying these rendering restrictions into camera preview.

## Verification and remaining hardware checks

Production renderer/mode host tests pass for 64, 88 and 128 pixel canvas heights,
including flat/gradient inputs, bounds/stride failures, freeze/resume, drained
photo requests, stopped-source recovery, stale view/session rejection, allocation
failures, stream ownership and worker cleanup. Camera/OLED-disabled combinations
also compile and report unavailable. Shared camera lifecycle, consumers, async
power, display feedback, layout, availability and session-role checks pass.

Waveshare, P4X-EYE and the XIAO baseline firmware build successfully. Physical
results are recorded in `WAVESHARE_P4_SUPPORT.md`; EYE and a true monochrome OLED
still require their own physical preview/control qualification. Hold/photo
semantics and readability need the user's check on the actual display.

Both P4 panels physically support color; their current shared UI adapters expose
a one-bit canvas. A later color-preview operation can live behind the display
HAL while retaining this monochrome fallback and the same camera service.
Changing the whole UI to color is not required for the first preview.
