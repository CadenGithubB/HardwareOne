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

The complete host suite currently passes 18/18, with sanitizers enabled on
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
