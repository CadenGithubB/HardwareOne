# OLED Health layout and graphs

The Health page uses the shared single-line list renderer and derives action
capacity from the content height after reserving 32 pixels for vitals. All seven
actions fit on the P4X-EYE's 128-pixel logical canvas. Smaller OLEDs scroll within
the remaining content region; action rows no longer overlap the footer.
Collection and low-power labels use `keep` for preserve and `?` for an unknown
observed state so the complete desired/observed pair fits on one line.

## Graph viewer

Open **Health → Graphs**, then choose Heart rate, HRV, SpO2, Temperature, Battery,
or one of the three Daily HR/HRV/SpO2 series. A/Select opens a graph. On a graph,
left/right or up/down changes metric, A/X requests the existing paced poll, and
B returns to the graph list. B from that list returns to the Health overview;
B from the overview follows normal global back navigation. Read-only navigation
is available to guests; poll and mutation actions retain their existing gates.

Graphs show chronological samples with an automatically scaled value axis,
latest value and units. Live-series time labels use receive ages; series that
include synthetic backfill say `Samples + history` rather than claiming a live
time range. Daily graphs show the retained day series and are labelled Daily,
not Today, since history can belong to an earlier day. Retained samples remain
viewable offline with an offline indicator. An empty series explains that no
samples are available instead of drawing a fabricated trace.

`G2_Health` owns all series. Its new `g2HealthGraphSnapshot` copies a selected
live/daily series into caller-owned storage under the existing series mutex,
orders backfill by time and releases the mutex before OLED drawing. The viewer
never modifies the lens's selected metric, navigation or graph-push scheduling.
No additional sample ring, BLE polling loop, history store or settings were
introduced. The disabled API clears its output and reports unavailable.
Health command refusals and logging results now use the shared checked OLED
command path instead of the old short `[DENIED]` header or dropped replies.

## Verification — 2026-11-02

Base revision `9674eb49`, with this milestone and the earlier feedback changes
reviewable in the existing working tree. No separate source copy was created.

- `test_oled_health.py`: shipping overview/graph rendering and navigation at
  64/88/128 pixels, all actions visible on P4, scroll bounds on OLED, owned and
  time-ordered snapshots, ring capacity, millis rollover, empty/offline data,
  guest read-only navigation, poll gating, back navigation and the disabled API.
  Framebuffer previews were inspected; they are synthetic render evidence.
- `test_health_logging_ui.py --sanitize`: 359 checks, including real shared
  OLED command reply mapping and existing command authorization/audit behavior.
- `test_r1_history_store.py --sanitize`: 45 scenarios, 22,466 assertions.
- `test_oled_layout.py --sanitize` and the existing G2 health graph-core C++
  tests passed. All five suites ran with ASan/UBSan.
- Saved builds: `p4-phone-pipe` image 6,138,192 bytes, app headroom 1,287,856
  bytes; display-disabled `xiao-default` baseline also passed.

Existing `build-config-p4-phone-pipe` (~1.1 GiB) and
`build-config-xiao-default` (~1.3 GiB) retain matching firmware/ELF/manifests for
reuse and recovery. No additional build directory was created. Device deployment
results follow; live ring graph appearance requires user observation with actual
ring data.

### Deployment

Flashed the application only at `0x10000` on the same connected P4X-EYE,
USB serial `02:48:57:31:04:73`, port `/dev/cu.usbmodem112101`. The device identity
was checked before flashing; its partition table had already been verified to
match this profile during the preceding deployment. Esptool verified the data
hash. Settings, models, filesystem and bootloader were preserved.

Binary SHA-256: `24f5ccd253b14dc5d2594480dfe9f8d92d37f2035fde3312deac0bcbe68b4280`.
Boot #47 reported crash count zero, a successful LittleFS mount and
`P4-EYE ST7789 online (spi)`. Its ELF fingerprint `7ef03b4c7` matched the built
ELF; the 30-second capture contained no panic/assert markers. These checks
establish successful deployment and display initialization, not visual or live
ring-data qualification. Task-only previews, captures, executables, scripts and
redirected logs were removed after recording the results.
