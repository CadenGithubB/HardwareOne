# G2 wear detection on firmware 2.3.2.14

## Scope and observed failure

This patch is confined to the typed `g2glasses` settings path and its codec.
The user requested `g2glasses weardetect off` for the connected G2 pair.
The active firmware is built from the `stt-speedups` checkout with the saved
`p4-glasses-test` profile. It retains the 15x5 Gen2 STT model, SDMMC, ESP-SR
being disabled, and the 800 ms Live text page.

The initial attempt encountered two separate restrictions:

- `pluginDead` (EvenCore display-app silence) was reported as RIGHT being
  disconnected, although native session heartbeats continued to receive ACKs.
- After reconnecting, the command refused firmware 2.3.2.14 because the generic
  settings allowlist covered only 2.2.4.34 and 2.2.6.10.

The first BASIC read in the reconnect capture received a 44-byte response in
about 70 ms, including battery and both exact firmware versions. A subsequent
read with the same magic 208 received no response within the wait. Further
reads with magic 208 also received no response. The failed refresh cleared the
cached snapshot; its missing result was not proof of a changed protobuf schema.

Private local evidence (not committed):

- `/private/tmp/hw1-wear-reconnect-1791747865238529000/serial.log`
- `/private/tmp/hw1-g2-control-1791748948591192000/reply.txt`
- `/private/tmp/hw1-settings-read-1791748983259879000/settings-read.txt`

## Primary archived firmware evidence

Offline inspection of archived stock G2 2.3.0.24 found a settings-specific
deduplication check which compares the incoming magic against the previous
magic and returns before dispatch on equality. It has no time window and
applies to reads as well as writes.

The nanopb package, settings-write, wear-inner and full-settings-reply
descriptors match archived 2.2.6.10 byte-for-byte. The wear setter still uses
f3 / f5 / f1. The full response builder always copies the configured wear
value into f4 / f10, but the protobuf encoder omits default zero scalars.
Thus an OFF ACK can contain `1a 02 2a 00`, and a full OFF read can omit f10.
This explains why requiring literal `08 00` or a presence bit would reject
a valid OFF result. Static evidence ends at 2.3.0.24; it does not replace
physical qualification of the connected 2.3.2.14 pair.

Primary archive: `/Users/cadbecaimacmini/Documents/ChatGPT/Even Realities Software Archive/`.

- `firmware/g2/g2-2.3.0.24-1dbdf37b03a1169c384945e94d671371.bin`:
  SHA-256 `187ccf2bcc5c17a212106e8a376745511e8289c4232b634a7ea94b9bf25a0979`.
- Extracted `ota/s200_firmware_ota.bin`: SHA-256
  `c07813ceaf7ed16502e94406c957eb8cf9d6e8a94ac4ba9bafaefa50ebc9ff46`,
  load base `0x437fe0`.
- Duplicate-magic check `0x4a9f0c`; parser dispatch `0x4aa09a`.
- Wear dispatch `0x46bfca -> 0x46c5a6 -> 0x4add6a`.
- Package/write/wear/read descriptors `0x7adb98` / `0x7adc70` /
  `0x7adc10` / `0x7adc88`.
- Full-status wear copy `0x4aa464`; ACK re-encoding `0x4aa1f6`;
  encoder default omission `0x49fba2` and zero test `0x49f8fe`.

## Patch boundaries

- Native settings use the existing promoted RIGHT connection readiness and
  generation checks, independently of the display-app watchdog.
- Settings messages receive changing request IDs. Fresh read responses and
  write ACKs are matched to RIGHT, its connection generation, the request ID
  and the expected payload; a successful BLE send alone is not success.
- Firmware 2.3.2.14 is admitted specifically for the wear-detection leaf
  (`DeviceReceiveInfoFromAPP` f3 / wear f5 / switch f1), with both temples
  reporting the exact version and a fresh correlated BASIC response required
  before writing. Other settings retain their existing compatibility rules.
- A missing wear field in an arbitrary/partial push remains unknown. The
  supported full BASIC reply uses a default-eliding scalar: absent f10 means
  zero only after the complete correlated response and exact firmware pair
  qualify that schema. An explicitly present empty wear submessage in the
  write ACK likewise encodes OFF. Silence is never confirmation.
- Settings parsers validate the entire payload before publishing results;
  malformed suffixes cannot turn a partially decoded value into confirmation.

## Verification status

Implementation and focused codec/command tests pass under ASan/UBSan.
The tests exercise production parsers, command handling, exact firmware sends
and connection-generation fences. Regression coverage includes empty OFF ACKs,
missing fields in partial updates, conflicting protobuf bodies, malformed version
strings, late/foreign/wrong-value ACKs, cold discovery with a partial cache, and
a refresh followed immediately by an unsolicited update. Existing connection,
client lifecycle, hijack epoch and TL send tests also pass.

Both required firmware builds passed:

- Saved `p4-glasses-test`: 5,458,176 bytes, 919,296
  bytes free in the actual device application slot (0x615000).
- Baseline `p4x_eye`: 6,653,472 bytes; built only and not selected for flashing.
- Selected app SHA-256: `e6df3d1245a26e06ce2e2066e933acfda1ab37d28c151c9ece49a76926fc6bcf`.
- Selected ELF SHA-256: `1fa2847c116e9d110e5e3718d26c48a9aa957b5dcda1a8a2c2d127862f64d8b1`.
- Saved profile still resolves ESP-SR=0, SDMMC=1, 15x5 Gen2 model=1.
- Previous deployed `live-speaker-paragraphs-15x5.bin` remains the rollback.

App-only flash at 0x10000 passed esptool hash verification. No-reset startup
capture matched ELF prefix `1fa2847c1`, mounted LittleFS and SDMMC, completed
setup, and brought both temples to Ready. Existing partitions, bootloader,
filesystems and SD model were preserved.

## Hardware qualification — OFF confirmed

After the user signed in locally, the exact 2.3.2.14 pair accepted the command.
RIGHT had already been marked plugin-silent at 38,370 ms; native settings worked
at 49,644 ms, directly exercising the corrected readiness gate.

- Two consecutive BASIC reads with magic 1 and 2 both answered, and reported
  wear ON. The pre-write read with magic 3 again reported ON and both exact
  firmware strings.
- OFF TX protobuf: `08 01 10 04 1a 04 2a 02 08 00`.
- RIGHT flag-0 ACK: `08 01 10 04 1a 02 2a 00`, about 41 ms later. This is a
  real 2.3.2.14 capture of the previously static-derived empty wear OFF ACK.
- Correlated full BASIC replies with magic 5 and 6 omitted f10 while retaining
  both exact firmware versions and the other settings. The OFF response body
  was two bytes shorter than ON, as expected for omitted scalar zero.
- Command result: `weardetect = 0 confirmed by fresh RIGHT readback and matching ACK`.
  A separate explicit refresh printed `weardetect off`.
- Debug verbosity was restored to OFF. No other setting was written.

Wear detection is OFF. ON-to-OFF transition, matching ACK, repeated fresh reads,
zero omission, and operation with the renderer idle were verified. The reverse
ON write and survival through a glasses power cycle were not exercised.

Private capture: `/private/tmp/hw1-wear-validate-1791749709028093000/`.
Durable private copies use prefix `wear-detection-23214-` in
`/Users/cadbecaimacmini/Documents/Codex/Scratch/hardwareone-p4-backup-20261011/`.
