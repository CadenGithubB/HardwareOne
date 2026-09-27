# G2 and R1 accessory testing — diagnostic qualification

Updated **2026-09-27** through the successful accessory checks in private runs
`20260927T184159Z-24d79651` and `20260927T185037Z-0e8fd136`.
Both the native XIAO ESP32-S3 Sense and P4/C6 have now demonstrated both G2
temples and the R1 ring connected together. The user confirmed visible display
text from each board. **The later clean-image qualification also passed; see
[FINAL.md](FINAL.md) for final hashes, role transitions and confirmed P4 ring
navigation.** The observations below preserve the diagnostic comparison.
This evidence does not replace the earlier immutable
[board-only qualification](RESULTS.md) or its binary hashes.

## Observed results

| Check | Native S3 | P4 with Hosted C6 |
| --- | --- | --- |
| G2 connection | Both temples connected, each at MTU 244. | Both temples connected at MTU 244 after the upstream TinyCrypt fix. |
| G2 display | User confirmed “HardwareOne S3 control test” was visible. | User confirmed the P4 test text was visible after the fix. |
| R1 setup | Encrypted CCCD subscription and application setup completed; reported ready with firmware profile `2.2.9.0003`, MTU 244. | Same ready state, profile and MTU after the SDK fix. |
| Three accessory links together | Both G2 links plus R1 remained connected through subsequent status checks. | Both G2 links plus R1 remained connected through subsequent status checks. |
| ESP-NOW coexistence | Two encrypted 401-byte messages, one each direction, verified while all three S3 accessory links were active. | Two encrypted 401-byte messages, one each direction, verified while all three P4 accessory links were active. |
| Client to Server transition | G2 off and R1 disconnected were verified, followed by a passing encrypted Server probe with initial connection and reconnect. | This diagnostic image timed out at the G2 client-initialization idle gate. The shared correction subsequently passed on the clean final image; see FINAL.md. |

Each mesh message used three fragments and had its received content/hash
verified. These are short bench checks, not endurance or range qualification.
The Server probe used the other board as its BLE client, with the host performing
protocol assertions over USB; it was not an Android application test.

## Two distinct fixes

Initially, R1 notification enablement failed on both boards with GATT status
`0x05`, insufficient authentication. The shared R1 code now sets
`ESP_GATT_AUTH_REQ_NO_MITM` on its cached CCCD before the existing checked
subscription. This requests link encryption suitable for a headless pairing
flow. The descriptor write still needs an acknowledged success; application
`pairAuth`, device identity, time and advertising setup remain separate gates.
No bond erasure, security bypass or R1 firmware-profile change was introduced.
The nine supporting host checks are recorded in [OFFLINE.md](OFFLINE.md).

P4 had a second failure: Bluetooth Secure Connections DHKey checks still failed
after the accessories had been connected to the Even app. The S3 then completed
the same three-link topology under that controlled condition. P4 subsequently
passed with the five-file Espressif TinyCrypt backport. This before/after result
supports the identified SDK regression rather than treating the post-reboot
app prerequisite as its complete explanation. The upstream report is
[ESP-IDF issue 19002](https://github.com/espressif/esp-idf/issues/19002); exact
provenance, preparation and remaining limits are in [SDK-FIX.md](../../SDK-FIX.md).

The successful P4 image still included temporary SMP diagnostic instrumentation.
Its application binary was **4,107,600 bytes**, SHA-256
`ea930b70dcceada0daf744b7f0aeef420a9b3e6e7006cb8d0ba597cdb6684a4c`.
The preceding diagnostic image without the TinyCrypt fix was **4,105,632 bytes**,
SHA-256 `55f2b2cb1553ce2eeafad4a66d6beb67efdf7b28df3efe85d2d2150dbe6bd718`.
These hashes identify this comparison only; they are not the final clean images.

## Known post-reboot prerequisite remains

The user reported an existing requirement to connect the glasses and ring to
the Even phone app after rebooting the accessories. Before the later runs,
the user did so, then disabled phone Bluetooth without rebooting either
accessory. The successful results above therefore apply to that prepared state.
This experiment has **not** removed or reproduced the complete Even-app
bootstrap requirement.

The baseline already checks G2 `AUTH` and right-arm `PIPE_ROLE`
acknowledgements, attempts acknowledged `TIME_SYNC` with a valid clock, then
sends `AppLaunch`. Those exchanges alone do not prove that every post-reboot
step performed by the Even app has been recreated. No unverified bootstrap
command was added to bypass the prerequisite.

Ring gesture routing has a separate dependency: baseline comments describe an
Even-app-established ring-to-glasses handshake, while the direct R1 client
observes gestures that already arrive through the glasses. Phone-hub gesture
relay remains outside that client's contract. Earlier `ringbridge` and
`ringtoglasses` experiments remain unregistered. Successful direct R1 setup
does not establish gesture routing or remove that separate setup dependency.
The later final run did demonstrate a physical ring swipe moving the P4 menu
selection, as confirmed by the user, in the Even-app-prepared state.

The S3 antenna was absent during early attempts and attached before the
successful control tests. Those early attempts are not a fair radio comparison
and do not establish a software regression.

## Clean-build qualification and remaining work

The preparation and build workflow now selects one clean, pinned IDF 5.5.5
Bluetooth component override for both targets. It contains the exact upstream
TinyCrypt changes and no SMP diagnostic instrumentation. Final clean builds,
their hashes and scoped device qualification are recorded in [FINAL.md](FINAL.md)
and [FINAL.json](FINAL.json); the diagnostic successes above remain separate.

After the successful P4 accessory hold and mesh check, switching from Client
to Server repeatedly failed at the three-second G2 initialization-idle gate.
Investigation identified a shared lifecycle counter left incremented when a
queued page-swap request was rejected by its lifecycle stamp. The corrective
change passed its regression check and the P4 transition on the final image,
which completed in approximately 300 ms. Both boards then passed initial and
reconnected encrypted Server sessions. P4 was subsequently restored to Client
with both G2 links and R1 connected.

Still untested or incomplete at this snapshot:

- Cold accessory boot without first using the Even app.
- Android discovery, authentication, commands and UI behavior.
- Longer repeated accessory reconnect and Client/Server transition endurance.
- Longer mixed-radio and three-accessory endurance checks.

Health-history extraction was not part of this test. Raw serial content,
device addresses, credentials and health data remain in ignored private
storage and are not included here. Investigation changes remain within the
isolated experiment; unrelated concurrent peripheral edits in the original
working tree are outside this report.
