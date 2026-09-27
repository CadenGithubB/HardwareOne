# P4/C6 and S3 web, Bluetooth and mesh coexistence investigation

Investigation phase completed on `codex/investigate-esp32-p4x-eye`, following
`../p4_mesh/`. All application and Arduino edits are confined to `private/app/`,
an independent copy. The original application and previous phase remain
untouched; reviewable changes are captured in the two overlay patches.

This phase enables the real HardwareOne HTTP interface and Bluetooth LE
services, with a shared Bluetooth hardware abstraction modeled on the existing
input/display/audio HALs. Native S3 and hosted P4/C6 use the same application
logic. The coordinator alone accesses serial ports, flashes or changes networks.

The S3 successfully fetched the real P4 web pages and exercised authentication,
APIs, CLI and logout. Encrypted BLE login, commands and reconnect passed with
P4/C6 as both central and server. Mesh messages passed during the central-role
BLE connection. The server-role run required Wi-Fi to be stopped on the S3
client; radio coexistence and connection reliability still need qualification.
G2/glasses/ring features remain disabled. No Mac Bluetooth access was used.

- [Results and remaining limits](results/2026-09-27/RESULTS.md)
- [Bluetooth HAL and integration work](HAL_DESIGN.md)
- [Reconstruct and build the isolated source](BUILD.md)
- [Board-to-board BLE test procedure](BLE_TESTING.md)

ESP-IDF **5.5.5** was sufficient for these tests; no IDF 6 migration was needed.
The build uses patched Arduino-ESP32 3.3.5 and Hosted 2.12.13. Private credentials,
builds and logs are ignored. Original whole-flash backups remain under
`../p4_espnow/private/backups/`. Nothing has been pushed upstream.
