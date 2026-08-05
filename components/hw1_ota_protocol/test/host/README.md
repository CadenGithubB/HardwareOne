# HardwareOne OTA protocol host tests

The host suite exercises only the ESP-IDF-free protocol core. It covers the
canonical manifest payload, fail-closed detached-signature boundary, identity
policy, record CRC/versioning, legal and illegal state transitions, retained
result acknowledgement, and representative power-loss reconciliation points.

Run from the repository root:

```sh
cmake -S components/hw1_ota_protocol/test/host -B /tmp/hw1-ota-host
cmake --build /tmp/hw1-ota-host
ctest --test-dir /tmp/hw1-ota-host --output-on-failure
```

The dual-slot NVS adapter and partition verifier are compiled as part of the
normal ESP-IDF build; they are not replaced with behaviorally different host
mocks here.
