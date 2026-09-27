# Clean firmware qualification — 2026-09-27

**Passed the scoped hardware checks on both boards.** The final images use the
same ESP-IDF 5.5.5 Bluetooth component with the official five-file TinyCrypt
backport, shared application fixes and no temporary SMP diagnostics. They
demonstrated real G2/R1 Client operation and separate encrypted BLE Server
operation. Machine-readable counts, statuses and hashes are in [FINAL.json](FINAL.json).

| Check | Observed result |
| --- | --- |
| S3 accessories | Both G2 temples and R1 connected together, all at ATT MTU 244; R1 setup verified. Two encrypted mesh messages passed while connected. |
| P4/C6 accessories | Both G2 temples and R1 connected together, all at ATT MTU 244; R1 setup verified. The user confirmed the HardwareOne menu was visible and an R1 swipe moved its selection. |
| Client to Server | Passed on each board after active accessory use. G2 off and R1 disconnected were checked before the Server probes. |
| Encrypted BLE Server protocol | Two sessions per board—initial connection and reconnect—at MTU 517. Each verified pre-login denial, named administrator login and eight encrypted command replies. |
| HTTP with P4 accessories active | 19 complete responses totaling 849,743 body bytes; five HTML pages, two JSON APIs, authenticated CLI, login/logout and protected access checks. Both G2 links and R1 were still up afterward. |
| Encrypted ESP-NOW coexistence | 12 verified messages, each 401 bytes in three fragments, across accessory, BLE Server and HTTP checks. |
| P4 CPU controls | 100 and 200 MHz accepted, unsupported 240 MHz rejected, then Performance restored the live clock to 400 MHz. |
| Restoration | P4 returned from Server to Client, reconnected both G2 temples and R1, and showed no pending ring connection. S3 remained in Server mode, advertising with no client connected. |

The final P4 Client-to-Server transition completed in approximately 300 ms,
after the earlier diagnostic image had stalled at its three-second idle gate.
The shared submission-counter correction has therefore passed this real
transition; its old-placement negative control is covered in [OFFLINE.md](OFFLINE.md).
This is one final-image transition per board, not an endurance claim.

## Exact application images

| Target | Application bytes | SHA-256 |
| --- | ---: | --- |
| P4/C6 | 4,106,304 | `ace7d278e1cc5c4f58150bf57f225baf97dd96f891298534f58232854bdc05c2` |
| Native S3 | 4,740,544 | `86ad27464d7ec918838ff60bf7814fd5a05e7b96f632566d694f064b42389332` |

Both use the clean `private/idf-components/bt` override. See
[SDK-FIX.md](../../SDK-FIX.md) for its pinned inputs, exact upstream commit,
reproduction checks and cryptographic qualification limits. The earlier
[board-only report](RESULTS.md) and [diagnostic accessory history](ACCESSORIES.md)
refer to different images and remain separate evidence.

## Evidence scope

The final serial run was `20260927T190726Z-f2d13afa`. Its two encrypted Server
probe runs were `20260927T190930.444941Z-ff139977` and
`20260927T191309.617057Z-0715ed71`; HTTP used
`20260927T191151.709369Z-45599085`. Raw evidence remains in ignored private
storage. The public JSON was constructed from an explicit allowlist; it contains
no device addresses, account names, network credentials, health values or raw
command responses.

The mesh total is 2 messages with S3 accessories, 4 during S3 Server sessions,
2 during P4 HTTP/accessory coexistence, and 4 during P4 Server sessions. Each
received message was checked against its expected content and hash. The four
Server sessions supplied 32 encrypted replies in total. The BLE client was the
other development board, with the host performing protocol assertions over USB.
HTTP similarly used the S3 radio fixture against the real P4 web server.
Neither test used Mac Bluetooth or the Android app.

No panic, failed-assertion, abort or heap-corruption signature was found in
either saved serial log for this run. That limited signature scan does not
assert that every error counter or warning was zero.

Final observed state: **P4 Client, both G2 links and R1 connected at MTU 244,
CPU 400 MHz, BLE Server uninitialized; S3 Server advertising, zero connected
clients, G2 and R1 stopped.** The P4 menu was requested again after restoration;
the user's visual/menu-navigation confirmation occurred earlier in this same
qualification run.
The coordinator then closed both serial ports without resetting either board.

## What remains unqualified

The accessories had first connected to the Even app after their reboot, then
phone Bluetooth was disabled without rebooting them. The successful menu and
ring gesture establish operation in that prepared state; they do not remove
the known post-reboot Even-app prerequisite or recreate its full bootstrap.

Android discovery, UI and end-to-end application behavior remain untested.
Power labels and live CPU changes were checked, but the exact physical power
panel was not separately confirmed. OLED is disabled in this profile; idle
40 MHz operation, sleep and active I2C/OLED clock changes are not qualified.
Long-duration accessory/radio endurance and full P4X-EYE peripheral support
also remain outside this run. [POWER.md](../../POWER.md) records the power policy.

All investigation changes remain in the isolated experiment and reproducible
overlays. This work did not modify the original application sources; unrelated
concurrent peripheral work in the main working tree is separate.
