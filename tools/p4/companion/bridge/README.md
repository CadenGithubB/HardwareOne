# HardwareOne ESP-NOW over ESP-Hosted bridge

This adapter supplies the standard `esp_now_*` API to a P4 host whose radio is
an ESP32-C6 companion, executing each call on the C6 through Hosted CustomRpc.
It targets the ESP-Hosted release pinned in
`components/esp_now_hosted/include/esp_now_hosted_companion.h` and
**ESP-IDF 5.5.x**. Wi-Fi initialization, interface selection, channel and
transport setup remain the caller's responsibility.

The pieces live where they ship: the host implementation and the shared wire
header are the production component `components/esp_now_hosted`
(`esp_now_hosted_host.c`, `include/esp_now_hosted_rpc.h`, `test/test_protocol.c`);
the C6 side is `esp_now_hosted_slave.c/.h` beside this file and is compiled
into the companion firmware by `tools/p4/companion/prepare.py`.

## Provenance and license

The companion implementation and protocol design derive from the Apache-2.0
[ESPHome slave overlay](https://github.com/esphome/esp-hosted-firmware/tree/14ee146ec71923aa250a7f743e5cff266a5ffb1a/slave-overlay),
commit `14ee146ec71923aa250a7f743e5cff266a5ffb1a`. The upstream license is preserved
in `LICENSE`; source headers identify the derivation and local changes. The host
implementation is plain ESP-IDF C and has no ESPHome dependency.

The actual CustomRpc declarations used are the pinned upstream
[`host/esp_hosted_misc.h`](../private/esp-hosted-mcu/host/esp_hosted_misc.h) and
[`slave/main/esp_hosted_peer_data.h`](../private/esp-hosted-mcu/slave/main/esp_hosted_peer_data.h).
Both use the callback `(uint32_t, const uint8_t *, size_t, void *)` and the
three-argument `esp_hosted_register_custom_callback(id, callback, context)`.

## Integration

- **P4:** depend on the `esp_now_hosted` component (it builds only for
  `esp32p4` and registers empty elsewhere). Call ordinary `esp_now_init()`
  after Hosted and Wi-Fi startup. `esp_now_hosted_host_start()` is available
  for explicit setup, and `esp_now_hosted_host_probe()` classifies the
  companion once at boot (bridge present / absent / transport error) without
  initializing ESP-NOW.
- **C6:** compile only `esp_now_hosted_slave.c` in the pinned Hosted `slave/main`
  component, with the shared `esp_now_hosted_rpc.h` and the existing slave
  headers in its include paths. Link the native ESP-NOW/Wi-Fi library. The
  constructor registers the CustomRpc handler; add `-u esp_now_hosted_slave_init`
  to pull this object from its archive. Alternatively define
  `ESP_NOW_HOSTED_SLAVE_AUTOSTART=0` and call `esp_now_hosted_slave_init()`
  explicitly in normal slave startup. `tools/p4/companion/prepare.py` does all
  of this when it generates the companion project.
- On **both** builds set `CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y` and
  `CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS=8`. The host needs three handler
  slots, the slave one. Additional Hosted examples may consume other slots.
- Include the **same** `esp_now_hosted_rpc.h` on both sides. Do not use the
  unmodified ESPHome header or simultaneously link its overlay. This experiment
  has distinct message IDs and an incompatible, explicitly versioned envelope.
- Never link both host and slave implementations into the same image.

Supported native signatures: init/deinit; register/unregister receive/send
callbacks; send; add/delete/modify/query peer; peer count; peer existence;
version; and PMK. `fetch_peer`, peer-specific rate, wake-window and other
unlisted APIs are intentionally not implemented. IDF 5.5 send callbacks receive
`const esp_now_send_info_t *`; the source also guards the older signature.

## Request and callback behavior

All operations, including callback registration, execute the corresponding
native API on the C6 and return its actual `esp_err_t`. Hosted's CustomRpc
transport acknowledgement alone is never reported as native success. The host
serializes exchanges through one mutex and matches response version, opcode
and 64-bit sequence. A timed-out sequence is never reused during that boot;
late, duplicate and mismatched replies are ignored. Random initial sequence
high bits also reduce collisions with stale traffic across host resets.

The bridge's native-response budget is two seconds, measured across the
CustomRpc submission and reply. Acquiring the serialization mutex also has a
two-second timeout. **ESP-Hosted's synchronous send can itself take its own
longer transport timeout**, so this is not a hard end-to-end execution deadline.
Transport failures, memory failures and missing responses return errors. A
timeout does not prove the native operation was not executed. There is no
automatic retry; query state or explicitly reinitialize before deciding how to
recover. After a companion reset, call init, register callbacks and restore
peers again.

The host RPC RX callback only validates/copies bounded messages or signals a matched
response. User receive/send callbacks run on a dedicated host task and may call
synchronous ESP-NOW APIs without blocking RPC RX. C6 native Wi-Fi callbacks also
copy into a queue, leaving transport work to a separate task. Each side has a
bounded 24-entry event queue; overload drops events rather than blocking a
radio/RPC task. Host counters are exposed by `esp_now_hosted_host_get_stats()`;
the slave worker logs queue/transport loss counts. A worker already inside user
code is not forcibly interrupted by unregister/deinit. Callback pointers and
received buffers are valid only for the duration of that callback.

C6 request execution and native replies use a separate request worker with a
four-entry queue. This separation is essential: in Hosted 2.12.13, CustomRpc
send enqueues into the same `pserial_task` queue that dispatches requests, using
`portMAX_DELAY`. Sending a reply directly from that consumer can deadlock when
events fill the queue. The bridge request callback only enqueues with zero wait.
If its queue/allocation fails, the request is dropped and the host reports a
timeout; a transport acknowledgement is still never mistaken for native success.

Events carry the latest successful init sequence as a lifecycle generation.
Queued events from an earlier init/deinit cycle are discarded. Host callback
registration generations also prevent previously queued events from being
delivered to a replacement callback. Bridge resources intentionally live until
reboot, including after native ESP-NOW deinit, avoiding queue/semaphore lifetime
races with an in-flight transport callback.

Pinned Hosted 2.12.13 retains its static custom-callback registry and its mutex
across `esp_hosted_deinit()`/`esp_hosted_init()`. The normal RPC event routes are
restored by Hosted init, so this bridge's existing handlers remain usable after
the harness's full radio restart. The bridge itself retains its worker resources
and does not need to repeat custom-handler registration for that pinned version.

## Data fidelity and limits

Frames are limited to ESP-NOW v2's 1470-byte maximum and are never truncated.
All envelopes validate exact lengths, maximum lengths, magic and version before
accessing payloads. `esp_now_get_peer()` and `esp_now_get_peer_num()` come from
the **actual C6 table**, not a success stub. The host keeps one mirror of the
table, by MAC, updated only together with a successful add/mod/del or deinit;
it carries the `priv` pointer (meaningless on the C6) and answers
`esp_now_is_peer_exist()`, the one query the shared mesh code issues for every
received frame and routing decision, so that check never waits on an SDIO
round trip. `esp_now_hosted_is_peer_exist()` still asks the C6 and preserves
transport errors for callers that need the companion's own answer. The mirror
goes stale only if the companion resets underneath the host, which nothing
recovers from yet. PMK/LMK values are not logged.

Receive metadata carries source/destination MAC, RSSI, channel and the native
timestamp. Remaining `wifi_pkt_rx_ctrl_t` fields are zero. Send metadata carries
source/destination MAC, interface, rate and transmit status on IDF 5.5; the native
raw 802.11 `data` pointer and `data_len` are not forwarded (NULL/zero on host).
Consumers requiring other metadata need an explicit protocol extension.

The native bool-only `esp_now_is_peer_exist()` necessarily returns false on a
transport failure. Tests or code needing the distinction can call
`esp_now_hosted_is_peer_exist(mac, &exists)`, which returns `esp_err_t` separately.

## Verification

`components/esp_now_hosted/test/test_protocol.c` checks wire sizes, exact
bounds, truncation/trailing bytes, wrong versions/magic, zero and wide
sequences, and maximum-size frames. It is part of the tracked host suite
(`esp_now_hosted_protocol` in `components/hardwareone/test/host`):

```sh
cmake -S components/hardwareone/test/host -B /tmp/hw1-host
cmake --build /tmp/hw1-host --target esp_now_hosted_protocol_tests
ctest --test-dir /tmp/hw1-host -R esp_now_hosted_protocol --output-on-failure
```

Build both actual targets and verify native error responses, callback reentry,
late-response rejection, peer-table readback, bidirectional delivery and radio
restart on the milestone harness before treating this experimental adapter as
qualified. The protocol test does not establish transport or radio correctness.
