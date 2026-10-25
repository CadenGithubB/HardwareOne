# Arduino BLE hooks for the phone pipe

The phone owner uses peer-scoped GAP/GATTC interceptors before Arduino dispatch.
Other peers retain the existing handling when the interceptor returns false.
Install/remove hooks only while quiescent; callback owners have static lifetime.
The pipe uses its dedicated application secure channel and does not configure
SMP, request encryption, or create/remove OS bonds.

`arduino.patch` is incremental against HardwareOne's recorded Arduino 3.3.5
baseline, including its hosted BLE and client-lifetime fixes. It changes
BLEDevice.h/.cpp and BLEClient.cpp. The five-file manifest also checks that
BLESecurity.h/.cpp remain at baseline (the former pipe SMP-policy API is removed).
It records exact inputs, outputs and patch hash, without deployment data.

After provisioning the recorded Arduino baseline:

```sh
git -C components/arduino apply --check ../../tools/ble-phone-pipe/arduino.patch
git -C components/arduino apply ../../tools/ble-phone-pipe/arduino.patch
python3 tools/ble-phone-pipe/check_sdk.py
bash docs/arduino-local-patches/verify_patches.sh
```

Do not force-apply or discard unknown nested-repository changes. The checker is
read-only; firmware builds and hardware qualification remain required.

The DISCONNECT guard returns from BLEClient dispatch before service fan-out for
another connection ID or peer address. Native phone and Arduino clients share a
locked 15-bit registration allocator. Its 64-entry reservation table (in place
of the original 4096-byte bitmap since 2026-10-22, memory audit BLE-03) holds IDs
through registration, connection and quarantine; failed initial registration,
confirmed UNREG or verified terminal host shutdown releases them, and a full
table refuses a registration rather than reusing an ID. Wrap cannot collide with
live/reserved Arduino or native owners.

## Application channel

`ENABLE_BLE_PHONE_PIPE` defaults off. `p4-phone-pipe` enables it alongside G2;
`p4-openclaw` is the baseline. The existing phone-only profile remains available.
Chat and Terminal retain shared Wi-Fi-first routing and end-to-end TLS.

Provision `phone secret <passphrase>` from an authenticated non-Bluetooth
superadmin source. It requires 10–128 bytes with uppercase, lowercase, digit and
symbol; it is stored encrypted, with the live setting and cached key in internal
RAM. The console `blesecret` remains separate. Changing/clearing the pipe secret
drains the pipe, invalidates its cache and clears app enrollment. Factory reset
requires a persisted secret clear before account deletion.

The matching Android endpoint must implement the revised approved design and
Android handoff. It initiates the existing secure-channel v1 handshake with the
separate `HW1-PIPE-v1` PBKDF2/HKDF domain. No plaintext pipe traffic is accepted.
After a five-second handshake, each complete inner pipe frame occupies one AEAD
DATA value with 25 bytes overhead. MTU must be at least 64. RX requires
WRITE_NO_RESPONSE, TX requires NOTIFY; CCCD uses a normal write. There is one
bounded pending local completion, with accessory-priority pacing. Authentication
failure erases keys and closes; handshake REJECT is best-effort with a 250 ms
bound. Failed authentication imposes a global 30-second cooldown.

Use `phone pair` under the existing local owner/session authority. READY
requires both the secure handshake and inner HELLO. Only then is versioned app
enrollment saved. Advertising addresses, including rotating addresses, are
connection hints, never identity proof. Old SMP metadata cannot authorize this
channel. `phone forget` clears the logical enrollment after draining, while
preserving the console address, account owner, dedicated secret and accessory
bonds. Android's target allowlist is separately configured for this app channel.

## Verification

Run the production-source host suites (new suite registration is stage 6):

```sh
python3 components/hardwareone/test/host/test_phone_pipe.py --sanitize
python3 components/hardwareone/test/host/test_phone_secure_channel.py --sanitize
python3 components/hardwareone/test/host/test_phone_gatt.py --sanitize
python3 components/hardwareone/test/host/test_phone_runtime.py --sanitize
python3 components/hardwareone/test/host/test_phone_forget.py --sanitize
python3 components/hardwareone/test/host/test_phone_enrollment_schema.py --sanitize
python3 components/hardwareone/test/host/test_phone_secret.py --sanitize
python3 components/hardwareone/test/host/test_phone_sdk_lifetime.py --sanitize
python3 components/hardwareone/test/host/test_factory_reset_wipe.py --sanitize
python3 components/hardwareone/test/host/test_ble_sc_policy.py --sanitize
python3 components/hardwareone/test/host/test_ble_sc_directed.py --sanitize
python3 components/hardwareone/test/host/test_ble_central_coordinator.py --sanitize
python3 components/hardwareone/test/host/test_ring_phone_yield.py --sanitize
python3 components/hardwareone/test/host/test_phone_recovery.py --sanitize
python3 components/hardwareone/test/host/test_phone_pipe_config.py
python3 tools/ble-phone-pipe/test_check_sdk.py
```

The secure-channel and GATT suites build pinned vendored libsodium and IDF
mbedTLS in disposable temporary directories. Supply `--idf <idf-root>` if needed;
`--crypto-build <native-build>` reuses a prepared directory with sodium/mbedtls
subdirectories. The tests use synthetic keys and real crypto with production
protocol code. They do not qualify Android interoperability, RF scheduling or
physical RAM availability. Continue the transport/TLS suites documented in
`components/hardwareone/test/host/README.md`; device TLS parity, Android reconnect,
background behavior and G2 coexistence remain hardware release gates.

## Current integration handoff

See [the Android integration handoff](../../docs/PHONE_PIPE_HANDOFF.md) for the
current D2/D3 contract and validation boundaries. A NO_RSP completion status of
`ESP_GATT_CONGESTED` (0x8f) means accepted by the pinned SDK, not permission to
retransmit. Congestion events control pacing. Ordinary transport failures during
handshake do not count as bad-key attempts; invalid authentication and handshake
expiry retain the 30-second cooldown.
