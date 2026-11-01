# Wi-Fi and Bluetooth coexistence: a cross-chip study (2026-10-17)

**What this is.** A read-only study of how Wi-Fi and Bluetooth share the radio on every HardwareOne board, and a
ranked plan to improve it in ways that help all chips, not only the P4.
- **Method.** Four parallel investigations (board configs, our own radio use, ESP-IDF mechanics, prior project
  evidence), one synthesis, then three adversarial reviews: correctness, risk and completeness. This document keeps
  only what survived the reviews.
- **Status.** Nothing here has been built or flashed. Line numbers refer to the working tree on this date.
- **Background.** The phone-pipe scan fix (firmware-007) came first; see `docs/PHONE_PIPE_HANDOFF.md`.

## 1. The short answer: is there an Espressif library for this?

**Yes, and it is already running.** ESP-IDF's software coexistence (the `esp_coex` component plus a prebuilt
`libcoexist.a`) is Espressif's answer. There is no separate library to add.

| Board | Chip | Coexistence |
| --- | --- | --- |
| feather_esp32_v2 | classic ESP32 | On, IDF default |
| feathers3, xiao_s3 | ESP32-S3 | On, IDF default |
| p4x_eye | P4 host + C6 companion | Runs on the C6; the P4 has none of its own |
| qtpy_esp32, xiao-no-bluetooth | no Bluetooth built | Not applicable |

**What it does.** One antenna, shared in time. The library picks a time-slice scheme from what each radio is doing.
- **The split while a BLE scan runs.** The tables are byte-identical on ESP32, S3 and C6, so on every chip a scan gets:

  | Wi-Fi state | BLE scan's share |
  | --- | --- |
  | associated | about 40% of each ~102 ms beacon period |
  | connecting | about 30% |
  | scanning | about 50% |

- **The classic ESP32 differs.** It also has its own software arbiter and the `FULL_SCAN` option, which is already on.
  Treat it as its own case, not a copy of the C6.

**There are almost no knobs:**
- **`esp_coex_preference_set()`** does nothing in IDF 5.5.5: it returns OK on all three chips. ESPHome's "prefer
  Bluetooth" call is dead code.
- **Bluedroid's `esp_bt_dev_coex_status_config()`** is handled only by the classic ESP32 controller.
- **`esp_coex_status_bit_set()`** with the BLE-Mesh "config" bit does raise BLE's share, to about 55%. But it is an
  undocumented use outside BLE Mesh, and the S3 controller can silently clear it.
- **ESP-Hosted adds nothing** for sharing inside the C6. Its coexistence option is for a second, external radio chip,
  which no board has.

**Real levers do exist, but on the Wi-Fi side:**
- **Shorter Wi-Fi scans:** per-channel dwell time, and a channel bitmap.
- **`coex_background_scan`:** a Wi-Fi scan option made for coexistence. It works on ESP32/S3 only; ESP-Hosted drops it.
- **Controller options:** the active-scan backoff cap, and `FULL_SCAN` on ESP32.

**So improvements that help every chip have to come from when and how HardwareOne uses each radio.**

## 2. What HardwareOne does today that matters

- **Wi-Fi power save is forced off** on every join and when ESP-NOW starts. Under coexistence this doesn't keep
  Wi-Fi awake: it sleeps outside its own slice anyway. So power-save mode is not the BLE fix.
- **The Wi-Fi reconnect "hunt" never stops.**
  - **How.** Arduino's STA handler reconnects at once on every drop reason, including "no AP found", with no backoff.
  - **Why it hurts.** Away from home the radio sits in the connecting/scanning schemes indefinitely, which give BLE
    the least airtime.
  - **The evidence.** A hunting device's ESP-NOW ACK latency measured about 245 ms, against 41 ms otherwise.
- **Wi-Fi scans are long.** Each one takes about 1.3–3.9 s: HardwareOne uses 100–300 ms per channel, against the IDF
  default of 0–120 ms. All are user- or UI-initiated (`wifiscan`, the Network pages, setup); there are no background
  scans.
- **ESP-NOW leaves Wi-Fi in AP+STA mode**, with a hidden AP beaconing every 100 ms.
  - Espressif rates ESP-NOW receive alongside BLE as stable only in STA mode.
  - When ESP-NOW stops, Wi-Fi doesn't go back to STA-only.
- **Direct connects** (the phone pipe; the ring's and glasses' Arduino `BLEClient::connect`) use Bluedroid's default
  initiator scan: a 30 ms window every 60 ms. That is the same half-duty pattern that made the phone scan miss.
- **The S3 boards enable both BLE 4.2 and 5.0 host features.**
  - **IDF advises against this.**
  - **Connects then depend on history.** Once any BLE-5 event happens (for example a peer moving a link to 2M PHY),
    every later connect uses the extended path with different parameters.
  - **The other chips are 4.2-only:** ESP32 and the P4.
- **Absent peers are retried forever at short intervals.**
  - **The cost.** While a scan runs, live links share BLE's ~40% with the scan, instead of getting the usual 50%.
  - **Example.** A phone left at home costs a 4 s scan about every 34 s.
- **Boot overlaps Wi-Fi with BLE reconnects.** On the P4 after the firmware-007 flash, the phone's first connect failed
  with HCI 0x3e while the glasses were reconnecting.
  - **What 0x3e means.** The link was started, but no packet got through in its first six connection events.
  - **Why it fits.** That is what a long radio turn by the other side looks like.
- **Nothing coordinates the two radios.** The only cross-radio gestures are two 150 ms sleeps before `openwifi` on the
  G2 Network page.

## 3. The plan, ranked

This is the reviewers' consensus order. Each lever names the chips it helps. "Measure" means a bench A/B before
adopting it.

| # | Lever | Chips | Effort | Why this rank |
| --- | --- | --- | --- | --- |
| 1 | **Measure first.** Per-scan and per-connect records for G2, ring and phone (like the phone pipe's `PIPE_SCAN`), a fuller Wi-Fi state tag, HCI-level connect attempts, and a debug readout of the coexistence status. | all | medium | Turns every other lever into an A/B. No behaviour change. |
| 2 | **HardwareOne owns the Wi-Fi reconnect.** A capped backoff (for example 5/15/45/90/180 s) replaces the endless hunt; the last channel is tried first. | all Wi-Fi boards | medium | Removes the worst coexistence state when away from home. Verified premise. |
| 3 | **ESP-NOW cleanup.** Return to STA-only when ESP-NOW stops, and give the hidden parking AP a long beacon interval. | all ESP-NOW boards | small | A verified defect; cheap. |
| 4 | **Shorter Wi-Fi scans.** Per-channel dwell at the IDF default, an optional channel bitmap, and `coex_background_scan` when associated (ESP32/S3). | all | small | Deterministic and cheap. |
| 5 | **S3: one BLE host feature set.** Pick BLE 4.2-only, matching ESP32 and P4. | S3 | small | Makes S3 connects deterministic and like the other chips. A precondition for #7. |
| 6 | **Longer tails for absent peers, and a cap on `ringscan`.** Retry less often after N misses; snap back on a trigger (user action, tap, Wi-Fi change, an LLM request needing the phone). | all | small | Less scan time while links stream. |
| 7 | **Continuous initiator window for direct connects** (`esp_ble_gattc_enh_open`, window = interval). Phone pipe on P4 first, then ESP32; S3 after #5. | ESP32, P4; S3 later | small | Same reasoning as the phone scan fix. **Not** a fix for 0x3e. Pair it with #6, because absent-ring retries would double radio duty. |
| 8 | **One bounded bring-up order** for boot and C6 restart. Each step has its own short limit, and BLE doesn't wait for Wi-Fi when the AP is absent. | all | medium | The real lever for 0x3e: keep connects out of Wi-Fi's connecting and scanning turns. Needs a baseline first. |
| 9 | **A narrow cross-radio broker.** Background Wi-Fi reconnect attempts briefly wait while BLE is discovering or connecting. | all | medium | Narrowed: never on the httpd or `cmd_exec` scan paths, because those would freeze HTTP. |
| 10 | **Active-scan backoff A/B** (controller config) for G2 and ring discovery. | all (C6 needs a reflash) | small | Config only. |
| 11 | **Slower server-mode advertising** after a fast window; stop when connected. | all | small | Advertising cadence only (see §5). |
| 12 | **One owner for Wi-Fi power save,** keeping NONE as the default. A MIN_MODEM A/B only after #3, in STA-only mode. | all | small | Correctness; little BLE gain expected. |
| 13 | **Bench-only coexistence GPIO-debug build,** to see the real slices on a logic analyser. | ESP32, S3, C6 | medium | Validates the airtime model. |
| 14 | **The status-bit "boost"** (`ESP_COEX_BLE_ST_MESH_CONFIG`) during a bounded discovery scan. | ESP32/S3 first; P4 only with a C6-side lease | medium | Bench only. It must re-assert and read back, because the controller can clear it. |
| 15 | **Per-chip controller parity:** ESP32 to 4 links (about 1 KB DRAM each), and a C6 channel-assessment A/B. | per chip | small | Capacity and stability, not airtime. |
| 16 | **Wi-Fi RX buffers.** | all | small | Needs the Arduino-side switch (`WiFi.useStaticBuffers`, about 51 KB DRAM) or a vendored change; low priority. |
| 17 | **Moving Bluetooth off core 0** on ESP32/S3, as coexist.rst suggests. | ESP32, S3 | large | Test-only. Undoes the I2C interrupt-separation fix; highest regression risk. |

**Lever 1 is built (2026-10-17, not yet on hardware).** It changes no radio behaviour. The phone pipe's status text
now names the source of a cancel instead of "phone operation cancelled".
- **Build flag.** `ENABLE_BLE_COEX_RECORDS` (on by default; off without Bluetooth). About 8 KB of flash (8,176 B
  measured on `xiao-openclaw` before the per-attempt record), about 8 KB of PSRAM (the logs, a 320 B line buffer and
  the 6 KB `blerecords` reply) and under 0.2 KB of internal RAM. Off, the hooks are no-op stubs, the Wi-Fi mirror is
  not built, and the readers report that the records are not in the build. `xiao-openclaw` turns it off: with it on,
  that image overflowed its app partition by 5,696 B. With them on, the XIAO S3 images are now tight: `xiao_s3`
  leaves 6,976 B and `xiao-default` 18,688 B. All 22 build lanes (ESP32, S3 and P4 boards and saved
  configurations) built cleanly on 2026-10-17.
- **Records.** `BLE_CoexRecords` keeps the last 16 scans and 16 connects for G2 (each temple), the ring and the phone,
  with per-peer outcome counters. `blerecords [scans|connects|clear]` shows them as whole lines within the 4 KB
  command reply; each record also prints two `BLE_COEX` log lines, the record and its Wi-Fi span (run `loglink on` to
  reach the web log and file). No addresses or names are kept.
- **Scans.** Purpose (discovery, saved, repair, reconnect, enroll); outcome (found, partial, missed, aborted when it
  ended well before its length, start-failed when the controller never confirmed it); duration, from the
  controller's start and end events when both were seen (`dur_ms=…(ctl)`), else the task's span (`(task)`); time to
  the first match; all reports; matches; best RSSI; and the scan shape. "Matches" counts differently per peer: every
  matching report for G2 and the phone, at most one per address for the ring (Arduino de-duplicates its callback).
- **Connects (G2, ring).** One per Arduino connect: purpose (saved address, discovery or repair), outcome, elapsed
  time, Arduino's connect stage, the GATT status and the disconnect reason for that target (0x3e, or 0x100,
  Bluedroid's "connection cancelled": about 30 s is its link timeout, a few ms a controller refusal), and the
  job's wait for other clients' setups on the one setup worker (`wait_ms`, per job, so both temples' records of one
  job repeat it; `docs/BLE_SETUP_WORKER_PLAN.md`). It runs from submission to the job's turn, minus the time the
  job itself was not ready (a role change, the ring's wait for G2 audio). A job held up 50 ms or more also logs
  `BLE_COEX: setup wait <job> ms=…`, which covers re-seeks that miss and make no connect record; the phone and the
  ring's retirement log that line too.
- **Attempts (phone).** One per attempt that found the phone, from GATT registration to READY or the first stop: the
  step reached (precheck, register, open, discovery, mtu, search, subscribe, cccd, secure, hello, ready), the first
  non-OK GATT status, the first disconnect reason for the phone's own address (also on a live link, such as 0x08
  during discovery), the cause (the `phonepipe stats` end-reason numbers) with its source for cancels (stopped,
  replaced, expired, service-changed; `yielded` went with the cancel itself in S1 of
  `docs/BLE_SETUP_WORKER_PLAN.md`), and the wait from admission to the setup worker's grant of its turn. They show
  in `phonepipe scan` after the scans, in `phonepipe stats` as counters, and in `blerecords connects`.
- **Wi-Fi** (`System_WiFiCoex.h`), per record: the state at its start and end (off, idle, connecting, hunting,
  scanning or associated), the flags at both ends (ESP-NOW, AP, STA, IP, `radiopower off`, a down C6, auto-reconnect,
  the boot connect), the last STA disconnect before it (reason and age), and Wi-Fi scans, connect attempts and STA
  disconnects during it. The mirror listens on Arduino's network event list, so registering it at boot creates no
  event loop or task, and no reader touches the Wi-Fi driver or the P4's RPC link. "Hunting" is inferred: auto-
  reconnect is on and the last disconnect, other than a voluntary leave, was under 10 s ago.
- **Not covered yet:** a readout of the coexistence scheduler's own status bits; inbound connections to the device's
  own GATT server; an attempt cut by a BLE host reset (no record); a record dropped when the log is busy for 50 ms;
  and a phone link failure that lands in the same 10 ms tick as a cancel, which reads as the cancel (the cancel is
  decided before the link's events, so no new link work starts on a cancelled attempt).

## 4. Measurement plan (before changing behaviour)

**Boards:**
- feather_esp32_v2 (its own arm: arbiter and FULL_SCAN);
- xiao_s3 or feathers3;
- p4x_eye;
- qtpy_esp32 as the no-Bluetooth control.

**The matrix.**
- **Wi-Fi states:** off, associated and idle, associated under HTTP load, hunting (AP off), and a repeated `wifiscan`
  loop.
- **BLE states:** off, server advertising, one idle link, G2 idle, G2 mic streaming, a ring or phone discovery scan,
  and a ring direct reconnect.
- **ESP-NOW:** off and on, as a separate pass.

**Metrics for each cell:**
- scan hit rate and time to the first match;
- connect time and the disconnect reason of a failure. Bluedroid retries a failed establishment up to three times
  without telling the app, so retries can't be counted: a success after retries only takes longer, and 0x3e on a
  failure means every retry failed;
- G2 mic packets/s and sequence loss;
- HTTP RTT median and p95;
- ESP-NOW ACK latency and loss over 1,000 sends;
- `perftop` IDLE0/IDLE1 on ESP32/S3;
- for boot order, 10 cold boots per board with a timeline.

**Controls:**
- the same AP and channel; record RSSI only, never names or addresses;
- quiet filesystem activity;
- fixed log levels, and `loglink on` before a capture: the `BLE_COEX` lines are ESP-IDF log lines, which otherwise
  reach only the serial console and can interleave with other output there;
- the firmware hash recorded for every run.

**The P4 starts here.** The firmware-007 phone-pipe A/B arms are the first part of this plan.

## 5. What not to do

- **Don't add `esp_coex_preference_set(ESP_COEX_PREFER_BT)`.** It is a no-op in IDF 5.5.5.
- **Don't build on `esp_bt_dev_coex_status_config()`.** Only the ESP32 controller handles it, and not over ESP-Hosted.
- **Never set `ESP_COEX_BLE_ST_MESH_STANDBY`.** It drops BLE to about 20% while associated. Never leave any mesh bit
  set without a bounded, re-checked scope.
- **Don't expect MIN_MODEM to fix BLE misses.**
  - Under coexistence Wi-Fi already sleeps outside its slice.
  - Power save is inert in AP+STA (ESP-NOW).
  - On ESP32/S3 the join-time PS_NONE also guards an I2C interrupt-WDT race; that needs a stress run before any
    change.
- **Don't "fix" `setMinPreferred` twice in `Bluetooth.cpp`.** It is copied from Arduino's own example and is valid as
  is. Changing the second call to `setMaxPreferred(0x12)` would ask centrals for faster connections, which means more
  BLE airtime and less for Wi-Fi.
- **Don't tune for the P4 in its own Kconfig.** It has no coexistence module, so tune the C6. Its Wi-Fi buffer values
  come from the P4's Arduino init over RPC.
- **Don't enable external coexistence.** It is for a second radio chip.
- **Don't gate ESP-NOW TX on radio activity.** That is a known deadlock trap.
- **Don't change the ESP-NOW connectionless power-save window or interval.** coexist.rst warns that it hurts
  Bluetooth.
- **Don't scan with a window below the interval.** Half-duty listening is what missed 44–61% under the 60/40 scheme.
- **Don't move cores, or put task stacks in PSRAM, without data and the glasses-plus-gamepad stress run.**
- **Don't put a broker wait inside `wifiScanForEach` on the httpd or `cmd_exec` paths.** It would freeze HTTP and the
  command queue.
- **Don't patch vendored Arduino files** where a HardwareOne-side call does the job. (`STA.cpp` already carries one
  local patch; record any new one in the pinning notes.)
- **Don't flash, reflash the C6, or commit** without the user's per-instance approval.

## 6. Corrections the reviewers made

These are listed so nobody re-derives the wrong version:
- **HCI 0x3e is not an initiator-window problem.** It happens after CONNECT_IND, when no packet is exchanged in six
  connection events. It fits a long Wi-Fi turn, so it belongs to bring-up ordering (#8), not to the continuous
  initiator (#7).
- **On S3, "set `is_aux=false`" doesn't stick.** With both 4.2 and 5.0 enabled, Bluedroid forces the extended connect
  path once a BLE-5 event has latched. Hence #5 before #7.
- **There are no background Wi-Fi scans to broker.** All scans are user- or UI-initiated. The automatic radio search is
  the reconnect hunt (#2).
- **The RX-buffer knob is Arduino's.** Not `makeArduinoCompatibleWifiInitConfig` (recovery re-inits only) and not the
  P4's `WIFI_RMT_*` (overwritten to 4).
- **The broker would freeze HTTP if it waited on the httpd path,** because `wifiscan json` runs on the single httpd
  task.
- **Owning the reconnect needs more than `setAutoReconnect(false)`:**
  - **No executor.** No long-lived Wi-Fi task exists; boot_net deletes itself.
  - **A conflated flag.** Arduino's flag doubles as the firmware's "Wi-Fi wanted" intent, which `radiopower`,
    `closewifi` and `wifiautoreconnect` read.
  - **So:** add a separate intent flag and a non-blocking timer ladder.
- **The C6's 46–49% "asleep" figures came from logs not in the repo.** Treat them as unverified until #1 reproduces
  them.

## 7. Open questions

- **ESP32 and S3:** do their controllers set the same BLE "scan" status bit as the C6 during a scan? #1 answers it
  cheaply.
- **Advertising alone:** does server-mode advertising alone hold Wi-Fi at about 50%?
- **BLE's idle slice:** does Wi-Fi use BLE's slice when BLE has nothing to send? A #13 trace would show it.
- **Peer addresses:** are G2 temple and R1 ring addresses stable across reconnects? That would allow passive or direct
  reconnects instead of active scans.
- **ESP-NOW channel:** does `esp_wifi_set_channel` hold the ESP-NOW channel in STA-only mode while unassociated?
- **The I2C reason for PS_NONE:** is it still valid after the move to the i2c-ng driver?
- **Feather V2 drops:** did its reason-200 and reason-204 Wi-Fi drops ever happen with BLE active? A Bluetooth on/off
  A/B settles it.
- **C6 multiple GATT clients:** does the IDF 5.5.5 C6 regression (esp-idf#18931) reproduce with two temples plus the
  ring or phone? It needs the four-link C6 image.
