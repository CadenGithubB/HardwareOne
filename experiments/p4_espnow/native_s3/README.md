# Native ESP32-S3 ESP-NOW control probe

Standalone ESP-IDF 5.5.5 firmware for the connected XIAO ESP32-S3 Sense.
This tests the native radio side of the P4 + C6 transport experiment. It does
not load HardwareOne, pair application identities, or establish encrypted
sessions. It starts a station radio on channel 6 without joining an access
point. No SSID or Wi-Fi password is required.

## Build and console

With the experiment's ESP-IDF 5.5.5 environment exported, from the repository root:

```sh
bash experiments/p4_espnow/build.sh s3
```

The project selects the built-in USB Serial/JTAG console. Flashing is handled
separately by the experiment coordinator after identifying and backing up the
board. It replaces the application; this source itself does not erase or
provision settings. The boot log includes `READY`, the station MAC, actual
configured channel, ESP-NOW version, and probe fingerprint.

Send newline-terminated ASCII commands to the USB serial console:

| Command | Effect |
| --- | --- |
| `stats` | Print frame, callback, peer, channel, and queue-drop counters. |
| `channel N` | Set channel 1–11; registered peers retain channel 0 and follow the radio. |
| `silence 1` | Stop discovery and echo responses while still receiving and logging. |
| `silence 0` | Resume responses and periodic discovery. |
| `restart_radio` | Unregister callbacks, deinitialize ESP-NOW and Wi-Fi, initialize again, and register the broadcast peer. Counters persist. |

## Wire contract

`main/milestone_wire.h` is the reusable C/C++ definition. Its packed 32-byte
header has the exact HardwareOne V4 field layout, but uses the reserved
experiment opcodes 200–202. Multi-byte fields are little-endian, as on both
ESP32-S3 and ESP32-P4.

- `magic = 0x3148`, `ver = 4`, `headerLen = 32`, `ttl = 1`.
- `flags = 0`, reserved fields = 0, `fragIndex = 0`, `fragCount = 1`.
- `origin` is the emitting node's station MAC; `msgId` identifies the request.
- `meshFingerprint = CRC16-CCITT("p4-milestone")`.
- `sessionId = 0`, `frameSeq = 0`: no cryptographic session is claimed.
- CRC16 is polynomial `0x1021`, initial value `0xffff`, no final XOR, over
  payload bytes only. A zero-length payload has header `crc16 = 0`.
- At payload byte offset `i` after the header, the expected byte is
  `(msgId * 31 + i * 17 + 0x5a) & 0xff`.

| Opcode | Direction / behavior |
| --- | --- |
| 200 DISCOVERY | Broadcast to FF:FF:FF:FF:FF:FF every two seconds; 64 bytes total. Receivers register its source as an unencrypted channel-0 station peer. |
| 201 REQUEST | Accept total lengths 32, 64, 128, or 250 after validating the complete header, CRC and pattern. |
| 202 ECHO | Reply unicast to the REQUEST radio source; preserve its `msgId`, length and payload; rebuild the header with the S3's own origin. Never echo an ECHO. |

RX callbacks copy source and destination addresses, RSSI, and bytes into a
32-event FreeRTOS queue. TX callbacks copy destination and MAC delivery status
into the same queue. All ESP-NOW peer operations, sends, parsing and logs run
outside callbacks. Queue saturation increments `callback_drops` and never
blocks the radio callback.

## Suggested acceptance run

1. Confirm both endpoints report channel 6 and discover each other's MAC.
2. From the P4, send REQUESTs cycling through 32/64/128/250 bytes. Validate
   byte patterns and correlated ECHOs on the P4; retain both serial logs.
3. Record accepted sends, MAC callbacks, application ECHOs, missing ECHOs,
   malformed frames, queue drops, and round-trip latency separately. A
   successful `esp_now_send` return or MAC callback alone is not an end-to-end
   acknowledgement.
4. Repeat enough 250-byte exchanges to expose a bridge length/truncation issue.
   HardwareOne uses at most 250 bytes despite newer ESP-NOW versions allowing
   more. Its fragmented-transfer ACK wait is 200 ms; compare latency to that.
5. Enable S3 silence and confirm P4 application timeouts. MAC delivery may
   still succeed because the radio remains on. Resume and verify recovery.
6. Move one endpoint from channel 6 to 1, verify missing replies, then move
   the other to 1 and verify recovery without recreating its channel-0 peers.
   Return both to channel 6.
7. Restart the S3 radio and verify broadcast discovery restores unicast peers.
8. Send a bad payload CRC, a correct CRC with an incorrect deterministic
   pattern, and an unsupported REQUEST length. Confirm `invalid` increments
   and no corresponding ECHO is returned.

This is a radio/backend compatibility control. It does not validate HardwareOne
pairing, HMAC authentication, ChaCha20-Poly1305 sessions, fragmentation or
multi-hop routing. P4+C6 and S3 are two logical radio nodes; a genuine relay
test needs a third independent radio node.
