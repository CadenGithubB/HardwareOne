# ESP-NOW over ESP-Hosted (P4 host side)

The ESP32-P4 has no radio. On the P4X-EYE, Wi-Fi and Bluetooth run on the
onboard ESP32-C6 through ESP-Hosted over SDIO, but ESP-Hosted does not forward
ESP-NOW. This component supplies the standard `esp_now_*` API on the P4 and
executes each call on the C6 through Hosted CustomRpc, returning the C6's
actual result. Receive and send callbacks arrive as bounded events and never run
in the RPC thread.

It builds only for `esp32p4`. The C6 must run the companion firmware built by
[`tools/p4/companion`](../../tools/p4/companion/README.md): the ESP-Hosted
slave pinned in [`include/esp_now_hosted_companion.h`](include/esp_now_hosted_companion.h)
plus the matching `esp_now_hosted_slave.c`. Both sides compile the same
`include/esp_now_hosted_rpc.h`. With stock Hosted firmware the bridge's
requests time out, so ESP-NOW fails to start; Wi-Fi and BLE use Hosted's own
RPCs and do not depend on this bridge.

## Boot-time check

`esp_now_hosted_host_probe()` sends one GET_VERSION request and classifies the
companion (`esp_now_hosted_bridge_state()`): a reply with any native status
proves the bridge is present; no reply within the bridge timeout means stock
firmware; a refused send is a transport error. `main/radio_backend.cpp` calls
it right after the SDIO transport is up and logs the verdict, together with a
comparison of the C6's reported Hosted version against the pinned one.

## Peer existence

`esp_now_is_peer_exist()` is the query the shared mesh router and crypto
handlers issue for every received frame. The native driver answers it from a
local table; a C6 round trip (about fifteen milliseconds over SDIO) would
dominate RX, so this host answers it from its mirror of the C6 table, which
changes only together with a successful add/mod/del or deinit on the
companion. `esp_now_hosted_is_peer_exist()` asks the C6 itself and preserves
transport errors. `esp_now_get_peer()` and `esp_now_get_peer_num()` always
come from the C6.

Provenance, protocol, threading and timeout details are in
[tools/p4/companion/bridge/README.md](../../tools/p4/companion/bridge/README.md).
The design derives from the Apache-2.0 ESPHome slave overlay; its license is
preserved there. `test/test_protocol.c` is run by the tracked host test suite
as `esp_now_hosted_protocol`.
