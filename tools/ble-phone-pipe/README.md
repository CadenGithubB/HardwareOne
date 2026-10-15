# Arduino BLE hooks for the phone pipe

The phone client needs a peer-scoped GAP interceptor before the Arduino wrapper's
security replies, plus a snapshot/restore API for its SMP configuration. Existing
G2 callbacks and default handling continue when the interceptor returns false.
The callback owner must be quiescent when installing/removing the interceptor;
callbacks must have static lifetime. A hook alone does not qualify pairing or
radio behavior.

`arduino.patch` is an incremental four-file patch against HardwareOne's existing
Arduino 3.3.5 working baseline, including its already-required hosted BLE and
client-lifetime fixes. It does not replace that baseline or capture unrelated
changes in the ignored Arduino checkout. `manifest.json` records exact inputs,
outputs and patch hash. No deployment data is included.

After provisioning the existing HardwareOne Arduino baseline, check and apply
from its nested Git checkout:

```sh
git -C components/arduino apply --check ../../tools/ble-phone-pipe/arduino.patch
git -C components/arduino apply ../../tools/ble-phone-pipe/arduino.patch
python3 tools/ble-phone-pipe/check_sdk.py
```

The checker is read-only. A baseline mismatch or a later SDK edit needs review;
do not force-apply, reset the checkout, or replace unknown local modifications.
The actual firmware build and phone/G2 coexistence tests remain required.

## Current integration checkpoint

The optional `ENABLE_BLE_PHONE_PIPE` flag defaults off. The saved
`p4-phone-pipe` configuration enables the native pipe alongside G2;
`p4-phone-only` compiles it with G2 disabled; `p4-openclaw` is the baseline.
The runtime owns scanning, authenticated enrollment, saved-bond reconnect,
stream lifecycle and scoped unpair. Server mode remains selectable. Chat and
httpTerminal now share Wi-Fi-first routing and strict end-to-end TLS over the
pipe, including in the G2-disabled build. The feature is still experimental:
Android RPA rotation/reboot, interrupted-pairing recovery, device TLS parity and
physical Android/G2 qualification remain release gates.

Use `blemode phone-client` and `phonepipe start` for a standalone central host;
G2 mode attaches the phone owner after G2 initialization. A local authenticated
admin who owns the phone record opens `phonepipe enroll`, compares the six-digit
number shown by `phonepipe status` with Android, then uses `phonepipe confirm`
with those digits. No numeric comparison is accepted automatically. Once the
strong bond is confirmed it is saved before pipe negotiation; a later HELLO
failure leaves a known bond but does not make the pipe READY. Use `connect` to
retry that bond, or scoped `forget` before enrolling afresh.
`phonepipe forget` cancels streams and requests removal of only that phone bond;
check status for completion. The native removal acknowledgement precedes clearing
pipe metadata; failure/timeout never reports success. Remove the P4 bond and
allowlist entry on Android separately. Legacy console identity and the shared
phone-account owner remain intact; accessory bonds are untouched.

The GATT pre-dispatch interceptor prevents the wrapper's automatic security
handling from replying on behalf of the phone. Other peers continue through
existing dispatch. The owner must install it while quiescent, retain static
callback storage, and hold the setup coordinator through security/discovery.
Only a verified terminal HAL shutdown can clear a quarantined registration.

Run the production-source host harnesses from the firmware root:

```sh
python3 components/hardwareone/test/host/test_llm_http_transport.py --sanitize
python3 components/hardwareone/test/host/test_llm_http_runtime.py --sanitize
python3 components/hardwareone/test/host/test_llm_http_idf_open.py --idf "$IDF_PATH" --sanitize
python3 components/hardwareone/test/host/test_phone_pipe.py --sanitize
python3 components/hardwareone/test/host/test_phone_identity.py --sanitize
python3 components/hardwareone/test/host/test_phone_gatt.py --sanitize
python3 components/hardwareone/test/host/test_phone_runtime.py --sanitize
python3 components/hardwareone/test/host/test_phone_forget.py --sanitize
python3 components/hardwareone/test/host/test_ble_server_stop.py --sanitize
python3 components/hardwareone/test/host/test_g2_client_lifecycle.py --sanitize
python3 components/hardwareone/test/host/test_companion_ble_shutdown.py --sanitize
python3 components/hardwareone/test/host/test_ble_central_coordinator.py --sanitize
python3 components/hardwareone/test/host/test_phone_pipe_config.py
python3 tools/ble-phone-pipe/test_check_sdk.py
```

These cover protocol/credit bounds, path replay policy, identity validation,
asynchronous GATT and late callbacks, and shared radio admission. They do not
establish Android interoperability, device TLS verification parity or G2 airtime
quality; those require the hardware matrix. `test_llm_phone_tls.py` additionally
runs real mbedTLS against a fixture server over bounded fake byte queues. Supply
`--mbedtls-source <pinned-sdk-mbedtls>` and `--mbedtls-build <host-build>` after
building that source with CMake (`ENABLE_PROGRAMS=OFF`, `ENABLE_TESTING=OFF`,
`USE_SHARED_MBEDTLS_LIBRARY=OFF`). Fixtures and keys live only in temporary
storage. Its host configuration differs from P4, so repeat acceptance tests on
the device; the host result is not a claim of device trust parity.
