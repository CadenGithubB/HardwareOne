# Portable JPEG decoding

HardwareOne's G2 JPEG file viewer, camera preview and camera stream share
`HAL_JPEG`. They request an image without selecting a processor. ESP32 and
ESP32-S3 use the existing Espressif TJpgDec software decoder. Targets exposing
`SOC_JPEG_DECODE_SUPPORTED` and a qualified driver can use the optional hardware backend, with software
fallback in the default `Auto` mode.

This change adds **decoding**. Camera-produced JPEGs still pass directly to
storage and streaming; they are not decoded and re-encoded. The Edge Impulse
module's separate conversion paths are unchanged by this milestone. Shared
encoding can be added when there is a raw-frame consumer that needs it.

## Application contract

- Input bytes remain caller-owned and must stay valid during the synchronous call.
- `Image` owns its output and frees it on destruction or `reset()`. It is not
  copyable. A failed decode leaves an empty image.
- Output is top-to-bottom, tightly packed **RGB888**, with explicit dimensions,
  byte count, stride and selected backend. An old G2 comment described the legacy
  JPEG conversion as BGR, but its TJpgDec configuration actually emits RGB.
- G2 software images use the existing `ps_alloc` policy and allocation tracing.
  Runtime PSRAM bypass selects software decoding and retains internal-memory allocation.
- Dimensions come from the image, not mutable global camera settings.
- Input/header bounds and output budgets are checked before either backend.
  Defaults preserve the file viewer's 1600-pixel dimension limit; its separate
  128 KiB compressed-file cap remains at the call site.
- Valid repeated marker fill bytes are normalized before decoding, preserving
  compatibility across ROM and bundled TJpgDec versions without relying on their
  different header-skipping behavior. Input storage is not modified.
- Truncated JPEGs, duplicate frames and malformed segment/table lengths fail
  cleanly. Progressive JPEG support is not added: the retained software decoder
  does not support it. Such inputs never reach the baseline-only accelerator.

## Backends

Software calls `esp_jpeg_decode` directly with the real destination capacity and
its own scratch workspace. This retains the prior library/colour settings while
avoiding `fmt2rgb888`'s shared static workspace and unbounded output-size promise.

The P4 backend accepts a conservative baseline subset: 8-bit colour, one
interleaved scan, standard component/table layouts and 4:4:4, 4:2:2 or 4:2:0
sampling. Dimensions must be multiples of eight. Other otherwise-supported
images, including grayscale and odd sizes, use software. `Auto` also falls back
if the accelerator is busy, cannot allocate, or returns an error.

Hardware DMA alignment and MCU padding are private to the backend. It owns a
fresh engine per request, allocates compatible buffers, and compacts padded rows
in place. A nonblocking admission guard prevents competing JPEG requests from
waiting on the same accelerator. `SoftwareOnly` and `HardwareOnly` modes are
available for qualification; application callers use `Auto`.

The current application has no other 2D-DMA consumer. Before adding PPA or another
independent 2D-DMA user, the pinned IDF driver's queued-job timeout cancellation
must be qualified or fixed; the JPEG guard cannot protect an unrelated driver.

## Compatibility and qualification

The shared application remains one codebase. CMake includes `esp_driver_jpeg`
only when the target advertises hardware JPEG decoding; all targets retain
`esp_jpeg`. No ESP-IDF 6 upgrade is required. The pinned IDF 5.5.5 JPEG driver
needs the narrow allocation-error unwind fix supplied with the experiment.
Its prepared component exports `HW1_JPEG_DRIVER_QUALIFIED=1`. Without that marker,
the HAL safely stays on software even on P4; merely enabling the chip capability
does not opt an unqualified driver into acceleration.

Host tests use original synthetic JPEG fixtures, the actual TJpgDec library and
production HAL code. Driver stubs cover dispatch, padding, failures, cleanup and
contention. They cannot prove that the silicon produces correct pixels or a
speed improvement. The standalone probe in `experiments/jpeg_portable/codec`
compares both backends and reports timing, pixel differences and concurrent
allocation behaviour on each physical board. See that experiment's results for
the checks actually completed.
