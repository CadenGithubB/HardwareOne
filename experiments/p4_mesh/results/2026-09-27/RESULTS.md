# HardwareOne application milestone — 27 September 2026

**The actual shared HardwareOne application runs on P4+C6 and the native S3
with ESP-IDF 5.5.5. The complete acceptance matrix does not yet pass.** Encrypted
text, explicit secure peer setup and session recovery work; discovery pairing
and S3-to-P4 file delivery still have reproducible failures.

All application integration changes were made in the ignored application copy.
The durable experiment contains reviewable patches, tools and results. The
original application checkout and its 16 pre-existing edited/deleted paths were
preserved. No Wi-Fi network credentials were supplied and neither board joined
an access point. No efuses were changed or security provisioning enabled.

## Tested hardware and firmware

- ESP32-P4X-EYE: P4 rev 3.2, 400 MHz, 16 MB flash, 32 MB PSRAM; onboard C6 radio.
- XIAO ESP32-S3 Sense: S3 rev 0.2, 8 MB flash, 8 MB PSRAM; native radio control.
- Shared minimal HardwareOne profile, Arduino-ESP32 3.3.5 with documented
  compatibility patches, ESP-IDF 5.5.5, Hosted 2.12.13.
- C6 firmware is unchanged from the successful transport milestone.

Exact binary sizes, SHA-256 hashes and source input hashes are in
[firmware.json](firmware.json). See [BUILD.md](../../BUILD.md) for reproduction.
Both EYE USB cables were connected; testing used its Debug endpoint. The other
USB connector does not expose the C6 ROM loader directly.

## Results on the final images

| Check | Result | Evidence / limitation |
| --- | --- | --- |
| Boot, PSRAM, CLI authentication, settings and LittleFS | Pass | Real application, provisioned as Meshed Node |
| Mutual authenticated discovery | Pass | Each board lists the other |
| Fresh request/accept pairing | **Fail** | Final S3 request did not reach P4 pairing UI; earlier P4-initiated acceptance also failed |
| Explicit secure peer setup | Pass | Reciprocal local send registries, pinned identities and matching ACTIVE session |
| Encrypted TEXT both directions | Pass | 1, 201, 202, 203, 400, 401 and 1024 bytes, exact reconstructed contents |
| Oversize TEXT rejection | Pass | 1025 bytes rejected on both; no partial received TEXT |
| 8 KiB binary file P4 → S3 | Pass | Receiver completion plus full byte-for-byte readback and SHA-256 |
| 8 KiB binary file S3 → P4 | **Fail** | Receiver got 40/41 chunks, 7992/8192 bytes; correctly rejected incomplete file |
| Session rekey | Pass | Both confirmed new keys; 401-byte encrypted delivery both ways after 5.5 s, beyond the 5 s old-key grace |
| P4 radio close/open | Pass | Retained session remained usable; 401-byte encrypted delivery both ways |
| Both boards rebooted | Pass | USB reset logs and increased NVS boot counters; saved identities/configuration/peers unchanged |
| Session after both reboots | Pass | New matched session; 401-byte encrypted delivery both ways |

The main run [application.json](application.json) has 25 passed checks and the
failed reverse file check. It used **explicit secure pairing**, so it cannot be
counted as a request/accept success. [discovery.json](discovery.json) records the
separate failed fresh-pairing run. Because the main run stopped at the file
failure, recovery was tested separately: [recovery.json](recovery.json), all 14
checks passed, preserving the saved pairing.

[persistence.json](persistence.json) separately proves both USB reopen resets
using fresh ROM/reset/application startup logs and NVS boot-counter increments.
It compares public identities, creation metadata, mesh configuration and
reciprocal saved peer registries before any manual session initiation, then
requires a new matched session and encrypted delivery. This is stronger evidence
than merely seeing a board boot. It does not test abrupt power loss during a
filesystem write or an independently failed C6.

At the end of the main run, both application RX-ring drop counters remained
zero. S3's aggregate failed-send counter rose from zero to one. That is not
enough to identify the missing frame or the layer that lost it. Full-app bridge
counters print at startup only, so those startup values cannot establish zero
bridge drops throughout this run. The earlier transport-only milestone did
exercise and collect its bridge counters separately.

## What the failures tell us

The PAIR_ACCEPT path sends one broadcast without an application acknowledgement
or retry and ignores the immediate send result. One diagnostic S3 build logged
an accepted opcode-41 send while P4 never completed its local registry. The
absence of its later type-dispatch log cannot distinguish radio loss from an
earlier validation drop. A focused S3-request/P4-accept test succeeded, but the
final repeat lost the request; this is not a dependable directional workaround.

Verbose tracing also confirmed a shared handshake race: two KEY_EX_HELLO replies
led to SESSION_OPEN 56916 and then 61967. The second replaced the first pending
session; CONFIRM for 56916 then had no matching in-flight state. A later session
converged. No handshake or wire-format changes were made to bypass this.

The file sender reported success while its receiver rejected the incomplete
file. Before the MAC-cache optimization, a reverse 8 KiB file also failed with
39/41 chunks. The final result improved to 40/41, but these individual runs do
not establish a causal improvement. Reliable file completion needs explicit
receiver confirmation and missing-chunk recovery, independently of CPU family.
The existing generic ACK is emitted before file validation/storage; waiting for
that ACK alone would still permit a false success. The failed-send counter is
aggregate and cannot tie its one failure to the missing chunk in this trace.

Selected sanitized serial evidence is in
[evidence-excerpts.json](evidence-excerpts.json); full redacted transcripts and
private build artifacts remain under the ignored `private/` directory.

## Port direction

IDF 6 is not required for this tested scope. Most application logic can remain
shared: command handling, identities, cryptography, sessions and messages all
interoperate across native and hosted radios. The important distinction is the
radio backend and its latency/failure behavior, together with board wiring and
target capabilities.

The copy-only overlay configures the EYE's SDIO/reset pins and Arduino Hosted
lifecycle, gates inactive dependencies, and fixes P4 build compatibility. Shared
RX handling keeps callback MAC data by value; a checked local identity cache
avoids repeated synchronous C6 calls. These changes preserve the wire protocol.

The next production work should address acknowledged/idempotent pairing,
single-flight session establishment and receiver-confirmed file transfer,
then package native/companion radio backends and board profiles. Power frequency
policy, PSRAM capability checks, C6 fault recovery and coordinated updates also
need qualification. Camera, display, audio, Bluetooth, optional sensors, OTA,
secure boot and flash/NVS encryption remain outside this milestone. Detailed
seams and findings are in [PORTING.md](../../PORTING.md).

## Device state and preservation

The connected boards retain the experiment firmware, local test administrator,
shared test mesh credentials and generated test files. They are configured on
channel 6 and paired. Credentials are ignored, mode 0600 and absent from these
deliverables. Original P4, C6 and S3 whole-flash backups remain preserved under
`experiments/p4_espnow/private/backups/`; restore instructions are in that
experiment's README. The P4 test filesystem was initialized only after its
original whole-flash backup was verified.

## Verification of the deliverables

Both final firmware builds passed. Cumulative patches applied to separate
baseline copies with zero fuzz and matched the built source byte-for-byte.
All 44 offline tests passed, including serial completion/redaction, protocol
evidence parsing, reboot-proof validation and the extracted shared MAC cache.
Final binary/input hashes were rechecked, saved results were scanned for the
generated secrets, and original edited/deleted application paths were compared
against their initial SHA-256 snapshot.
