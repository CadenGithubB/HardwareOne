# Battery integration results — 2026-09-28

The shared battery backend is implemented and installed on the connected
P4X-EYE. The connected XIAO ESP32-S3 also runs the updated application, with its
battery monitor correctly disabled because it has no onboard voltage divider.
This work is on `codex/jpeg-portable` in the isolated worktree. The primary
checkout was not modified. Nothing was pushed.

## Verified

| Check | Result |
| --- | --- |
| Complete P4 app, IDF 5.5.5 | Built, 4,446,288 bytes; fits existing 6,377,472-byte app partition |
| Complete S3 app, IDF 5.5.5 | Built, 4,742,592 bytes; fits existing 5,984,256-byte app partition |
| Real SDK ADC compatibility | Classic ESP32 line-fitting and S3 curve-fitting compile/link passed |
| Host tests with sanitizers | 16 compiled configurations passed: policy, six board configurations, six backend/lifecycle/JSON variants, three OTA policies |
| Web JavaScript tests | Battery display transitions and embedded JavaScript syntax checks passed (six unittest checks) |
| Board-only hardware suite | All 14 checks passed; no panic observed |
| Post-reboot state | Existing admin login, mesh and BLE peers, mesh configuration, power settings and stopped HTTP state preserved on both boards |

The P4 used GPIO49, ADC2 channel0, 6 dB attenuation, factory ADC calibration and
the schematic divider ratio 1332/332. Its initial BAT-node measurement was
4.111848 V. Twelve polling samples demonstrated the existing periodic sampler
refreshing, with values from 4.001767 to 4.111848 V. Three successive calibration
reloads succeeded. Further readings were valid at 100, 200 and 400 MHz:

| P4 CPU clock | BAT-node reading |
| --- | --- |
| 100 MHz | 4.024335 V |
| 200 MHz | 4.132661 V |
| 400 MHz | 4.039380 V |

After the final reboot, the reading was 4.172530 V. These are different samples,
not controlled measurements of a CPU-clock effect. The tests establish that
sampling and recalibration function at each clock; they do not establish
absolute analog accuracy or battery state of charge. All samples kept cell
presence, charging and USB power unknown.

Existing reciprocal encrypted mesh sessions worked, including exact 401-byte
messages in both directions. The S3 joined the P4's temporary test AP and made
the HTTP requests itself; the Mac's Wi-Fi and Bluetooth were not used. The
battery API rejected unauthenticated requests, accepted the existing login,
returned valid battery JSON and rejected requests again after logout. The
complete 36,930-byte battery HTML page was received. This is HTTP transport and
content evidence; physical display layout was not tested.

The human-readable `batterystatus` command reported unknown cell/charging/USB
states. Nine new estimated-status CSV rows were found, with blank charging and
USB columns. Existing history was preserved. The boards were rebooted after
the AP test; neither remained joined to Wi-Fi, their HTTP servers were stopped,
the original P4 400 MHz and S3 240 MHz settings were restored, and serial
`loglink` routing was returned to off. All test serial handles are closed.

## Deployment and evidence

Both original full flashes were backed up and verified before modification.
Each new application image was written and verified only at `0x10000`. The
new partition-table binaries matched the device backups byte for byte; neither
partition tables, bootloaders nor data partitions were flashed. C6 firmware was
not changed. The successful battery applications remain installed.

- [Build fingerprints](build-results.json) record image, input, manifest,
  partition and build-log hashes.
- [Hardware evidence](hardware-results.json) records the 14 checks, readings,
  boot verification and final preservation checks without credentials.
- [SDK ADC results](sdk-adc-results.json) and [SDK procedure](SDK_ADC.md) cover
  classic ESP32 and S3 driver compatibility.
- [Integration workflow and usage](README.md) explains the source changes and
  inherited isolated application snapshots.

Earlier build attempts are retained in private logs: the input verifier first
stopped after source edits, and CMake then found missing forwarding wrappers.
Inputs were refreshed and the wrappers added before the final successful
builds. Neither failed attempt produced or flashed a test application.

## Remaining physical limits

A physical cell was not confirmed for this run, and no multimeter comparison
was available. A charger can drive BAT without a cell. The observed variation
could reflect that condition, actual power/load changes, or the high-impedance
divider; this test does not distinguish them. A known connected cell and a
meter are needed to validate divider accuracy and settling. Consequently,
percentage/status are explicitly estimates rather than a fuel-gauge claim.

The stock EYE does not expose software-readable charge, standby or VBUS
signals, so those states cannot be made authoritative by a driver change.
No new physical test of an external S3 divider or MAX17048 was possible; their
coverage here is host tests and, for the ADC path, real SDK compilation. OLED,
G2/R1 and Android connection behavior were not retested without those
accessories. Battery support does not require an ESP-IDF 6 upgrade.
