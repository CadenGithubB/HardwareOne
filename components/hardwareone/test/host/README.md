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
the frozen fixture. The current established result is 12 families, 152 kinds,
and exactly 2,877 payload bytes (2,878 including a command-buffer NUL).

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
