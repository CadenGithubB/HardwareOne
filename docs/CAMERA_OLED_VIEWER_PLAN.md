# Camera preview on the shared OLED-style UI

The camera backend is now qualified on Waveshare for snapshots and recording.
There is currently no registered OLED camera-preview mode. The web preview and
G2 camera/image paths are separate existing consumers.

## What the thermal viewer teaches us

`i2csensor_mlx90640_oled.h` already demonstrates mode registration, snapshot
ownership, unavailable states and authorized start/stop commands. Its current
image renderer uses three temperature bands: off, a checkerboard of whole
sensor cells, and on. It ignores `oledThermalColorMode` and clips drawing to
the literal height 64. Its per-frame min/max mapping can also change contrast
as a hot/cold extreme enters the scene. Reuse its integration pattern, rather
than copying these rendering restrictions into camera preview.

## Recommended first version

1. Add one Camera mode through the existing OLED registry, using the common
   camera service and owned frames. Keep photo resolution, quality and saved
   color images independent of the preview's presentation choices.
2. Capture/decode on a bounded preview worker while this mode is visible.
   Draw only a completed cached bitmap on the UI task. Start conservatively
   around 2–3 updates/second and measure frame time, memory and touch latency;
   busy/failure states keep navigation responsive and identify the real cause.
   Mode changes cancel preview work and discard late results safely.
3. Use the shared JPEG decoder; fit the image into the actual content rectangle,
   preserve aspect ratio, and downsample before converting to one bit. Do not
   change the camera's global saved frame size to suit one screen.
4. Convert to luminance, apply bounded contrast/shadow adjustment, then use a
   fixed spatial ordered-dither pattern anchored to destination pixels. This
   approximates gray with dot density. Smooth automatic contrast over time to
   avoid pumping; do not alternate frames to fake gray. Offer a simple hard
   threshold as an optional high-contrast mode. Defer edge detection and extra
   filters until the basic live view is useful.
5. Provide live/frozen indication, freeze/resume, and save through the existing
   authorized camera command path. Keep controls and status outside the image.
   Support 128×64 OLED, the taller EYE canvas and Waveshare landscape without
   fixed panel dimensions in the renderer. Leaving the page stops preview work;
   camera power remains with the existing service and explicit start/stop actions.
6. Verify gradients/dark scenes/edges, aspect ratio, bitmap bounds, late-worker
   cancellation, unavailable hardware, save failures and camera-disabled builds.
   Measure real preview/touch/STT coexistence on Waveshare, then verify EYE and
   a true monochrome OLED if connected.

Both P4 panels physically support color; their current shared UI adapters expose
a one-bit canvas. A later color-preview operation can live behind the display
HAL while retaining this monochrome fallback and the same camera service.
Changing the whole UI to color is not required for the first preview.
