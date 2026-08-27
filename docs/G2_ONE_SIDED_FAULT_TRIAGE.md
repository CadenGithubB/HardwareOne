# Diagnosing a one-sided Even G2 fault with an ESP32

*Using [Hardware One](https://github.com/CadenGithubB/HardwareOne) as an independent second host for your glasses.*

If one temple of your G2s is misbehaving, the Even app can only tell you "it's not
connecting." An ESP32 running Hardware One connects to **each temple as a separate BLE
peripheral** and prints per-arm counters, so you can tell a dead temple from a bad
pairing without sending the glasses back blind.

---

## Read this first, or you will misdiagnose it

The G2s are **two independent BLE devices** that do not sync with each other, and they
are **not symmetric**:

| | Left temple | Right temple |
|---|---|---|
| Display / lens rendering | none — by design | **all of it** |
| Inbound notifications (events, battery, acks) | **silent** on firmware 2.2.0.24 and 2.2.7.14 | everything |
| Microphone (LC3 audio) | **yes** | silent on 2.2.0.24 |

So on stock firmware these are **normal, not faults**:

- Left battery reads `?` — the left arm never pushes a battery frame.
- Left `rx=0` forever — nothing arrives on its notify channel, ever.
- Nothing ever displays "on the left lens" — the lens is driven from the right temple.

Judge the left arm by **whether it connects and accepts writes**, and by **whether its
mic streams**. Judge the right arm by **notifies, heartbeat acks, and display**.

---

## Setup (ESP32)

Any ESP32 or ESP32-S3 with BLE works. **No screen, gamepad, or sensors needed** — a bare
dev board on USB is enough; you drive everything from the serial console.

```bash
git clone https://github.com/CadenGithubB/HardwareOne.git
cd HardwareOne
```

In `components/hardwareone/System_BuildConfig.h`, confirm:

```c
#define ENABLE_BLUETOOTH        1
#define ENABLE_G2_GLASSES       1
```

Then build and flash (`esp32` for classic ESP32 boards, `esp32s3` for S3 boards):

```bash
idf.py set-target esp32s3 && idf.py -p /dev/ttyUSB0 flash monitor
```

First boot runs a setup wizard — create an admin account when prompted. Then, in the
serial monitor:

```bash
login <your-user> <your-pass>
```

> **Close the Even app and turn off your phone's Bluetooth first.** Each temple accepts
> one central at a time. If the phone holds the link, the ESP32 sees nothing.

---

## The five-minute triage

```bash
debugg2 1
debugg2lifecycle 1
openg2 auto
g2status
```

`g2status` is the whole picture in one line:

```
state=connected L=up R=up mtu=L244/R244 batt=L-1/R87 tx=L120/R340 rx=L0/R902
```

An MTU still sitting at `23` means that side never finished negotiation — a bad sign on
its own.

Then fill in the details:

```bash
g2info          # per-side MAC, name, MTU, battery, firmware version
g2battery       # forces a fresh battery request on both temples
g2protostats    # per-service TX/RX counts — shows exactly which channels are alive
g2dumpframes    # last ~32 envelopes, for forensics right after a drop
```

## Isolating the bad side

This is the test the phone app can't do — **connect one temple on its own**:

```bash
closeg2
openg2 left      # or: openg2 right
g2status
```

A temple that won't connect *by itself* is a temple problem. A temple that connects fine
alone but drops when both are up is a link/interference problem, not dead hardware.

Other per-side probes:

```bash
g2recover        # reconnect the missing arm without dropping the good one
g2verbose on     # log every advert seen, with name and RSSI, during a scan
g2scan
```

**Right-arm (display) test:**

```bash
g2show Hello from the ESP32
g2clear
g2reopen         # if the lens app hung and won't relaunch
```

**Left-arm (mic) test** — the only real proof the left temple is alive:

```bash
g2micon          # starts the LEFT arm's audio stream
g2micstats       # run a few seconds later — frame count should be climbing
g2micoff
```

---

## Reading the result

| What you see | What it means |
|---|---|
| Bad side never appears in a scan (`g2verbose on` + `g2scan`) | That temple isn't advertising — flat battery, or its firmware isn't booting. Charge in the case and retry. |
| Appears in the scan, but `openg2 <side>` fails or drops immediately | Link-layer or firmware fault on that temple. This is a hardware/RMA signal. |
| `L=up`, MTU negotiated, but `rx=0` and `batt=?` | **Normal on 2.2.0.24 and 2.2.7.14.** Not your fault indicator. |
| `L=up` but `g2micstats` frames stay at 0 after `g2micon` | Left temple's audio path is the fault — connects, won't stream. |
| `R=up` but `g2show` renders nothing, log says "plugin silent" | Right lens app task is hung. Try `g2reopen`, then `closeg2` + `openg2`. Often it's just that the glasses aren't being worn — the firmware idles the task. |
| Log says "TX wedged — forcing disconnect" for one side | That temple's BLE stack locked up; firmware fault, recovers on reconnect. |
| **Both** temples connect and behave fine from the ESP32 | The glasses are healthy. Your problem is phone-side pairing state — re-pair from the app. |

That last row is the most valuable outcome: it's the one result that saves you from
shipping working hardware back for repair.

---

## No ESP32 on hand?

A generic BLE scanner app (nRF Connect, LightBlue) gets you the first row of that table
only: both temples should advertise as `Even G2_<id>_L_...` and `Even G2_<id>_R_...`. If
the bad side never shows up there either, that's already a strong hardware signal and you
don't need the ESP32 to file the ticket.

---

*Hardware One is open-source ESP-IDF firmware; the G2 protocol notes behind this doc are
in `docs/G2_PROTOCOL.md` in the same repo. Firmware behaviour above was verified against
G2 firmware 2.2.0.24 and 2.2.7.14 — a future Even OTA may restore left-arm
notifications, which would change the "normal" rows in the table.*
