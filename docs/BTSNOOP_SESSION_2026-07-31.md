# Btsnoop session notes — 2026-07-31

Phone HCI captures against the shipping Even app (**com.even.sg 2.2.7**) on a GrapheneOS
Pixel Fold, targeting G2 glasses and R1 ring. This note consolidates what the captures proved
(and what they did not).

Related backlog: [`DEVICE_SETTINGS_BACKLOG.md`](DEVICE_SETTINGS_BACKLOG.md).  
R1 briefs: [`R1_RING_IMPLEMENTERS_BRIEF.md`](R1_RING_IMPLEMENTERS_BRIEF.md),
[`R1_RING_ONEPAGER.md`](R1_RING_ONEPAGER.md).

---

## 1. Capture setup

| Item | Result |
|---|---|
| **Enable HCI snoop log** | Works. Log under `/data/misc/bluetooth/logs/btsnoop_hci.log` |
| **Snoop socket (8872)** | Accepts then closes empty — useless here |
| **Log filtering MAP** | Ignore (SMS MAP privacy; not BT useful) |
| **`bugreportz`** | Broken on this device |
| **`adb bugreport`** | Works; pull + unzip `FS/data/misc/bluetooth/logs/*` |

Artifacts live under:

```
.scratch/btsnoop/                  # zips + extracted logs
.scratch/even-phone-pull/          # live APK pull (base.apk, arm64.apk, libapp.so)
```

G2 settings capture used in this note:

```
.scratch/btsnoop/even-btsnoop-g2settings.zip
.scratch/btsnoop/br_g2/FS/data/misc/bluetooth/logs/btsnoop_hci.log
```

### Phone BT / work-profile gotcha

Even app was in **Work profile (user 10)** with AppOps `BLUETOOTH_*=ignore`. Fixed with
`pm grant` / `cmd appops set allow` for BT + location; reinstall then pairing worked.

### Envelope parse reminder (G2)

```
AA | 21=TX / 12=RX | seq | len | totFrags | fragIdx | sid | flag | protobuf… | CRC16-CCITT-FALSE LE
```

CRC is over **protobuf only**. Multi-fragment messages must be reassembled by `(dir, seq, sid)`
before decode. Declared `len` = pb bytes on that frag (+ 2 CRC on last frag).

SID map used here: `docs/g2_proto/service_id_def.proto` /
`components/hardwareone/System_G2_Protocol.h`.

---

## 2. APK / `libapp.so` (shipping 2.2.7)

Live pull matches `/Users/morgan/even-app-extract` — **com.even.sg 2.2.7**.

Community tree `docs/FlutterApp-main/` is **incomplete** vs the shipping binary. `libapp.so`
exposes SET surfaces the backlog previously treated as non-existent, including:

- `setHealthSettingsStatus`
- `setSystemSettingsStatus`
- `setHealthEnable`
- sport run / file export / wear-hand UI strings

Implication: FlutterApp-main alone is not a complete allowlist for “what the app can write.”
Btsnoop + `libapp.so` strings beat the stale community RE.

---

## 3. R1 ring — HW-proven writes

### 3.1 `systemSettingsStatus` (subCmd `0x0F`) SET — real

Backlog item 4 assumed GET-only interest; shipping app also **SETs** `0x0F`.

- Payload shape: `[u32 unix_ts LE][flags…]`
- **byte5** flips with wear-hand / low-power style toggles (`1 ↔ 0`)
- Both observed SETs were acked

Rename of the ring in the UI did **not** put a name on the ring BLE link (app-local).

### 3.2 `userInfo` (`0x04`) — field map decoded

Profile goes to the ring (write-only; empty ack) **and** cloud `/v2/g/user_info`.

| Offset | Meaning | Evidence |
|---|---|---|
| `b0` | Gender: `0`=Male, `1`=Female, `2`=unset/default | UI toggles |
| `u16LE @2` | Height **cm** | 8'11"/100 lb → 272 cm / 45 kg; later 2'0"/200 kg |
| `u16LE @4` | Weight **kg** (integer) | same |
| rest | often 0; DOB not clearly on-ring | — |

This **supersedes** the backlog caveat that field semantics were undecoded. Composing a
non-default payload is now HW-grounded (still no read-back oracle).

### 3.3 Still open on R1

- `hr measure` reproducibility
- measure/daily for spo2 / temp / hrv / sleep
- focused btsnoop of wear-hand alone
- blutter on `libapp.so` for bitmap semantics of `0x0E` / `0x0F`
- sport / file-export probes
- Avoid blind SETs on dangerous system-module opcodes

---

## 4. G2 glasses — settings capture

Firmware seen in restore frames: **`2.2.6.10`** (left/right version strings).

### 4.1 HeadUp — confirmed shipping setter (SID `0x09`)

`DeviceReceiveInfoFromAPP` → `deviceReceiveHeadUpSetting` (`g2_setting.proto`):

| User action | Wire |
|---|---|
| HeadUp off | `headUpSwitch = 0` |
| HeadUp on | `headUpSwitch = 1` |
| Angle 19 | `headUpAngle = 19` |

Glasses **ACK with the same body** (magic-correlated echo). Immediately before the off
toggle, a basic-settings restore still showed `headUpSwitchRestored=1`,
`headUpAngleRestored=40`. After off, subsequent restore omitted the switch field (proto3
default → off) while angle lingered at 40 until the explicit angle write.

**Backlog impact:** HeadUp is no longer “schema-only / no setter” — it is a live
phone→glasses write we can replay.

### 4.2 Dashboard widgets master / tile config (SID `0x01`)

One `Dashboard_Receive` with `DashboardDisplaySetting` in the whole G2 capture:

| Field | Value |
|---|---|
| `displayMode` | 4 |
| `statusDisplayOrder` | WEATHER(1), MESSAGE(2), POWER(3) |
| `widgetDisplayOrder` | NEWS(1), SCHEDULE(3), STOCK(2), STOCK(2) |
| `halfDayFormat` | 1 |
| `temperatureUnit` | 2 (°F) |

Context: user disabled then re-enabled **Widgets** on the glasses. Nearby short frames clear
schedule/stock-style content; those clears also appear periodically later as sync noise, so
the **display-setting** frame is the durable config signal.

No second `DashboardDisplaySetting` appeared when individual collection items were removed
(see §4.3) — that UI is a different channel.

### 4.3 Widget / app **collection** edits (SID `0x03` Menu) — the important correction

User removed items from the selectable collection in the app and sent to the glasses
(Notification → Dashboard → Even AI). That is **not** Even AI launch traffic and **not**
`DashboardDisplaySetting`.

It is a **Menu SID `0x03` membership list** push. After HeadUp angle=19:

| Δt | Menu app IDs sent | Removed |
|---|---|---|
| +21s | Dashboard(1), EvenAI(7), Conversate(11), Nav(8), Translate(5), Teleprompt(6), `266` | **Notification (4)** |
| +52s | EvenAI(7), Conversate(11), Nav(8), Translate(5), Teleprompt(6), `266` | **Dashboard (1)** |
| +73s | Conversate(11), Nav(8), Translate(5), Teleprompt(6), `266` | **Even AI (7)** |

IDs match `service_id_def` SIDs for the named apps. **`266` is unidentified** (not in the
small SID enum we use day-to-day).

**Correction:** earlier read of SID `0x07` `CTRL` status 2/3 as “Even AI worked” was wrong.
User was editing the collection; Even AI did not successfully run. The collection removals
are the Menu list above. ~90s later, SID `0x07` still shows `ENTER`(2) / `EXIT`(3) with
device echoes (and a prior RX `WAKE_UP`-ish status 1) — treat as failed/partial Even AI
touch or UI side-effect, **not** proof of a working session.

### 4.4 Notification control (SID `0x04`)

Only one clear short TX in this capture (connect-time style):

- `NOTIFICATION_CTRL`: `notifEnable=1`, `autoDispEnable=1`, `dispTime=5`, `avoidDisturbEnable=0`

Removing the **Notification** item from the collection did **not** send `notifEnable=0`.

### 4.5 Noise around the same window

- Large SID `0x01` stock content pushes (AMD / INTC / UI tickers)
- SID `0x14` Health coaching text (“resting pulse…”, sleep advice) — content, not collection
- Periodic weather + empty schedule clears on Dashboard
- Connect ritual: universe units, dominant hand, gestures, wear detection, Even AI `CONFIG`

### 4.6 G2 — what we can replay now

| Knob | Path | Confidence |
|---|---|---|
| HeadUp switch | SID 9 / `DeviceReceive_Head_UP_Setting.f1` | **HW proven** |
| HeadUp angle | SID 9 / `.f2` | **HW proven** |
| Dashboard tile order / counts | SID 1 / `DashboardDisplaySetting` | **HW proven** (one sample) |
| App/widget collection membership | SID 3 / menu item list (`f4` = app id) | **HW proven** (three stepwise removals) |
| Even AI enter/exit | SID 7 / `EvenAIControl` | Seen on wire; **not** a successful user session here |

---

## 5. Parsing pitfalls learned this session

1. **TX is `AA 21`, RX is `AA 12`** — filtering the wrong preamble hides the phone→device SETs.
2. **Reassemble fragments** before protobuf decode; stock/news bodies are multi-frag.
3. **Menu collection ≠ dashboard widgets.** Same English word “widgets” in the app UI maps to
   at least two protocols (SID 1 display setting vs SID 3 menu list).
4. **SID 7 traffic ≠ “Even AI worked.”** Collection removal of Even AI is SID 3; SID 7 may
   still fire around UI interaction.
5. Work-profile AppOps can make BT look “broken” while snoop/HCI still exists.

---

## 6. Suggested backlog follow-ups

Update [`DEVICE_SETTINGS_BACKLOG.md`](DEVICE_SETTINGS_BACKLOG.md) when convenient:

1. **R1 `0x0F` SET** is real (not only GET); document byte5 hand/low-power flip.
2. **R1 `userInfo` field map** (gender / height cm / weight kg) — drop “undecoded” caveat;
   keep no-read-back warning.
3. **G2 HeadUp** — promote to replayable item (switch + angle).
4. **G2 Menu collection** — new item: push SID 3 membership list to add/remove Notification /
   Dashboard / Even AI / etc.
5. **G2 DashboardDisplaySetting** — optional item for news/stock/schedule/status tile order.
6. Note FlutterApp-main incompleteness vs shipping `libapp.so` 2.2.7.

---

## 7. OTA captures (same day, later)

Successful Even-app updates were also snooped (ESP32 disconnected). Details and
tooling: [`OTA_PASSIVE_CAPTURE.md`](OTA_PASSIVE_CAPTURE.md),
[`OTA_RESEARCH_FINDINGS_2026-07-31.md`](OTA_RESEARCH_FINDINGS_2026-07-31.md),
[`tools/btsnoop/README.md`](../tools/btsnoop/README.md).

| Capture | Path |
|---|---|
| G2 flash | `.scratch/btsnoop/g2-ota-20260731-234555/` (use `btsnoop_hci.combined.log`) |
| R1 flash | `.scratch/btsnoop/r1-ota-20260731-235323/` → `dfu_out/` (app `2.2.7.0005`) |

**Lessons:** always keep `.last` + current (65535-packet rotation); G2 = SID
`0xC0`/`0xC1`; R1 = `otaStart` → disconnect → Nordic Secure DFU (not SMP on wire);
init hash = `SHA256(bin)[::-1]`.

```bash
./tools/btsnoop/pull_hci.sh g2-ota   # builds combined log
python3 tools/btsnoop/ota_extract.py …/btsnoop_hci.combined.log -o …/out
python3 tools/btsnoop/r1_dfu_extract.py …/btsnoop_hci.log -o …/dfu_out
```

---

## 8. Quick command crumbs

```bash
# Prefer the helper (copies .last + builds combined):
./tools/btsnoop/pull_hci.sh settings-capture

# Manual bugreport pull
adb bugreport .scratch/btsnoop/even-btsnoop-NAME.zip
unzip -o even-btsnoop-NAME.zip -d br_NAME 'FS/data/misc/bluetooth/logs/*'

# Live APK / libapp
adb shell pm path com.even.sg
# pull base.apk + split; extract lib/arm64-v8a/libapp.so
```

Decoder source of truth for envelope layout:
`components/hardwareone/System_G2_Protocol.cpp` (`g2BuildEnvelope` / `g2ParseEnvelope`).
