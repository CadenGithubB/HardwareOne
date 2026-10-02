# HardwareOne host tests

Host-side tests for firmware logic that is pure enough to compile off-target.
They compile the **real** headers from `components/hardwareone/`, not copies —
the same rule `updater/test/host/CMakeLists.txt` states for the recovery
throttle. `System_LLMUtf8.h` is kept dependency-free (no Arduino, no ESP-IDF, no
project headers) precisely so this is possible; the same boundary applies to
`System_MapViewportCore.h`. If either gains a firmware-only include, its host
target stops building and the coverage silently disappears.

Run from the repository root:

```sh
cmake -S components/hardwareone/test/host -B /tmp/hw1-hardwareone-host
cmake --build /tmp/hw1-hardwareone-host
ctest --test-dir /tmp/hw1-hardwareone-host --output-on-failure
```

## Untrusted web file responses

`web_file_response_policy` compiles the production CSP/header helper and the
policy guards of both file read/view handlers with mock HTTP header storage.
It checks both sandbox variants, MIME/cache policy, and aborting on failure of
each header insertion before reaching file streaming, including polling cleanup.
Source guards cover placement before all format/raw branches and prevent branch
overrides. It does not execute firmware streaming or emulate browser security.

All file responses disable scripts/forms. SVG documents additionally get an
opaque origin. Escaped text and inert media retain their origin for authenticated
viewer links and native playback; this does not enable scripts. The SVG decision
uses the same decoded path suffix as the image MIME branch, including mode=raw.

For real browser checks against the compiled production headers, run:

```sh
python3 -B components/hardwareone/test/host/test_web_file_response_policy.py --serve-fixture
```

Open the printed loopback URL. The unprotected baseline must execute its SVG
script and record authenticated synthetic GET/POST/event probes. Protected SVG
and raw SVG must retain their static picture and produce no probes, including
after clicking the event-handler button. Check escaped text and its authenticated
navigation link (must report True), native audio, and binary downloads. The
home page's script and fetch of protected plain
text must still work. The fixture uses only a synthetic cookie and local data;
it does not contact a device. Browser checks are manual and are not part of
CTest. SVGs requiring scripts or external resources intentionally lose those
features; static inline styling and embedded data images remain allowed.

## Web batch response buffering

`web_batch_handlers` extracts and compiles the production local/bonded HTTP
batch handlers, settings marker helpers, and settings cleanup class on every
run. It uses the vendored ArduinoJson implementation and its Arduino String
adapter, with host mocks for String storage, HTTP, command execution, session
identity, and allocation. The `.cpp` harness is a template for the Python runner,
not a separately compiled copy of the handlers. ASan/UBSan follow `HW1_SANITIZE`.

Coverage includes reply ownership/escaping, empty slots and counts, ordinary
command errors, confirmations, settings finalization, session invalidation,
server shutdown, and response-document allocation failures. Failed buffering
must not skip command execution/cleanup or expose partial results as success.
The final `String` serialization path is deliberately unchanged by this
refactor. These tests do not measure physical PSRAM placement, internal-heap
savings, real executor races, or final-String allocation failure on hardware.

## Filesystem and G2 PSRAM output

`fileread_psram`, `filesystem_psram_listing`, and `g2_files_psram` compile the
actual extracted handlers/walkers/viewer with the production `PsramBuffer`.
They mock filesystem, authentication, HTTP/BLE/rendering and crypto-key/open
boundaries; the listing permission source contract still checks the real
lock-bound authorization implementation separately. ASan/UBSan follow
`HW1_SANITIZE`.

Coverage includes complete listing JSON/text parity, independent HTTP/command
output ownership, SD naming/counts, permissions-query order, allocation and
capacity failure, fileread encoding/offset/EOF fields and raw-BLE send ordering,
G2 raw/pretty/parse-error output, and malformed encrypted rows that expand on
failure. The existing String reader/decrypt/wrap APIs remain available to other
viewers. Streaming wrap state is additive and does not change chat/event calls.

`memutil_tests` and `memutil_no_psram` additionally exercise the real owned-buffer
growth, self-append, NUL/bounds checks, sticky failures, reuse, and PSRAM fallback
or bypass with mocked heap backends. These are not physical heap measurements.

No directory pagination or new client protocol is introduced. Commands retain
the 4096-byte result budget including NUL; HTTP listings have independently
owned growing storage and are not capped to the command budget. Failed output
allocation never publishes a partial successful listing. Physical PSRAM usage,
BLE congestion/disconnects, SD I/O failures and actual G2/OLED presentation still
require device testing.

`http_feature_gates` preprocesses the actual build header using its deployment
override hook for 100 network/web/custom-WiFi/custom-HTTP combinations. It checks
that the web page flags, including Power, are zero whenever HTTP is disabled.
It does not claim that all unrelated hardware combinations link or boot.

## ToF PSRAM object lifecycle

`tof_psram_lifecycle` extracts the production `tofInit`, `tofTask`, and
`ps_delete` template into a host harness. Mock sensor/I2C/allocation boundaries
verify PSRAM-preferred allocation, placement construction, initialization
failures, reinitialization, and the task's shutdown branch. Every teardown must
destroy the object before freeing its storage and clear the global pointer.
ASan/UBSan follow `HW1_SANITIZE`; the existing `memutil_tests` cover the real
allocation helper's PSRAM fallback/bypass policy with a mocked heap backend.

This moves only the VL53L4CX object's inline state. The vendor driver code,
I2C/Wire buffers, task stack, cache, and sensor timing settings are unchanged.
Host tests cannot establish physical RAM placement, ranging accuracy, or the
polling-time impact of PSRAM access; those require an on-device check.

## Optional PSRAM for sensor objects

Application-owned heap driver objects use `AllocPref::PreferPSRAM`: external
RAM is preferred, but an absent/exhausted external heap or the PSRAM bypass
routes them to byte-addressable internal RAM. No sensor parent allocation uses
`RequirePSRAM`. Genuine exhaustion of available internal/external memory still
returns allocation failure; this policy is not a guarantee that every feature
configuration fits a board without PSRAM.

`sensor_parent_lifecycle` extracts the production IMU, APDS, servo, gamepad, and
ANO initialization functions and task shutdown blocks into host boundary mocks.
It checks placement construction and matching destruction, failed initialization
and retries, and Seesaw's existing retain-across-retry/stop behavior. Vendor-owned
I2C helpers, Wire buffers, locks, static caches, and driver algorithms are outside
this migration. The tests do not exercise real hardware or task races.

`memutil_tests` and `memutil_no_psram` compile the real allocation implementation
with and without `BOARD_HAS_PSRAM`, respectively. They cover runtime-absent
PSRAM, exhausted-PSRAM fallback, bypass, true allocation failure, and matching
placement construction/destruction. Heap-capability backends are mocked, and
ASan/UBSan follow `HW1_SANITIZE` for both targets and the lifecycle harness.

## OTA PSRAM replies

The OTA-enabled main image lazily retains its command reply buffers using
`PSRAM_STATIC_BUF`, preserving the old static returned-pointer lifetimes and
capacities. The eight common replies total 2,972 bytes, with another 512 bytes
when Bluetooth is enabled. Only their pointers remain in fixed internal BSS;
actual buffers prefer PSRAM and fall back to internal RAM when it is absent,
exhausted, or bypassed. Unused commands do not allocate their buffers. The
separate recovery updater and non-OTA command stubs are unchanged.

`ota_psram_replies` executes extracted production status/journal-reset handlers,
the BLE JSON formatter, and the real buffer/JSON macros against host boundary
mocks and vendored ArduinoJson. It checks allocation failure and retry,
persistent reuse, capacities, JSON allocation failures, and journal allocation
failure before mutation. Source guards cover all ten buffers, all three JSON
documents, explicit pointer capacities, BLE reply allocation before upload
mutations, and credential-buffer allocation before copying a secret.

The existing PSRAM/no-PSRAM MemUtil tests cover the actual fallback policy.
ASan/UBSan follow `HW1_SANITIZE`. These tests do not run a real OTA upload,
validate hardware PSRAM placement, or simulate the full BLE/flash state machine.

## System Event catalog coverage

`event_catalog_tests` links the real `System_EventCatalog.cpp` and exercises
the public provider plus its dependency-free C++17 validation/index core. It
pins the reviewed 12-family/152-kind order, checks every global and grouped
boundary, verifies the legacy `boot` alias and fallback behavior, walks an
interleaved synthetic family with more than 32 kinds, counts `new`/`new[]`
calls during provider reads, and stresses concurrent immutable lookups.

`event_catalog_json_tests` links the real provider and
`System_EventCatalogJson.cpp`, while hostile fixtures call the production
provider-shaped JSON core directly. It verifies quote, backslash, C0 and UTF-8
handling; exact size and buffer precedence; NUL/malformed-UTF-8 preflight;
sink-failure byte accounting; and the real `CMD_RESULT_MAX` budget. Its
`--dump-json` mode emits only the production serializer bytes, while
`--dump-typed` emits an independent ordinal/length/hex traversal protocol.
`event_catalog_json_parse` invokes both modes, parses the JSON with Python's
standard library, and compares every family and kind with typed traversal and
the frozen fixture. The current established result is 13 families, 159 kinds,
and exactly 3,051 payload bytes (3,052 including a command-buffer NUL).

`event_catalog_text_tests` exercises the real dependency-light human listing
core used by `events kinds`. It covers basic and exact-fit packing, a canonical
token wider than the former 120-byte staging array, whole-token flush/retry,
sink failure, invalid providers, and zero-callback rejection when one complete
header or token cannot fit the named 255-byte debug payload.

`event_catalog_fixture` compares the private row source with both the reviewed
grouped v1 JSON fixture and an independent 152-row declaration-order fixture,
without rewriting either one. `event_catalog_structure` fails closed under
optimized Python and enforces the repo-wide private-row include allowlist,
legacy-macro and copied-vocabulary guards, single table/index owner, provider
dependency boundary, immutable storage, JSON/text-core include allowlists,
single serializer ownership, typed-provider and CLI/HTTP delegation, removal
of obsolete catalog ArduinoJson builders, and absence of allocation and
locking APIs.

For Phase 3, the same structure test now checks both native OLED event pickers.
It requires direct calls to `systemEventCatalogFamilyCount()`,
`systemEventCatalogFamilyAt()`, and `systemEventCatalogFamilyKindAt()`; rejects
the former 24-entry arrays/build functions and scalar compatibility scans; and
requires the automation wizard to retain provider family/kind ordinals rather
than a copied canonical-name buffer. `notification_integration_guards` also
requires the notification editor to pass the resolved typed record's full name
to the bounded one-kind mutation command. These are source contracts: the host
suite cannot execute the Arduino/OLED renderer or synthesize real button input.

The complete host suite currently passes 45/45, with sanitizers enabled on
native targets. The restoring five-profile firmware matrix remains the Phase 2
provider/adapter evidence, including both ordinary recovery builds and exact
source-config restoration. The ordinary FeatherS3 build performed after the
Phase 3 edits also succeeded with `DISPLAY_TYPE=0`. A separate temporary
FeatherS3 compile-coverage profile enabled the level-4 custom OLED/gamepad
gates and `DISPLAY_TYPE=1`, compiled both active picker bodies without errors,
and confirmed that both archive members refer to the three native indexed
operations and no catalog-JSON symbol. This establishes compilation and link
ownership, not physical OLED behavior.

A separate authenticated live-browser acceptance reached the ordered 12-family/
152-kind web picker and preserved selection without saving. That is web UI
evidence only: it did not expose the raw HTTP response/auth exchange and does
not exercise OLED. Physical OLED/BLE/G2/UART procedures and live HTTP failure
injection remain outside these host guarantees.

## I2C transaction-policy coverage

`i2c_transaction_options_tests` executes the production manager transaction
template against deterministic host stubs. It starts with an already-registered
gamepad identity, then proves consecutive calls independently apply 100 kHz / 80
ms and 400 kHz / 15 ms policies to the bus clock and mutex wait. The companion
`i2c_transaction_contract` guard keeps timing out of duplicate registration,
requires every standard and NACK-tolerant helper to forward explicit per-call
options, and requires boot identities to use their configured physical bus.

## MQTT lifecycle coverage

`mqtt_lifecycle_gate_tests` compiles the production packed atomic gate and
checks command-first, stop-first, duplicate-stop, stop-during-start, retry, and
two-thread admission-versus-stop orderings. The companion
`mqtt_lifecycle_contract` guard pins the firmware integration: MQTT command
admission precedes queue submission and lasts through response publication,
all driver teardown is main-loop-affine, both shutdown commands use the shared
request path, and pending teardown runs before periodic publication.

To build and run only the dependency-free map geometry target (without the
Python-backed allocation inventory):

```sh
cmake --build /tmp/hw1-hardwareone-host --target map_viewport_core_tests
/tmp/hw1-hardwareone-host/map_viewport_core_tests
```

Runs in well under a second. ASan and UBSan are on by default — turn them off
with `-DHW1_SANITIZE=OFF` if your toolchain lacks them, but understand what you
lose: the walk-back in `utf8TrimPartialTail` reads *backwards* through the
buffer, and an earlier draft ran off the front of it for a one-byte window
holding a lone continuation byte. Without a sanitizer that is invisible, because
the byte it reads is almost always mapped.

## What `test_llm_utf8.c` covers, and why

`utf8TrimPartialTail` shortens a served chunk so it never ends inside a
multi-byte UTF-8 sequence. It sits at the two offset-addressed poll endpoints
(`/api/llm/result` and `llmresult json <offset>`). Getting it wrong corrupts
answers silently, so the properties are asserted rather than assumed:

- **Range and no out-of-bounds**, exhaustive over every byte string of length
  0..2 against exact-sized `malloc` — the OOB case is reachable at length 1.
- **No split leak**: every prefix of a valid stream trims to a whole number of
  complete sequences, across ASCII, 2-, 3- and 4-byte characters and CJK.
- **No false trim**: a chunk already ending on a boundary is returned intact.
- **The stall invariant**, `n >= 4` implies the result is `>= 1`. This is what
  guarantees a 511-byte serving window can never serve nothing, and therefore
  that the endpoint cannot livelock. It is the single most important assertion
  here: a fix that stalls the stream would be worse than the bug it replaces.
- **Named regressions** so two specific rejected designs cannot come back: a
  "never return 0" guard (it fires on the ordinary slow-streaming case and
  re-introduces split characters) and an unbounded walk-back.

Deliberately **not** asserted: idempotence. `trim(trim(p,n)) == trim(p,n)` holds
on well-formed input but fails by design on malformed bytes, which pass through.
Asserting it would forbid that pass-through — and passing malformed bytes
through is what keeps the caller's cursor advancing.

Both rejected designs above were verified to fail this suite before it landed.

## Map viewport geometry coverage

`map_viewport_core_tests` compiles the production
`System_MapViewportCore.h`. It verifies cardinal displacement against the
actual 288×144 visible axes, 90-degree screen-to-geographic rotation, OLED/G2
maximum-zoom scale clamps, rotated edge overscroll, and equivalence between one
accumulated multi-step request and repeated isolated requests within float
center precision. The test has no Arduino, ESP-IDF, renderer, filesystem, BLE,
or map-file dependency.

## Memory allocator and tracker coverage

`memutil_tests` compiles the real `System_MemUtil.cpp` against a deterministic
ESP-IDF heap-capability stub. It verifies all five routing policies, strict
placement, PSRAM fallback accounting, bypass behavior, zero-sized requests,
calloc overflow, failed-realloc pointer preservation, exact-once
`realloc(ptr, 0)`, and ArduinoJson's matching realloc contract.

`memtracker_core_tests` compiles the real dependency-light registry used by the
firmware tracker. It verifies repeated-tag aggregation, actual DRAM/PSRAM byte
accounting, fallback/failure counters, deterministic top-K snapshots,
full-table handling without out-of-bounds writes, updates to existing tags at
capacity, reset epochs, and counters beyond 4 GiB.

`raw_allocation_inventory` runs the comment/string-aware scanner over the main
`components/hardwareone` application. The pre-migration boundary is 12 direct
`malloc` calls plus five existing `calloc` calls. `System_MemUtil` itself,
tests, and third-party code are excluded deliberately; changing that boundary
requires an explicit test update. The standalone recovery updater is outside
this component and currently has six first-party `malloc` calls of its own.


## Portable JPEG decoder

`jpeg_portable` compiles the production `HAL_JPEG.cpp`, `HAL_JPEG_Software.cpp`
and `HAL_JPEG_P4.cpp`. The default tests use controlled codec/SDK/heap boundaries
and execute metadata validation, all header truncations, duplicate frames,
segment/scan errors, capacity limits, explicit backend selection, software
PSRAM-to-internal-memory fallback, partial-result cleanup, and automatic fallback.
The software harness also extracts and executes the production G2 adapter,
checking its shared allocator tag/policy and software-only selection when the
runtime PSRAM bypass is enabled. Custom allocation failure must not escape that
policy by silently using the raw SDK allocator.
The P4 backend is compiled with no SoC capability, with an unqualified driver,
and with both capability and driver qualification enabled. Its SDK mock
covers RGB channel order, 4:4:4/4:2:2/4:2:0 padded-row compaction, input copying,
allocation/engine/process/output-size/deletion failures, retry after failure,
and nonblocking admission while another call owns the decoder. ASan/UBSan follow
`HW1_SANITIZE`. These mocks cannot establish real DMA or timeout behavior.

To also compile the installed real TJpgDec and the legacy `fmt2rgb888` converter:

```sh
python3 -B components/hardwareone/test/host/test_jpeg.py --sanitize \
  --jpeg-component /path/to/managed_components/espressif__esp_jpeg \
  --camera-component /path/to/managed_components/espressif__esp32-camera
```

Or configure CMake with `-DHW1_JPEG_COMPONENT=... -DHW1_CAMERA_COMPONENT=...`.
This optional corpus suite checks software decoding for fifteen original
synthetic fixtures at all three external TJpgDec optimization levels, including odd dimensions, grayscale and red/blue channel
checks. Whenever the legacy converter succeeds, output must match it byte-for-byte.
The real FASTDECODE=1 regression reproduces three legacy 4:2:0 workspace failures;
FASTDECODE=2 exhausts legacy scratch for all fifteen baseline fixtures. The new
configuration-sized private workspace must decode all fifteen in every mode.
Progressive JPEG fails cleanly in both the legacy and new software path;
progressive support is not introduced. Another 192 concurrent decodes per optimization level compare
all pixels to the sequential output, exercising the new per-call workspace.

The third-party wrapper's input callback signature is adapted in a temporary file
from 32-bit `unsigned int` to host `size_t` to match TJpgDec on 64-bit desktops.
Its body, the decoder algorithm and the legacy conversion implementation are
unchanged. ESP-IDF heap calls are mocked with the host allocator; this does not
measure physical PSRAM placement, S3 ROM-decoder behavior or P4 JPEG hardware
speed/quality. The normal test run explicitly reports when real corpus parity
was not requested. Fixture regeneration needs Pillow; normal runs use committed
JPEG files and need no image-generation dependency.


## Shared battery backend and power admission

Run the focused portable battery checks:

```sh
python3 components/hardwareone/test/host/test_battery.py --sanitize
```

The runner compiles the production `BatteryPolicy.h`, the unmodified backend,
lifecycle, snapshot/accessor, calibration and JSON functions extracted from
`System_Battery.cpp`, and the exact OTA `powerIsSafe()` function. SDK ADC,
FreeRTOS synchronization, GPIO, clock, event delivery and MAX17048 transport are
mock boundaries. The JSON checks use the repository's real ArduinoJson library.
Tests cover:

- Estimated SOC bounds/monotonicity; unknown presence, USB and charging;
  stale/error handling, successful sample at boot millisecond zero, and wraparound.
- Actual board selection for EYE, generic P4, classic Feather ESP32, XIAO Sense,
  and FeatherS3 fuel gauge, including an explicitly false EYE board marker.
- P4/S3 curve calibration and classic ESP32 line calibration (eFuse, nominal Vref,
  and strict eFuse-only mode); GPIO-derived unit/channel use, partial reads,
  saturation, conversion/configuration/calibration failures, and resource cleanup.
- Concurrent recalibration blocked behind a paused hardware read, with readers
  retaining the previous coherent snapshot until the operation completes.
- Deferred fuel-gauge probe, retry after I2C failure, independent VBUS freshness,
  CRATE deadband, disabled monitoring, and battery-full notification hysteresis.
- Telemetry nulls for unknown/failed/stale readings and complete JSON serialization
  within the CLI/web response buffer.
- OTA admission under ADC, fuel-gauge and unmonitored build policies, including
  invalid/stale values, independent fresh VBUS and the explicit force override.

These checks do not validate resistor values, analog accuracy, actual SDK ADC
routing, a connected cell, battery chemistry, or the charger circuit. Target
builds and physical/meter comparison remain separate qualification steps.


## Shared camera controls and consumers

```sh
python3 components/hardwareone/test/host/test_camera_consumers.py --sanitize
python3 -m unittest tools.webui.tests.test_camera_page tools.webui.tests.test_embedded_js_syntax
```

The C++ runner compiles the whole production G2 camera settings page with only
runtime boundaries mocked, plus the real HAL descriptor and frame-validation
implementation. It checks fixed resolution IDs/dimensions, invalid IDs, JPEG
marker and byte-budget admission, release ownership, backend-specific settings
and resolution rows, no command on an unsupported control, stale taps after a
capability change, failed command submission, and text truncation at every
capacity from 1 to 1023 bytes. The JavaScript test executes the actual served
camera script against a small DOM adapter to verify supported options, disabled
controls, capability changes, command gating and visible CLI errors. These are
not sensor, browser layout, G2 transport, SD recording or physical camera tests.


### Edge Impulse JPEG capacity

```sh
python3 components/hardwareone/test/host/test_edge_impulse_jpeg.py --sanitize
```

This compiles the actual Edge Impulse conversion guard and shared JPEG parser.
Only the legacy converter is mocked, writing the complete inspected RGB size
so a missed capacity check is observable under ASan. Real synthetic small/VGA
JPEGs retain their actual geometry. HD input, undersized or null output,
malformed headers, truncated input and a one-byte file cannot reach conversion;
decoder failure cannot publish stale dimensions. Both camera and stored-file
call sites are checked for use of this capacity-aware boundary. No additional
full-image RGB allocation or inference/model behavior is introduced.

### Camera / sensor I2C ownership

```sh
python3 components/hardwareone/test/host/test_camera_i2c_reservation.py --sanitize
```

This runs the actual `System_BuildConfig.h` reservation policy and unchanged
manager constructor, `initBus`, `cmd_i2cbusenabled` and `cmd_i2c2busenabled`
definitions in 18 host variants: P4, S3 and classic ESP32, camera enabled and
disabled, with the DVP SDK I2C1 option undefined, explicitly false, and true.
P4 always reserves hardware I2C0 when its camera is compiled. DVP follows the
SDK-selected port, and camera-disabled builds reserve neither controller.

The runtime checks verify logical bus 0 maps to Wire1 / hardware I2C1 and bus 1
to Wire / hardware I2C0. The unreserved controller initializes normally while
the reserved one performs no Wire begin, clock/timeout setup, power-pin writes,
online event or bus-state update. Each CLI enable command rejects its reserved
controller without persisting or changing either bus's existing configuration;
the unreserved bus remains independently configurable. Disabling remains allowed,
and empty and validation-only commands make no writes.

Wire, GPIO, logging, core-affinity dispatch, Arduino String parsing and settings
persistence are host adapters. These tests do not prove physical controller
routing, ISR placement or camera/SCCB operation on a board.

`test_jpeg_validation.py --jpeg-component /path/to/espressif__esp_jpeg --sanitize`
compiles the shared entropy validator against the actual pinned TJpgDec codec.
It exercises FASTDECODE 0/1/2 with scaling enabled and disabled, complete and
truncated entropy, normalization, unsupported formats, and allocation failure.
An optional `--private-bad /path/to/observed.jpg` checks a local failure artifact
without adding private photographs to the repository.

## Local STT language-model decoder

`stt_lm_tests` compiles the production `stt/stt_lm.cpp` (host SHA-256 and
allocation path) against HW1LM1 files the test writes itself
(`stt_lm_testlib.h`), per `experiments/stt_train/lm/FORMAT.md`. It covers SHA-256
vectors; header/section/padding/digest/string-order/n-gram-order/word-id
rejection behind a valid digest; ARPA backoff scoring; custom-word
normalisation, limits and de-duplication; and the prefix beam search: blanks,
repeats, leading/double/trailing spaces, capacity truncation, OOV scoring and
`<unk>` context, hotword bonus, byte-order tie-breaks, >255-letter words,
cancellation polling and argument limits. 3000 random small cases are compared
with the independent map-based float64 reference decoder in `stt_lm_testlib.h`;
a case is skipped only when some beam cut or final pick is within 1e-6 nats and
is not a structural tie (equal LM parts): exact real-number coincidences reached
through different arithmetic round differently in any two implementations. Both
decoders follow the readings `experiments/stt_train/lm/hw1lm.py` documents:
`char_prune` never prunes the repeat of a prefix's last symbol (space after space
is blank-like), and custom-word lines split like Python `str.splitlines()`.

`stt_lm_parity` runs `test_stt_lm_parity.py` over `fixtures/stt_lm/` (tiny.lm,
caseN.i8, custom_words.txt and a JSON manifest written by the host builder's
reference decoder). `stt_lm_tool` decodes each case with the same decoder and
the production greedy CTC; both texts must match. With no manifest the test is
reported as skipped (exit 77), not passed.

`stt_local_lm` (`test_stt_local_lm.py`) compiles the actual
`System_STTLocal.cpp` with `ENABLE_STT_LM` on and off against VFS/heap/log/
runtime fakes and the real decoder: load once per session, one-shot lifetime,
custom words, missing/invalid/truncated files, PSRAM refusal, release under
memory pressure, cancellation mid-read and log-once behaviour. The runtime side
of the hook (Decoded/Fallback/Cancelled, unterminated output, release before a
memory refusal) is covered in `test_quartznet_cache.py`.

Host benchmark (not a CTest; optimised, unsanitised build):

```sh
build/stt_lm_tool bench --frames 1000 --beams 16,32
```

It builds a synthetic 20k-word / 400k-bigram / 400k-trigram LM and 20 s of
synthetic logits. Timings are host-only; the P4 has no double-precision FPU, so
score arithmetic there is software double and must be measured on the device.
