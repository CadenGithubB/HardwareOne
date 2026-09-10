# Filesystem and G2 PSRAM implementation — 2026-09-05

Implemented against the existing working tree, preserving unrelated edits. No
device was flashed or restored. These are internal-heap allocation changes,
not a claim of fixed BSS savings or measured physical free-heap improvement.

## Changes and compatibility boundaries

- `System_PsramBuffer.h`: caller-owned, NUL-terminated, checked growing output.
  All allocations use the existing `PreferPSRAM` policy with internal fallback.
  Size/overflow/allocation failures stay sticky until explicit reuse; partial
  output must not be sent as success. Small owner metadata remains internal.
- Filesystem listings: one output allocation instead of an ordinary String
  array body plus full envelope. HTTP owns its reply per request; commands have
  private persistent storage. The text command uses a request-local buffer and
  the existing `const char*` broadcast path. Public String APIs remain available.
- `fileread`: persistent 4096-byte preferred-PSRAM reply, direct escaping/base64,
  no temporary base64 String or oversized escaped-text retry. Exact escaped-path
  budgeting preserves ordinary windows and reduces unusual-path windows only
  where needed. Consumers must continue advancing by returned `len`.
- G2 Files: checked local preferred-PSRAM input and PSRAM JSON document. Raw text
  and pretty JSON stream into the existing final wrapped body without a display
  String. The reader's 12 KiB cap, pages, raw parse-error fallback, paired-user
  identity and read authorization remain in place. Existing shared readers and
  wrappers used by CLI/OLED/Settings/chat/events keep their APIs and semantics.
- Capture presentation decoding: preflight capacity includes expansion of short
  damaged encrypted rows into `[undecryptable row]`. Moving unread input out of
  the way prevents overwriting the tail. A checked pointer/length/capacity
  overload rejects insufficient storage without mutation; the existing String
  overload remains available and checks growth. Key storage and cryptography
  are unchanged. Raw downloads are never automatically decrypted.

Listings retain complete results, field types/key order, SD mount/prefix/count
behavior, admin filtering and the existing lock-bound permission queries. The
toolbar permission query still runs after traversal in the same permission
scope. No network transmission occurs under that traversal lock. Control bytes
in filenames are now escaped to valid JSON rather than emitted literally.

Commands retain the 4096-byte result budget including NUL. Oversized command
listings return an explicit error, not truncated success. HTTP listings are not
capped to that command budget. New HTTP allocation failures return HTTP 500
with a complete error envelope; ordinary missing/denied-directory and explicit
admin-precheck status behavior is unchanged.

BLE raw file responses still deliver the body before metadata. The metadata is
now fully allocated/formatted before the body is sent. Zero-length EOF and the
existing failed/unavailable binary-send fallback are preserved. Command
dispatch, session handling, `/api/files/read`, ESP-NOW binary listing, and
migration backup/restore endpoints are not rewritten.

## Configuration defect caught by the builds

The HTTP-off build exposed an existing omitted dependency: `ENABLE_WEB_POWER`
was not reset alongside the other web-page flags. It now becomes zero in the
existing `!ENABLE_HTTP_SERVER` block. Power-management behavior is unchanged.
A new test preprocesses the actual header for 100 network/web/custom-WiFi/HTTP
combinations. The coarse CMake HTTP filter still includes some guarded-out web
translation units in custom HTTP-off profiles; changing its migration-server
inclusion rules is separate work, not necessary for this correctness fix.

## Verification

All new regression suites pass with AddressSanitizer/UndefinedBehaviorSanitizer:

| Suite | Coverage |
| --- | --- |
| `fileread_psram` | Actual handlers/encoders, BLE on/off, mocked external/internal allocation, exact response boundaries, byte roundtrips, OOM/retry, offsets/EOF, raw send order |
| `g2_files_psram` | Actual reader/viewer/reveal code, real ArduinoJson and wrap/pager, PSRAM/no-PSRAM builds, malformed rows, bounded JSON parity, read/page caps |
| `filesystem_psram_listing` | Actual walk/builders/HTTP/command wrapper, text/JSON parity, permission-query order/lock boundaries, SD/escaping, ownership interleaving, allocation failure |
| MemUtil tests | Real allocator and buffer against mocked heap capabilities, fallback/bypass/no-PSRAM, growth/self-append, NUL limits, failure/reuse |

The complete application host suite has 45 tests and passes with sanitizers
enabled. The command-ingress guard follows the by-value executor queue item,
and the generated command reference carries the paginated-help example. The
real listing-permission source contract passes separately from the mocked
permission-view harness.

Existing consumer checks also pass: 14 Python OTA backup tests, 38 web UI tests,
and all 36 migration-tool checks with its optional firmware-contract check.
The migration tool uses complete `HWBACKUP` bundles, not these listing/file-read
APIs, so it needs no new protocol or backup format for the PSRAM changes.

Firmware checks use disposable source/config snapshots:

| Configuration | Scope/result |
| --- | --- |
| FeatherS3 current feature defaults | Full compile/link/image passes; G2/BLE/HTTP/HTTPS/OLED/MQTT/ESP-NOW/UART enabled |
| Feather ESP32 V2 current feature defaults | Full compile/link/image passes across the same interface families |
| FeatherS3 HTTP/Bluetooth off | Full compile/link/image passes after the web Power dependency fix; OLED/MQTT/ESP-NOW/UART retained |
| ESP32 headless, genuine PSRAM-off Kconfig | Changed filesystem/crypto/web/allocator translation units compile; this is not a full no-PSRAM firmware link |

See the [build verification record](/private/tmp/hw1-filesystem-build.31tUL2/README.md)
for exact source/configuration hashes, compiler macros, artifacts, initial
sandbox/cache failures and successful retries. No live build configuration or
release key was changed for the matrix.

### XIAO follow-up verification

The current working tree was also checked on the XIAO ESP32-S3 using isolated
source/config snapshots. No firmware source or live configuration changes were
needed, and nothing was flashed or published.

| Configuration | Result | Image / available margin |
| --- | --- | --- |
| Ordinary XIAO Sense, current shared feature defaults | Full compile/link/image PASS; camera/PDM/HTTP/HTTPS on, Bluetooth/G2 off as specified by the board SDK defaults | 4,843,200 B; 1,141,056 B free in factory app partition (19.07%) |
| `pocket_assistant/xiao_s3` main | Full compile/link/signed-image PASS; Bluetooth/G2/R1/HTTP/CM5 on, onboard camera/PDM/I2C/OLED off | 5,181,440 B; 716,800 B free in OTA slot, 323,584 B below stricter release cap |
| `pocket_assistant/xiao_s3` recovery updater | Full compile/link/signed-image PASS; PSRAM deliberately disabled | 856,064 B; 315,392 B below factory/release limit |

The paired OTA audit also passes identity, layout, signing-key agreement,
signature, security and size gates using a disposable audit-only key. The
generic IDF warning that the main image does not fit `factory` is expected:
that partition holds the smaller updater, while the main belongs in `ota_0`.
Both main profiles resolve to 8 MB flash and 80 MHz octal PSRAM. The ordinary
build retained the current I2C/OLED/gamepad selection rather than using a slim
test profile. The Pocket Assistant SDK overlay was verified to keep Bluetooth
and G2 genuinely enabled, not merely requested by the feature header.

Fresh ELF inspection confirms the 3,100-byte JPEG decoder `work` buffer is in
`.ext_ram.bss` at `0x3c52e9fc` in Pocket Assistant, where G2 JPEG viewing uses it.
The ordinary G2-off/Edge-Impulse-off Sense image does not link that decoder or
workspace at all, so no 3,100-byte saving is claimed for that profile.

The seven relevant host suites (filesystem listing, fileread, G2 Files, both
MemUtil profiles, HTTP gates and OTA replies) and ten deployment-contract tests
also pass on this follow-up. XIAO SD reads use CPU SPI copies into the caller's
storage, not DMA into the new PSRAM buffer. Hardware transfer/render tests
remain outstanding. Dynamic internal-RAM fallback is preserved, but the
existing main firmware's external BSS/noinit configuration already requires
working PSRAM at boot; this check does not claim runtime survival of a failed
PSRAM chip.

Detailed artifacts: [ordinary XIAO report](/private/tmp/hw1-xiao-filesystem-check.yTfXkn/README.md)
and [Pocket Assistant paired-build report](/private/tmp/hw1-xiao-deployment-check-20260905.Y9XNQJ/README.md).

## Remaining device validation / separate follow-ups

- No default pagination was introduced. The Python backup client, web consumers
  and Android listing parser still expect complete directory results.
- A read-only Android check confirmed compatible listing/file/map response
  parsing. It also found a **pre-existing** media bug: `requestMediaWindow`
  requests `bin`, but `onMediaReadCapture` does not consume `enc:"raw"` bodies.
  The Maps path already does. The Android repo was not edited.
- Real BLE multi-chunk transfers/reconnects, G2/OLED presentation, simultaneous
  HTTP/command traffic, SD I/O failures and physical PSRAM exhaustion still need
  hardware validation. Host mocks are not hardware or full task-race tests.
- Compare internal free heap/largest block before, during and after repeated
  listings, raw/pretty G2 viewing, malformed capture viewing and file downloads.
  Savings depend on content and whether allocations actually use PSRAM;
  boards without it intentionally fall back to internal RAM.
