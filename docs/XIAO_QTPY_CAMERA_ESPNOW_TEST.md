# XIAO camera and QT Py ESP-NOW hardware verification

Date: 2026-10-09. This bench test closes the real-camera and peer-radio gaps
left by the [QT Py storage self-test](QTPY_STORAGE_TEST.md). It exercises
production capture, image storage, encrypted messaging, file transfer and
remote filesystem APIs on a XIAO ESP32-S3 Sense and QT Py ESP32 Pico.

## Configurations and artifact identity

`xiao-default` is unchanged, with camera and ESP-NOW enabled. The new
`configurations/qtpy-espnow.json` copies `qtpy-oled` with exactly one saved
choice changed: `NETWORK_FEATURE_LEVEL` from 2 to 3, enabling ESP-NOW while
retaining its OLED and sensor selections. Shared flags and board definitions
are unchanged. Both profiles built using ESP-IDF 5.5.5, firmware 0.99.96.1.

| Normal profile | App bytes | App-partition headroom |
|---|---:|---:|
| `xiao-default` | 5,878,800 | 105,456 |
| `qtpy-espnow` | 3,443,632 | 2,540,624 |

Exact SHA-256 identities, independently read from the normal build artifacts:

| Artifact | SHA-256 |
|---|---|
| XIAO normal app | `e5a07603c35c1a447abac12a03fe59f8c5b7c5fc37bbbe6ca0fcdbe23e4f413d` |
| XIAO normal ELF | `620028f64330010aa6cb7c7658063ffd3e025963f00da92a522f837038ae8b45` |
| QT Py normal app | `a9f9c89b5c061dceb42146fabd5b02ea6a2051cfde2e10643aab7ff7be8bc7b2` |
| QT Py normal ELF | `3075791b4daa73b0d06125ba4a415e1bb7e2c10f7af30590415174d916615068` |
| QT Py temporary diagnostic app | `d3d207577ba4c53aadeae1520b518a0610a6e92aee3c7af125ee431c1cbc02cc` |
| QT Py temporary diagnostic ELF | `fd35000175f9fd5c036126fd40ef9f9814f48702ecfafcc4cde3991e4c083940` |

The temporary QT Py image calls the existing remote-filesystem client APIs
from a one-shot, explicitly triggered diagnostic. Its header hash is
`7d896260d637e62fc7b29f2ee669ea35f2de63fa951b80c6739fd2de24b0f2cf`.
Diagnostic results below are distinguished from the completed normal-image
restoration and reboot smoke check.

## Camera and image storage

The XIAO detected an OV3660 through the DVP backend. Two camera starts
succeeded, including a stop/start cycle. Two measured captures returned
2,109 and 8,000 bytes. Four `camerasave` calls wrote the following JPEGs to
the dedicated `/hw1-camera-selftest-20261009` LittleFS folder:

| Image | Independently decoded dimensions | Bytes |
|---|---:|---:|
| `img_000000.jpg` | 240 × 240 | 2,087 |
| `img_000001.jpg` | 320 × 240 | 2,616 |
| `img_000002.jpg` | 640 × 480 | 7,986 |
| `img_000003.jpg`, after restart | 640 × 480 | 7,905 |

All four were downloaded using production `fileread`; byte counts and hashes
matched the transfer records, and both image and filesystem inventories
agreed. Independent Pillow `verify()` and full `load()` succeeded at the
expected dimensions. This qualifies JPEG decoding and storage/readback,
not visual image quality. `cameratiny` returned 1,221 bytes at 160 × 120,
correctly reporting that it exceeded one radio packet; subsequent status
confirmed restoration to VGA and quality 12. The tiny frame was not downloaded.

Logs were not warning-free: each start emitted an I2C “not initialized”
message and an OV3640 probe mismatch before successful OV3660 detection.
Restart emitted two `NO-SOI` warmup warnings. No requested capture or save
failed. Boot count stayed at 10 and crash count at 0 during these checks.
Input-related loop-stall notices occurred; startup warnings remain observed,
not fixed or excluded by this test.

## Physical ESP-NOW results

Both radios used channel 6 without joining Wi-Fi. Each peer registry contained
the other board; pairing produced matching ACTIVE session IDs and opposite
session directions. Bond mode remained disabled.

- Encrypted text arrived in both directions with exact unique payloads and
  matching message IDs. Receiver history reported `enc:true`, and sender
  history reported delivered. No send failures, RX-ring drops or stream drops
  were reported after this exchange.
- XIAO `imagesend` delivered the 7,986-byte VGA JPEG to the QT Py. Independent
  receiver readback was byte-identical to the original; both SHA-256 values
  were `d65082b51a027a1de5f348efe09eac7c1ff625647fa28a0cd19dcb339cc4b5f0`.
- **24 diagnostic checks passed, zero failed, in 1,841 ms**, using production
  `fsListSendRequest`, `fsStatSendRequest` and `fsGetSendRequest` over the radio.
  LIST returned 34 visible fixtures in pages of 32 and 2 with exact names,
  sizes, readable permissions and no duplicates. A separately verified hidden
  fixture was excluded. Missing-directory and file-as-directory errors matched.
- STAT returned coherent capacity/rounded usage for `/` and `/sdata`, with
  `/sdata` routed to internal storage. This establishes routing and arithmetic,
  not cache freshness. Absent `/sd` returned NOT_FOUND with zero counters.
- Missing-file GET returned NOT_FOUND and zero size. A successful GET accepted
  1,024 bytes and delivered the exact deterministic payload into a previously
  absent inbox destination. Independent readback equaled `bytes(range(256))*4`,
  hash `785b0751fc2c53dc14a4ce3d800e69ef9ce1009eb327ccf458afe09c242c26c9`.

Post-radio status reported zero crashes and HTTP stopped on both boards.
The 24 checks include preconditions, not 24 independent scenarios. File
transfer was XIAO to QT Py; filesystem RPC used QT Py as client and XIAO as
server. This does not qualify security generally, multi-hop/range/interference,
loss/retry stress, files above 128 KiB, SD, video/long-duration streaming or
power-loss durability. Loop-stall notices preclude a claim of clean timing.

## Normal firmware and final state

The normal QT Py application was restored with hash-verified flashing. Boot
reported ELF prefixes `3075791b4` on QT Py and `620028f64` on XIAO. No diagnostic
hook remains in `HardwareOne.cpp`, which matches its pre-test snapshot. Original
profiles and dependency locks also match; no permanent firmware fix was needed.

Both boards reloaded their saved peer and established ACTIVE encrypted sessions
after reboot without re-pairing. Exact text receipts passed in both directions.
With ESP-NOW running, XIAO captured another 240 × 240 JPEG, saved it, and sent it
to QT Py. The 2,104-byte source and receiver files matched exactly, independently
decoded, and shared SHA-256
`c237d2863f6871b1bcbcd53c465ff1f49ea8e36c105e1e322f1c8ce12212591e`.
A 1,024-byte binary file sent QT Py → XIAO also matched exactly, SHA-256
`3af6dbef8362452d2b45ad97deb9e43180fb90aac309860e26e123860cce62a7`.

During this normal-firmware test, boot counts stayed 59 (QT Py) and 11 (XIAO),
with zero crashes. QT Py had three failed sends at the first post-boot baseline;
that count stayed three throughout the tested messages and transfers. XIAO
stayed at zero. RX-ring and stream-drop counts stayed zero. This establishes
successful tested delivery, not loss-free startup. Camera startup warnings
recurred before the successful final capture.

All owned camera/radio files, fixture directories and received copies were
removed; both receive directories were empty. The complete saved camera section
matches its original snapshot: `/photos`, storage location 1 (SD), retention
100, resolution 10, quality 12, auto-capture off and auto-start off. The camera
is stopped. A settings save also serialized previously absent Maps defaults
on XIAO (zoom 1, layers 1023, cache 1280 KiB), matching the current registry;
no existing Maps value was replaced.

The requested radio setup remains: `qtpy` and `xiao-sense` are securely paired
on the primary mesh, channel 6, with ESP-NOW enabled/running on both. Bond mode
and HTTP remain off; neither joined Wi-Fi. QT Py reports OLED File Browser
with display login. Both serial sessions are logged out and closed; XIAO logout
was explicitly completed without toggling reset lines. Display appearance was
not visually requalified. SD storage remains untested.

The private local camera/ESP-NOW evidence archive includes camera
jobs 001–003 and decoding record; radio jobs 005, 007–009; normal-image jobs
011–013; final logout, live logs and the diagnostic bundle with `hashes.json`.
The two build folders' `camera-espnow-test-status.json` and results files index
the retained local evidence. Credentials and captured images are excluded from
this report. Normal manifests remain in the two build folders.
