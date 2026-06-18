# BLE Secure Channel — device→app message framing (v1)

This documents a wire-format change the **Android app must implement** to recover from
BLE fragment loss. Firmware emits it as of the BLE-link-reliability change; an app that does
not parse it will misread every device→app message.

## Why

BLE notifications are fire-and-forget and connection-interval paced. A large device→app
result (e.g. `memreport`, an `espnowmessages json` page of ~2–3 KB) is split into many
~200-byte encrypted frames. Before this change those frames were just a byte stream: one
dropped frame corrupted reassembly, and the app had no way to know *which* message was
incomplete or where the next began — so it silently parsed a short/garbled page and never
advanced its `since` cursor (the "timed out / no response" loop).

Firmware-side reliability (bounded notify backpressure) now makes drops rare, but this
framing makes any remaining loss **detectable and recoverable**.

## What did NOT change

- The handshake (`SC_HELLO` `0x01` / `SC_HELLO_ACK` `0x02` / `SC_CONFIRM` `0x03` /
  `SC_CONFIRM_ACK` `0x04` / `SC_REJECT` `0x05`) is unchanged.
- The `SC_DATA` (`0x10`) wire frame is unchanged: `[0x10][ctr(8, big-endian)][ciphertext][tag(16)]`,
  ChaCha20-Poly1305-IETF, key `kD2C`, 12-byte nonce = `DIR_D2C(0x00000001, 4B BE) || ctr(8B BE)`.
- The **app→device** direction is unchanged: the app keeps sending unframed command
  plaintext, one command per `SC_DATA` frame. This header is device→app only.

## What changed: decrypted plaintext now carries a 5-byte header per frame

After decrypting an `SC_DATA` frame, the plaintext is no longer raw text. It begins with:

```
offset 0: ver       (1 byte)  always 0x01
offset 1: msgId_lo  (1 byte)  message id, little-endian
offset 2: msgId_hi  (1 byte)
offset 3: fragIdx    (1 byte)  0-based fragment index within this message
offset 4: fragCount  (1 byte)  total fragments in this message (1..255)
offset 5: payload    (N bytes) up to 195 bytes of the actual message text
```

A whole device→app message = the concatenated `payload` of all `fragCount` fragments sharing
the same `msgId`, in `fragIdx` order.

- `msgId` increments per message, per connection, and wraps at 65536.
- `fragCount == 1` is the common case (any message ≤ 195 bytes): single self-contained frame.
- Max message size with this framing is `255 * 195 ≈ 49 KB`. The CLI pages large results
  (e.g. `espnowmessages json`) well under that.

## App reassembly algorithm

```
on SC_DATA decrypted -> plaintext:
    ver, msgId, fragIdx, fragCount = plaintext[0..4]
    if ver != 0x01: drop (unknown framing)
    payload = plaintext[5:]

    buf = buffers[msgId]            # keyed by msgId
    if buf is empty:
        buf.fragCount = fragCount
        buf.parts = array[fragCount]
    buf.parts[fragIdx] = payload

    if all fragCount parts present:
        message = concat(buf.parts)
        delete buffers[msgId]
        deliver(message)            # this is one complete CLI result / line batch
```

### Loss detection & recovery

- The `SC_DATA` counter (`ctr`) is strictly increasing. If the app sees a **gap** in `ctr`,
  a frame was dropped — the in-progress `msgId` will be missing a `fragIdx`.
- Apply a short per-`msgId` reassembly timeout (e.g. 1–2 s). If a `msgId` is still
  incomplete when it fires, discard that partial message and **do not advance the request
  cursor** (`since`). Re-issue the same request; the device resends the full page with a new
  `msgId`.
- Never deliver a partially-assembled message to the JSON parser.

## Pulling a remote command's output (`espnowremote` → `espnowmessages`) — SERIALIZE

This is separate from the frame reassembly above and is **required** for large remote
results (e.g. `espnowremote <mac> <user> <pass> memreport json`, ~88 lines).

A remote command's output is streamed back and stored as many individual records in a
**rolling per-peer ring** on the device. The app retrieves them by paging:
`espnowmessages json <sinceSeq> <mac>` returns up to 8 records with `seq > sinceSeq`,
oldest first. The ring is finite (≈250 records) and **wraps** — if it overflows while you
are still paging, unread records are evicted and lost. So the app MUST pull serially:

1. **One remote command at a time.** Do not issue another `espnowremote` (or re-issue the
   same one) until the current pull has fully completed.
2. **Page strictly forward.** Track the highest `seq` seen; request
   `espnowmessages json <highestSeq> <mac>` repeatedly, advancing `highestSeq` to the max
   `seq` in each page.
3. **Stop on the empty page.** A response of `{"schema":1,"messages":[]}` means "caught up
   / done" — that is the terminator for the pull.
4. **Do not restart at `sinceSeq=0` mid-pull.** Restarting re-reads whatever is currently in
   the rolling ring (which now also contains a *new* run's records), so old output from a
   previous run reappears interleaved with new output. Only start from 0 for a deliberate
   "show everything currently buffered" view.
5. Records carry `reqId` (the originating command's id). To display *only* the result of the
   command you just issued, filter the paged records by that `reqId`.

Pages are capped to 8 records so each stays a valid, single JSON object well under the BLE
result-buffer limit. `seq` is global and monotonic across runs (it does not reset per
command); `reqId` is what groups a single command's output.

> Note: the ring is a stopgap sized for ~2–3 reports of headroom. Following the serialize
> rules above keeps a single pull race-free. (A future device change may add a per-`reqId`
> immutable snapshot fetch that removes the ring race entirely; this section is the contract
> until then.)

## Firmware references

- Emitter: `bleScSendEncrypted()` in `components/hardwareone/System_BleSecureChannel.cpp`
  (`SC_FRAME_VER`, `SC_APP_HDR=5`, `SC_MAX_PAY_FRAME=195`).
- Reliability: `bleRawNotify()` in `components/hardwareone/Bluetooth.cpp` (bounded
  congestion backpressure via `onStatus`).
- History ring (paged by `espnowmessages`): `MESSAGES_PER_DEVICE` in
  `components/hardwareone/System_ESPNow.h` (PSRAM depth 250); paging in `getPeerMessages()`.
