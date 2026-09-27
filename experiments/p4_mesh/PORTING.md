# What this means for a production port

The experiment builds the same HardwareOne mesh application for the native S3
radio and the P4's C6 radio. ESP-IDF 5.5.5 is sufficient for these builds and the
on-device mesh work. Moving to IDF 6 is a separate migration, not a prerequisite
established by this experiment.

Keep the application protocol, authentication, identities, sessions, filesystem
commands and message handling shared. Separate the following concerns:

| Concern | Appropriate home |
| --- | --- |
| Native or companion radio, initialization, transport health | Radio backend |
| EYE SDIO wiring, C6 reset/boot pins, flash and PSRAM configuration | Board profile |
| Available CPU frequencies and power policy | Target capabilities / power backend |
| Camera, display, audio, touch and storage drivers | Optional peripheral backends |
| Message history size and queue capacity | Explicit memory policy |
| P4 and C6 firmware compatibility, update and recovery | Coordinated firmware lifecycle |

`radio_backend.cpp` is a deliberately small integration seam. On S3 it does
nothing. On P4 it sets the EYE wiring before using Arduino's own Hosted lifecycle
helper. This is necessary because the generic Arduino P4 variant supplies
different SDIO pins. The experiment then lets normal HardwareOne/Arduino code
initialize Wi-Fi, AP+STA and ESP-NOW.

The shared RX queue now keeps the callback's destination MAC by value. The old
code retained whether the destination was broadcast, then queried the local STA
MAC on every drain pass. On P4 that meant a synchronous C6 request even with an
empty queue: the first application log showed about 40 requests/second, averaging
14.82 ms each. Retaining the callback data avoids that remote call and also
preserves the exact AP/STA destination. This does not change the wire format.

A second shared optimization caches the checked STA identity for `isSelfMac`.
That helper runs repeatedly during routing and RX; each old call fetched the MAC
again, which becomes a blocking RPC on P4. Initialization now checks the query
result, publishes the cache before RX admission, and invalidates it only after
successful shutdown joins. A short lock protects publication and readers. Calls
outside an active radio instance retain a checked driver fallback. This keeps
native and hosted identity semantics shared without changing message framing.
Other MAC-query sites still need a broader remote-API audit.

Arduino initialization also resets IDF log levels after Hosted's constructor
has configured them. The experiment restores the three noisy RPC tags to WARN;
warnings and errors remain visible.

## Remaining work

- **Power policy:** the existing application asks for 240 MHz at startup. The
  P4 Arduino implementation rejects that request and remains at its configured
  400 MHz. Qualify power modes using target-supported frequencies and check
  change results.
- **Memory capability checks:** `System_ESPNow.h` still uses older PSRAM config
  symbols. P4 consequently gets 16 history entries per peer while S3 gets 250,
  despite P4's larger PSRAM. Use the current capability or a deliberate memory
  budget. Both retain the same 1024-byte TEXT limit; the test reads each message
  immediately, including its six fragments at that limit.
- **Remote API failure handling:** check MAC, channel and peer-count query
  results before consuming output. Native calls that used to be cheap and
  effectively local can fail or block across the companion transport.
- **Companion lifecycle:** qualify unexpected C6 resets, unplug/recovery and
  coordinated P4/C6 firmware updates. A successful normal radio close/open or
  host reboot does not prove recovery from a failed companion.
- **Peripheral support:** the shared mesh profile disables camera, display,
  microphone, Bluetooth, sensors and other optional features. None is qualified
  by this milestone.
- **Build/dependency policy:** retain per-target locks and capability-gated
  components. The isolated build had to exclude inactive peripheral libraries
  and repair unguarded optional-feature references; it is not a complete
  production deployment profile.
- **OTA/security provisioning:** the image chip-ID mapping is correct for P4,
  but production OTA partitions, rollback, companion updates, secure boot and
  flash/NVS encryption remain untested. No efuses were changed.

## Shared mesh reliability findings

The first discovery/request/accept attempt left only the S3's local pairing
registry complete. Both sides acquired peer identities and later established a
session, but ordinary sends from P4 returned `not found` until its local peer
record was explicitly added. The current PAIR_ACCEPT path sends one broadcast,
ignores its send result, and has no acknowledgement/retry. The exact cause of
that lost acceptance is not established. A diagnostic S3 build in
`private/pair-trace-20260927T113402/` reported opcode 41 `sent=true`, with no
immediate `esp_now_send` error; P4 still lacked its peer record and a type-41
dispatch log. Both application RX-ring drop counters stayed zero. This rules out
an immediate native send rejection for that attempt, but not delivery loss or an
early validation drop: the normal type log follows header, fingerprint and HMAC
validation. Neither the bridge nor the opcode dispatch table excludes type 41.
Check reciprocal registries rather than treating the accepting board's success
message as completion. The temporary diagnostic source was removed from the
final overlay; its binary and build log remain under `private/`.

The session handshake race is now confirmed by the verbose trace in
`private/pair-trace-20260927T112731/`: S3 sent two KEY_EX_HELLO messages,
then two SESSION_OPEN messages for session IDs 56916 and 61967. The second
replaced the first establishing session, and CONFIRM for 56916 logged
"no in-flight session" before 61967 succeeded. Pairing starts key exchange both
through its queued initial heartbeat and explicitly; repeated key-exchange
replies with pending data can each replace the establishing session. This is a
shared application race, independent of whether the radio is native or hosted.
Reliable, idempotent pairing and handshake initiation deserve separate shared
protocol work. This investigation does not weaken authentication or substitute
a test protocol to bypass these observations.

Pairing is not reliably directional. A focused S3-request/P4-accept trace did
complete both registries, but the final-cache repeat discovered both boards and
then lost the S3 request before the P4 pairing UI. Both application RX-ring drop
counters were zero. The final message/file matrix therefore used explicit
secure peer setup, and does not qualify the discovery request/accept workflow.

File delivery also remains unqualified. On the final images, all tested TEXT
sizes worked in both directions and the P4-to-S3 8 KiB file passed byte-for-byte.
The reverse file reached P4 with 40/41 chunks (7992/8192 bytes), and its receiver
correctly rejected it while the sender reported success. Both RX-ring counters
were zero; S3's aggregate failed-send counter increased by one. The file path
needs a receiver-confirmed result and missing-chunk recovery. The shared MAC
cache did not eliminate this loss; it must not be presented as a reliability
fix. See the result report for the exact firmware and run evidence.

The existing generic packet ACK is not a file-commit acknowledgement: encrypted
RX emits it after authentication/decryption but before FILE_END completeness,
CRC and storage checks. FILE_DATA does not request generic ACKs, retries only
immediate send rejection, and the later radio callback increments an aggregate
failure counter without retaining transfer/chunk identity. All chunks also
reuse a transfer ID and generic fragment index zero. A future shared file
protocol should correlate encrypted acknowledgements with peer, transfer ID,
stage and chunk index, retry missing chunks within bounds, and report success
only after receiver commit. Wire type names FILE_ACK, FILE_PROGRESS and FILE_NACK
already exist; implementing them requires compatibility review. Merely adding
ACK_REQ to FILE_DATA/FILE_END is insufficient.

The initiator's later outgoing-request timeout narrows the first pairing failure
further: successful entry into the deferred-completion enqueue would already
have cleared that pending flag. Thus a full deferred command queue after an
accepted frame is not the explanation for that run; an earlier frame loss,
validation drop or allocation failure remains possible. The previous RX path
already reconstructed broadcast destinations correctly, so the MAC-copy cleanup
is not evidence of fixing the pairing failure.

For a failing repeat, enable `loglevel debug`, `debugespnowcore on` and
`debugespnowrouter on`, and compare `espnowstats json` (especially `rxRingDrops`)
before/after. Require reciprocal registries, then sessions and encrypted messages.
Restore ordinary debug settings afterward. Arduino's STA_START also reapplies
its cached modem-sleep setting, explaining a ps0-to-ps1 log transition. That alone
does not establish RX loss: IDF's default connectionless RX mode keeps RF active
without non-Wi-Fi coexistence. Qualify that policy separately.
