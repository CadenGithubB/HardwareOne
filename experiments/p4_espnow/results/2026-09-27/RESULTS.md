# P4/C6 ↔ S3 transport milestone: passed

On 27 September 2026, the ESP32-P4X-EYE exchanged ESP-NOW packets with the
XIAO ESP32-S3 Sense through the EYE's onboard C6. All **26 test steps passed**.
The run used ESP-IDF **5.5.5** on all three chips and ESP-Hosted **2.12.13**;
IDF 6 was not required. No SSID, Wi-Fi password, or access-point association was
used.

The P4 runs application/test logic and an ESP-NOW API adapter. Requests travel
over SDIO to native ESP-NOW on the C6; receive/send callbacks return to the P4.
The S3 uses its native radio as the control. Both EYE USB cables were connected,
but programming and control used its Debug USB endpoint. Neither USB connector
provided direct C6 programming; the temporary P4 programmer used the internal
UART/reset/boot wiring.

## Measured delivery

There were **1,540 successful round trips out of 1,540 normal attempts**, with
zero unexpected timeouts, rejected sends, invalid payloads or late echoes.
Each reply was checked against its message ID, source/destination, length, CRC
and deterministic payload. The remaining **20 attempts intentionally timed out**
with the S3 silent or on a different channel, then communication recovered.

| Main run | Replies | Mean RTT | 99th percentile | Maximum RTT |
| --- | ---: | ---: | ---: | ---: |
| 32-byte frames | 100/100 | 10.00 ms | 16.92 ms | 18.78 ms |
| 64-byte frames | 100/100 | 11.07 ms | 18.81 ms | 19.61 ms |
| 128-byte frames | 100/100 | 12.45 ms | 21.42 ms | 22.70 ms |
| 250-byte frames | 100/100 | 16.29 ms | 27.48 ms | 29.73 ms |
| 250-byte sustained run | 1,000/1,000 | 16.19 ms | 32.47 ms | 57.42 ms |

Seven recovery/peer-test runs contributed another 140 successful 250-byte
round trips. The maximum RTT across all successful runs was **57.418 ms**,
below the probe's 200 ms deadline, chosen to match HardwareOne's fragment-ACK
budget. Requests were sequential with a 20 ms gap after each reply; this is a
latency/delivery measurement, not a maximum-throughput benchmark.

## Behavior verified

- Bidirectional broadcast discovery and unicast request/echo delivery.
- Real C6 peer-table queries, existence, modification, deletion and restoration.
  Duplicate and missing-peer operations returned the native error codes.
- Filling the native table reached 20 entries and returned `ESP_ERR_ESPNOW_FULL`;
  all 18 temporary entries were removed and the two real entries remained.
- Ten silent-peer timeouts and ten channel-mismatch timeouts completed without
  a hang; normal delivery resumed after each condition was removed.
- Successful delivery on channels 6 and 11, ending on channel 6.
- S3 radio-stack restart, plus two complete Hosted/Wi-Fi/ESP-NOW restarts on
  the P4, each resetting the C6 and rebuilding peers before successful delivery.
- Final bridge counters: zero malformed messages, stale responses, dropped
  events, request timeouts and transport errors. Both probe callback queues
  reported zero drops. TX callbacks included expected radio failures during
  the channel-mismatch test.

The automated run lasted approximately 78 seconds. This proves the tested
transport path on these boards; it does not establish long-duration stability,
range, maximum concurrency or coexistence with ordinary Wi-Fi/Bluetooth traffic.

## Bring-up findings retained in the implementation

1. Host and C6 SDIO modes must match. The initial packet/streaming mismatch was
   caught by Hosted's startup assertion; both now use streaming, four data
   lines, 20 MHz, and 1,000 Hz FreeRTOS ticks.
2. The P4 programmer must release the C6 boot strap before enabling/resetting
   it. Its UART and USB input buffers also need room for full transfer blocks.
3. Serial-flasher 1.10.0 required two local read fixes: SLIP decoding when the
   first payload byte is escaped, and allowing a read ending at the exact flash
   boundary. The patch and a regression test are preserved.
4. C6 CustomRpc requests execute in their own worker. Sending replies from the
   Hosted queue's own consumer risks deadlock when that queue fills.
5. The bridge forwards native API results and actual peer state. A transport
   acknowledgement alone is insufficient evidence that a radio operation worked.

## Scope and next milestone

This is an isolated experiment, using HardwareOne's 32-byte V4 header and
32–250-byte packets with experimental discovery/request/echo opcodes. It does
**not** run the full HardwareOne application, pairing, authenticated/encrypted
sessions, fragmentation or three-node relaying. The EYE P4 and its C6 together
form one logical radio node, so only two nodes were available.

The result supports keeping one application and mesh implementation with a
small radio backend boundary: native ESP-NOW for ESP32/S3, and this Hosted
adapter for P4+C6. Board pins and reset wiring belong in board configuration;
radio availability should be selected by capabilities/backend rather than CPU
architecture. The next milestone is to connect that boundary to HardwareOne's
actual discovery, pairing, encryption and fragmentation code and repeat tests
against an S3 running the application. The dependency/build issues documented
in the earlier investigation still need resolution for the full application.

## Evidence and final device state

- [Machine-readable test results](summary.json) contain every step and latency
  aggregates. [Firmware/source identities](firmware.json) bind these results to
  the built probes.
- Timestamped serial logs and individual packet events are retained locally in
  `../../private/test-runs/20260927T144613.777864Z/`.
- P4: revision 3.2, 16 MiB flash; radio MAC on its C6: `fc:01:2c:e0:b9:a8`.
  C6: revision 0.2, 4 MiB flash. S3: revision 0.2, 8 MiB flash,
  MAC `68:ee:8f:50:e9:d0`.
- Both boards remain on the working probe firmware, channel 6, S3 silence
  disabled, and no measured ping active. The C6 runs the matching bridge image.
- Verified complete original flash images for P4, C6 and S3 remain in ignored
  `../../private/backups/`. P4/S3 backups passed flash verification; the C6 backup
  matched its streamed SHA-256 and an independent full-flash hash. Each C6 image
  written for this milestone also passed native verification and SHA-256 readback.
  The original firmware has not been restored; instructions are in the main README.
- All 16 pre-existing modified/deleted repository paths matched their starting
  snapshot after testing. This work changed no HardwareOne production source.
