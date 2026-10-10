# Filesystem self-testing — 2026-10-09

The accumulated filesystem changes pass 21 host suites, 41 on-device storage
checks, 28 on-device HTTP checks, and a final CLI smoke test on the normal QT Py
firmware. The on-device check counts include preconditions and cleanup checks;
they are not counts of independent scenarios. No permanent firmware changes
were needed during this verification.

## Host verification

A fresh host build passed all 21 selected CTest suites in 11.74 seconds. The
selection covers directory consumers, PSRAM file reads, checked text writes,
upload lifecycle, capacity sampling/cache, ESP-NOW filesystem responses, image
inventory/saves, permissions and downstream settings persistence. The compiled
C++ targets and 13 extraction runners used ASan/UBSan; the remaining tests check
source/configuration contracts.

The selection and exact commands are preserved in the local evidence manifest.
Fault-injection coverage includes short writes and backend failures that the
attached hardware could not safely or reliably induce.

## QT Py storage diagnostic

A temporary build called production filesystem/VFS/ImageManager methods on the
real LittleFS volume. All 41 checks passed in 4.401 seconds:

- Direct and atomic text saves, empty content, replacement and temporary cleanup.
- A real rename refusal preserved the original nonempty directory and its
  sentinel, while removing the failed temporary file.
- Directory entries, capacity snapshots, `/sdata` routing and absent-SD errors.
- Two FreeRTOS tasks completed 20 atomic saves to one path; every read contained
  a complete known payload and no temporary file remained.
- Binary image-buffer save, inventory, metadata, PSRAM readback and deletion;
  internal fallback when SD was absent; rejection of invalid buffers, failed
  opens and destinations routed to the wrong storage tier.

Image tests used a synthetic JPEG-marked byte buffer, not a camera capture or
decoder. Camera fields were changed only in RAM, verified absent from the
settings registry, then restored. The normal QT Py profile disables the camera.
The diagnostic forced these production storage methods into its linked image.

## QT Py HTTP diagnostic

The production HTTP server and handlers were exercised with real lwIP sockets
over device loopback. All 28 checks passed in 7.022 seconds:

- Authentication rejection/login, text/binary/empty uploads, exact VFS and HTTP
  readback, directory listing and coherent capacity output.
- Missing content and traversal rejection without creating output.
- A client disconnected after sending 5,000 bytes of an incomplete request;
  the server reported failure, removed partial output, and accepted a subsequent
  valid upload.
- Polling flags and filesystem locks returned to their entry state. The web
  session was logged out, the server stopped and all private fixtures removed.

The Wi-Fi radio stayed off; saved Wi-Fi and HTTPS feature choices were unchanged.
This exercises real HTTP transport/handlers without qualifying radio networking.

## Normal firmware restoration

The application-only flash at `0x10000` was hash verified. The normal build is
byte-identical to the pre-test production artifact, with 2,890,928 application
bytes and 3,093,328 bytes free in the application partition.

| Build | Application SHA-256 | ELF SHA-256 |
| --- | --- | --- |
| Storage diagnostic | `af7f74c5f43861e9a145ad2baa3e45e5d03214155945fe569ab05380721799e4` | `6bb3f6322dc7a91231748193435649e27f494c7cc29b60f761faf7d82cc5a63d` |
| HTTP diagnostic | `95fb2f47ed1d106c16e2c3e51477d02eda4c065ec3f24da0639bb0a2b4af5c97` | `1b3205408aef29299b1da68d2b28fb7652418396f0dd26f24a78be2d10b37c7a` |
| Restored normal firmware | `8f16236b2eca34187aab79776f175d27c5fa1b23de2d7f8d5d0d2d6a19db6e19` | `7b861a3eadf712a60cbb1dda87efa7a9e789e8a672120ed6b9e0ddfe6f113c54` |

The final boot reported ELF prefix `7b861a3ea`. CLI create, text/binary write,
exact base64 readback, names/types/sizes in directory listings, rename, delete
and directory removal passed. All private test directories were absent afterward.
Crash count remained zero, the server/radio were off, and no panic, heap
corruption or watchdog-reset marker appeared in the captured runs. File Browser
was selected with a display login; serial was logged out and closed. This run
checked display status, not visual rendering.

Temporary hooks were removed. `HardwareOne.cpp`, both saved profiles and both
dependency lock files match their pre-test snapshots. No flash erase occurred;
a complete private flash backup was taken before testing.

## Evidence and limits

Local `build-config-qtpy-oled/storage-selftest-status.json` and
`storage-selftest-results.json` separate diagnostic coverage from the final
normal-image smoke test. `storage-selftest-evidence/` preserves logs, runner
sources, the host selection and diagnostic artifacts. The earlier slice records
remain historical; the generic hardware status now points to this run.

Two initial harness assumptions were corrected and their failed attempts
retained: HTTP setup incorrectly required the saved Wi-Fi feature choice to be
off as well as the radio; the final CLI check initially expected full paths where
the public JSON contract returns entry names. Neither required a firmware fix.

Physical SD-card writes/timestamps, camera capture, an ESP-NOW peer, Wi-Fi radio
transport and power-loss durability remain unqualified. Arduino flush/close
cannot expose every backend error; image saves still use the existing direct
create/truncate policy. XIAO remains covered by its earlier build and host
results, not by this physical QT Py run.

The subsequent [XIAO/QT Py camera and ESP-NOW test](XIAO_QTPY_CAMERA_ESPNOW_TEST.md)
qualifies real camera captures and peer-radio filesystem/file transfers with
its own recorded firmware identities. Physical SD storage remains untested.
