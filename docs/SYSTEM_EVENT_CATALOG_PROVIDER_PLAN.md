# Shared System Event Options Pipeline — Design and Implementation Plan

**Date:** 2026-08-25 · **Status:** Phase 0 core/safety slice, Phase 1 typed
catalog provider, Phase 2 JSON/text adapters, and Phase 3 direct-indexed OLED
consumers implemented; automated source/host evidence is established, while
formal physical/device/fault evidence remains pending

**Companions:** [SYSTEM_EVENT_BUS_PROMPT.md](SYSTEM_EVENT_BUS_PROMPT.md) ·
[NOTIFICATION_EVENT_INTEGRATION_PLAN.md](NOTIFICATION_EVENT_INTEGRATION_PLAN.md) ·
[APP_JSON_CONTRACT.md](APP_JSON_CONTRACT.md)

**Implementation checkpoint (2026-08-25):** Phase 0 now has the shared command
limits, extent-safe event-kind masks, transactional notification-list mutation,
settings load→mutate→save transactions, notification-cache generation fencing,
bounded Dashboard batches, and fail-closed UART/BLE/async command ingress. Host
and browser regressions pass. The five Phase 1 board/gate matrix profiles pass,
and ordinary FeatherS3 and XIAO-S3 artifacts were rebuilt afterward. This does
**not** yet complete Phase 0: the compile-time-gated device fault harness and
signed physical-device checklist evidence remain open. Phase 1's typed catalog
provider and Phase 2's shared JSON/text adapters are implemented as recorded in
their checkpoints below. The Phase 2 host suite passes 18/18. Its full default
matrix passes ordinary FeatherS3, G2-without-Automation FeatherS3, ordinary
XIAO-S3, ordinary classic ESP32, and XIAO-S3 with optional consumers disabled,
then rebuilds both ordinary artifacts and restores the config header exactly.
The generic Secure-BLE client and pinned offline lane pass, but its physical
Secure-BLE run and the deterministic live HTTP fault test remain pending.

**Phase 3 checkpoint (2026-08-26):** the OLED automation and notification
pickers now traverse `systemEventCatalogFamilyCount()`, `FamilyAt()`, and
`FamilyKindAt()` directly. Their two 24-entry caches/build scans are gone; the
automation wizard retains provider ordinals and re-resolves the full canonical
kind at selection and submission, while the notification editor resolves each
visible/action row and keeps its existing bounded one-kind mutation path. The
complete sanitizer-backed host suite passes 18/18, including structural guards
that reject the old caches and scalar compatibility scans. The previously
recorded authenticated browser acceptance also passed, but it exercised the web
picker only and did not capture raw HTTP response/auth evidence. A subsequent
ordinary FeatherS3 build succeeded with `DISPLAY_TYPE=0`; CMake therefore
excluded `OLED_Mode_Automations.cpp`, and `OLED_Utils.cpp` compiled only its
display-disabled side. A separate compile-coverage run then enabled the custom
OLED/gamepad gates plus `DISPLAY_TYPE=1` and compiled both active pickers
successfully. Archive inspection found each OLED object referring to the three
native indexed operations and neither referring to the catalog JSON adapter.
The coverage harness itself was corrected because it had previously set the
outer display selectors without setting the level-4 custom gates. This is
OLED-enabled compiler/link evidence, not physical OLED evidence. Navigation,
high-kind, invalid-ordinal, and persistence checklist rows remain `PENDING`.
Future G2/Android authoring in Phase 4 has not started as part of this work.

---

## 1. Decision summary

Create one unconditional, native `System_EventCatalog` module that owns the
compiled event-kind vocabulary and exposes one typed event-options provider.
Every catalog-backed consumer starts with the same family/kind records, order,
lookup rules, and bounds behavior. JSON is an output adapter over that provider,
not a second catalog interface or an internal storage format.

The pipeline stays unified through the semantic event-option layer and branches
only where presentation or transport actually differs:

```text
                         immutable catalog storage
              System_EventCatalogRows.h + catalog .h/.cpp
                                  |
                                  v
                     shared typed options provider
                 family / kind / order / lookup / bounds
                    /                |                 \
                   /                 |                  \
          OLED page adapter   ESP32 G2 page adapter   JSON output adapter
                   |                 |                  /             \
                   v                 v                 v               v
              local OLED      G2 protocol encoder   HTTP         command reply
                                     |                 |          BLE/UART/
                                     v                 v          ESP-NOW/MQTT
                               BLE Central link      web UI             |
                                     |                                  v
                                     v                         Android / CM5 / clients
                         external G2 glasses
```

The G2 glasses are external hardware and cannot read the ESP32's C++ tables.
The current G2 *page adapter* is local: it runs on the ESP32, builds today's page
rows, converts them to the G2 widget protocol, and sends those packets to the
glasses over BLE. Under this target design, a future event picker in that
adapter resolves its event-option rows from the typed provider. The glasses
receive rendered widget data and return gestures; they are not presently an
independent catalog/JSON client.

The browser Promise cache and `<select>` renderer stay in
`WebServer_Utils.cpp`. They manipulate DOM nodes and JavaScript Promises, so
they remain presentation code. After migration, the OLED adapter and any
ESP32-side G2 event picker use the typed provider directly. A companion app
receives the same semantic content through the JSON output adapter and an
authenticated transport.

This gives the project one source of truth and one semantic pipeline without
forcing a local consumer to encode the 2.9-KB catalog as JSON and immediately
parse it again. It also does not pretend that a web dropdown, a 128×64 OLED
list, and the external glasses can share their final drawing or wire code.

**Impact verdict:**

- Phases 1–3 converge existing options safely at the typed-provider boundary;
  they need no event-ring migration, runtime catalog cache, or G2 wire change.
- Phase 4 G2 authoring is feasible only if supported glasses prove byte-exact
  echo of the existing arbitrary ListObject container name. The per-page token,
  length-aware parser, staged page swap, and three freshness checks are required
  to keep late taps from an old page out of a new automation draft.
- WiFi/BLE observation automations are a separate follow-on. A true “spin up,
  inspect, report, spin down” action requires a resumable automation run,
  cancellable scan coordination, generation-checked radio ownership, and an
  explicit BLE role/session broker. It must not be implemented by blocking the
  current condition evaluator or calling today's scan helper and assuming the
  radio turned back off.

---

## 2. Terminology and boundaries

The design uses the following terms precisely:

| Object | Meaning | Lifetime / owner |
|---|---|---|
| **Catalog storage** | Every valid canonical event kind and its display family | Immutable for the firmware image; owned only by `System_EventCatalog` |
| **Typed options provider** | The presentation-neutral API that enumerates and resolves catalog families/kinds | Stateless view over catalog storage; the common semantic pipeline for every consumer |
| **JSON output adapter** | A serialized representation of the provider records | Created on demand for a transport response; never authoritative and never parsed by local consumers merely to regain the same records |
| **Interface adapter** | OLED page logic, ESP32 G2 page logic, browser code, or an app repository that turns options into a particular interaction | Interface-owned and replaceable; may keep only ephemeral navigation/session state |
| **Event ring** | The latest 48 occurrences that actually happened | Mutable runtime state in `System_Events`; lossy by design |
| **Interface cache** | A temporary copy used to render one page/session | Optional and interface-owned; never authoritative |

This plan is about catalog storage, the typed options provider, and its output
adapters. It does not change the 48-entry ring, event posting, automation
matching, notification history, or retention.

In this plan, **dynamic picker** means an interface asks the provider/endpoint
for the current firmware's rows when it opens instead of embedding a private
140- or 152-name copy. It does not mean the compiled catalog mutates while the
firmware is running. Live WiFi/BLE observations are dynamic runtime data and
therefore use the separate snapshot provider described in §9.9.

“One pipeline” means that every interface projection originates from the same
typed option records and ordering before its transport or presentation adapter.
Remote web/app clients receive JSON bytes and reconstruct their own client-side
models; they do not receive C++ records. It does **not** mean every endpoint uses
JSON or the same renderer. External hardware describes a physical boundary;
JSON describes one possible data representation. The current G2 boundary uses
the G2 widget protocol, while web/app boundaries use JSON.

It also does not implement WiFi or BLE proximity scanning. A future scoped
scan watcher may add canonical kinds such as `wifi_network_appeared` or
`ble_device_disappeared`; once those kinds are added to the catalog, the work
in this plan makes them automatically available to every provider-backed
consumer. Interfaces that do not yet have an authoring picker still need that
UI work. Dynamic SSID/device choices also need the separate scan-result
provider; the static event catalog supplies only event-kind names. Scan cadence,
RSSI, hysteresis, addresses, and proximity privacy remain a separate design.

---

## 3. Verified current state

These facts were checked against the current source tree on 2026-08-25. The
as-built `docs2/` status was 219/428 fresh, with 200 stale, 5 modified, and 4
missing documents, so it was used only for discovery. Security, persistence,
capacity, and transport claims below were verified directly in current source.

### 3.1 Canonical data is already generated once

`SYSEVT_FAMILY_LIST` and `SYSEVT_KIND_LIST` live in
`components/hardwareone/System_Events.h:102-319`. The kind table generates the
numeric enum, canonical snake_case name, and family assignment. The header
already declares the correct persistence rule: names are public and stable;
numeric enum values are internal and may move.

`components/hardwareone/System_Events.cpp:138-190` generates the parallel
name/family tables and implements:

- `systemEventFamilyName()`;
- `systemEventKindFamily()`;
- `systemEventKindName()`;
- `systemEventKindFromName()`.

The last function is case-insensitive and keeps the legacy read alias `boot` →
`boot_finished`. The alias is accepted when reading old configuration but is
not enumerated as a canonical kind.

Current measured catalog facts:

| Fact | Current value |
|---|---:|
| Families | 12 |
| Canonical kinds | 152 |
| `SYSEVT_COUNT` including `SYSEVT_NONE` | 153 |
| Largest family | Sensors, 21 kinds |
| Compact grouped JSON payload | 2,877 bytes |
| `CMD_RESULT_MAX` capacity | 4,096 bytes including terminator |
| Current command-buffer headroom | 1,218 usable bytes |

The full vocabulary is compiled on every profile. It means “valid names this
firmware understands,” not “producers currently enabled by this build.” A kind
whose producer is feature-disabled remains valid and may simply never fire.

### 3.2 Native access is scalar, so consumers rebuild lists themselves

The existing accessors answer one name/family question at a time but do not
provide “family count,” “kind at ordinal,” or “kind N within this family.”
Consequently, every native picker has to scan `1..SYSEVT_COUNT-1` and build a
private list.

OLED currently does this twice:

- the automation create wizard materializes a 24-pointer array in
  `components/hardwareone/OLED_Mode_Automations.cpp:144-176`;
- the notification editor materializes parallel 24-entry name/id arrays in
  `components/hardwareone/OLED_Utils.cpp:498-521`.

Twenty-four is safe only accidentally: the largest family currently has 21
kinds. A family that grows by four entries silently truncates on OLED.

**Phase 3 implementation update:** the paragraph above records the design
baseline. Both private materializations and their build functions have now been
removed. The two OLED consumers keep only interface navigation/selection state
and resolve family/kind records through the indexed provider at render and
action boundaries.

### 3.3 JSON construction is duplicated

The same nested family×kind loop is written in two places:

- `events kinds json` in `components/hardwareone/System_Events.cpp:360-404`;
- `GET /api/events/kinds` in
  `components/hardwareone/WebServer_Server.cpp:3354-3377`.

They currently emit the same compact shape:

```json
{
  "families": [
    { "n": "Connectivity", "k": ["wifi_connected", "wifi_disconnected"] }
  ]
}
```

The values come from the one catalog, so the names do not drift, but the schema,
ordering loops, allocation policy, overflow handling, and comments can drift.
The current `cmd_events` transport comment is already stale: it still claims
MQTT has a 2,048-byte result buffer, while the current outbound contract in
`components/hardwareone/System_CommandTypes.h:125-146` assigns it
`CMD_RESULT_MAX`.

### 3.4 The web client is shared correctly within the browser

`components/hardwareone/WebServer_Utils.cpp:774-930` now owns the shared
browser client:

- lazy fetch and successful-result cache;
- response validation and duplicate rejection;
- concurrent request coalescing and retry after failure;
- display-name humanization;
- asynchronous `<select>` population and preserved unknown stored values.

The Automations, Dashboard, and Settings pages consume that helper. This code
belongs in the web layer because it manipulates DOM nodes and JavaScript
Promises. This plan does not move it into C++ or duplicate it elsewhere.

### 3.5 G2 is external hardware behind a local ESP32 page adapter

`components/hardwareone/G2_Glasses.h:8-16` defines the ESP32 as a BLE
Central/GATT Client connecting to the Even Realities glasses. The glasses do
not execute this repository's C++.

The current UI decision and row-building code nevertheless runs on the ESP32.
For example, `components/hardwareone/G2_Page_Automations.cpp:174-210` builds
local C-string rows and calls `g2ShowListPage()`. That function deep-copies the
rows into an ESP32 page-swap job
(`components/hardwareone/G2_Glasses.cpp:21244-21292`); the G2 transport later
encodes widget messages and reaches the physical glasses through BLE GATT
`writeValue()` (`components/hardwareone/G2_Glasses.cpp:11234-11246`). This is
the precise boundary the target diagram must show:

```text
typed provider -> ESP32 G2 page adapter -> G2 widget protocol -> BLE -> glasses
```

The G2 Automations page is currently list/detail/run/enable/disable only.
`components/hardwareone/G2_Page_Automations.cpp:105-133` displays a persisted
event trigger's `on` string, but there is no create/edit flow or catalog picker.
The separate G2 System Events viewer calls `systemEventKindName()` while
walking the 48-entry history ring
(`components/hardwareone/G2_Glasses.cpp:7925-7955`). It is a ring viewer, not
an event-options picker.

G2 authoring is therefore a clean slate for a future family-first picker. That
picker belongs in the ESP32 G2 page adapter and consumes the same typed provider
as OLED. It does not justify adding a catalog-only screen before a create/edit
workflow exists, and it does not require a new JSON-capable runtime on the
glasses.

### 3.6 The phone-companion BLE command channel can transport the catalog

`events` is a read-only command registered in
`components/hardwareone/System_Utils.cpp:2919-2927`. An authenticated companion
app can already issue `events kinds json` through the ordinary phone BLE
GATT-server command lane.

The current 2,877-byte result works over the established Secure Channel because
that path fragments, paces, authenticates, and reassembles multi-frame replies.
The request text `events kinds json` is well below the phone BLE command
channel's separate 511-byte inbound cap; only the response needs fragmentation
here.
It does **not** work reliably over plaintext BLE: the plaintext path calls one
unconfirmed notification through `esp_ble_gatts_send_indicate(..., false)`
(`components/hardwareone/Bluetooth.cpp:1632-1668,1779-1804`). Even the maximum
BLE ATT payload is 514 bytes (`MTU-3`), far below the current catalog.

Initial companion-app integration will therefore require an established Secure
Channel for the full catalog. Plaintext paging is a deliberate follow-up, not
something this refactor will imply works. This lane is distinct from the
ESP32's BLE-Central links to the G2 temples: companion command JSON is not sent
over the G2 widget/session protocol.

Today a plaintext client can still explicitly send the command when Secure
Channel enforcement is optional. Firmware then attempts one oversized
notification, and the asynchronous result callback does not propagate the send
failure back into command success. This plan does not add an origin-dependent
gate to the catalog command. “Do not fetch the full v1 catalog in plaintext” is
therefore an app/client rule until a framed or paged protocol is implemented.

### 3.7 Existing OLED high-kind editing is unsafe

This is a release-blocking prerequisite, not optional cleanup.

`NotifViewer` correctly owns eight 32-bit words (256 bits) in
`components/hardwareone/System_Notifications.h:76-102`. The OLED personal-mute
editor still uses the old width:

- `ncMuteTest()` rejects every kind `>=128`;
- `ncToggleMute()` declares `uint32_t mask[4]`;
- it then indexes `mask[kind >> 5]` without a bounds check.

These are at `components/hardwareone/OLED_Utils.cpp:504-541`. Current kind IDs
128–152 write one word past that local array. Toggling a lower kind also
rebuilds the replacement list without existing high-kind mutes, silently
clearing settings created through web or CLI.

The provider migration must fix this before claiming OLED supports the whole
catalog.

### 3.8 Full-list mute commands already exceed the inbound command lane

Fixing the eight-word mask is necessary but not sufficient. OLED rebuilds one
replacement command containing every muted name. The Dashboard does the same
for “Mute all.” With today’s 152 names, the worst-case command is 2,264 bytes:

```text
notifyusermute peer_online,peer_offline,...,automation_action_dropped
```

`ExecReq::line` has 2,047 usable bytes
(`components/hardwareone/System_CommandTypes.h:152-156`), and both central queue
submission paths copy with `strncpy()` instead of rejecting an oversized line:
sync at `components/hardwareone/System_Utils.cpp:5501-5510` and async at
`components/hardwareone/System_Utils.cpp:5597-5621`. A complete catalog picker
would therefore expose a second failure:
stack safety would be repaired, but “mute all” could still save a truncated,
incorrect set.

The implementation must add bounded incremental mutations rather than raising
the command-line ceiling just for this UI:

```text
notifyusermute set <kind> <on|off>
notifyusermute patch <+kind|-kind>[,...]
notifyusermute all
notifyusermute none

notifyusershow set <kind> <on|off>
notifyusershow patch <+kind|-kind>[,...]
notifyusershow all
notifyusershow none
```

For each command, `on` adds that canonical kind to the command's own list and
`off` removes it; the two lists remain independent. `all` fills that list and
`none` clears it. `patch` validates its entire comma-separated delta first,
then adds `+kind` entries and removes `-kind` entries in one save; duplicate or
contradictory entries are errors. Keep the existing comma-list replacement
syntax for compatible, in-budget callers. OLED uses the one-kind form.
Dashboard uses `all`/`none` for those exact operations and greedily packs only
changed kinds into as few `patch` commands as possible, each within
`CMD_INPUT_MAX`, then submits those commands as a CLI batch. The central sync
submission path must reject, not truncate, any command that does not fit
`ExecReq::line`; the async submission path must enforce the same limit.

With all 152 current names represented as additions, the greedy pack produces
two commands of 2,037 and 405 bytes. Tests must derive these chunks from the
catalog and the named limit rather than hard-code either count or length.

The one-kind and patch forms each perform one load/validate/mutate/save
operation. They change only requested canonical tokens, deduplicate known
tokens, and preserve unrelated syntactically valid stored tokens that this
firmware does not know (important after a downgrade). `all` deliberately
replaces the list with all canonical kinds known to this image; `none`
deliberately empties it. A parse or validation failure performs no save. A save
failure returns `Error:` and invalidates/reloads whatever state the storage
contract left authoritative.

That load/validate/mutate/save sequence must also be one filesystem transaction.
Today `mergeAndSaveUserSettings()` locks its internal load and save separately,
so a web settings patch and a command/OLED notification patch can both read the
same old document and the later save can erase the earlier disjoint change
(`components/hardwareone/System_Settings.cpp:3483-3563`). Add an outer
`FsLockGuard`-owned transaction helper and implement both generic merge and
notification-list mutation through it; the nested load/save guards are already
reentrant-safe (`components/hardwareone/System_Mutex.cpp:81-101`). Existing-user
password and gamepad-password updates also have separate load/save gaps
(`components/hardwareone/System_User.cpp:1543-1577,1749-1778`) and must move to
the helper; compute password hashes before taking the filesystem lock, then
patch the latest document inside it. New-user full-file initialization may stay
a direct save because no prior document exists to preserve. Validate command
grammar before taking the lock, then load the current list, run the bounded
core mutation, update JSON, and save before releasing it. This prevents lost
updates; it does not claim every possible storage/power failure rolls back. A
named pre-commit temp-write fault must leave the old file unchanged, while
later/ambiguous failures return an error and force an authoritative reload
rather than reporting success.

`saveUserSettings()` must also stop treating any positive `serializeJson()`
count as a complete write (`components/hardwareone/System_Settings.cpp:3516-3544`).
Measure the exact compact document length before opening output. For both the
temporary and direct-fallback files, success requires `written == expected`
and an observed `File::size() == expected` after the existing `flush()` call; a
short write is failure even when `written > 0`. Never rename a short temporary
file. A short direct fallback is destination-touched/ambiguous, so it returns
error, invalidates preference cache state, and reloads whatever storage
actually made authoritative. Do not cite `Print::getWriteError()` as evidence:
the current Arduino FS `File::write()` path does not set that state, `flush()`
returns `void`, and the lower wrapper does not surface its `fflush`/`fsync`
result. The count-plus-size checks detect observable short output; they do not
prove media durability or an unobservable flush failure. If durable flush
error propagation becomes a requirement, add a separately reviewed checked
VFS writer instead of overclaiming what this API can report.

The existing notification preference cache needs one related concurrency
repair. `notifViewerResolve()` currently drops `gUserPrefsMutex`, loads from
flash, then reacquires the mutex and publishes the result
(`components/hardwareone/System_Notifications.cpp:320-344`). A user-settings
transaction can commit and call `notifUserPrefsInvalidate()` during that load,
after which the resolver can republish the just-invalidated old document. Add a
cache-only generation guarded by `gUserPrefsMutex`: capture it on a miss,
perform the flash load with no cache mutex held, and publish/copy the result
only if the generation is unchanged. On mismatch, discard the load and retry
the lookup/load sequence. Invalidation clears every slot and advances that
generation under the same mutex. Never hold `gUserPrefsMutex` while acquiring
the filesystem guard; this preserves one-way lock ordering and lets a completed
save linearize before any subsequently published preference view.

---

## 4. Goals and non-goals

### Goals

1. One compiled owner for family and kind metadata.
2. One typed, presentation-neutral provider for lookup, indexed enumeration,
   ordering, and bounds behavior; every catalog-backed adapter consumes it.
3. Zero-allocation, no-cache local enumeration suitable for OLED and the
   ESP32-side G2 page adapter.
4. No fixed per-family consumer cap.
5. One JSON output adapter over the typed provider for HTTP and command
   transports; no independent JSON-building loops.
6. Preserve canonical names, family order, kind order, and the current JSON
   `families[].n` / `families[].k` contract.
7. Preserve old C++ include sites and lookup behavior during extraction.
8. Make invalid indices, output overflow, and transport failure explicit.
9. Keep the core provider unconditional and independent of JSON grammar, HTTP,
   Bluetooth, OLED, G2, Automation, FreeRTOS, and the event ring.
10. Add host-testable invariants so catalog growth fails loudly instead of
   disappearing from one interface.
11. Make the G2 physical/runtime boundary explicit: the local adapter consumes
    provider records; only G2 protocol packets cross BLE to the glasses.

### Non-goals

- No change to `SystemEvent`, `SYSEVT_RING_SIZE`, event posting, cursors, or
  automation matching.
- No new event kinds in this change.
- No WiFi/BLE scanner, scan cache, presence state, RSSI policy, or proximity
  data on the wire.
- No runtime filtering by feature availability.
- No shared pixel, DOM, or lens renderer.
- No requirement that local firmware adapters serialize and reparse JSON.
- No new JSON parser, catalog endpoint, or HardwareOne runtime on the G2
  glasses.
- No G2 wire-schema, characteristic, or glasses-firmware change in the catalog
  refactor. Optional Phase 4 deliberately uses the existing arbitrary
  ListObject container-name field as a per-page correlation token.
- No full G2 or Android automation create/edit UI hidden inside this refactor.
- No persistent browser/app catalog database.
- No numeric enum values on disk or wire.
- No claim that the full catalog works over plaintext BLE.

---

## 5. Target ownership

| Concern | Owner after this plan | Rule |
|---|---|---|
| Family/kind row declarations | `System_EventCatalogRows.h` | One private, intentionally repeat-included row source; no public list macro |
| Public enums and typed API | `System_EventCatalog.h` | Generated from the private row file, then all expansion macros are undefined |
| Immutable tables and typed options provider | `System_EventCatalog.cpp` | The only semantic family/kind enumeration path; no heap, locks, or runtime cache |
| JSON adapter contract | `System_EventCatalogJson.h` | Transport-neutral sink/status API; no UI or physical-transport types |
| JSON grammar/escaping/order | `System_EventCatalogJson.cpp` | Output adapter that walks the typed provider; never a parallel table reader |
| Event occurrence ring | `System_Events.h/.cpp` | Unchanged |
| `events` command | `System_Events.cpp` | Thin text/JSON adapter over catalog |
| `/api/events/kinds` | `WebServer_Server.cpp` | Thin HTTP streaming adapter |
| Browser fetch/cache/selects | `WebServer_Utils.cpp` | Web-only presentation |
| OLED list navigation | OLED callers | Store screen-specific cursor/scroll state; resolve semantic rows through the typed provider |
| ESP32 G2 list navigation | G2 page adapter | Store page/generation state; resolve semantic rows through the typed provider and encode G2 widgets |
| Physical G2 glasses | External BLE endpoint | Receive G2 widget packets and return gestures; do not read C++ or catalog JSON in the current design |
| Android fetch/cache | companion app | Lazy, Secure Channel, connection-scoped |

The name `System_EventCatalog` is intentional. It separates immutable type
metadata from the mutable event bus while keeping both in the general system
layer. “Typed options provider” describes the public role of that module; it is
not a second file, store, cache, or vocabulary.

---

## 6. Shared typed provider contract

### 6.1 File extraction

Add:

- `components/hardwareone/System_EventCatalogRows.h`;
- `components/hardwareone/System_EventCatalog.h`;
- `components/hardwareone/System_EventCatalogCore.h`;
- `components/hardwareone/System_EventCatalog.cpp`;
- `components/hardwareone/System_EventCatalogJson.h`;
- `components/hardwareone/System_EventCatalogJsonCore.h`;
- `components/hardwareone/System_EventCatalogJson.cpp`;
- `components/hardwareone/System_EventCatalogTextCore.h`.

Move into them:

- the family/kind vocabulary, converted from public
  `SYSEVT_FAMILY_LIST` / `SYSEVT_KIND_LIST` macros into private repeat-included
  `System_EventCatalogRows.h` rows;
- `SystemEventFamily` and `SystemEventKind`;
- generated family labels, kind names, and family assignments;
- the four existing name/family lookup functions.

The public declarations and the only definitions of the four legacy lookup
functions move to `System_EventCatalog.h/.cpp`; they do not remain defined in
`System_Events.cpp`. `systemEventCatalogFindKind()` is the canonical lookup
implementation. `systemEventKindFromName()` is its compatibility wrapper that
returns the process-local id or `-1`; the three name/family accessors read the
same immutable tables directly.

`System_Events.h` includes `System_EventCatalog.h`. Existing producers and
consumers that include only `System_Events.h` remain source-compatible. New
code that needs metadata but not the ring includes `System_EventCatalog.h`
directly.

`System_EventCatalog.h` defines a narrow expansion macro, includes the private,
intentionally no-guard `System_EventCatalogRows.h` to generate each public enum,
then immediately undefines it. The production `.cpp` repeat-includes the same
row file for tables/indexes. No
`SYSEVT_*_LIST` macro remains available to arbitrary consumers, and a registered
source-boundary test rejects direct row-header inclusion outside the catalog
module/tests. This makes “all adapters use the typed provider” structural rather
than a review-only convention.

`System_EventCatalog.cpp` is added to the unconditional source list beside
`System_Events.cpp` in `components/hardwareone/CMakeLists.txt`. The JSON adapter
`.cpp` is also unconditional because both the always-built command layer and
optional HTTP layer consume it; its header remains separate so native-only
callers do not acquire JSON declarations.

`System_EventCatalogCore.h` is an internal, dependency-free implementation
seam used by the production provider `.cpp` and host tests. Its validation,
table/index, and lookup templates accept descriptor arrays, which lets tests
exercise oversized families without maintaining a copied production model.
Keep this core C++17-compatible so it builds under the existing host-test
standard even though firmware currently compiles with a newer language mode.
Validation returns a constexpr error bitmask/result rather than unconditionally
hard-failing inside the template. Production applies
`static_assert(validate(productionDescriptors).ok())`; negative host fixtures
can therefore instantiate the exact core and assert the expected error without
making the test translation unit uncompilable.

`System_EventCatalogJsonCore.h` is the corresponding internal JSON-adapter test
seam. It accepts a provider-shaped view rather than raw production row arrays, so
host fixtures can exercise escaping, malformed descriptors, size preflight,
and sink failures while production still walks the public typed-provider
semantics. JSON grammar and escaping do not leak into
`System_EventCatalogCore.h`. Its emitter/counting entry returns an explicit
status plus byte count for injected views. `System_EventCatalogJson.cpp` is a
thin production adapter that invokes this same emitter; tests must not validate
a parallel hand-copied implementation.

Core string descriptors are explicitly `{data, length}` rather than bare C
strings. Synthetic provider and JSON-adapter fixtures can therefore include
embedded NUL and malformed UTF-8 bytes and prove preflight rejection. The
production view derives the length while visiting each compile-time-validated
literal; it does not need a second persistent length table. Public
`FamilyInfo`/`KindInfo` views may still expose immutable `const char*` because
production literals are guaranteed NUL-terminated and valid.

The public indexed functions below are the single semantic pipeline. OLED,
the ESP32 G2 page adapter, human CLI output, and the JSON output adapter all
obtain families and kinds through this contract. Raw generated arrays remain
private to `System_EventCatalog.cpp`; interface code and transport adapters may
not include the private row file merely to rebuild their own traversal. The
internal core template may operate on injected descriptor views for tests, but
the production JSON output adapter is wired to the same provider semantics and
order as local consumers.

### 6.2 Proposed public types and functions

```cpp
// Generated at compile time as longest canonical kind token + trailing NUL.
inline constexpr size_t SYSTEM_EVENT_KIND_TOKEN_CAP = /* generated */;

struct SystemEventCatalogFamilyInfo {
  SystemEventFamily id;       // process-local only; never persist or serialize
  const char* label;          // immutable, valid for the process lifetime
  size_t kindCount;
};

struct SystemEventCatalogKindInfo {
  SystemEventKind id;         // process-local only; never persist or serialize
  SystemEventFamily family;
  const char* name;           // canonical persisted/wire token; immutable
};

size_t systemEventCatalogFamilyCount();
size_t systemEventCatalogKindCount();  // excludes SYSEVT_NONE

bool systemEventCatalogFamilyAt(
    size_t index, SystemEventCatalogFamilyInfo* out);

bool systemEventCatalogKindAt(
    size_t index, SystemEventCatalogKindInfo* out);

bool systemEventCatalogFamilyKindAt(
    SystemEventFamily family,
    size_t index,
    SystemEventCatalogKindInfo* out);

bool systemEventCatalogFindKind(
    const char* name, SystemEventCatalogKindInfo* out);
```

These two records are the shared typed event options. Do not introduce a second
`EventOption` copy with the same fields. A local picker holds only ordinals and
its own visual navigation state, then resolves the current family or kind on
demand. Persisted or transported selections remain canonical names, never
ordinals or enum ids. Any asynchronous draft owns a
`SYSTEM_EVENT_KIND_TOKEN_CAP` buffer (or an equivalently bounded owned string),
copies the full token, and fails rather than truncating it.

Keep the existing functions as stable compatibility entry points backed by
the new tables:

```cpp
const char* systemEventFamilyName(uint8_t family);
uint8_t systemEventKindFamily(uint8_t kind);
const char* systemEventKindName(uint8_t kind);
int systemEventKindFromName(const char* name);
```

### 6.3 Required semantics

- `FamilyAt` enumerates private family-row declaration/display order.
- `KindAt` enumerates global declaration order, excluding `SYSEVT_NONE`.
- `FamilyKindAt` preserves declaration order within the requested family.
- The implementation must not assume each family occupies one contiguous
  range in the private kind rows; later tier additions are interleaved.
- Null output pointers and out-of-range indices return `false` and do not
  mutate caller storage.
- `FindKind` is ASCII case-insensitive, accepts the legacy `boot` alias, and
  returns the canonical `boot_finished` view.
- Enumeration never emits `none` or aliases.
- Returned strings point into immutable compiled storage and remain valid for
  the life of the firmware image.
- The API is task-safe by immutability. It is synchronous and allocation-free.
  It is not an ISR contract.
- Every adapter observes the same records and order. An adapter may truncate a
  *display label* to fit its screen, but may not truncate the option set or use
  the truncated label as the persisted identity.
- Cursor, scroll-window, pagination chrome, gesture generations, and DOM state
  remain interface-owned because they describe different interaction models.
  The shared coordinate system is family ordinal plus within-family ordinal;
  resolution and bounds come from this provider.

### 6.4 Generated index and compile-time invariants

Generate one compact family-order index from the same private row file:

- one `uint8_t` kind-id entry per canonical kind;
- one family offset/count table.

This costs roughly 180–200 bytes of immutable data and makes
`FamilyKindAt(family, ordinal)` O(1), without the two OLED arrays or any runtime
initialization. The current name and family tables move rather than duplicate.
`FamilyInfo` and `KindInfo` are caller-filled return values, not permanent arrays
of structs; do not add a second 152-record metadata table, a virtual provider,
`String`, or lazy initialization merely to present the typed API.

Compile-time checks must prove:

1. every canonical kind has a non-empty snake_case name and no embedded NUL;
2. names are unique under ASCII case-folding;
3. every family label is non-empty, unique, valid UTF-8, and contains no
   embedded NUL;
4. every kind names a valid family;
5. every canonical kind appears exactly once in the family-order index;
6. family offsets/counts cover exactly `systemEventCatalogKindCount()` rows;
7. `SYSEVT_NONE` is never exposed by catalog iteration;
8. every declared family contains at least one canonical kind;
9. no canonical name is `boot`, which remains a read-only lookup alias;
10. the catalog fits the process-local enum representation:
    `SYSEVT_COUNT <= UINT8_MAX`;
11. `SYSTEM_EVENT_KIND_TOKEN_CAP` is exactly the longest canonical token plus
    its trailing NUL, and every generated copy site can hold it.

The enum limit in item 10 is intentionally exact. Because `SYSEVT_COUNT` is
itself an enumerator of `enum SystemEventKind : uint8_t`, the current
representation can hold at most 254 canonical kinds (`NONE`, 254 live ids,
then `COUNT == 255`).
If a 255th canonical kind is needed, move the count sentinel outside the enum
or widen the representation before adding it; the current
`SYSEVT_COUNT <= 256` assertion cannot express that safely.

Consumer-specific capacity checks remain with their consumers so the provider
does not depend on them:

- `System_Notifications.h` keeps its
  `SYSEVT_COUNT <= NOTIF_KIND_MASK_BITS` assertion;
- `System_Automation.cpp` replaces private literal word counts with a named
  `AUTOMATION_EVENT_MASK_WORDS` and asserts that its capacity covers
  `SYSEVT_COUNT`;
The provider validation core centrally rejects exact canonical names `boot`,
`none`, `set`, `patch`, `all`, and `list`: those tokens already mean a legacy
lookup alias, zero-kind sentinel, personal-list mutation, or device-policy list
operation. `test_event_catalog.cpp` instantiates all six hostile rows during
constant evaluation and checks valid near-neighbours; the structural source
guard independently checks the production rows, while
`test_notification_kind_list.cpp` pins the matching stored-token grammar.

Numeric enum order remains internal. Persisted automation and notification
configuration continues to use only canonical names.

---

## 7. JSON output adapter

### 7.1 API

The serializer is the one JSON output adapter over the typed provider. It is
not a peer source of event options and is not the internal contract for OLED or
the ESP32 G2 page adapter. It is transport-neutral: it writes bytes to a caller
callback and does not include HTTP, BLE, FreeRTOS, or ArduinoJson types in the
public contract. The declarations below live in
`System_EventCatalogJson.h`; `System_EventCatalog.h` exposes typed options only.

```cpp
using SystemEventCatalogJsonSink =
    bool (*)(void* context, const char* data, size_t len);

enum class SystemEventCatalogJsonStatus : uint8_t {
  Ok,
  InvalidArgument,
  BufferTooSmall,
  SinkFailed,
};

// Production catalog only: cannot fail because that view is compile-validated.
size_t systemEventCatalogJsonSize();  // excludes trailing NUL

SystemEventCatalogJsonStatus systemEventCatalogWriteJson(
    SystemEventCatalogJsonSink sink,
    void* context,
    size_t* bytesWritten = nullptr);

SystemEventCatalogJsonStatus systemEventCatalogJsonToBuffer(
    char* out,
    size_t capacity,
    size_t* bytesWritten = nullptr,
    size_t* requiredCapacity = nullptr);  // required includes trailing NUL
```

Sink contract: returning `true` means the callback synchronously accepted all
`len` bytes; returning `false` means it accepted zero bytes at the serializer
boundary. `bytesWritten` counts bytes accepted by prior successful callback
calls. It is not a claim that TCP, BLE, or another physical transport delivered
those bytes. A transport that supports partial acceptance must loop inside its
adapter or use a different adapter—it cannot report a partial chunk through
this Boolean contract.

`systemEventCatalogWriteJson()` can return `Ok`, `InvalidArgument`, or
`SinkFailed`; it never returns `BufferTooSmall`.
`systemEventCatalogJsonToBuffer()` can return `Ok`, `InvalidArgument`, or
`BufferTooSmall`; its internal memory sink cannot return `SinkFailed`.

Output-parameter rules are deterministic:

- initialize `bytesWritten` to zero before validation;
- on `WriteJson(Ok)`, it is the full JSON byte count; on `SinkFailed`, it is
  only the prior fully accepted callback chunks;
- a null sink is `InvalidArgument` and makes no callback;
- a null destination is always `InvalidArgument`, including when capacity is
  zero; capacity zero with a non-null destination is `BufferTooSmall`;
- on buffer success, `bytesWritten` excludes the NUL and
  `requiredCapacity == bytesWritten + 1`;
- on `BufferTooSmall`, `bytesWritten == 0`, `requiredCapacity` is still exact,
  and `out[0]` is cleared when capacity is nonzero;
- on `InvalidArgument`, both optional sizes remain zero.

### 7.2 Wire compatibility

The initial serializer migration in Phase 2 preserves the exact existing v1
shape and order:

```json
{"families":[{"n":"Mesh & ESP-NOW","k":["peer_online","peer_offline"]}]}
```

Rules:

- no numeric kind or family ids;
- no flat duplicate `kinds` array;
- no alias entries;
- output is byte-identical to current ArduinoJson for the present catalog
  strings; for future/synthetic inputs, use standards-correct JSON escaping:
  prefix quotation marks and backslashes with a backslash; use the JSON short
  escapes for backspace, form feed, newline, carriage return, and tab; use
  lowercase `\u00xx` for the remaining bytes `0x01..0x1f`; leave `/` raw; copy
  valid UTF-8 bytes unchanged. The `\u00xx` behavior is an intentional
  standards-correct extension for control bytes the current catalog does not
  contain, not a claim that today's ArduinoJson formatter emits those escapes;
- an embedded NUL or invalid UTF-8 descriptor is invalid catalog input and is
  rejected before the first sink callback (the production rows are checked at
  compile time; synthetic core fixtures exercise this runtime guard);
- property and array order remain deterministic;
- a buffer result is always NUL-terminated on success;
- a too-small buffer is cleared and returns `BufferTooSmall`, never a plausible
  truncated prefix;
- a streaming sink may already have received a prefix when it fails, so the
  adapter must abort/close that response and report `SinkFailed`.

Do not add schema/revision fields in the first migration. Existing clients need
no format change, and the initial app cache is scoped to one live connection.
A versioned/paged v2 can add revision metadata when persistent caching or
plaintext BLE paging is actually implemented.

### 7.3 Implementation constraints

- Validate the descriptor view completely before invoking the first real sink;
  production descriptors also carry the compile-time checks in §6.4.
- Hand-emit the small fixed grammar by walking the same typed family/kind
  provider contract used by local adapters. Production JSON code must not walk
  a second private table or include/traverse the private row file.
- Implement the exact JSON escaping rules above rather than relying on today’s
  safe characters.
- Use a bounded stack staging buffer (target about 256 bytes) so HTTP does not
  create one network chunk per token.
- No heap allocation, PSRAM document, global output buffer, or mutex.
- Implement `systemEventCatalogJsonSize()` by running the same generic emitter
  through a counting sink. Do not maintain a second size grammar.
- The buffer adapter preflights exact capacity before writing.
- Keep the serializer itself free of the 4-KB command limit. The command
  adapter owns that limit; HTTP can stream beyond it.

The parameterized `System_EventCatalogJsonCore.h` emitter used for hostile
synthetic fixtures must obey the same provider-shaped operations as production.
This preserves an injection seam without turning the test descriptor arrays
into an alternative production pipeline. Malformed-view size tests call the
core's status-bearing counting API; the public `systemEventCatalogJsonSize()`
has no invalid-input channel because only the compile-validated production view
can reach it.

---

## 8. Adapter and consumer migrations

All migrations follow one rule:

```text
resolve typed option -> apply interface policy -> render/serialize at boundary
```

The provider owns identity, family membership, order, counts, and bounds. Each
adapter owns only behavior that cannot be shared meaningfully: OLED cursor and
pixel-window state, G2 page/tap generation and widget encoding, browser DOM
state, or remote connection caches. This is one semantic pipeline with several
last-mile adapters, not several catalogs.

### 8.1 HTTP and browser

`handleEventsKinds()` becomes:

1. authenticate exactly as today;
2. preflight the immutable catalog and exact serialized size before emitting a
   response body;
3. set `application/json`;
4. pass an `httpd_resp_send_chunk()` sink to the JSON output adapter;
5. terminate the chunked response only on success;
6. return a small HTTP 500 object only for an internal failure detected before
   the first chunk attempt;
7. after streaming begins, return `ESP_FAIL`/close the connection on sink
   failure—never append a JSON error tail or chunk terminator.

This removes the handler’s private ArduinoJson tree and shared 4-KB static
buffer. It also means future catalog growth is not artificially limited by
`CMD_RESULT_MAX` on HTTP.

`hw.getEventKindFamilies()` and `hw.fillEventKindSelect()` stay in
`WebServer_Utils.cpp`. Their response validation, successful per-page cache,
unknown-kind preservation, and loading/error UI remain unchanged.

The Dashboard notification editor must stop constructing a full replacement
command. Track its initial mask, send `notifyusermute all` / `none` for those
exact actions, and greedily pack only changed kinds into bounded
`notifyusermute patch` commands through `/api/cli/batch` using the syntax from
§3.8. Treat a batch as a sequence, not a transaction: if any result fails,
report the failure and reload the saved server state before allowing another
edit so a partial batch is never represented as fully saved. This avoids both
the 2,264-byte replacement line and one filesystem write per checkbox.

Move that editor logic into one named raw-string script block in
`WebPage_Dashboard.h`, streamed by the page and extracted verbatim by the web
test harness. Do not test a hand-copied JavaScript model; the current general
syntax walker does not extract Dashboard's ordinary C++ string fragments.
Pass the numeric `CMD_INPUT_MAX` into that block from C++ page generation (or
as an explicit function argument); do not hand-type `2047` in JavaScript. Kind
tokens and command grammar are ASCII, so JavaScript string length equals the
wire byte count for this packer.

### 8.2 `events kinds` and command transports

The human `events kinds` output iterates the provider instead of manually
rescanning enum values. Move its packing into a dependency-light,
provider-shaped `System_EventCatalogTextCore.h` adapter with a fallible
whole-line sink. The current loop's 120-byte staging buffer is unsafe for
future longer canonical tokens: it advances `len` by `snprintf()`'s required
length even when the buffer truncated, so the next append can form an
out-of-bounds pointer (`components/hardwareone/System_Events.cpp:408-428`).

The dependency-light core receives a caller-owned line buffer and payload
capacity; production passes `DEBUG_MSG_SIZE - 1` from `System_Debug.h` rather
than duplicating `255` in the core. Before its first callback, the text adapter
validates every family header and canonical token against that
`broadcastOutput()` payload limit.
It must then append a token only when the entire indent/separator plus token
fits. Otherwise it flushes the existing nonempty line and retries the whole
token in an empty line. If any complete header/token cannot fit, preflight
returns an explicit error before emitting output; it never truncates the token,
advances by a would-have-written length, or emits a partial success. The
production sink NUL-terminates only after a checked append and calls
`broadcastOutput()` once per complete line. That existing function returns
`void`, so the command retains today's downstream queue-delivery semantics and
does not claim it can observe a later queue drop; the core's Boolean sink
failure remains useful to prove it stops issuing further callbacks. A host
fixture with a canonical token longer than the old 120-byte staging width
proves the retry path and a token beyond the transport limit proves preflight
failure with zero callbacks.

`events kinds json` calls `systemEventCatalogJsonToBuffer()` with its existing
`CMD_RESULT_MAX` PSRAM return buffer. On overflow it returns an `Error:` result
so the central command funnel records a failure. The adapter must have a test
that proves the current full payload plus NUL fits 4,096 bytes.

This one command adapter serves secure BLE, UART, ESP-NOW, MQTT, `/api/cli`, and
other command transports. No second BLE characteristic or unsolicited catalog
push is added.

### 8.3 OLED automation picker

Delete:

- `sWizKindPtrs[24]`;
- `sWizKindCount`;
- `wizBuildKindList()`;
- the fixed `sWizEventKind[40]` selection copy.

The family screen uses `systemEventCatalogFamilyCount()` and `FamilyAt()`. The
kind screen gets the selected family’s `kindCount` and resolves each visible or
tapped row with `FamilyKindAt()`. Store only navigation state such as selected
family, cursor, and scroll offset.

Keep the chosen family ordinal and within-family ordinal in the wizard, then
re-resolve the `KindInfo` at confirmation and immediately before command
submission. Display may truncate the label, but submission appends the complete
canonical name directly from the image-lifetime record. If re-resolution or
command-capacity validation fails, remain on confirmation and save nothing;
never copy through the former 40-byte buffer or persist a truncated token.

No family can silently disappear because it grew beyond a local array.
The selected family ordinal and within-family ordinal are the shared coordinate
model. OLED keeps its existing cursor/scroll implementation because a 128×64
wraparound list is not the same interaction as a paged lens or DOM select.

### 8.4 OLED notification picker and mandatory mask repair

Delete:

- `sNcKindNames[24]`;
- `sNcKindIds[24]`;
- `sNcKindCount`;
- `ncBuildKindList()`.

Resolve visible rows and taps through `FamilyAt()` / `FamilyKindAt()`.

In the same release, repair the mask width:

- add a dependency-light `System_EventKindMask.h` containing array-size-aware
  test/set/toggle helpers used by production notification and OLED code,
  including const/volatile array-reference overloads for the file-static device
  policy masks;
- replace literal `128` and manual word indexing with those helpers;
- delete the local `uint32_t mask[4]` mutation/rebuild buffer entirely;
- read the selected bit directly from the correctly sized
  `NotifViewer::muteMask`, send the inverse through the one-kind command, and
  re-resolve the viewer only after success;
- let the shared helper bounds-check every bit access from the actual array
  word count;
- prove a low-kind toggle preserves existing high-kind mutes.

The helper takes the bit index as `size_t` and derives capacity from the array
reference. Test returns `false` outside the array; set/toggle return `false`
without mutation outside the array and `true` when applied. It only owns bit
bounds, not catalog semantics, so callers still reject `SYSEVT_NONE` and kinds
outside the current catalog before saving.

OLED must no longer rebuild the full replacement command. Its toggle sends
`notifyusermute set <kind> <on|off>`. Extend the shared personal-kind command
implementation so `notifyusermute` (and the symmetric `notifyusershow` path)
supports bounded one-kind mutation, a validated multi-kind `patch`, and
`all`/`none`. Keep the old comma-list form for compatible callers that fit the
inbound limit.

Extract the parse/validate/mutate transaction into dependency-light
`System_NotificationKindListCore.h`; `System_Notifications.cpp` remains the
identity/JSON-file/load-save/event wrapper. The core consumes a provider-shaped
catalog resolver plus repeatable caller-owned current-token visitation and an
output sink, validates the complete operation, and produces a mutation result
before any save begins.
Define a syntactically valid preserved unknown token as 1–63 lowercase ASCII
`[a-z0-9_]` bytes, excluding `boot`, `none`, `set`, `patch`, `all`, and `list`;
bound the whole stored list to `NOTIF_KIND_MASK_BITS` entries. Semantics are
explicit:

- `set` and `patch` validate the current stored target list because they
  preserve it. Known aliases canonicalize; duplicate known tokens collapse by
  canonical name; valid unknown stored tokens deduplicate exactly and survive;
  malformed or over-limit current data fails without saving.
- A `patch` delta is stricter than stored-data cleanup: duplicate operations
  after alias canonicalization (for example `+boot,+boot_finished`) and
  contradictory `+kind,-kind` operations are errors, not silently deduplicated.
  Unknown delta tokens are errors.
- Legacy replacement validates/canonicalizes/deduplicates only its replacement
  input, matching current behavior; `all` and `none` generate their complete
  target directly. Those three operations deliberately ignore the old target
  list, so they can repair a malformed list while preserving unrelated user
  settings in the same document.

Do not implement the 256-token bound as `char current[256][64]`, a parallel
output matrix, or two 256-entry `TokenView` arrays on the 8-KB command stack.
The core is multipass: it re-visits strings held by the already-loaded PSRAM
JSON document, rescans bounded command input, and passes `{pointer,length}`
views into the catalog resolver/output sink instead of copying known tokens to
a 64-byte scratch. The 63-byte cap applies only to preserved unknown tokens;
known canonical names are bounded by the provider's generated
`SYSTEM_EVENT_KIND_TOKEN_CAP`, never by a second literal. Fixed catalog-
membership/delta bitsets keep target core scratch below 512 bytes, and bounded
O(n²) comparison is used only for at most 256 preserved unknown tokens. After
complete validation, the wrapper replaces the in-memory target array and checks
every JSON insertion; allocation/output failure discards the unsaved document
and returns `Error:`. Measure peak core stack and the wrapper's PSRAM/internal-
heap fallback; OOM is an explicit no-save result.

The production wrapper calls that core inside a new user-settings transaction:
one outer filesystem guard spans load of the latest document, extraction of the
current list, core mutation, JSON replacement, and save. Rework
`mergeAndSaveUserSettings()` over the same helper so HTTP patches cannot
interleave their own load/save gap with a command task. Move existing-user
password and gamepad-password RMW writers onto it as well, with expensive hash
derivation outside the lock. The helper exposes a guarded test hook only in the
device-test build to pause after load, inject zero/short temp writes, or force
the rename fallback and inject zero/short writes after the disposable
destination has been opened. The latter cases deliberately model destination-touched,
non-rollback-safe failure; every failed save path invalidates the preference
cache before returning so a caller cannot keep an optimistic or stale view. No
hook is a production command/API. Parsing and catalog validation remain outside
the lock where they do not depend on current state, while list-preservation
validation necessarily uses the locked current document.

Repair the cache miss/publish race in `notifViewerResolve()` at the same time.
A dedicated user-preference cache generation is read and advanced only while
`gUserPrefsMutex` is held. A miss captures that generation, releases the mutex,
loads from flash, reacquires the mutex, and either publishes against the same
generation or discards/retries after an invalidation. Copy the selected values
to the caller before releasing the cache mutex, establishing a clear
linearization point. The resolver never holds the cache mutex across a
filesystem read, so the user-settings transaction and cache invalidation do not
introduce a filesystem-lock/cache-lock cycle. The device-test build exposes a
one-shot pause after flash load and before the generation check to prove a
concurrent committed mutation cannot be overwritten by a late cache fill.

Add a dependency-light `System_CommandLimits.h` with the named
`CMD_INPUT_MAX`, `CMD_RESULT_MAX`, and a constexpr input-length predicate. Move
the existing outbound constant from `System_CommandTypes.h` into that header so
production executor arrays, the JSON adapter host budget test, UART, and
Dashboard generation all consume the same real limits. Size `ExecReq::line` in
`System_CommandTypes.h` as `CMD_INPUT_MAX + 1`.
`submitAndExecuteSync()` must use that predicate, set an explicit error result,
and reject a longer line before its early-boot direct or queued path can execute
it. `submitCommandAsync()` must use the same predicate before
allocation/queueing and return `false`. Silent truncation is never a valid
fallback. The dependency-light predicate gives the host suite a production
test seam; on-device integration verifies both submission functions actually
apply it.

`CMD_INPUT_MAX` is the central queued-executor ceiling, not a promise that every
transport accepts that much. A transport may impose a smaller documented cap;
its effective maximum is the smaller value and it must also reject whole input
rather than truncate it.

Replace `System_UartLink.cpp`'s private `kUartLineCap = 2047` with the shared
constant while retaining its discard-the-whole-line behavior at the boundary.
Update both command-registry usage strings in `System_Settings.cpp`; built-in
help must advertise replacement, `set`, `patch`, `all`, and `none` forms.

The mask repair is independently valuable and remains if the catalog refactor
is rolled back.

### 8.5 G2

Do not add a standalone catalog browser. Current event history rendering keeps
using the compatibility name accessor.

When G2 gains automation create/edit support, its event-trigger step should be
family-first. The code that owns this state is the **ESP32 G2 page adapter**, not
the glasses:

```cpp
enum class G2EventPickerLevel : uint8_t { Families, Kinds };
enum class G2EventSwapPhase : uint8_t {
  Idle,
  PreparingCandidate,
  SuspendedBeforeTransition,
  Transitioning,
};

enum class G2EventRowAction : uint8_t {
  Back,
  Family,
  Kind,
  PreviousPage,
  NextPage,
};

struct G2EventRowBinding {
  G2EventRowAction action;
  size_t providerOrdinal;
};

// Picker policy: one Back row, up to 12 provider options, Prev and Next.
inline constexpr size_t G2_EVENT_ITEMS_PER_PAGE = 12;
inline constexpr size_t G2_EVENT_CHROME_ROW_CAP = 3;
inline constexpr size_t G2_EVENT_VISIBLE_ROW_CAP =
    G2_EVENT_ITEMS_PER_PAGE + G2_EVENT_CHROME_ROW_CAP;  // 15
// "ev" + 8 hex lifecycle digits + 8 hex cookie digits + trailing NUL.
inline constexpr size_t G2_EVENT_WIRE_CONTAINER_CAP = 19;
static_assert(G2_EVENT_VISIBLE_ROW_CAP == 15);

struct G2EventPagePresentation {
  bool valid;
  G2EventPickerLevel level;
  size_t familyOrdinal;
  size_t page;
  uint32_t lifecycleEpoch;
  uint32_t pageCookie;
  uint32_t presentationEpoch;  // zero while pending; set only on commit
  char wireContainerName[G2_EVENT_WIRE_CONTAINER_CAP];
  size_t rowCount;
  G2EventRowBinding rows[G2_EVENT_VISIBLE_ROW_CAP];
};

struct G2EventPickerState {
  uint32_t menuGeneration;  // supplementary redraw bookkeeping, not tap authority
  G2EventSwapPhase swapPhase;
  G2EventPagePresentation activePresentation;
  G2EventPagePresentation pendingPresentation;
};
```

The 15-row cap is an event-picker policy, not a universal G2 limit: pages in
this firmware choose different buffers, and the current Automations list can
submit 17 rows (`components/hardwareone/G2_Page_Automations.cpp:44,180-210`).
Twelve option rows keep all 12 current families on one family page (plus Back)
and split the largest current 21-kind family into two pages. The event adapter
always passes `G2_EVENT_VISIBLE_ROW_CAP` to `g2PaginatorWriteChrome()`, asserts
the returned row count does not exceed it, and tests 0/1/12/13-item boundaries
plus a synthetic 25-kind family. Put the G2-only row-plan/binding math in a
dependency-light `G2_EventPickerCore.h` that accepts a provider-shaped count/
resolve view; the production wrapper supplies `System_EventCatalog`, while host
tests supply 0/1/12/13/25-kind fixtures. A device-test-only, read-only 25-kind
fixture uses the same core and real force-recreate list path to render the
middle page as Back + 12 options + Prev + Next (all 15 rows). Its selection
returns only to the harness and can never save an automation. Current
production families can reach at most 14 rows, so physical 15-row evidence must
name this fixture rather than pretending a 21-kind production family reaches
it. Allocation, fragmentation, and real-lens tests use that exact worst case.
Raising 12 later is a deliberate page-policy/memory/hardware-test change, not
something catalog growth does silently.

Rows are resolved from the shared typed provider at render and tap time. The
adapter may use the existing G2 paginator, but it must map a tap back to the
provider ordinal under the current presentation epoch/lifecycle and page-swap
state before accepting it. Menu generation remains useful for redraw cookies,
but current page-swap jobs only log a menu-generation mismatch; it is not the
authoritative stale-input fence.

The current BLE RX slab captures temple side, that side's connection
generation, lifecycle epoch, and presentation epoch with the packet
(`components/hardwareone/G2_Glasses.cpp:4981-5029`), and `handleDevEvent()`
receives those stamps (`components/hardwareone/G2_Glasses.cpp:5597-5600`). The
ListEvent branch then drops them when it calls `tapDispatcherEnqueue(idx,
iname)` (`components/hardwareone/G2_Glasses.cpp:5813-5828`). That enqueue
function re-samples the then-current presentation epoch
(`components/hardwareone/G2_Glasses.cpp:20886-20899`); the worker validates the
re-sample (`components/hardwareone/G2_Glasses.cpp:20606-20624`) and later calls
`G2PageModule::handleTap(uint32_t idx)`, which discards identity before the page
module runs (`components/hardwareone/G2_Glasses.cpp:9704-9775` and
`components/hardwareone/G2_Glasses.h:1098-1123`). Cross-arm
`shouldDedupHijackTap()` also re-samples the current presentation epoch
(`components/hardwareone/G2_Glasses.cpp:9535-9545`), so a delayed tap from page
A can pollute page B's dedup window before enqueue.

Those RX stamps alone cannot prove the physical page that originated a tap. A
page-A notification first delivered to `g2RxPacketEnqueue()` only after page B
commits is stamped with B's then-current lifecycle/presentation; RIGHT's B
CREATE/ACK does not order a delayed LEFT callback. Therefore Phase 4 also gives
every event-picker presentation an opaque wire tap token. Format the exact list
container name as `ev` plus the eight-hex-digit lifecycle and eight-hex-digit
page cookie, store it in pending/active presentation state, and pass it through
the existing arbitrary `containerName` field of the ordinary G2 ListObject.
`List_ItemEvent.ContainerName` already echoes that field into `cname`; route an
event-picker tap only when the echoed name exactly matches the committed
presentation before dedup. The page cookie remains an ESP32 state key; its
derived container token is the deliberately transported correlation value.
It is not an authorization secret. If supported glasses firmware does not echo
the exact container name, the picker does not ship on that firmware—epoch
stamping, item names, or a timing quarantine are not claimed as an equivalent
hard stale-page proof. This is a real feasibility gate: the current CREATE
helper calls non-default container names experimental and records that an older
dual-pane experiment did not permit its intended use
(`components/hardwareone/G2_Glasses.cpp:18332-18343`). Before building the
picker, prove two distinct names can each CREATE/ACK, render, and echo unchanged
on every supported glasses firmware; failure blocks this design rather than
silently weakening it.

Formatting must return exactly 18 lowercase ASCII bytes plus NUL and fit the
existing 32-byte receive field. Preserve the protobuf `ContainerName` byte
length during parsing: accept the reserved token only when `length == 18`, all
16 suffix bytes are lowercase hex, and a length-aware comparison matches all
18 bytes. Do not validate only the truncated `cname[32]` C string; embedded NUL,
extra suffix bytes, uppercase, and an overlong wire field must fail. Reserve
cookie zero; if the 32-bit page cookie would wrap to a value already usable in
the same lifecycle, advance/revoke the lifecycle under the normal ordered
transition or fail the page creation rather than reuse a wire token. Host tests
cover formatting, malformed wire forms, and the wrap boundary.

The ListEvent router must also become aware of this *correlation envelope*.
Today only the literal `cname == "app"` reaches `tapDispatcherEnqueue()` after
the named special-container branches
(`components/hardwareone/G2_Glasses.cpp:5675-5828`). Preserve that legacy path,
but also admit only the exact length-aware reserved `ev[0-9a-f]{16}` shape into
the preclaim-aware dispatcher while carrying the validated full token. Do not turn every
arbitrary container name into a generic tap. A reserved event-picker token is
owned by this path: if the current module has no picker preclaim, or the
Automations preclaim has no matching active picker, treat it as `Stale` and
consume/drop it rather than falling through to text/Sys or legacy `handleTap`.
For an ordinary `"app"` event in today's Automations modes, the preclaim returns
`NotClaimed` and existing dispatch behavior remains unchanged.

Phase 4 must carry the **original RX-captured** side, that temple's connection
generation, lifecycle epoch, and presentation epoch, plus the exact echoed
wire-container token parsed from that ListEvent, into cross-arm dedup and
through `tapDispatcherEnqueue`, `TapDispatchEntry`, worker validation, and the
page callback. Re-sampling current state at dedup/enqueue—or calling a getter
in the adapter—is not an equivalent fix: a page swap can start after the packet
was captured or between any later check and mutation. Adding the side,
connection stamps, and bounded token changes `TapDispatchEntry`, so update its
queue-storage/size static assertions at
`components/hardwareone/G2_Glasses.cpp:20411-20442` rather than silently
growing the reserved storage.

Preserve source compatibility for the existing page registry: append two
optional tap callbacks to the end of `G2PageModule`, keep the existing
`handleTap(uint32_t)`, and leave existing aggregate initializers and roughly
twenty legacy handlers zero-initialized/unchanged. The first is a strictly
bounded pre-dedup validator returning
`G2TapPreclaim::{NotClaimed, Fresh, Stale}`; the second is the context-aware
dispatch callback returning `G2TapDisposition::{Handled, NotHandled}`. For an
ordinary `"app"` event, the Automations preclaim returns `NotClaimed` in today's
list/detail/run/enable/disable submodes. For any reserved `ev` token it claims
the correlation path: an active picker acquires only the adapter guard, checks
`Idle`, active lifecycle/presentation, and the exact echoed token against the
single authoritative `activePresentation`, and returns `Fresh`; absence or
mismatch returns `Stale`, without rendering, logging, allocating, or mutating
the draft. The context
wrapper returns `NotHandled` for today's modes and `Handled` for every new
picker tap—including a rejected stale picker tap, which must never fall through
and be reinterpreted by the legacy handler. Add one
`g2PageHasTapHandler()` predicate and use it at the earlier text/Sys fallback
ownership checks as well as final dispatch; the current checks at
`components/hardwareone/G2_Glasses.cpp:5911-5927,6109-6134` look only at the
legacy pointer and would otherwise misclassify a context-only module.

Also append an optional idempotent `onPresentationRevoked()` hook. G2 terminal,
root-menu, recovery, authoritative-arm/full-session/topology disconnect, and
lifecycle-advance paths invoke it for the page whose presentation is actually
invalidated even when no swap is pending. A non-authoritative temple disconnect
does not invalidate the global page and does not invoke this hook. The
Automations hook clears active/pending/phase under the same adapter
synchronization and performs no render. A prior `Committed` terminal does not
replace this later lifecycle notification.

Make wire-token/freshness validation and cross-arm dedup one indivisible claim,
not two adjacent checks. Keep the expected token in exactly one place—the
Automations adapter's committed `activePresentation`; do not add a second
broker-owned “current token.” Add a short `gTapFreshnessMux` and a helper that
enters the order `gSyncLifecycleMux` → `gTopologyMux` → `gTapFreshnessMux` →
optional page-adapter guard. It first compares original side/that temple's
connection generation and original global lifecycle/presentation against live
G2 state, then invokes the registered bounded preclaim while the same G2 locks
remain held. For the picker, that callback compares the echoed container token
and epochs against the committed adapter state; `Stale` exits before any dedup
read/write, while `Fresh` permits the helper to claim the slot. A missing
preclaim means legacy `NotClaimed` behavior, never implicit acceptance for a
picker page. Connection-generation publishers take topology then freshness;
lifecycle publishers take sync-lifecycle then freshness; presentation-epoch
advance and dedup state take freshness. Adapter transitions take the adapter
guard only after those G2 locks when a combined claim is required, and adapter
code never calls a G2-locking API while already holding it. No path acquires
those locks in reverse. The helper returns `Stale`, `Duplicate`, or `Accepted`,
stores the original presentation/token in the slot, and never re-stamps the
context. Thus reconnect, lifecycle reset, page transition, or late old-page
wire token cannot land between freshness validation and dedup mutation, and
stale A cannot evict a valid page-B record.

Carry the unchanged stamps and echoed token in `TapDispatchEntry`. Immediately
before page dispatch, the worker calls a second shared
`g2TapRevalidateFreshness()` helper. It acquires the same
sync-lifecycle→topology→freshness→adapter order, compares all four stamped
dimensions, and invokes the same non-mutating preclaim against the sole
`activePresentation` token owner, but performs **no** dedup read or write. A
reserved `ev` context proceeds only on `Fresh`; missing callback,
`NotClaimed`, or `Stale` hard-drops it. This catches a reconnect, swap, or
adapter revocation after enqueue without introducing mirrored token state or
poisoning the dedup slot. Neither check replaces the original context. Do
**not** freeze both temple generations in the page
mapping: a non-authoritative temple can reconnect without changing the current
global page, and a fresh tap from its new generation must remain usable. That
disconnect advances only the affected temple's connection generation; it does
not advance global lifecycle/presentation or revoke the adapter
(`components/hardwareone/G2_Glasses.cpp:2077-2110`). The page cookie remains
adapter-local; only its derived opaque `wireContainerName` crosses the existing
G2 ListObject metadata field and returns in the ordinary ListEvent. The
**final** Automations mutation claim again validates the original
side/generation, global lifecycle/presentation, and exact echoed wire token,
then binds them to the complete idle committed `activePresentation` and mapped
row/page cookie in one ordered critical claim. Its adapter state guard is a short
nonblocking critical-section guard acquired only after the G2 freshness locks;
adapter code never acquires G2 locks while already holding it. Only bounded
state/canonical-token copying and in-memory draft mutation occur inside; render,
filesystem, BLE, allocation, and page-swap calls occur after release. Swap
admission/suspension uses the same ordering. If the tap claim wins first, it
applies while the old page and arm incarnation are authoritative; if swap
suspension or reconnect wins first, the old tap fails. Thus a swap or one-arm
reconnect between worker validation and adapter mutation cannot apply the old
row.

The adapter lock protects only state claims, not rendering or swap calls. A
navigation tap atomically validates/copies its `G2EventRowBinding`, reserves a
fresh operation/page cookie, and moves to `PreparingCandidate`; it then releases
the lock before formatting rows, allocating a job, or calling the page-swap
API. Candidate-build failure reacquires the lock and returns to `Idle` only if
that operation cookie still owns the preparation. This both rejects a second
tap during preparation and prevents deadlock when the swap API synchronously
calls back into the adapter. Selection/draft mutation likewise copies the full
canonical identity under a short claim and calls no renderer, filesystem, BLE,
or page-swap function while holding the adapter lock.

Build new page/row mapping as candidate state with a fresh page cookie. Current
`g2ShowListPage()` returns after deep-copy and enqueue, not after CREATE/ACK, so
admission is insufficient to publish that mapping. Extend the Phase 4 page-swap
request with the caller's page cookie and staged lifecycle callbacks, not a
single ambiguous Boolean:

1. Candidate allocation/deep-copy failure occurs in `PreparingCandidate`,
   before broker admission; the owning operation returns to `Idle` and the
   active mapping remains usable.
2. `Admitted` is a synchronous, veto-capable handshake under the shared
   swap/adapter synchronization, executed before `xQueueSend()` can expose the
   job. It must match the expected preparation cookie, install the complete
   `pendingPresentation`, and suspend `activePresentation`. If it cannot bind
   that exact state, the broker does not enqueue/continue and emits a
   pre-transition terminal outcome. An enqueue failure after a successful
   handshake does the same; same-lifecycle rollback resumes active.
3. `TransitionStarted` is a second synchronous, veto-capable handshake before
   the first physical lens mutation, including REBUILD or SHUTDOWN. It must
   match suspended pending state and atomically retire active. A veto stops the
   worker before lens mutation and emits a pre-transition terminal outcome;
   same-lifecycle rollback may resume active.
4. Exact CREATE/ACK success promotes the entire pending presentation and
   reports its committed presentation/lifecycle epoch. Any failure after
   `TransitionStarted` clears both mappings and enters recovery.
5. Real worker teardown/drain is not a resumable same-lifecycle cancellation:
   `g2UiWorkersQuiesce()` advances presentation before draining
   (`components/hardwareone/G2_Glasses.cpp:21114-21158`). Its terminal outcome
   frees pending and clears active. Root-menu recovery, page-invalidating
   authoritative/full-session/topology disconnect, and lifecycle replacement
   likewise always clear both mappings. A non-authoritative one-arm disconnect
   is only a per-temple generation change and is not one of these terminals.

Every request that reaches either handshake receives exactly one terminal
outcome, including enqueue failure, teardown drain, recovery, and any
page-invalidating disconnect.
Order is monotonic (`Admitted` → optional `TransitionStarted` → one terminal
result). A failed/stale **handshake is a veto and is never ignored**; only a
duplicate or late terminal notification may be ignored after proving its
cookie/lifecycle no longer owns pending state. This prevents a physical page
from advancing without an authoritative row map, a lost completion from
leaving active suspended, or a stale completion from reviving it.

An **admitted page-swap request** has an explicit cross-module terminal order.
On its commit, abort, failure, teardown drain, recovery, or page-invalidating
disconnect path,
invoke the adapter's state-only terminal callback while that request still owns
the broker admission fence; the callback updates or clears adapter presentation
state under its short state lock and must not render, allocate, or call back
into the broker. Only after that state is published may the broker release/end/
fail/cancel `gPageSwapAdmission`, and only after release may it destroy the
page-job arguments that supplied the copied cookie/result. This closes the
interval in which a tap could pass the broker after the physical transition but
before the adapter learned which row map is authoritative. A device-test pause
immediately before broker-fence release proves taps remain excluded while
terminal state is already published and observe only the correct map after
release.

Idle revocation is a separate path: when a committed picker is invalidated by
an authoritative-arm/full-session/topology disconnect, root recovery, or
lifecycle replacement and no swap is admitted,
there is no `gPageSwapAdmission` owner or job to release. Under the existing G2
lifecycle/topology synchronization, advance the global lifecycle and/or
presentation epoch **before** invoking `onPresentationRevoked()`. Worker
revalidation rejects old-stamped queued taps; a packet stamped after the advance
still cannot match the old epoch stored in `activePresentation`. The idempotent
callback then clears active/pending/phase under the adapter lock. A paused race
between epoch advance and callback proves both old-epoch and new-epoch taps are
rejected and no nonexistent swap fence/job is touched.

Publish the entire pending presentation only on exact committed success; do not
overwrite the active row bindings while merely preparing a candidate. A tap is
accepted only in `Idle`, when the original RX dispatch context, active page
cookie, connection/lifecycle identity, and committed presentation epoch match
in the atomic claim. The adapter must not copy a fixed maximum number of kinds.
Its two bounded row-binding arrays cover only currently renderable lens rows,
not the catalog. Canonical name is copied into the
automation draft's owned
`SYSTEM_EVENT_KIND_TOKEN_CAP` buffer only after the selected ordinal is
re-resolved successfully; the draft does not retain a display-row pointer or
numeric id.

The resulting row text is passed to `g2ShowListPage()`, encoded into the G2
widget protocol, and sent over the existing BLE Central link. No catalog JSON
crosses that link and no glasses-side JSON parser is assumed. That is a default
for the current integration, not a claim that JSON could never be supported: a
future independently programmable G2 runtime could become a remote JSON
consumer, but it would be a new endpoint and protocol feature.

Before the picker uses that renderer, add an explicit safe swap policy such as
`G2ListSwapPolicy::ForceRecreate` to the page-swap API (or disable the unsafe
fast path globally after its own regression review). The current default-on
same-row-count `REBUILD-list` path
(`components/hardwareone/G2_Glasses.cpp:2594-2604,19412-19428`) is exactly what
adjacent source says can crash the glasses plugin when the item content changes
(`components/hardwareone/G2_Glasses.cpp:19200-19220`). Adjacent catalog pages
often have the same row count and different rows, so the future event picker
must force SHUTDOWN/CREATE and must not infer safety from equal counts.

The provider work can ship before the G2 authoring UI. That follow-on consumes
the stable API rather than changing it. Existing G2 history and automation
summary behavior remains unchanged in the provider release.

### 8.6 Android Bluetooth companion app

The app-side catalog repository should be added when the app gains automation
create/edit support:

1. wait for an authenticated, established Secure Channel;
2. lazily issue `events kinds json` when an event-trigger editor opens;
3. capture/reassemble the existing multi-frame JSON command reply;
4. validate `families[].n`, `families[].k`, snake_case names, and uniqueness;
5. coalesce simultaneous callers;
6. cache only a successful result for that live device connection;
7. clear the cache on disconnect or firmware/session replacement;
8. preserve a stored unknown kind as unavailable instead of silently changing
   it.

Do not request the full catalog on plaintext BLE. The current reply lane does
not fragment plaintext command results. If plaintext support becomes a product
requirement, add a separately versioned cursor/max-byte page command and test
every response against the negotiated `MTU-3`; do not overload the STATUS
characteristic or advertise that full v1 is plaintext-safe.

This is initially enforced by the Android client, not by the firmware command
handler. A third-party plaintext client may still issue the command and trigger
the existing oversized-send failure. If firmware-wide prevention is desired,
that is a separate transport-aware command-result change.

The target companion app is maintained outside this firmware tree. At Phase 4,
verify its then-current automation-summary behavior against the pinned app
revision. Any event-trigger summary must retain and display the stored canonical
`on` value rather than a generic `event` label; add that behavior to the app
tests if it is not already present. This plan does not claim the state of an
unpinned external checkout.

### 8.7 UART/CM5 and other command clients

UART’s authenticated command path can already carry the current 2,877-byte
JSON result inside its 4,096-byte command result buffer (the payload plus its
trailing NUL must fit). A future CM5 consumer may issue `events kinds json`
through the ordinary request/reply collector.

Do not put the catalog into spontaneous UART event frames. Those frames are a
different, bounded notification lane, not a metadata transport.

---

## 9. Impact analysis of the converged pipeline

### 9.1 What convergence changes

The provider becomes the compatibility boundary for all event-option
projections. A family/order/bounds bug can therefore affect several interfaces
at once, while a fix applies to all of them. The control is to keep the provider
small, immutable, dependency-light, and exhaustively tested, then retain
adapter-specific tests for row/tap/DOM/wire behavior.

The arrows in the architecture diagram are not equivalent transformations. A
local indexed provider read returns immutable records without copying their
strings. A JSON round trip serializes and escapes all 2,877 current bytes,
buffers or streams them, parses them back into tokens, and only then recovers
records the ESP32 already had. That extra work would be modest when performed
occasionally—the catalog is small, so JSON-everywhere is technically viable—but
it creates an unnecessary internal schema/parser dependency and another
failure/allocation path.

| Consumer path | Required transformations after the provider | Consequence |
|---|---|---|
| OLED | Screen policy and pixel rendering | No JSON buffer/document; existing input behavior remains local |
| ESP32 G2 page adapter | Row/page policy, G2 widget encoding, BLE send | The external glasses remain a protocol endpoint; G2 encoding is required whether JSON exists or not |
| HTTP/command client | JSON encoding, then transport-specific framing | One wire schema, while HTTP/secure BLE/UART keep their own delivery limits and authentication |
| Hypothetical JSON-for-local UI | JSON encode, JSON parse, then the same UI/G2 work above | Feasible but adds a conversion without removing the final renderer/protocol adapter |

Convergence therefore occurs at typed meaning, not at the final byte format.
Name lookups used by automation matching or ring viewers may call the same
catalog directly; they do not need to pretend to be visual pickers.

### 9.2 UI and G2 behavior impact

- OLED receives the complete option set without 24-entry materialization, but
  keeps its own wrap/clamp, scroll-window, label-width, and button behavior.
- The ESP32-side G2 adapter will use the same family/kind ordinals and canonical
  identities, while retaining the existing G2 paginator, tap-row mapping,
  presentation/lifecycle fences, page-swap admission, supplementary redraw
  generations, and asynchronous lens jobs.
- The physical G2 glasses remain external. Phases 1–3 change neither their
  firmware nor `System_G2_Protocol`; no catalog JSON is sent to either temple.
  Phase 4 reuses the existing arbitrary ListObject container name and echoed
  ListEvent field as an opaque per-page correlation token; it adds no new wire
  field or glasses-side parser.
- `g2ShowListPage()` currently deep-copies submitted rows. A future picker may
  format only the bounded visible page, but tap handling must map the current
  row back to a provider ordinal under the same presentation epoch/lifecycle.
  Candidate row mapping is published only after a staged swap completion
  commits the exact page cookie and presentation epoch—not merely after enqueue
  admission. A failure proven to precede physical transition may resume the
  still-visible active mapping; a failure at or after transition start,
  page-invalidating disconnect/reconnect, root recovery, or lifecycle
  replacement accepts neither old nor pending rows. A non-authoritative temple
  reconnect is instead handled solely by per-arm generation freshness and does
  not discard the still-current map. Page shrink and stale taps must never
  select a row outside the committed mapping.
- Equal-sized G2 pages are a high-risk case, not an optimization opportunity.
  The current default-on `REBUILD-list` fast path can send different rows into
  a live same-count list even though source documents that operation as a plugin
  crash trigger. The picker requires an explicit force-recreate path. Current
  safe page changes wait 500 ms between SHUTDOWN and CREATE, then wait for the
  CREATE response (`components/hardwareone/G2_Glasses.cpp:19452-19489`), so lens
  page navigation has at least that latency plus BLE/ACK time.
- OLED, G2, and web may format or truncate labels differently. Canonical names,
  not formatted row text, remain the identity saved into an automation.
- Unknown persisted canonical names remain visible as unavailable until the
  user deliberately replaces them; no adapter silently falls back to row zero.

A general cursor/paginator is deliberately **not** added in Phases 1–3. Current
OLED flows already differ (automation wraps while notification behavior
clamps), and G2 authoring does not yet exist. If two implemented adapters later
share pure family→kind transition math, extract only that caller-owned math into
a dependency-light helper. Do not put OLED scroll state, G2 chrome/generation,
or browser cache state into the provider.

### 9.3 Memory and performance impact

- Existing name/family tables move; they are not copied.
- The family-order index adds roughly 180–200 bytes of immutable storage.
- Removing the two OLED materialized lists recovers their pointer/id arrays.
- Typed provider lookup and indexed enumeration allocate nothing.
- JSON serialization uses a small bounded stack chunk and caller-owned output.
- HTTP drops its static 4-KB catalog buffer and ArduinoJson document.
- The command path retains one bounded return buffer because the command
  handler contract returns a `const char*`.
- Notification list mutation reuses the loaded PSRAM-backed user-settings
  document and a multipass core with fixed bitsets plus pointer/length token
  views; it must not place 256×64-byte current/output matrices on the 8-KB
  executor stack or impose the unknown-token 63-byte cap on known catalog
  names. Measure the target-below-512-byte core scratch, JSON document PSRAM/
  internal fallback, and explicit OOM/no-save path.
- Each user-settings save gains one `measureJson()` traversal and an observed
  post-flush size check so partial output cannot be mistaken for success. The
  current file API supplies no checked flush/fsync result, so this is an
  observable-length guard rather than a durability claim. This adds
  CPU work only to settings writes, not event posting, catalog reads, display
  redraws, or preference-cache hits; record write latency in the device cases.
- A future G2 picker does not allocate a JSON document for the catalog. The
  existing G2 Automations page still parses persisted automations JSON for its
  separate storage purpose, so this change does not claim to remove ArduinoJson
  from that page or binary.
- A G2 render is not allocation-free: existing page-swap code deep-copies rows.
  The current list lifecycle also uses a SHUTDOWN/settle/CREATE swap and an
  8-KB PSRAM-preferred protocol scratch buffer because changed-list REBUILD is
  unsafe on the glasses firmware
  (`components/hardwareone/G2_Glasses.cpp:18340-18392,19200-19220`). The future
  picker therefore renders one visible page per navigation action rather than
  materializing or transmitting the whole catalog. That scratch allocation can
  fall back to internal heap, and each swap separately allocates its pointer
  array and row copies, so repeated paging must be tested for largest-block loss
  and recovery under constrained PSRAM/internal heap—not only steady-state free
  bytes. The zero-allocation claim applies only to the typed provider and
  indexed accessors.

Measure final `.rodata`, internal DRAM, stack high-water marks, and PSRAM deltas
from build artifacts and device tests; do not rely only on the estimates above.

### 9.4 Transport and compatibility impact

- The existing `families[].n` / `families[].k` JSON shape remains unchanged, so
  the current web client has no migration.
- HTTP and command output gain one emitter but retain distinct authentication,
  framing, buffering, and failure behavior. “Shared JSON” does not mean shared
  physical delivery.
- Phone-companion Secure Channel behavior and the 4,096-byte command result
  buffer budget, including trailing NUL, remain as documented. Its roughly
  195-byte secure payload frames make
  the current catalog about 15 frames with at least roughly 420 ms of configured
  inter-frame pacing
  (`components/hardwareone/System_BleSecureChannel.cpp:242-321`), so the app
  fetch remains lazy and connection-cached. Plaintext phone BLE remains unable
  to carry the full response reliably.
- G2 BLE-Central traffic remains G2 widget/session protocol traffic. It does not
  use the phone BLE command channel or its Secure Channel fragmentation. The
  current build treats G2 client mode and phone BLE-server mode as mutually
  exclusive, so no design may assume both catalog transports are available in
  the same live BLE role.
- Numeric enum ids remain process-local. Canonical names remain the only
  persisted and transported identities, so table reordering cannot corrupt
  stored automations.

### 9.5 Build and dependency impact

- `System_EventCatalog` and `System_EventCatalogJson` are unconditional because
  the command layer is always available. Both must compile when OLED, HTTP,
  Automation, Bluetooth, or G2 is disabled.
- Provider headers contain no JSON sink/status, Arduino, FreeRTOS, HTTP, OLED,
  or G2 protocol types. The JSON declarations live in their own adapter header.
- OLED source gains only the typed-provider dependency. No G2 source changes in
  Phases 1–3 beyond compatibility verification.
- Production tables and algorithms remain out-of-line so including the public
  header from several adapters does not instantiate duplicate tables.
- Moving the row vocabulary/public enums changes include ownership across many
  existing event producers. `System_Events.h` retains the compatibility include,
  and the build matrix must catch feature-gated include or link regressions.

### 9.6 Concurrency and lifetime impact

- Catalog tables are immutable after link; typed reads are lock-free and
  reentrant.
- JSON-adapter state is caller-stack-only and reentrant. The sink callback
  consumes bytes synchronously before returning; there is no global serializer
  buffer or new task.
- Provider string pointers have firmware-image lifetime, but formatted UI rows
  do not. Any asynchronous adapter must copy row text into its existing owned
  job/session storage before returning. Current `g2ShowListPage()` already does
  this; tests must prevent a future zero-copy regression.
- Selection state is caller-owned. OLED automation, OLED notifications, HTTP,
  and a future G2 picker can run independently without a singleton cursor or
  mutable provider cache.
- Phase 4 G2 tap admission adds one short freshness critical section spanning
  original arm-generation/lifecycle/presentation validation and cross-arm
  dedup mutation, with final adapter revalidation under the declared lock
  order. It performs no allocation, formatting, BLE, filesystem, queue wait,
  or test pause while held; measure worst-case hold time and lock contention.
- Incremental notification edits introduce a persistence read-modify-write
  boundary. A filesystem guard must span the complete user-settings
  load→mutate→save transaction for both command and HTTP merge paths; separately
  locked load/save calls can lose disjoint concurrent edits even when the
  dependency-light mutation core is correct.
- Cache invalidation alone is insufficient if a resolver loaded old flash state
  outside its cache mutex. Its miss path must compare a mutex-guarded cache
  generation before publishing, discard/retry after any intervening save, and
  never acquire the filesystem guard while holding the cache mutex. This adds
  one generation compare on a miss, not on provider traversal or cache hits.
- HTTP, command, OLED, and G2 adapters retain their current task,
  authentication, and generation-fencing rules. None of the new provider or
  JSON functions is promised ISR-safe.

### 9.7 Security and privacy impact

- The catalog contains compiled vocabulary and family labels only. It contains
  no event occurrences, usernames, SSIDs, BSSIDs, BLE addresses, RSSI values,
  or proximity history.
- Catalog discovery is not authorization. Automation creation continues to
  validate the canonical name in firmware, and the command authorization path
  still decides whether the write may occur.
- Canonical names are metadata, not trustworthy evidence that a physical
  device is present. Future proximity triggers must not authorize
  security-sensitive actions merely from spoofable names/addresses.
- JSON escaping remains mandatory even though current strings are compiled
  literals; future labels must not be able to break the transport document.
- Secure phone BLE is required here for framing/reassembly as well as session
  integrity. The fact that catalog names are non-secret does not make an
  oversized plaintext notification reliable.
- The future dynamic SSID/BLE scan-result provider remains separate. It has
  runtime lifetime, freshness, privacy, spoofing, RSSI, and hysteresis rules
  that an immutable static options provider intentionally does not acquire; see
  §9.9.

### 9.8 Testing and maintenance impact

- Provider tests become the single proof of family/kind completeness, order,
  lookup, non-contiguous-family handling, and bounds behavior.
- JSON-adapter tests separately prove exact wire parity, escaping, sizing, and
  partial-sink failure. A cross-projection test parses JSON and compares every
  family and canonical kind to typed traversal.
- Adapter tests still prove UI-specific behavior. Provider correctness alone
  cannot prove OLED wrap/clamp behavior, G2 page/tap generation mapping, web
  Promise caching, or transport fragmentation.
- Future catalog additions require one private row-header edit and provider
  tests, then are automatically available to provider-backed adapters. An
  interface without an authoring screen still needs that screen; “available”
  does not mean visibly exposed everywhere.
- Broader reuse increases the review blast radius of provider API changes.
  Keep the public records minimal and additive; treat ordering and canonical
  names as compatibility-sensitive even though enum numbers are not.

### 9.9 Consequences for the future WiFi/BLE scan-result provider

The converged static catalog does not become a generic bucket for live scan
rows. Current WiFi scan callbacks expose transient records only during a
synchronous visit (`components/hardwareone/System_WiFi.h:26-67`), already a
different contract from image-lifetime catalog strings. A later scanner design
must account for these consequences:

- **Atomic snapshots, not naked pointers.** A rate-limited worker fills an
  unpublished bounded buffer, then publishes a generation only after a complete
  successful scan. Consumers hold a snapshot/lease or copy the chosen identity
  before release. Expose `busy`, `unavailable`, `error`, `stale`, and explicit
  truncation separately; an empty successful RF environment is not the same as
  a failed scan.
- **Two activation modes, without an always-scanning UI.** An on-demand
  asynchronous `scan/match` step can start a one-shot worker inside an
  automation that was triggered by something else, yield only that run, then
  request stop, join the worker, release its radio lease, and destroy its task/
  state before resuming a result branch. This exactly supports “spin up,
  investigate, report, spin down” once cleanup has
  completed; it is not an immediate-kill promise. It cannot itself create the
  event that started that same automation. A rule whose trigger is
  `appeared`/`disappeared` needs observation *before* the trigger fires, so it
  requires an explicit armed scan window (for example a schedule/session), not
  the fiction that it scans only after it has triggered. A small subscription
  controller reference-counts overlapping armed windows, starts no radio worker
  when the count is zero, coalesces or skips missed cadences rather than queueing
  scans, and issues cancellation when the last window closes. An explicit user
  `Scan`/`Refresh` action may request one snapshot for a picker; ordinary
  redraws never do. Scan cadence, minimum RSSI, hysteresis, disappearance
  timeout, cancellation deadline, and maximum run time belong to the request/
  subscription—not to the static catalog.
- **Automation continuation, not a blocking condition.** Today
  `evaluateCondition()` returns `bool` synchronously and is called directly by
  the custom main-loop scheduler and manual/conditional paths
  (`components/hardwareone/System_Automation.cpp:2000-2018,2806-2880,
  3697-3718,4262-4282`;
  `components/hardwareone/HardwareOne.cpp:2627-2652`). A seconds-long RF call
  there would stall unrelated firmware work. The first on-demand design is
  therefore an asynchronous branching action, not a new synchronous condition.
  Refactor the manual/scheduled command-list loop behind a small resumable step
  executor whose result is `Continue`, `Pending`, or terminal failure. Add a
  bounded `AutomationRunContinuation` owned by that executor: unique run id,
  automation id plus a configuration generation, next command/branch cursor,
  original clock/event fire context, matcher/request, authorized identity,
  deadline/cancel generation, and explicit match/no-match/error next steps. Do
  not retain pointers into the parsed JSON. On resume, reload by id, verify the
  unchanged generation/enabled state, and reconstruct the remaining commands
  from the cursor; a successful create/edit/enable/disable/delete/reload bumps
  the generation under the same run-registry synchronization. A conservative
  global generation that cancels unrelated pending runs is acceptable for v1;
  an untracked mutation path is not.

  When a scan action returns `Pending`, atomically publish the continuation,
  stop the command loop before `ci + 1`, and return to the main loop. The
  current tail at `components/hardwareone/System_Automation.cpp:4299-4339`
  must **not** increment
  `executed`, post `SYSEVT_AUTOMATION_FIRED`, append `AUTO_END`, or call
  `rescheduleAfterFire()` while that run is pending. The due scheduler also
  recognizes the pending registry so the same overdue clock occurrence cannot
  start again. Define an explicit bounded policy for a second event/manual fire
  of the same automation (v1 may return/coalesce `BUSY`); it may not overwrite
  the live continuation.

  After matching, the scan owner first completes cleanup/join/radio release,
  then posts one typed result to the automation executor queue. That queue
  atomically claims the same live run/generation, selects the match/no-match/
  error branch, and resumes at the captured cursor exactly once. All exits use
  one compare-and-complete finalizer. A completed resumed run performs the old
  `executed`/`AUTOMATION_FIRED`/`AUTO_END`/clock-reschedule tail once. A timeout,
  queue/allocation failure, or explicit cancel with no handled error branch
  records one failure/cancel terminal and advances an old clock occurrence at
  most once so it cannot busy-loop; edit/disable/delete cancellation never
  writes an obsolete reschedule over the new/disabled configuration. Reboot
  drops in-memory runs and follows the existing persisted-due replay policy.
  Disable, edit/delete, timeout, reboot, or manual cancellation invalidates the
  run and requests worker stop; late results fail the claim and produce no
  commands, proximity events, duplicate terminal log, or reschedule. A later
  condition syntax may reuse this continuation and a `Pending` result, but must
  never make the main-loop call block.
- **Cancellable WiFi scan coordinator.** The current `wifiScanForEach()` is a
  useful record/cleanup seam but is not yet the promised lifecycle: it calls
  `WiFi.enableSTA(true)`, then uses blocking
  `esp_wifi_scan_start(..., true)` and exposes neither a stop token nor prior
  radio-mode restoration (`components/hardwareone/System_WiFi.cpp:260-343`).
  Refactor it behind one coordinator-owned request state machine. Start the
  driver scan nonblocking, wait for scan-done, cancellation, or the request's
  hard deadline, and let only the coordinator owner call
  `esp_wifi_scan_stop()`, clear any owned AP list, and complete exactly once as
  `OK`, `CANCELLED`, `TIMEOUT`, or `DRIVER_ERROR`. A UI/automation task only
  advances a cancellation generation and wakes the owner; it never frees the
  request or driver list. Cancelled/timed-out scans publish no snapshot. The
  last subscriber waits only up to the documented stop/join bound, after which
  an honest teardown error is reported rather than pretending the worker is
  gone. Preserve the existing ESP-NOW channel-change notification and never
  expose a partially consumed driver list.
- **Generation-checked WiFi radio lease.** Under the WiFi/ESP-NOW mode
  coordinator, capture the exact prior radio mode, whether STA was already on,
  connection/shared-owner state, and a radio-mutation generation. Mark
  `enabledStaByThisLease` only when this request alone turns STA on. On release,
  restore the exact prior mode only if that flag is true, the generation still
  proves no intervening WiFi/ESP-NOW/user mutation or new owner, and no shared
  claimant now needs STA. Already-on, connected, AP+STA, and ESP-NOW-owned
  states are left intact. If ownership changed, relinquish the lease without
  restoring stale state. Every mode/channel owner that can race this path must
  publish through the same generation/ownership contract. This closes the
  current gap where the first nominally one-shot scan can leave STA/radio on.
- **BLE central-operation arbitration.** G2 and Ring work share the
  process-global `BLEScan` callback and serialize connection/scan activity. A
  generic BLE snapshot worker must join that arbitration or use one fan-out
  broker; it must not independently replace advertised-device callbacks
  (`components/hardwareone/G2_Glasses.cpp:3435-3438` and
  `components/hardwareone/G2_Ring.cpp:5029-5069`). It also must respect the
  firmware's phone-server versus G2/R1-client role boundary: a scan request may
  not silently call `openble`, persist another role, disconnect a phone, G2, or
  R1 session, or suppress reconnect intent. Define and test a broker capability
  matrix. Idle client mode may scan through the broker. A connected phone-server
  or active G2/R1 session permits a snapshot only when that exact concurrent GAP
  scan/session combination and callback restoration are proven on hardware;
  otherwise return explicit `BUSY_SESSION`/`UNAVAILABLE_ROLE` and leave the
  session untouched. Any future opt-in temporary role/session lease is a
  separate feature with capture, quiesce, bounded cancellation, and exact
  restore semantics—not the default one-shot behavior.
- **Row identity is not match semantics.** A WiFi snapshot row can use BSSID to
  distinguish access points, but the automation must explicitly choose SSID
  matching versus a specific BSSID. For BLE, use a resolved identity address
  where the stack can prove one; otherwise address plus address type is only an
  observed-row key with rotation and spoofing limitations, not durable identity.
  Never silently substitute display name, SSID, or address for the requested
  match mode.
- **Automation configuration must be structured.** A static kind such as
  `wifi_network_appeared` is only the transition type; it cannot encode which
  network, address mode, minimum RSSI, or timeout. A future `ScanMatcher`
  record owns medium, explicit match mode/value, thresholds, hysteresis, and an
  opaque matcher id. An on-demand automation action returns a typed status/
  result only to its claimed run continuation. A pre-trigger watcher posts the
  generic transition with only the minimum opaque/redacted correlation needed for the
  automation engine to resolve authorized configuration—never a raw SSID/MAC
  dump in ring history. Separate observation sharing from policy-state sharing:
  all compatible subscriptions may consume one complete radio snapshot, but
  several rules use one transition state machine only when their **full
  normalized transition-policy key** is identical—medium, match mode/value,
  RSSI threshold, appearance and disappearance hysteresis, absence timeout,
  evaluation cadence/window, and every other field that can change an edge.
  The same SSID/BSSID/address under different thresholds or timing owns
  independent state even though it reuses the same observations.
- **Separate security boundary.** Static event kinds may be broadly
  discoverable, while nearby SSIDs, BSSIDs, BLE addresses, RSSI, and freshness
  are environmental data. A future scan-result endpoint needs separate
  authentication/authorization, redaction, privacy, and connection-scoped
  caching rules; it does not inherit `/api/events/kinds` visibility merely by
  presenting rows to the same interfaces. A target intentionally selected for
  matching persists only inside that authorized automation's structured
  settings and follows the settings backup/export access rules; ephemeral
  neighboring rows, raw advertisements, and unmatched identities are not
  persisted. Logs, command results, and event history use an opaque matcher id
  or redacted label unless an authenticated caller explicitly requests the
  protected configuration.
- **State transitions require trustworthy snapshots.** The first complete,
  successful, untruncated snapshot seeds matcher state without an `appeared`
  burst; unchanged observations emit nothing. Failed, stale, timed-out,
  cancelled, or truncated snapshots do not advance appearance/disappearance
  hysteresis or establish absence. `disappeared` is emitted once only after the
  configured number/time of later complete successful snapshots establishes
  absence; a new valid sighting is required before another disappearance edge.
  A one-shot query still reports its explicit failure/truncation status to its
  caller instead of turning it into “not present.”
- **Post only coalesced matcher edges under a ring budget.** Do not create one
  catalog kind or 48-entry-ring occurrence per advertisement, AP row, or
  subscribing automation. Coalesce an edge only by the full transition-policy
  key above, never merely by target identity/mode; subscribers with the exact
  same key consume one edge, while differing thresholds/cadences retain their
  own correct edges. Enforce a configuration-time cap on active unique policy
  keys plus a hard per-cycle post budget below ring capacity with reserved
  headroom for unrelated events. A bounded pending-edge strategy must define
  overload explicitly; it may defer edges but must not silently post an
  unbounded burst or duplicate an edge per identical subscriber. This is
  required because the ring is only 48 entries
  (`components/hardwareone/System_Events.h:321-363`) and an occurrence is stored
  before automation matching (`components/hardwareone/System_Events.cpp:265-273`),
  so “one post per subscribing automation” would already consume history before
  matching could coalesce it. Names and addresses remain spoofable and cannot
  authorize security-sensitive actions by themselves.

This scanner is a follow-on project, not hidden work in Phases 0–4. Its minimum
firmware surface is `System_WiFi.h/.cpp` for the cancellable coordinator/radio
lease, `System_Automation.h/.cpp` for run continuations and the async branch
action, the BLE/G2/R1 ownership layer for one callback/role broker, structured
automation-settings persistence for `ScanMatcher`, and dedicated host/device
tests. `System_EventCatalog` would gain only reviewed generic transition kind
rows; it would not own snapshots, radios, run jobs, matcher state, or nearby
identifiers.

Before the scan-result provider or proximity triggers ship, add acceptance
tests for startup baseline, repeated advertisements, unchanged snapshots,
failure/timeout/cancellation, cancel while a driver scan is active, bounded
last-subscriber stop/join latency, bounded-buffer truncation, matcher
coalescing, same-target rules with different RSSI/hysteresis/cadence policies,
RSSI and disappearance hysteresis, disable/last-subscriber teardown, explicit
one-shot refresh, no overlapping or queued missed scans, and serialized BLE
callback ownership. Prove the main loop remains responsive while an on-demand
action is pending. Put the action between two harmless commands and prove the
later command, `executed` increment, `AUTOMATION_FIRED`, `AUTO_END`, and
reschedule are all absent before its result. Match/no-match/error each resumes
the intended branch/cursor only after cleanup and reaches the single terminal
tail at most once. Source/host checks require manual and scheduled loops to
short-circuit on `Pending`, require every authoritative edit/enable/disable/
delete/reload path to bump the run generation, and require the scheduler to
suppress a duplicate overdue occurrence. Disable/edit/delete, timeout,
queue-full, cancellation, and a late completion cannot resume stale commands,
emit a duplicate terminal, or overwrite new scheduling state. Exercise WiFi
off→one-shot→off, already-connected state preservation, AP+STA, ESP-NOW/shared
ownership, and an intervening mode mutation that invalidates restoration.
Exercise BLE while phone-server connected, G2
connected, R1 connected, and idle-client, including cancellation and exact
callback/session restoration or the documented explicit refusal. Instrument
radio-start counts to prove ordinary UI redraws cause zero scans, and drive the
maximum configured simultaneous transitions to prove one scan window cannot
overflow the 48-entry ring or duplicate an edge for multiple subscribing
automations.

---

## 10. Implementation work packages

Each package has a clean rollback boundary. Do not combine the future ESP32 G2
page-adapter or companion-app authoring experiences with the core extraction.
Every phase that changes first-party source also owns the corresponding
`docs2/docsctl.py update --changed` queue, per-file/subsystem edits, review,
acceptance, browser rebuild, and final `status`/`check` **before that phase can
ship**. Documentation is not deferred across the Phase 1–3 release boundary or
held for optional Phase 4 work.

### Phase 0 — establish regression baselines and repair OLED mask safety

1. Create `docs/testing/SYSTEM_EVENT_CATALOG_DEVICE_CHECKLIST.md` and a
   compile-time-gated device harness for internal-only, non-production fault
   scenarios. The gate is absent from every normal profile; production link-map
   checks reject its symbols. Populate Phase 0 rows now and require each later
   phase to add/update its own rows before that phase ships.
2. Record the ordered family/name fixture from current source with a read-only
   source parser; the current macro still sits behind `Arduino.h`, so the real
   dependency-light C++ target starts in Phase 1.
3. Extract the production array-size-aware bit operations into
   `System_EventKindMask.h` and add dependency-free host tests for IDs 127,
   128, and the current fixture value 152. In Phase 1, bind the last case to
   `SYSEVT_COUNT - 1` through the new dependency-light catalog header.
4. Repair the OLED mask width and verify a low-kind toggle retains high-kind
   settings.
5. Add `set`/`patch` plus `all`/`none` mutations to `notifyusermute` and
   `notifyusershow`; put production parse/validate/mutate semantics in
   `System_NotificationKindListCore.h`, then move OLED/Dashboard away from
   generated full-list commands and update registered help text.
6. Add a user-settings transaction helper whose outer filesystem guard spans
   load→callback mutation→save; rework generic merge and notification-list
   mutation over it, migrate existing-user password/gamepad-password RMW
   writers (hash outside the lock), and verify concurrent disjoint web/command/
   credential patches cannot overwrite each other. Fence notification cache
   miss publication with a cache generation so a flash load begun before a
   commit cannot republish after that commit invalidates the cache; invalidate
   on failed/ambiguous save outcomes as well as success. Pre-measure serialized
   settings and require exact serialized byte counts plus the observed
   post-flush file size before rename/success; test nonzero short writes in both
   temp and direct paths. Record that the current `File` API exposes no checked
   flush/fsync result, so this gate detects observable short output but does not
   assert media durability.
7. Extract the dependency-light executor input limit/predicate and reject
   oversized sync and async submissions instead of truncating them; make UART
   consume the same limit without weakening its whole-line discard rule.
8. Audit every sync/async submitter for an intentional input above the named
   limit and update each transport/UI to surface the explicit rejection.
9. Test the worst-case all-kinds state and confirm no submitted individual
   command exceeds `ExecReq::line`.
10. Confirm the existing web and command payloads contain the same 152 names in
   the same order.

**Rollback:** keep the mask repair, transactional bounded notification
mutations, command input rejection, and their tests even if later phases are
reverted.

### Phase 1 — extract the shared typed options provider

1. Add private `System_EventCatalogRows.h`, public
   `System_EventCatalog.h`, `System_EventCatalog.cpp`, and the unconditional
   CMake entry.
2. Add `System_EventCatalogCore.h`, kept C++17-compatible for the host suite.
3. Move row declarations into the intentionally repeat-included private row
   header; generate public enums in the dependency-light API header and leave
   no public list macro.
4. Add the real C++ host target against that production header/core before
   moving the lookup implementations.
5. Move tables and lookup implementations; make `System_Events.h` include the
   catalog header.
6. Add indexed family/global iteration and the generated family-order index.
7. Convert `systemEventKindFromName()` into the compatibility wrapper over
   `systemEventCatalogFindKind()`; keep the other legacy fallback behavior.
8. Prove public provider/core headers contain no JSON, renderer, protocol, or
   transport declarations.
9. Add and run the §12 restoring build-profile script in explicit
   `--provider-only` mode; at this phase it checks only
   `System_EventCatalog.cpp.obj`/provider ownership because the JSON adapter
   does not exist yet.
10. Add Phase 1 provider memory/link evidence rows to the device checklist;
    do not mark later HTTP/OLED/G2 rows from provider-only results.

**Acceptance:** zero wire/UI behavior change; all existing includes build.

**Rollback:** remove the new module and restore moved blocks; no persisted data
or schema migration is involved.

#### Phase 1 implementation checkpoint — 2026-08-25

The provider extraction and compatibility integration are implemented. The
private rows, dependency-light public API/core, compact family-order index,
legacy lookup definitions, `System_Events.h` compatibility include, and
unconditional component source entry are present. The sanitizer-backed host
suite passes 15/15, including production-provider traversal, frozen 12-family /
152-kind order, hostile core fixtures, allocation checks, concurrent reads,
and source-boundary checks. The web UI regression suite remains 26/26.

The ordinary `xiao_s3` firmware build and its generated provider object have
been inspected. In that object, `.data` and `.bss` are both zero. The
immutable production tables/index are:

- family-label pointers: 48 bytes;
- canonical-kind pointers: 608 bytes;
- kind-family bytes: 152 bytes; and
- generated family-order index: 177 bytes.

The same-toolchain pre-extraction tables were 48 + 612 + 153 bytes. Removing
the `none` pointer/family slots and adding the typed index therefore adds 172
bytes of immutable metadata when the index is linked. Current Phase 1 callers
still use the compatibility accessors; section garbage collection drops typed
traversal functions that are not referenced by that image.

A prior authenticated live-browser smoke against a running XIAO verified 12
families, 152 unique event rows, the expected tail through
`automation_action_dropped`, and cancel/reopen preservation without saving or
mutating automation settings. That is useful compatibility evidence, but it is
not the formal disposable-admin, fault-harness, or five-profile build-matrix
acceptance run.

All five `tools/build_event_catalog_coverage.sh --provider-only` profiles pass:
ordinary FeatherS3, FeatherS3 with G2 enabled and Automation disabled,
ordinary XIAO-S3, ordinary classic ESP32, and XIAO-S3 with optional consumers
disabled. Each row started with a full clean build and verified its resolved
gates, provider compile/link ownership, map/object/application artifacts, and
the absence of a Phase 2 JSON adapter. The script's exit cleanup rebuilt the
ordinary FeatherS3 image; its first ordinary XIAO-S3 cleanup configure failed
before CMake selected a C++ compiler, after all five matrix rows had passed. A
subsequent clean ordinary XIAO-S3 rebuild passed, and the original
`System_BuildConfig.h` bytes, mode, and modification time were restored. Phase
1 deliberately made no JSON, OLED, or G2 behavior change; those migrations were
assigned to Phases 2 and 3, whose later checkpoints record their current state.

### Phase 2 — add the JSON output adapter and migrate transports

1. Add `System_EventCatalogJson.h/.cpp` and the provider-shaped internal
   `System_EventCatalogJsonCore.h` test seam.
2. Implement sink, size, and bounded-buffer JSON paths by walking the typed
   provider; do not read the private row file or tables directly.
3. Add JSON escaping, exact-size preflight, sink-failure, and typed↔JSON parity
   tests.
4. Replace the CLI JSON construction.
5. Replace the HTTP JSON construction with a chunk sink.
6. Replace human CLI enumeration with the checked provider-shaped text adapter;
   preflight every complete header/token, flush-and-retry only whole tokens, and
   return an explicit error with zero callbacks if one cannot fit the named
   `broadcastOutput()` payload.
7. Delete obsolete ArduinoJson catalog-building code and stale transport
   comments.
8. Run the full default §12 build-profile script, now requiring both provider
   and JSON-adapter objects/symbol ownership in every profile.
9. Run the hash-pinned offline Secure Channel client vectors, then populate and
   run Phase 2 checklist rows for the generic physical Secure BLE client,
   command boundary, and deterministic fail-after-N HTTP sink wrapper.
10. Publish the v1 `events kinds json` shape and ordering, 4,096-byte command
    result-buffer budget (payload plus trailing NUL), phone-companion Secure
    Channel/reassembly requirement, and
    unsupported full-payload plaintext notification behavior in
    `docs/APP_JSON_CONTRACT.md`, linking the existing Secure Channel framing
    contract. The generic Phase 2 client consumes this contract; Android does
    not own or delay it.

**Acceptance:** the `families` tree is byte-for-byte equivalent to the current
v1 fixture, and overflow cannot produce partial valid-looking JSON.

**Rollback:** adapters can temporarily return to their old loops while the
typed provider remains.

#### Phase 2 implementation checkpoint — 2026-08-25

The transport-neutral JSON contract/core/production adapter and the checked
human-text core are implemented. `System_EventCatalogJson.cpp` adapts only the
public typed provider into the tested provider-shaped JSON core; it does not
include the private rows or provider validation/index core. The adapter exposes
exact size, synchronous sink, and bounded-buffer paths with explicit status and
no heap, global output cache, or lock.

`events kinds json` now uses the shared bounded-buffer adapter with
`CMD_RESULT_MAX = 4096`; the current compact v1 payload is exactly 2,877 UTF-8
bytes and requires 2,878 bytes including its trailing NUL, leaving 1,218 usable
payload bytes of headroom. Human `events kinds` uses the checked text core with
`DEBUG_MSG_SIZE - 1`, complete preflight, whole-token flush/retry, and explicit
failure instead of the former unchecked 120-byte `snprintf()` loop. The
guest-readable `/api/events/kinds` handler (subject to the normal web
authentication policy) preflights the same serializer and streams through
`httpd_resp_send_chunk()`; after an attempted sink failure it returns failure
without appending an error tail or success terminator.

The host suite passes 18/18, with sanitizers enabled on native targets. The real
adapter tests cover escaping, exact size/buffer precedence, sink failure and
current command budget; an independent Python process compares production
`--dump-json` output with typed traversal and the reviewed 12-family/152-kind
fixture byte-for-byte.
The text tests cover exact fit, a token wider than the old 120-byte staging
buffer, sink failure, and zero-callback over-limit preflight. Structure guards
enforce core include allowlists, single public serializer ownership, typed
provider delegation, CLI/HTTP delegation, and removal of obsolete catalog
ArduinoJson builders.

The full default Phase 2 coverage script passes all five profiles with both the
provider and JSON adapter independently owned in every archive: ordinary
FeatherS3, G2-without-Automation FeatherS3, ordinary XIAO-S3, ordinary classic
ESP32, and XIAO-S3 with optional consumers disabled. Its EXIT recovery then
passes ordinary FeatherS3 and XIAO-S3 rebuilds and restores
`System_BuildConfig.h` bytes, mode, and nanosecond mtime exactly. The pinned
offline Secure Channel/client lane also passes 24 protocol tests and validates
the 12-family/152-kind/2,877-byte payload. The default-off device harness,
physical Secure-BLE client/reassembly run, UART/HTTP comparison, deterministic
live HTTP fail-after-N sink case, and physical plaintext-client refusal check
remain `PENDING` in the device checklist. Phase 2 therefore has complete
automated implementation/build evidence, not completed physical/release
acceptance.

### Phase 3 — migrate existing local typed consumers

1. Refactor the OLED automation picker to direct indexed access.
2. Refactor the OLED notification picker to direct indexed access.
3. Delete both 24-entry caches and build functions.
4. Exercise the last row of every production family. Exercise a synthetic
   non-contiguous family above 32 through the production
   `System_EventCatalogCore.h` template used by the real index.
5. Confirm the current ESP32-side G2 history remains on the compatibility name
   accessor and the automation summary remains a catalog-neutral presentation
   of its persisted trigger string. No catalog JSON or new G2 protocol frame is
   introduced.
6. Populate and run Phase 3 OLED navigation, high-kind, invalid-ordinal, and
   persistence rows through the physical device/harness combination.

**Implementation checkpoint (2026-08-26):** steps 1–5 are implemented in the
OLED adapters and protected by the 18/18 host suite. The source guards require
both consumers to call the three indexed provider entry points, reject the old
24-entry arrays/builders and scalar name/family scans, and require the
automation selection to remain a pair of provider ordinals. Production provider
tests still walk every row of every family and the synthetic 35-kind,
non-contiguous family. No catalog JSON or new G2 picker/protocol frame was added
for Phase 3.

Step 6 remains open. The authenticated browser acceptance already recorded in
the device checklist proves the web projection, not OLED behavior and not the
formal raw HTTP row. The ordinary FeatherS3 build made after the Phase 3 edits
resolved `DISPLAY_TYPE=0`, so it excluded the active pickers. A subsequent
temporary OLED-enabled compile-coverage profile built both changed bodies with
zero compile errors and verified their native-provider archive references.
The planned invalid-selection harness seam and every real-display navigation,
high-kind, and persistence procedure are still required before physical Phase
3 acceptance.

**Rollback:** each OLED consumer can be reverted independently; the typed
provider and JSON output adapter remain useful.

### Phase 4 — add remote and local authoring adapters as separate features

1. Android: add the lazy Secure-Channel catalog repository alongside the
   future automation builder.
2. G2: extend `G2_Page_Automations.cpp` as the ESP32-side family-first picker
   alongside the future create/edit flow. Resolve typed provider rows locally
   through the G2-specific provider-shaped `G2_EventPickerCore.h`, with
   0/1/12/13/25-kind synthetic host coverage, and reuse the existing G2
   paginator and lifecycle/presentation fences. Before implementation, run the
   real-glasses feasibility gate with two non-default container names across
   every supported firmware and require CREATE/ACK/render plus byte-exact
   ListEvent echo; stop Phase 4 if it fails. Then
   extend the list page-swap API with an explicit `ForceRecreate` policy plus a
   caller page cookie and staged admitted/transition-started/terminal callbacks,
   with committed success carrying presentation/lifecycle (or independently
   disable/prove safe the global fast path). Maintain separate active and
   pending mappings: pre-transition abort resumes the still-current active
   mapping, while a failure after transition start clears both and recovers.
   Give each candidate a unique 18-character `ev<lifecycle><page-cookie>`
   container name, pass it to the existing arbitrary-container-name CREATE
   path, and commit it with the page mapping. Preserve the legacy `"app"`
   default for unrelated pages; extend ListEvent routing only for the exact
   reserved `ev[0-9a-f]{16}` form and hard-drop that form when no fresh picker
   preclaim owns it. Extend cross-arm dedup, ListEvent enqueue,
   `TapDispatchEntry` (and its queue-size assertions), worker validation, and a
   trailing optional context-aware page callback so original RX-captured side,
   per-temple connection generation, presentation, lifecycle identity, and the
   exact echoed `List_ItemEvent.ContainerName` reach the dispatcher while
   legacy page handlers remain source-compatible. Validate the echoed token,
   original side/generation, and global lifecycle/presentation together with
   dedup read/write in one ordered freshness claim by invoking the bounded
   adapter preclaim under G2→adapter lock order; keep `activePresentation` as
   the single expected-token owner rather than mirroring it in the broker.
   Revalidate all four stamps plus the token in the worker through the same
   preclaim/lock path without touching dedup, hard-drop an unclaimed reserved
   token, and revalidate all four stamps plus the token again in the final
   adapter/page-cookie mutation claim;
   never re-stamp the tap or freeze both arm generations in page state. Every
   event-picker content/page change
   must bypass same-count `REBUILD-list` and use
   SHUTDOWN/500-ms-settle/CREATE. Publish row mapping only from the exact
   committed completion; suspend it at broker admission before the worker can
   observe the job. For an admitted swap, publish its terminal state while its
   page-swap admission fence still excludes taps, release that fence only
   afterward, and destroy job storage last. For idle revocation with no swap
   owner, advance lifecycle/presentation identity first and then clear adapter
   state without touching a nonexistent fence/job. Route ownership through the
   shared legacy/preclaim/context handler predicate and revoke even an idle committed
   presentation on an authoritative/full-session/topology disconnect, root
   recovery, or lifecycle replacement. A non-authoritative temple disconnect
   advances only that arm's connection generation and preserves the global
   presentation.
   Project only visible rows through `G2_Glasses` /
   `System_G2_Protocol` to the external glasses. This reuses ordinary G2
   ListObject/ListEvent metadata; it adds no catalog JSON, new characteristic,
   protobuf field, or glasses firmware, but shipping is gated on real supported
   glasses echoing the exact container name without truncation.
3. CM5, if needed: add an authenticated request/reply client using the same
   command.
4. If plaintext BLE becomes required, design and version pagination as its own
   protocol change.
5. Populate and run the separate Phase 4 Android and real-glasses checklist
   rows, including the read-only synthetic 25-kind/15-row lens page, staged swap
   failures, stamped-context races, and the raw post-commit wire-token race.

**Acceptance:** no interface stores a second authoritative catalog, no
interface silently substitutes the first kind when a persisted value is
unknown, and the physical G2 glasses receive only ordinary G2 protocol
render/input traffic—not event-catalog JSON.

### Phase 5 — documentation and cleanup

1. Remove obsolete comments describing web/CLI copies.
2. Re-run dead-code/reference searches for the deleted arrays/builders.
3. Record measured memory and payload sizes. The app JSON contract was already
   published and reviewed with its Phase 2 transport behavior; Phase 5 only
   corrects it if final measurements differ.
4. Run a final documentation audit for outstanding queue/baseline debt. Any
   cleanup source change still performs its own per-phase docs2 update, review,
   acceptance, browser rebuild, and check; Phase 5 is not a delayed substitute
   for the gates above.

---

## 11. File-by-file implementation map

| File | Planned change |
|---|---|
| `components/hardwareone/System_EventCatalogRows.h` | New private intentionally repeat-included owner of every family/kind row; no include guard by design, with direct inclusion restricted to the catalog module/tests |
| `components/hardwareone/System_EventCatalog.h` | New dependency-light generated enums, token-cap constant, typed family/kind records, indexed provider API, and compatibility lookup declarations; no public list macro or JSON API |
| `components/hardwareone/System_EventCatalogCore.h` | New C++17-compatible internal validation/index/lookup templates shared by production and synthetic host fixtures; no JSON grammar |
| `components/hardwareone/System_EventCatalog.cpp` | New immutable tables/indexes, typed provider, and lookups |
| `components/hardwareone/System_EventCatalogJson.h` | New transport-neutral JSON sink/status/size/buffer adapter contract |
| `components/hardwareone/System_EventCatalogJsonCore.h` | New C++17-compatible provider-shaped JSON emitter/validation seam for hostile synthetic fixtures |
| `components/hardwareone/System_EventCatalogJson.cpp` | New JSON escaping/serialization output adapter over the typed provider |
| `components/hardwareone/System_EventCatalogTextCore.h` | New dependency-light provider-shaped human-command adapter with full preflight, a Boolean whole-line sink, checked packing, retry-after-flush semantics, and zero-callback too-long-header/token failure |
| `components/hardwareone/System_EventKindMask.h` | New dependency-light, array-size-aware bit test/set/toggle helpers |
| `components/hardwareone/System_NotificationKindListCore.h` | New dependency-light, multipass bounded parser/mutator over pointer/length visitation for replacement, set, patch, all, none, canonicalization, valid unknown-token preservation, strict canonical patch duplicate detection, and repair-vs-preserve semantics without per-token matrices |
| `components/hardwareone/System_Events.h` | Include catalog; retain attribution and ring declarations |
| `components/hardwareone/System_Events.cpp` | Remove moved tables/lookups; adapt JSON `events kinds` output and replace the unsafe 120-byte human loop with the checked text adapter |
| `components/hardwareone/System_Automation.cpp` | Name the eight-word event mask capacity and assert it covers the catalog |
| `components/hardwareone/System_Notifications.h` | Retain notification mask word-count/assertion and `NotifViewer` storage ownership |
| `components/hardwareone/System_Notifications.cpp` | Replace local `maskTest`/`maskSet` with the shared helper (including volatile device masks); add `set`, validated delta `patch`, and whole-list `all`/`none` mutations for both personal kind lists; add a cache-miss generation fence so an old flash load cannot republish after invalidation, plus only a device-test-guarded pre-publish pause hook |
| `components/hardwareone/System_UserSettings.h` | Add the guarded load→mutate→save transaction callback/status contract used by generic merge and notification-list edits |
| `components/hardwareone/System_Settings.cpp` | Include `System_UserSettings.h` so declarations/definitions are compiler-checked; implement the outer-FS-lock user-settings transaction; rework `mergeAndSaveUserSettings()` over it; pre-measure and require exact temp/direct serialized counts plus observed file sizes before rename/success without claiming unavailable flush-error telemetry; invalidate preference cache on every failed save path; provide only device-test-guarded pause/zero-or-short-write and disposable destination-touched fallback-failure hooks; and update registered `notifyusermute` / `notifyusershow` usage text for the complete grammar |
| `components/hardwareone/System_User.cpp` | Move existing-user password and gamepad-password load→patch→save writers to the transaction helper while deriving expensive hashes before the lock; retain direct full-file saves only for new-user initialization |
| `components/hardwareone/System_CommandLimits.h` | New dependency-light `CMD_INPUT_MAX`, moved `CMD_RESULT_MAX`, and input-length predicate shared by production, page generation, and host tests |
| `components/hardwareone/System_CommandTypes.h` | Include shared limits, remove its private result-limit definition, derive executor line/output arrays from the named constants, and document inbound/outbound limits separately |
| `components/hardwareone/System_Utils.cpp` | Reject overlength sync input before direct/queued execution and async input before queueing; only under `HW1_EVENT_CATALOG_DEVICE_TEST`, include the harness table and register its otherwise-absent `eventcatalogtest` command module |
| `components/hardwareone/System_UartLink.cpp` | Replace private `2047` with `CMD_INPUT_MAX` while preserving whole-line discard behavior |
| `components/hardwareone/CMakeLists.txt` | Build the typed provider and JSON output adapter unconditionally; add default-OFF `HW1_EVENT_CATALOG_DEVICE_TEST` cache option that alone compiles the isolated device harness and its component-wide test definition |
| `components/hardwareone/WebServer_Server.cpp` | Replace private JSON builder/static buffer with serializer sink; in device-test builds only, wrap it with a one-shot, case-ID-keyed fail-after-N-accepted-chunks hook that aborts/closes the response |
| `components/hardwareone/WebServer_Utils.cpp` | No ownership move; only adjust validation if the wire schema is later versioned |
| `components/hardwareone/WebPage_Dashboard.h` | Replace generated full-list mute commands with bounded mutations and place the editor logic in one test-extractable raw-string block |
| `components/hardwareone/OLED_Mode_Automations.cpp` | Delete the 24-entry cache and fixed 40-byte chosen-kind copy; retain semantic ordinals, re-resolve at confirmation/submission, persist the full canonical token, and expose stale/OOB ordinal injection only to the compiled device harness |
| `components/hardwareone/OLED_Utils.cpp` | Delete 24-entry notification cache; use safe 256-bit handling and one-kind commands; expose notification cursor/OOB injection only to the compiled device harness |
| `components/hardwareone/G2_EventPickerCore.h` | Phase 4 dependency-light G2-specific row-plan/binding core over a provider-shaped count/resolve view; production injects the typed catalog and tests inject boundary counts, without becoming a universal OLED/browser paginator |
| `components/hardwareone/G2_Page_Automations.h/.cpp` | No Phase 1–3 picker; future typed-provider-backed ESP32 authoring adapter wraps `G2_EventPickerCore`, owns family/kind/page/submode state and canonical selection, keeps the single authoritative complete active/pending presentations with bounded visible-row bindings, unique bounded `wireContainerName`, and explicit swap phase, exposes a bounded pre-dedup token/epoch validator under the declared G2→adapter lock order, and in its final mutation claim atomically revalidates original arm generation plus global lifecycle/presentation/echoed token/page cookie against an idle committed page |
| `components/hardwareone/G2_Glasses.h/.cpp` | No Phase 1–3 behavior change; Phase 4 adds/tests force-recreate list swaps with caller-supplied container names; preserves the protobuf container-name length and adds an exact 18-byte lowercase reserved-token ListEvent route beside the unchanged legacy `"app"` route, with no generic arbitrary-container dispatch and hard stale drop when unclaimed; veto-capable admitted/transition-started handshakes and pre-transition-aborted/committed/post-transition-failed terminals keyed by page cookie; end-to-end original RX side/per-temple-generation/lifecycle/presentation identity and exact echoed container token through one ordered freshness-plus-adapter-preclaim-plus-cross-arm-dedup operation, ListEvent enqueue, `TapDispatchEntry`, a worker-time no-dedup freshness/preclaim revalidation, and new trailing optional preclaim/context-aware `G2PageModule` callbacks while retaining legacy `handleTap`; fixed global/topology/freshness/adapter lock ordering and no reverse acquisition; one shared legacy/preclaim/context handler predicate; an optional idempotent page-invalidating presentation-revocation callback that is not fired for a non-authoritative one-arm disconnect; admitted-swap terminal publication before its admission-fence release/job destruction plus epoch-first idle revocation with no fence/job; updated tap-entry queue-storage size assertions; and one-shot swap-stage/idle-revoke/nonblocking-raw-RX-defer/dispatcher-pause/allocation hooks only in the compiled device harness, while continuing to transport rendered rows/taps over the G2 BLE-Central session and use compatibility lookup for history |
| `components/hardwareone/System_G2_Protocol.h/.cpp` | No production code change if the existing builder fixture confirms byte-exact arbitrary `containerName` encoding; retain the catalog-agnostic schema/codec rather than adding JSON/catalog transport (ListEvent length/echo parsing is tested in `G2_Glasses`) |
| `components/hardwareone/test/host/fixtures/event_catalog_v1.json` | Reviewed golden family/name order and compact v1 wire payload captured before extraction |
| `components/hardwareone/test/host/test_event_catalog.cpp` | New production typed-provider/index/lookup/invariant tests |
| `components/hardwareone/test/host/test_event_catalog_json.cpp` | New production JSON-adapter escaping, exact bytes, sizing, sink-failure tests, plus test-only typed/JSON dump modes |
| `components/hardwareone/test/host/test_event_catalog_text.cpp` | Exercise the production human-text adapter with exact-fit packing, a synthetic canonical token longer than the old 120-byte staging width, sink failure, and explicit over-transport-limit preflight rejection with zero callbacks and no truncation |
| `components/hardwareone/test/host/test_event_catalog_json_parse.py` | Invoke the production host binary dump modes, parse JSON with Python stdlib, and compare every record to typed traversal |
| `components/hardwareone/test/host/test_event_catalog_structure.py` | Enforce provider/JSON module boundaries, banned allocation and lock/critical-section APIs, immutable table qualifiers, single private production row owner/direct-include allowlist, and production JSON-core delegation |
| `components/hardwareone/test/host/test_event_kind_mask.cpp` | Exercise the production mask helper at word boundaries and the final kind under sanitizers |
| `components/hardwareone/test/host/test_notification_kind_list.cpp` | Exercise the production multipass mutation core, including validate-before-output, strict canonical patch duplicates/contradictions, repair-vs-preserve semantics, 256-row scratch bounds, aliases, long known tokens, and unknown-token preservation |
| `components/hardwareone/test/host/test_command_limits.cpp` | Exercise the production input predicate at 2,047/2,048-byte boundaries and expose the real 4,096-byte result-buffer budget, including trailing NUL, to catalog JSON tests |
| `components/hardwareone/test/host/test_g2_event_picker_core.cpp` | Phase 4 synthetic 0/1/12/13/25-kind page, chrome, shrink, row-binding, exact 18-byte wire-container formatting, and page-cookie wrap tests against the production G2-specific core; the 25-kind middle page proves all 15 visible slots |
| `components/hardwareone/test/host/test_command_input_guards.py` | Verify sync/async guard placement and that UART derives its whole-line cap from the shared constant |
| `components/hardwareone/test/host/test_notification_integration_guards.py` | Keep production notification adapters and registered CLI help aligned with replacement, `set`, `patch`, `all`, and `none` grammar |
| `components/hardwareone/test/host/test_user_settings_transaction_guards.py` | Enforce declaration/definition include checking, outer transaction use by generic merge and every existing-user RMW writer, the reviewed direct-save allowlist for new-user initialization, all-error cache invalidation, and generation validation before an out-of-lock preference load is published |
| `components/hardwareone/test/host/CMakeLists.txt` | Register the six Phase 0–2 C++ executables, the Phase 4 G2 picker executable, and all five Python checks with CTest; apply ASan/UBSan compile and link options to every new native target and link the provider stress test with `Threads::Threads` |
| `components/hardwareone/test/host/README.md` | Document the catalog invariants and test command |
| `components/hardwareone/test/device/System_EventCatalogDeviceHarness.h/.cpp` | Default-excluded harness and test-only command table for named direct-async, settings transaction/cache/late-failure, OLED invalid-state, HTTP one-shot sink, read-only synthetic G2 25-kind/15-row rendering, G2 swap-stage/allocation/terminal-order/idle-revocation/freshness-claim, raw post-commit wire-token reinjection, and paused-tap race cases; its handler requires a named admin session whose current transport is UART, hooks auto-disarm, and every request/result carries a unique case id |
| `tools/webui/harness/event_kind_catalog_harness.js` | Permanent browser behavioral scenarios |
| `tools/webui/tests/test_event_kind_catalog.py` | Drive the real embedded helper against valid/invalid catalogs |
| `tools/webui/harness/notification_kind_editor_harness.js` | Execute the extracted production Dashboard block against bounded all/none/packed-diff and partial-batch scenarios |
| `tools/webui/tests/test_notification_kind_editor.py` | Extract the named Dashboard raw block and drive it through the permanent harness |
| `tools/build_event_catalog_coverage.sh` | Serialized full-clean board/gate matrix with Phase 1 `--provider-only` and Phase 2 full modes, per-row desired-config reset, asserted flags/object ownership, and metadata-preserving final baseline restoration/rebuild of directories touched by temporary profiles |
| `tools/build_event_catalog_device_test.sh` | Build the default-OFF harness in a separate FeatherS3 test directory with `-DHW1_EVENT_CATALOG_DEVICE_TEST=ON`, never overwrite an ordinary board artifact, and print the exact flash/restore commands |
| `tools/test_event_catalog_device.py` | Drive named harness cases over the physical test UART, correlate case-id logs/results, collect evidence, auto-disarm hooks, and clean the disposable identity; it never enables a hook over HTTP/BLE |
| `tools/ble_secure/secure_channel_v1.py` | Reusable test-client implementation of the firmware's X25519/PSK/HKDF/ChaCha20-Poly1305 handshake, counters, and five-byte reply-fragment reassembly; no production secret persistence |
| `tools/ble_secure/fixtures/secure_channel_v1_vectors.json` | Synthetic non-production handshake/AEAD/reassembly vectors used to pin the client implementation byte-for-byte |
| `tools/ble_secure/test_secure_channel_v1.py` | Offline vectors, counter-gap, incomplete-message, and replacement-session isolation tests for the generic client protocol core |
| `tools/ble_secure/requirements.txt` | Exact compatible, hash-pinned `bleak`/`PyNaCl` dependency set for the offline/physical clients, reviewed and updated deliberately |
| `tools/ble_secure_event_catalog_client.py` | Generic Phase 2 physical client: preflight that the device is advertising in phone-server role, connect to the command UUIDs from `BLE_IDF.h`, prompt without echo for Secure Channel and login secrets, authenticate, issue `events kinds json`, reject incomplete/session-spliced replies, and compare the parsed catalog with the golden fixture without logging credentials; it refuses rather than attempting to mutate the ESP32 BLE role over a nonexistent connection |
| `tools/test_ble_secure_event_catalog_client.sh` | Create/reuse an isolated tool venv, install the hash-pinned requirements with `--require-hashes`, run offline protocol vectors in `--offline` mode, and invoke the physical client only when explicitly requested; dependency absence is a hard Phase 2 failure, not a skipped pass |
| `docs/testing/SYSTEM_EVENT_CATALOG_DEVICE_CHECKLIST.md` | Phase-owned hardware, transport, external-client, and fault-injection procedures with exact builds, disposable identities, expected logs/results, evidence fields, BLE server/G2-R1 role capture-quiesce-restore steps, and cleanup/recovery; it does not mark host simulations as device passes |
| `docs/APP_JSON_CONTRACT.md` | In Phase 2, document the v1 grouped event-catalog command/shape/order, 4,096-byte result-buffer budget including trailing NUL, Secure Channel framing requirement, and unsupported full-payload plaintext behavior; Android later consumes rather than originates this contract |
| Android companion repository | Future parser/repository/cache and automation-picker tests |

---

## 12. Verification matrix

Verification is deliberately split into two evidence tiers:

1. **Automated repository-local verification** runs from a clean checkout and
   proves provider, serializer, parser, browser behavior, source structure, and
   board/link ownership without claiming a physical peripheral was exercised.
2. **Hardware, transport, external-client, and fault-injection acceptance** is
   recorded in `docs/testing/SYSTEM_EVENT_CATALOG_DEVICE_CHECKLIST.md`. Each
   named procedure records the exact firmware/app revision and configuration,
   fixture or disposable identity, steps, expected logs and externally visible
   result, captured evidence, and cleanup/recovery. A host seam can complement
   these procedures but cannot mark a real OLED, G2, BLE, UART, or HTTP-stream
   row as passed.

The implementation phase that introduces a behavior owns its corresponding
checklist rows. Phase 1–3 release does not need unfinished Phase 4 G2/Android
authoring rows to pass; Phase 4 cannot ship those features until its rows pass.

### 12.1 Automated repository-local verification

#### Host typed-provider tests

- Exact current counts: 12 families and 152 canonical kinds.
- Sum of per-family counts equals global kind count.
- Every global kind appears exactly once in family iteration.
- Names match `^[a-z0-9_]+$` and are unique case-insensitively.
- Family labels are non-empty, unique, and valid UTF-8.
- Every kind points to a valid family.
- Global and within-family order match the current fixture.
- Name → id → canonical name round-trip for every kind.
- `boot` resolves to `boot_finished` but never appears in enumeration.
- Compile-time/host checks reject canonical rows named `boot`, `none`, `set`,
  `patch`, `all`, or `list`, and prove near-neighbours remain valid.
- Legacy fallbacks remain exact:
  `systemEventKindName(SYSEVT_NONE) == "none"`,
  `systemEventKindFamily(SYSEVT_NONE) == SYSEVT_FAM_SYSTEM`, invalid kinds
  return `"?"` / `SYSEVT_FAM_SYSTEM`, an invalid family label returns `"?"`,
  and null or empty lookup names return `-1`.
- Null/OOB requests fail without changing output storage.
- First/last kind and first/last kind of every family resolve correctly.
- A test fixture with more than 32 kinds in a family remains fully iterable,
  including interleaved/non-contiguous declaration order.
- A synthetic empty family or invalid family reference is rejected.
- Public provider/core headers expose no JSON sink, status, grammar, Arduino,
  renderer, or transport type.
- Link/map inspection finds one production catalog table/index owner and no
  template-instantiated copies in OLED, G2, HTTP, or command objects.
- A multithreaded host stress test performs independent indexed reads and
  lookups and proves concurrent reentrancy/result consistency with no
  cross-talk. ASan/UBSan are not race detectors, so the no-lock/no-mutable-state
  claim is established separately by source inventory and table qualifiers;
  do not label this test as TSAN evidence.
- A test-scoped global `new`/`new[]` counter remains unchanged across repeated
  production indexed reads/lookups, and a registered source inventory rejects
  `new`, `malloc`, `calloc`, `realloc`, `ps_alloc`, `String`, mutex/semaphore/
  lock guards, and critical-section APIs in the provider implementation. The
  zero-allocation/no-lock claim is scoped to these provider APIs, not G2
  rendering or transport adapters.
- `SYSTEM_EVENT_KIND_TOKEN_CAP` equals the longest production canonical token
  plus NUL.

#### Host JSON output-adapter tests

- JSON parses, contains every kind exactly once, and preserves the v1 shape.
- Parsed JSON families/kinds exactly match typed-provider traversal in family
  order, kind order, labels, and canonical names; all six reserved tokens are
  absent.
- Synthetic descriptors prove quote, backslash, and C0 bytes `0x01..0x1f` are
  escaped; `/` remains raw; valid UTF-8 bytes remain unchanged; embedded NUL
  and malformed UTF-8 are rejected before any sink call.
- `JsonSize()+1` succeeds; one byte less returns `BufferTooSmall`, an empty
  destination, and the required capacity.
- Null destination with any capacity, including zero, is `InvalidArgument`;
  non-null destination with zero capacity is `BufferTooSmall`. Output sizes and
  destination clearing follow the §7.1 precedence exactly.
- A deliberately failing sink returns `SinkFailed`; `bytesWritten` equals the
  sum of prior fully accepted callback chunks and excludes the rejected chunk.
- Current command payload plus NUL is below `CMD_RESULT_MAX`.

#### Host mask and command-limit tests

- The production array-size-aware helper tests, sets, clears, and toggles IDs
  127, 128, 152, and an out-of-range ID without an invalid memory access.
- Toggling a low bit in an eight-word fixture preserves every high word.
- The production command-input predicate accepts 2,047 bytes and rejects 2,048
  bytes without including Arduino, FreeRTOS, or executor code.

#### Host notification-kind mutation tests

- The production `System_NotificationKindListCore.h` handles replacement,
  one-kind set, multi-kind patch, all, and none with caller-owned bounded
  storage and no filesystem/identity/Arduino dependency.
- Unknown, malformed, overlength, and over-count command input fails before
  producing output. Patch duplicates are detected after canonicalization, so
  `+boot,+boot_finished` fails; opposite signs for one canonical kind fail.
- Known aliases canonicalize. Legacy replacement input deduplicates by canonical
  name, preserving existing replacement behavior; patch input never silently
  deduplicates an operation.
- Stored unknown tokens matching 1–63 lowercase `[a-z0-9_]` bytes survive
  set/patch and deduplicate exactly; reserved, malformed, or over-limit stored
  tokens fail set/patch. Replacement/all/none ignore and repair a malformed old
  target list while preserving unrelated document keys.
- A 256-entry synthetic stored list runs through the repeatable multipass
  visitor with fixed bitsets, no per-token stack matrix or heap allocation in
  the core, and measured core scratch below 512 bytes. A synthetic known
  canonical token longer than 63 bytes resolves and emits intact, proving the
  unknown-token limit did not become a second known-token cap.
- Output capacity/sink failure prevents save; production JSON insertion OOM
  discards the unsaved document. No partially emitted mutation becomes
  authoritative.

#### Automated command, UI, and boundary checks

- `test_notification_kind_list.cpp` exercises replacement, `set`, `patch`,
  `all`, and `none`; `test_notification_integration_guards.py` checks that both
  registered notification verbs advertise that grammar.
- The all-kinds state is representable without generating a full-list command.
- The extracted production Dashboard packer derives two current worst-case
  delta commands (2,037 and 405 bytes) and keeps every command within
  `CMD_INPUT_MAX` under a synthetic larger catalog.
- A source-level guard-placement test verifies both submit functions call the
  production predicate before the sync early-boot branch and before either
  request allocation. This proves placement without pretending to tear down a
  live executor queue on the host.
- A transaction-boundary source check requires `System_Settings.cpp` to include
  its declaring `System_UserSettings.h`, requires generic merge and every
  existing-user password/notification writer to use the transaction helper,
  and permits direct `saveUserSettings()` only at reviewed new-user full-file
  initialization sites. It also requires every save-error exit to invalidate
  preference cache state and requires `notifViewerResolve()` to generation-
  validate an out-of-lock flash load before cache publication without holding
  the cache mutex during filesystem I/O. The save-path check rejects
  `written > 0` as a success predicate: both temp and direct paths must compare
  the serialized count against one pre-measured expected length, compare the
  observed file size after the existing void flush against that same length,
  and forbid rename before those checks pass. It must not treat
  `getWriteError()` as a
  checked FS/flush signal or claim durability the current API cannot observe.
- The production human `events kinds` command delegates to the tested text
  adapter. Host cases prove exact-fit packing, flush-and-retry for a synthetic
  canonical token longer than the old 120-byte staging buffer, whole-line sink
  failure, and zero-callback preflight rejection when one complete family
  header or token exceeds the named 255-byte output payload. Source checks
  require production to pass `DEBUG_MSG_SIZE - 1`, forbid unchecked advancement
  by a truncated `snprintf()` return, and do not pretend the existing void
  `broadcastOutput()` reports later queue drops.
- OLED source/reference checks find no fixed 24-row materialization or fixed
  chosen-kind token buffer. Synthetic provider coverage exceeds the former
  24/32-row boundaries; confirmation/submission paths re-resolve ordinals and
  do not copy event options into `OLEDScrollState`.
- Existing embedded JavaScript syntax tests pass. The permanent browser harness
  proves concurrent consumers share one request, success caches per page,
  failure/malformed input can retry, duplicate/invalid names are rejected,
  unknown persisted kinds remain selected and unavailable, and stale async
  completion cannot replace a newer selection.
- The Dashboard browser harness proves `all`/`none` and arbitrary edits use
  bounded, greedily packed delta commands; neither current nor synthetic
  all-kinds state creates a full replacement line or one save per checkbox.
- Provider-release source/protocol assertions prove no `events kinds json`
  request, catalog payload, or JSON parser is introduced on the G2 temple GATT
  links. Phase 4 source checks additionally require the original RX-captured
  connection/lifecycle/presentation stamps and exact parsed container token to
  flow through cross-arm dedup, ListEvent enqueue, `TapDispatchEntry`, worker,
  and the optional context page callback. They require one lock-ordered
  token/freshness-plus-dedup claim against the exact echoed token and original
  side/generation/lifecycle/presentation, coherent worker revalidation through
  the same preclaim/lock path with no dedup access, and
  final adapter revalidation of all four stamps plus the token with the page
  cookie. They require `activePresentation` to be the only expected-token owner
  and require its bounded preclaim to run inside the combined G2→adapter claim;
  they forbid a mirrored broker token, a split check→dedup mutation,
  re-sampling/storing current presentation in dedup/enqueue, treating RX-time
  epoch stamps alone as proof of the originating physical page, or a
  current-epoch getter as the Automations adapter's stale-tap decision. Legacy
  page aggregate initialization remains valid. Protocol fixtures and
  real-glasses acceptance must prove the existing arbitrary CREATE container
  name returns exactly in `List_ItemEvent.ContainerName`. Routing checks retain
  the literal `"app"` legacy path, preserve the raw protobuf string length, and
  admit only exact 18-byte `ev[0-9a-f]{16}` tokens to the new path. Embedded
  NUL, suffix, truncation, uppercase, wrong length, and unclaimed reserved-token
  fixtures hard-drop; checks forbid generic arbitrary-container dispatch or
  fallback reinterpretation. The worker helper likewise requires `Fresh` for a
  reserved token and contains no dedup-slot mutation.

### 12.2 Hardware, transport, external-client, and fault-injection acceptance

The internal-only cases use one narrowly scoped test profile. Normal builds do
not define `HW1_EVENT_CATALOG_DEVICE_TEST` and do not compile the harness or
hook symbols. `tools/build_event_catalog_device_test.sh` creates a separate
FeatherS3 build directory with the CMake option on; it never reuses or labels a
normal artifact. The harness is controlled only over the physical test UART,
prints a loud test-firmware banner, requires a unique case id, and exposes no
HTTP/BLE arming endpoint. One-shot hooks auto-disarm on consumption, timeout,
disconnect/recovery, or case end. The runner rejects leftover hooks, removes
the disposable identity/settings, captures logs/artifacts, and directs the
operator to fullclean/build/flash the ordinary profile afterward. The ordinary
build matrix asserts the option is off and no harness/hook symbol is linked.

The compiled test profile adds exactly one otherwise-absent registry command:
`eventcatalogtest <case-id> <case> [args...]`. Its handler rejects unless
`currentAuthContext().transport == SOURCE_UART`, the task has a named logged-in
identity rather than `AuthBypass`, and `currentExecIsAdmin()` is true. It emits
machine-readable `EVENTCAT_TEST <case-id> <phase|PASS|FAIL> ...` lines so the
UART runner cannot confuse delayed output from another case. The production
source/link checks prove the module row, command string, dispatch function, and
all hook symbols are absent when the option is off; transport checks prove the
same test firmware rejects attempts through web, plaintext/Secure BLE, MQTT,
ESP-NOW, OLED, G2, or the physical USB console.

Named harness cases map to the otherwise unreachable seams:

| Case | Deterministic internal seam |
|---|---|
| `async_input_boundary` | Calls production `submitCommandAsync()` directly with benign 2,047/2,048-byte inputs |
| `settings_rmw_barrier` | Pauses the first user-settings transaction after load while a second disjoint writer attempts entry |
| `settings_precommit_fail` | Fails one temp open/write before rename, then disarms and reloads |
| `settings_temp_short_write` | Accepts a nonzero strict prefix of the measured JSON into the temp file; verifies serialized count/file-size mismatch, no rename, and byte-identical old destination without relying on unavailable flush-error state |
| `settings_destination_touched_fail` | For one disposable identity, forces rename failure, opens/truncates the direct-overwrite destination, then fails before serialization; expects error, cache invalidation, and an authoritative reload rather than rollback |
| `settings_destination_short_write` | After forced rename failure, accepts a nonzero strict prefix in the disposable direct fallback; expects error, cache invalidation, and reload of actual storage state rather than false success |
| `settings_cache_fill_race` | Pauses a resolver after its old flash load but before cache-generation validation, commits and invalidates a disjoint notification mutation, then proves the old load is discarded/retried |
| `oled_invalid_selection` | Injects stale/out-of-range family/kind ordinals into the adapter without synthesizing button timing |
| `http_sink_fail_n` | Arms a single catalog response to reject serializer chunk N and force handler abort/close |
| `g2_picker_15_rows` | Supplies a read-only synthetic 25-kind provider view to the production G2 picker core/list-swap path, renders its 15-row middle page, reports the 12 option ordinals plus Back/Prev/Next actions to the harness, and hard-disables automation persistence |
| `g2_swap_stage` | Selects pre-admission, pre-transition abort, transition-started failure, committed completion, or terminal-published-before-admission-release for one page cookie |
| `g2_idle_revoke_race` | With an idle committed page and no swap owner, triggers an authoritative/full-session invalidation, pauses after lifecycle/presentation advance but before revocation callback, injects old- and new-epoch taps, then clears state without touching swap admission/job storage |
| `g2_freshness_claim_race` | Repeatedly races the production lock-ordered freshness+dedup claim against the production test-guarded generation-publication helper on the other core; an after-compare hook only sets an atomic marker and never blocks/yields, proving publication is wholly before/after the claim and stale work never replaces the seeded valid slot |
| `g2_tap_pause` | At RX→dedup/enqueue, copies one decoded tap plus original side/gen/lifecycle/presentation/token into a bounded defer slot and immediately returns the control owner; in the separate tap worker, may pause before its no-dedup preclaim revalidation or after it but before the final all-stamp/token/page-cookie adapter claim |
| `g2_postcommit_wire_token` | Before `g2RxPacketEnqueue()` stamps a complete page-A ListEvent notification, copies its raw bytes into one bounded slot and immediately returns; after B commits, re-injects those bytes through the production enqueue/parser so they acquire B's current epoch stamps but retain A's echoed container token |
| `g2_alloc_fail` | Fails the named page-job/row/scratch allocation once, never a global allocator indefinitely |

Real-button/lens/radio behavior remains a physical checklist step even when a
harness seam establishes the hard-to-reach state.

#### Notification persistence and command ingress

- Run persistence mutations under a disposable test identity and remove its
  settings afterward; never use an operator's real notification preferences.
- `notifyusermute` and `notifyusershow` retain backward-compatible in-budget
  replacement syntax and correctly apply `set`, `patch`, `all`, and `none`.
- A patch rejects an unknown, duplicate, contradictory, or malformed delta
  before save; unrelated known and valid unknown stored tokens survive.
- A deterministic two-task barrier pauses a notification transaction after
  load while a disjoint web-style patch attempts the same user. The second
  transaction waits, then loads the committed first result; the final document
  contains both edits. Repeat with two disjoint notification-list patches and
  with a password/gamepad-style patch whose hash was prepared before entry;
  every final document retains both unrelated changes.
- The named pre-commit temp-open/zero-write and nonzero-short-write hooks return
  `Error:`, remove the incomplete temp, never rename it, and leave the original
  file byte-identical. Evidence records measured bytes, attempted bytes,
  accepted bytes, and final temp/destination sizes. This proves observable
  short-write handling only; it does not label the void `flush()` as checked or
  claim media durability.
- Separate later/ambiguous cases use only a disposable identity, force the
  rename fallback, and either fail immediately after opening/truncating the
  destination or accept a nonzero strict prefix. Neither may claim rollback or
  treat a positive count as success: each returns `Error:`, invalidates the
  preference cache, and records the authoritative reload result before cleanup
  reconstructs/removes that disposable file.
- In the cache-fill race, pause a resolver after it loads the old document but
  before its generation check. Commit a notification mutation and invalidate,
  then resume the resolver. It discards the old load, retries, returns the new
  rule, and leaves no valid cache slot containing the pre-commit masks. Exercise
  the real main/OLED task pairing on device and inspect lock diagnostics; no
  path holds `gUserPrefsMutex` while waiting on the filesystem lock.
- A maximum-size stored-list mutation records command-task stack high-water,
  PSRAM/internal allocation delta, and injected allocation failure. It stays
  within the §8.4 scratch budget and performs no save on OOM.
- An authenticated web batch accepts a harmless 2,047-byte sync command and
  rejects a 2,048-byte command without executing its prefix.
- A dedicated on-device executor procedure calls production
  `submitCommandAsync()` directly with benign unknown-command strings: 2,047
  bytes is admitted to the ordinary error callback, while 2,048 bytes returns
  `false` before request allocation/queueing and produces no callback or action.
  Do not label this as an ESP-NOW wire test: ESP-NOW command ingress is capped
  at 218 bytes, and phone-companion Secure BLE is capped by `cmdBuf[512]` before
  either can reach the central 2,047-byte executor boundary.
- UART accepts a 2,047-byte line and discards a 2,048-byte line whole, with the
  checklist capturing the response/log and proving no truncated prefix ran.

#### OLED device behavior

- Navigate every family and its last kind. Automation wrap and notification
  clamp remain intentional and distinct.
- A label truncated for the 128-pixel display still selects the original typed
  record and persists its full canonical name; forced ordinal re-resolution
  failure performs no command or save.
- Toggle notification kinds 127, 128, and 152 through the real OLED picker. A
  pre-existing high-kind mute survives toggling a low kind, and personal-mute
  and device-policy views resolve the same canonical name/id.
- Stale cursors and out-of-range input clamp or fail without mutating settings.
  No OLED action constructs or submits a command longer than 2,047 bytes.

#### HTTP server and streaming failure

- Authenticated HTTP and CLI outputs contain identical family/name sequences.
- Arm `http_sink_fail_n` over the physical UART, then make one authenticated
  catalog request. The test-build sink accepts exactly N chunks, rejects the
  next, and forces handler abort/close; the client observes an incomplete/
  failed response and the server sends neither a chunk terminator nor a JSON
  error tail. Record the case-id server log and packet/client capture. A remote
  client RST alone is not deterministic evidence because TCP buffering may
  accept the whole 2.9-KB response before the close is observed.

#### G2 real-glasses behavior and fault injection

- Provider release: existing System Events history still resolves every name,
  and existing automation details still display stored `on` strings.
- Phase 4 registry compatibility: one shared `g2PageHasTapHandler()` predicate
  recognizes legacy-only, preclaim-only, or context-only modules at every early
  ownership/fallback check. Representative legacy pages still receive taps;
  context-only fixture pages are not treated as handlerless. The Automations
  preclaim returns `NotClaimed` for ordinary `"app"` events in existing modes,
  Fresh for the exact active picker token, and Stale for every unmatched
  reserved token while `activePresentation` remains the only expected-token
  owner. Its
  context wrapper returns `NotHandled` for existing Back, Run now, Enable,
  Disable, and async-redraw flows so each reaches the legacy handler exactly
  once, while picker rejection returns `Handled`. Queue-size assertions and
  measured tap-queue internal DRAM are updated for side/connection stamps and
  the bounded echoed token.
- Initial and worker-time fixtures send a syntactically reserved token with a
  missing preclaim, `NotClaimed`, and `Stale`; every case hard-drops before the
  page callback and leaves dedup/draft unchanged. Pause a genuinely Fresh tap
  after initial dedup/enqueue, commit another picker token, then resume: the
  worker's same-lock-path, no-dedup preclaim rejects it. A control tap whose
  token remains active proceeds, proving the worker did not require a mirrored
  broker token.
- Phase 4 production family-first navigation reaches every real catalog row
  without a copied catalog. The host G2-core fixtures prove 0/1/12/13/25-item
  boundaries, Prev/Next/Back mapping, page shrink, exact wire-token formatting,
  and no same-lifecycle cookie reuse at wrap. On real glasses, current
  production families exercise their actual pages (at most 14 rows); the
  harness's read-only 25-kind fixture renders the exact 15-row middle page and
  maps each of its 12 option taps to the correct full fixture identity and its
  three chrome taps to Back/Prev/Next, without invoking save. A truncated
  production lens label still selects its full canonical name.
- Before any picker acceptance, create two pages with distinct generated
  container tokens and prove supported real glasses echo each token exactly in
  `List_ItemEvent.ContainerName` for both option rows and repeated chrome names
  such as Prev/Next. Omission, truncation, normalization, or substitution is a
  Phase 4 compatibility failure; do not fall back to item names, RX-time epoch
  stamps, or a timing quarantine.
- Phase 4 exercises every swap stage separately. Pre-admission allocation
  failure returns the owning `PreparingCandidate` operation to idle. A stale/
  rejected `Admitted` handshake prevents queue exposure; post-handshake enqueue
  failure and an injected same-lifecycle pre-transition cancellation drop
  pending and resume current active. A stale/rejected `TransitionStarted`
  handshake prevents physical mutation. After successful transition start,
  CREATE/ACK failure clears both. Real `g2UiWorkersQuiesce()` drain advances
  presentation, emits one terminal outcome, frees pending, and does **not**
  resume active. Only exact page-cookie commit publishes pending; duplicate/
  late terminals cannot leave state suspended or revive it, and neither
  handshake can be ignored while the generic worker continues.
- For each terminal class of an admitted swap, pause after the adapter publishes
  its terminal state but before that request releases `gPageSwapAdmission`. A
  concurrent tap remains outside the adapter while the fence is held; after
  release it sees exactly the committed map or the cleared/recovered state. Job
  storage remains alive until after terminal publication and fence release,
  and terminal callbacks perform no rendering or broker recursion.
- Phase 4 includes an instrumented race: pause after dispatcher admission,
  win broker admission and suspend the active mapping, then resume the
  Automations callback. The original RX-stamped context must fail its atomic
  page-cookie/epoch claim and must not mutate the draft. This specifically
  proves identity is not re-sampled/lost at ListEvent enqueue or the current
  `handleTap(uint32_t)` boundary and that tap claim cannot overlap suspension.
- One stamped-context race uses the one-slot nonblocking RX defer hook: copy A's
  decoded index/name, exact token, and original side/generation/lifecycle/
  presentation, return immediately to `g2_ctrl_owner`, commit presentation B
  with ordinary ACK draining, then re-inject A from the harness task. Exercise
  both orders. For stale A-left → B-right, A is rejected without a slot mutation
  and B is accepted. For B-right → stale A-left → mirrored B-left at the same
  row inside 150 ms, A is rejected without replacing B and the final B-left is
  deduplicated, so B executes exactly once. No test hook may block the ACK owner.
- A separate hard-boundary race arms `g2_postcommit_wire_token` before
  `g2RxPacketEnqueue()` stamps a complete raw page-A ListEvent. The BLE callback
  copies the bounded raw notification and immediately returns without blocking
  the control/ACK owner. Commit page B, then re-inject A through the production
  enqueue and parser: it deliberately receives B's current lifecycle/
  presentation stamps but retains A's wire token. Exact token mismatch rejects
  it before dedup or draft mutation, after which a real B tap at the same option
  row and a same-named chrome row still works. This is the regression that
  prevents RX-time stamps alone from being mistaken for physical-page proof.
- The freshness-claim race uses a test-only atomic marker after comparison and
  a generation publisher on the other core; neither blocks/yields inside the
  critical claim. It records at least one overlapping publication attempt and
  proves the publication serial is entirely before or after the combined
  freshness+dedup operation. A stale claimant never replaces a previously
  seeded valid B slot. Source checks enforce the declared
  sync-lifecycle→topology→freshness order on every participating writer.
- Commit a page through RIGHT, queue an old LEFT-generation tap, reconnect only
  LEFT, and prove the old tap is rejected while a fresh LEFT-generation tap is
  accepted without recreating the still-current page. The rejected old LEFT tap
  does not seed cross-arm dedup: a real RIGHT tap on the same row inside the
  nominal dedup interval is also accepted. This rejects frozen dual-generation
  page state, missing per-arm freshness checks, and freshness validation placed
  only after dedup mutation.
- Pause the tap worker after its coherent freshness validation but before the
  final Automations mutation claim, reconnect that same non-authoritative arm,
  then resume. The final claim revalidates original side/generation together
  with lifecycle/presentation, exact echoed token, and page cookie and rejects
  the old tap without mutating the draft; a fresh tap from the new generation
  remains usable.
- After a picker page is committed and no swap is in flight, trigger an
  authoritative-arm/full-session disconnect, root recovery, and lifecycle
  replacement separately. The idempotent page-revocation callback clears
  active/pending/phase before reconnect; it does not depend on another swap
  terminal callback or claim/release
  `gPageSwapAdmission`. In the idle-revocation race, pause after the global
  lifecycle/presentation advance and before the callback. Both an old-stamped
  queued tap and a newly stamped tap fail, then the callback clears the old map.
- Two adjacent equal-row-count pages with different canonical kinds bypass the
  default-on `REBUILD-list` path, perform SHUTDOWN/CREATE, and remain stable on
  real glasses.
- Repeated Prev/Next navigation under constrained PSRAM and internal
  largest-block conditions either succeeds or leaves the prior mapping/session
  safe after allocation failure; no progressive fragmentation or stale tap is
  observed. The read-only 25-kind fixture uses the exact 15-row cap, and page
  size uses the lens row budget to limit repeated
  500-ms-plus-CREATE/ACK latency.

#### Phone companion Secure BLE transport — Phase 2

- Before starting the phone client, the operator-side checklist/runner uses an
  authenticated physical console/UART path to capture `blemode`, `blestatus`,
  `g2status`, and `ringstatus`. It then runs `openble`, which persists server
  role, cancels reconnect intent, and tears down G2/R1 Central ownership; the
  procedure waits for successful quiescence and verifies `blemode` reports
  server plus active advertising before the client scans. A deferred G2/R1
  teardown or lifecycle fault is a failed setup, not a skipped transport test.
- The repository-owned generic client—not the deferred Android repository—uses
  the command service UUIDs from `BLE_IDF.h`, passes synthetic offline v1
  handshake/AEAD/reassembly vectors, prompts without echo for the Secure
  Channel and named-login secrets, logs neither, establishes a fresh channel,
  logs in inside it, issues `events kinds json`, reassembles the complete
  multi-frame reply, parses valid catalog JSON, and matches typed/HTTP
  family/name order. Its exactly pinned `bleak`/`PyNaCl` environment and test
  invocation are recorded with the evidence.
- Disconnect during reply yields an incomplete failure and cannot splice frames
  into a replacement Secure session; counter gaps, duplicate fragments,
  conflicting fragment metadata, timeout, and an old connection's late callback
  are all hard failures, never partial parser input. The same client documents
  that the full roughly 2.9-KB v1 reply remains unsupported on the plaintext
  notification lane.
- `docs/APP_JSON_CONTRACT.md` is updated and reviewed in this phase with the v1
  response shape/order, 4,096-byte result-buffer budget including trailing NUL,
  Secure Channel framing link, and plaintext limitation. Android Phase 4 is
  tested against this already-
  published contract rather than becoming its source of truth.
- Cleanup disconnects the phone and proves no Secure session/reassembly remains,
  then restores the captured persisted role. If the original role was client,
  run `blemode client`; only if G2 or R1 was actually connected before setup,
  restart it with `openg2 saved` or `ringconnect reconnect` respectively and
  wait for the captured healthy state. If the original role was server, restore
  its original running/advertising state: leave/re-run `openble` if it was
  running, or run `closeble` followed by `blemode server` if the persisted role
  was server but the server was originally stopped. Record all outcomes; a
  failed restore fails the checklist and requires the documented ordinary-
  profile recovery before any later G2 evidence is collected.

#### Android companion behavior — Phase 4

- The pinned app revision rejects duplicate names, wrong shapes, and incomplete
  capture; lazy concurrent fetches coalesce; disconnect/session replacement
  invalidates the connection-scoped cache and cannot publish an old reply.
- A stored unknown kind remains visible as unavailable. Event-trigger summaries
  display their stored canonical `on` value rather than a generic type label.
- The Android client never issues the full-catalog request on a plaintext
  session; a direct third-party plaintext attempt remains explicitly
  unsupported by this change.

### 12.3 Automated build matrix

Add `tools/build_event_catalog_coverage.sh` so this is repeatable rather than a
hand-edited promise. It must require an already-exported ESP-IDF environment,
serialize every board build (all board configurations share generated
`partitions.csv`), take a metadata-preserving backup of
`System_BuildConfig.h`, and begin **every** row by restoring the baseline bytes,
applying that row's deliberate flag edits, and giving the desired content a
fresh dependency timestamp. Assert every temporary value before building. For
the row's exact board directory, run `tools/build_board.sh <board> fullclean`
before `tools/build_board.sh <board> build`; an ordinary incremental build is
not sufficient after content is restored to an older revision. Record the row
start and prove the catalog object(s), link map, application binary, and
manifest were regenerated afterward. Inspect `BUILD_INFO.md` and fail if the
resolved gates do not match the table, but never use that source-recompiled
manifest as the sole proof that the linked binary used those flags.

In Phase 1 `--provider-only` mode, every profile verifies the unconditional
`System_EventCatalog.cpp.obj` and provider table/index ownership. In the default
Phase 2+ mode, it additionally requires
`System_EventCatalogJson.cpp.obj` and the public JSON adapter symbols. Use
`compile_commands.json`, object/link timestamps, link map, and `nm` checks prove
sole ownership of the named provider APIs and four named catalog data symbols.
Separate source guards reject the known duplicate-table shapes and direct row
consumers, but do not claim to detect every possible alternate encoding. For
temporary gate profiles, assert representative expected-present/absent gated
objects or symbols as linked-binary evidence; flag text alone does not prove
coverage.

The temporary FeatherS3 and XIAO rows reuse `build-feathers3/` and
`build-xiao_s3/`, so restoration cannot stop at `reconfigure`: that would leave
the last linked binary built with temporary flags and can leave a stale
manifest after the header timestamp changes. The `EXIT` cleanup must capture
the original status, restore the header bytes/mode with a **fresh** mtime, then
fullclean and build the ordinary FeatherS3 and XIAO profiles serially to replace
every temporary object/binary and regenerate manifests. Verify baseline gates,
object/link regeneration, binary symbol evidence, and exact header bytes. Only
after both baseline rebuilds succeed may cleanup restore the header's original
mtime (and confirm its original mode/bytes). Preserve the original failure
status; if the matrix succeeded but cleanup rebuild fails, return the cleanup
failure. A final source status check must prove both content and metadata match
the initial snapshot. Never leave a temporary manifest or binary as the
apparent current board configuration.

The script runs these exact profiles:

| Profile | Command / temporary values | Coverage |
|---|---|---|
| FeatherS3 current | `tools/build_board.sh feathers3 fullclean`, then `tools/build_board.sh feathers3 build` | ESP32-S3; HTTP, OLED, Automation, Bluetooth, and G2 enabled |
| FeatherS3 G2 without Automation | Restore baseline bytes, set only `ENABLE_AUTOMATION=0`, retain `ENABLE_BLUETOOTH=1` and `ENABLE_G2_GLASSES=1`, then fullclean/build FeatherS3 | Proves the G2 core/history compatibility lookup and provider linkage build when the G2 Automations adapter is stubbed out; neither all-on nor G2-off profiles cover this mixed gate |
| XIAO current | `tools/build_board.sh xiao_s3 fullclean`, then `tools/build_board.sh xiao_s3 build` | ESP32-S3; board SDK config derives Bluetooth/G2 off while HTTP, OLED, and Automation remain enabled |
| Classic ESP32 current | `tools/build_board.sh feather_esp32_v2 fullclean`, then `tools/build_board.sh feather_esp32_v2 build` | Second chip target with HTTP, OLED, Automation, Bluetooth, and G2 enabled |
| XIAO optional consumers-off | Restore baseline bytes; set `NETWORK_FEATURE_LEVEL=0`, `WEB_FEATURE_LEVEL=0`, `I2C_FEATURE_LEVEL=0`, `DISPLAY_TYPE=0`, `INPUT_DEVICE_TYPE=0`, `ENABLE_BLUETOOTH=0`, `ENABLE_G2_GLASSES=0`, `ENABLE_R1_HEALTH=0`, `ENABLE_AUTOMATION=0`, `ENABLE_HTTPS=0`, and `ENABLE_BONDED_MODE=0`; then fullclean/build XIAO | Proves the catalog remains linked when the optional web, OLED, BLE, G2, and Automation consumers are compiled out; the always-on command layer may still consume it |

There is no supported non-PSRAM board profile in the repository. Do not invent
one for this change. The provider's no-allocation property is instead proved by
its dependency-light host build, API implementation review, and allocation
tests.

Run the automated repository-local verification sequence from the repository
root:

```sh
cmake -S components/hardwareone/test/host -B /tmp/hw1-hardwareone-host
cmake --build /tmp/hw1-hardwareone-host
ctest --test-dir /tmp/hw1-hardwareone-host --output-on-failure
python3 -m unittest discover -s tools/webui/tests -t .
tools/test_ble_secure_event_catalog_client.sh --offline
tools/build_event_catalog_coverage.sh
```

The catalog module must compile in every profile because it is unconditional.
This sequence does not satisfy the §12.2 hardware/fault-injection tier; record
those phase-owned procedures and evidence in
`docs/testing/SYSTEM_EVENT_CATALOG_DEVICE_CHECKLIST.md` before release.

---

## 13. Acceptance criteria

The core implementation is complete only when all of the following are true:

- [ ] There is one private row owner in `System_EventCatalogRows.h`; no public
      list macro or direct adapter inclusion exists.
- [ ] There is one typed family/kind provider; OLED, human CLI, the JSON
      adapter, and any future ESP32 G2 picker resolve the same records/order.
- [ ] No JSON sink/status, HTTP, BLE, OLED, G2, or ring types leak into the
      provider API or provider core.
- [ ] Typed iteration performs no heap allocation and holds no lock.
- [ ] OLED has no 24-entry event-kind arrays.
- [ ] OLED high-kind notification edits are safe through the final kind.
- [ ] OLED and Dashboard notification edits never generate a whole-catalog
      command; `set`/`patch`/`all`/`none` mutations preserve the intended mask.
- [ ] Generic merges, notification mutations, and existing-user credential
      patches share one filesystem-locked load→mutate→save transaction and
      preserve deterministic concurrent disjoint edits.
- [ ] Synchronous and asynchronous command submission reject overlength input
      before execution instead of executing a truncated prefix.
- [ ] Human `events kinds` output preflights complete lines and handles a token
      wider than the former 120-byte staging buffer without truncation or
      out-of-bounds pointer arithmetic.
- [ ] User-settings saves reject observable nonzero short output by exact
      serialized-count and file-size checks, without claiming an unavailable
      checked flush/fsync result.
- [ ] HTTP and command JSON use the same output adapter, and parsed JSON exactly
      matches typed-provider traversal.
- [ ] Existing `families[].n` / `families[].k` clients require no migration.
- [ ] Canonical snake_case names and within-family display order are unchanged.
- [ ] `boot`, `none`, `set`, `patch`, `all`, `list`, and numeric ids are absent
      from the wire.
- [ ] Too-small command buffers fail explicitly without truncated JSON.
- [ ] A generic authenticated test client reassembles the full catalog over the
      phone-companion Secure BLE command lane; Android-specific parser/cache UI
      acceptance remains Phase 4.
- [ ] The G2 boundary is documented as ESP32 page/transport code → G2 widget
      protocol/BLE → external glasses; existing history/summary adapters remain
      unchanged, and no catalog JSON is introduced on the temple links.
- [ ] No new persistent catalog/cache file exists.
- [ ] Normal profiles compile/link no device harness or fault-hook symbols;
      test firmware uses a separate build/artifact and is replaced by an
      ordinary fullclean build after device acceptance.
- [ ] The automated repository-local sequence passes, and every hardware,
      transport, external-client, or fault-injection row owned by the shipping
      phases has signed evidence and cleanup recorded in the device checklist.
- [ ] A source/reference search finds no obsolete private catalog builders.
- [ ] Affected as-built documentation is reviewed and accepted explicitly.

Phase 4's ESP32 G2 page-adapter extension/external lens interaction and Android
authoring UI are follow-on acceptance sets. They do not block shipping the
typed provider, JSON output adapter, or existing-consumer migration.
Phase 4 adds its own G2 acceptance: typed provider → ESP32 picker adapter →
force-recreate G2 page projection → external glasses, including ordinal/tap
mapping, exact echoed per-page container token, original RX-stamped identity
propagated without re-sampling, complete active/pending presentation promotion,
staged pre-/post-transition failure outcomes, post-commit raw late-delivery
rejection, exact stale-input fences, allocation failure, repeated paging, and
real-hardware plugin stability.

---

## 14. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Extraction changes persisted meaning | Keep names unchanged; numeric ids remain internal; round-trip every canonical name |
| Family index assumes contiguous declaration blocks | Generate explicit family-order index and assert exact coverage |
| A shared provider bug reaches every projection | Keep the API minimal/immutable; exhaustively test typed traversal and compare every output adapter to it |
| “One pipeline” becomes a universal UI state machine | Share typed identity/order/bounds only; keep OLED movement, G2 paginator/generation, and DOM state in their adapters |
| Manual serializer emits invalid JSON | Generic escape routine, exact-size preflight, parse/golden/failure tests |
| Human `events kinds` packing overruns or truncates a future long token | Route it through the checked whole-line text adapter; flush and retry only complete tokens, explicitly fail above the named transport limit, and test beyond the old 120-byte staging width |
| CLI catalog grows past 4 KB | Adapter budget test and explicit `BufferTooSmall`; add versioned pagination before raising the limit |
| HTTP stream fails after partial body | Return `SinkFailed` and abort/close; never append a valid-looking success tail |
| OLED catalog growth hides rows | Remove materialized fixed arrays entirely |
| OLED high-kind mask corrupts stack/settings | Mandatory Phase 0 256-bit repair plus sanitizer and preservation regressions |
| Full mute list exceeds `ExecReq::line` | Add bounded `set`/packed-`patch`/`all`/`none` mutations, migrate both UIs, and reject rather than truncate oversized sync/async input |
| Concurrent incremental settings edits lose unrelated fields | Put generic merge, notification-list mutation, and existing-user credential patches behind one reentrant-FS-lock transaction; hash/parse outside where possible; prove disjoint two-task interleavings |
| A positive partial JSON write is published as valid settings | Pre-measure compact JSON; require exact serialized bytes and observed post-flush file size before rename/success; inject nonzero short writes in temp and direct fallback paths, while documenting that the current void-flush API cannot prove durability |
| An old out-of-lock preference load repopulates cache after a save | Guard cache invalidation/publication with a dedicated generation, discard/retry mismatched miss loads, invalidate every failed/ambiguous save outcome, and prove the paused fill race without holding the cache mutex across filesystem I/O |
| A caller relied on the old truncated-prefix behavior | Audit every submitter; treat explicit rejection as the intended compatibility change and surface it at each origin |
| App mistakes catalog for capabilities | Document vocabulary-vs-availability; do not filter names by runtime producer state |
| Plaintext phone BLE is assumed to fragment | Require Secure Channel for v1 full fetch; explicitly test/label plaintext unsupported |
| UI caches become competing stores | Successful, ephemeral page/connection caches only; firmware provider remains authoritative |
| A formatted/truncated row becomes identity | Resolve typed ordinals under current state and persist only the full canonical name |
| Equal-count G2 picker pages take unsafe `REBUILD-list` path | Add a force-recreate page policy and prove adjacent equal-count/different-content pages use SHUTDOWN/500-ms-settle/CREATE on real glasses |
| Async G2 row/tap outlives its page | Give each event-picker page a unique bounded token in the existing ListObject container name and require its exact ListEvent echo before dedup; preserve that token and original RX connection/lifecycle/presentation stamps end to end without re-sampling; combine token/all-stamp freshness and dedup read/write under one ordered claim, coherently revalidate in the worker, and atomically revalidate token, arm generation, lifecycle/presentation, and page cookie again in the final adapter mutation claim; keep complete active/pending presentation records and a staged swap phase; suspend tap claims atomically at broker admission, resume active only for a proven pre-transition abort, retire it at `TransitionStarted`, promote the entire pending row map only from exact page-cookie committed success, publish each admitted-swap terminal before releasing its admission fence/job storage, handle authoritative/full-session idle revocation by epoch advance before adapter clear without touching a nonexistent swap owner, preserve the page across a non-authoritative arm-generation change, clear both after a physical-transition failure/recovery, and re-resolve the tapped provider ordinal before mutation; menu generation is supplementary only |
| External G2 is mistaken for a JSON client | Show the ESP32 adapter and G2 protocol boundary explicitly; add no temple catalog payload in this refactor |
| G2 scope balloons into an automation-editor rewrite | Ship provider first; design the ESP32 G2 create/edit adapter as a separate consumer feature |
| An on-demand scan blocks the main loop or lets later automation work run early | Implement it first as an asynchronous branching action with a bounded run/generation/cursor continuation; `Pending` short-circuits the command loop and defers later commands plus fired/end/reschedule bookkeeping to one exact-once finalizer; test cancellation invalidation and main-loop responsiveness, and never wait inside today's `evaluateCondition()` |
| A nominally one-shot WiFi scan leaves STA/radio enabled | Use a generation-checked radio lease that restores only state this request introduced; preserve connected/AP+STA/ESP-NOW owners and test off→scan→off plus ownership races |
| Last scan subscriber closes while the driver call is active | Replace the blocking helper contract with one coordinator-owned nonblocking scan state machine, bounded cancel/stop/join, exact-once completion, and no snapshot publication on cancel/timeout |
| A BLE scan silently changes phone/G2/R1 role or callback ownership | Use one fan-out/arbitration broker and an explicit role/session capability matrix; default to `BUSY_SESSION`/`UNAVAILABLE_ROLE` unless concurrent operation is proven, never silently persist/switch/disconnect, and verify callback/session restoration |
| Device fault hooks escape into production | Compile them only under a default-OFF CMake test profile, arm only through physical UART, auto-disarm, use a separate artifact, and reject their symbols in every normal link map |

---

## 15. Deliberately deferred decisions

These do not need to be guessed during the core refactor:

1. **Persistent app caching and revision hashes.** Add only with a versioned
   wire response when offline authoring is required.
2. **Plaintext BLE paging.** Design around negotiated byte budgets, cursors,
   and a versioned response; do not retrofit fields ambiguously into v1.
3. **Runtime producer availability.** If needed, add a separate capability
   layer. Do not delete canonical names from the catalog based on feature
   gates.
4. **Human display labels for every kind.** Web may continue humanizing names;
   constrained native interfaces may show canonical tokens. A universal label
   table is a separate flash/translation decision.
5. **Full G2/Android automation authoring.** The ESP32 G2 page adapter consumes
   the typed provider and projects G2 protocol frames to the external glasses.
   Android consumes the command/JSON contract. They require different
   interaction, session, and authorization tests.

---

## 16. Documentation work when implementation lands

This file lives in `docs/` because it is a future design. Do not mark `docs2/`
fresh merely for writing this plan.

After first-party source changes in **each implementation phase**, follow
`docs2/README.md` exactly before that phase ships:

1. run `python3 docs2/docsctl.py status`;
2. run `python3 docs2/docsctl.py update --changed`;
3. inspect `docs2/meta/update_queue.json`;
4. update every affected per-file and subsystem page;
5. run `python3 docs2/docsctl.py check --skip-browser --strict-new`;
6. accept only documents actually reviewed, with reviewer and reason;
7. rebuild the private browser;
8. run final `status` and `check` and report any remaining baseline debt.

Expected per-file documentation additions/updates include:

- new `files/System_EventCatalogRows.h.md`,
  `files/System_EventCatalog.h.md`,
  `files/System_EventCatalogCore.h.md`,
  `files/System_EventCatalog.cpp.md`,
  `files/System_EventCatalogJson.h.md`,
  `files/System_EventCatalogJsonCore.h.md`,
  `files/System_EventCatalogJson.cpp.md`,
  `files/System_EventCatalogTextCore.h.md`,
  `files/System_EventKindMask.h.md`, and
  `files/System_NotificationKindListCore.h.md`;
- `files/System_Events.h.md` and `files/System_Events.cpp.md`;
- `files/System_Automation.cpp.md`, `files/System_Notifications.h.md`,
  `files/System_Notifications.cpp.md`, `files/System_UserSettings.h.md`,
  `files/System_Settings.cpp.md`, `files/System_User.cpp.md`,
  `files/System_CommandLimits.h.md`, `files/System_CommandTypes.h.md`,
  `files/System_Utils.cpp.md`, and `files/System_UartLink.cpp.md`;
- `files/WebServer_Server.cpp.md` and `files/WebPage_Dashboard.h.md`;
- `files/OLED_Mode_Automations.cpp.md` and `files/OLED_Utils.cpp.md`;
- `files/test_event_catalog.cpp.md`,
  `files/test_event_catalog_json.cpp.md`,
  `files/test_event_catalog_text.cpp.md`,
  `files/test_event_kind_mask.cpp.md`,
  `files/test_notification_kind_list.cpp.md`, and
  `files/test_command_limits.cpp.md` for the six Phase 0–2 recursively
  discovered host C++ sources;
- `files/G2_EventPickerCore.h.md` and
  `files/test_g2_event_picker_core.cpp.md` when Phase 4 lands;
- `files/System_EventCatalogDeviceHarness.h.md` and
  `files/System_EventCatalogDeviceHarness.cpp.md` for the gated device harness;
- `files/BUILD_SYSTEM.md` for the host CMake/CTest registrations and sanitizer
  coverage, default-OFF device-test profile/scripts, and harness-symbol absence
  checks as well as the new restoring board script, object checks, and
  temporary-profile artifact cleanup;
- `files/G2_Page_Automations.h.md`,
  `files/G2_Page_Automations.cpp.md`, `files/G2_Glasses.h.md`,
  `files/G2_Glasses.cpp.md`, `files/System_G2_Protocol.h.md`, and
  `files/System_G2_Protocol.cpp.md` only if Phase 4 changes those files.

Expected subsystem updates include:

- `systems/diagnostics-events-notifications.md`;
- `systems/settings-registry.md`;
- `systems/identity-and-accounts.md` for the user-settings transaction contract
  and existing-user credential writer migration;
- `systems/automation-engine.md`;
- `systems/command-pipeline.md` and `systems/uart-host-link-and-cm5.md`;
- `systems/web-server-core.md` and `systems/web-pages.md`;
- `systems/oled-screen-modes.md`;
- `systems/g2-lens-apps.md` and `systems/g2-glasses-core.md` when the ESP32 G2
  picker/transport projection exists;
- `systems/bluetooth-gatt-server-and-links.md` in Phase 2 when the generic
  Secure-BLE catalog command is verified; do not wait for Android and do not
  describe that GATT-server lane as the G2 BLE-Central lane.

Keep the as-built docs explicit: catalog storage is immutable metadata, the
typed provider is its one semantic view, JSON and UI rows are projections, the
48-entry ring is runtime occurrence history, and interface caches are temporary
views—not additional sources of truth.
