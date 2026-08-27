# Design Brief: Raspberry Pi CM5 Carrier Board with ESP32 Coprocessor Socket

**Scope:** Schematic + 4-layer PCB layout for a hobbyist/prototype carrier board.
**Quantity target:** prototype run (JLCPCB/PCBWay class fab, 1oz copper, standard stackup).
**This brief is self-contained.** All module pin numbers below were verified against the
official datasheets listed in References; please re-verify against the same documents
rather than third-party pinout sites.

---

## 1. What this board is

A carrier for a **Raspberry Pi Compute Module 5** that also sockets a **Seeed XIAO
ESP32-S3** (14-pin XIAO form factor) as a radio/sensor coprocessor. The CM5 runs
Linux; the XIAO runs its own firmware. The two communicate over a UART link — and
that is the **only** connection between them.

**Isolation requirement (deliberate, load-bearing):** the CM5 must have no ability
to flash, reset, or de-power the module. No USB data connection between CM5 and
module, no reset line, no CM5-GPIO-controlled power switch. Firmware updates happen
only through the module's own USB-C connector, by the operator, with a cable. The
UART satisfies this by construction: the module exposes no BOOT/GPIO0 strap
externally, so ROM download mode cannot be entered over UART regardless of what the
CM5 transmits.

The XIAO must be hot-swappable with any XIAO-footprint module (e.g. Adafruit QT Py
series), so **all module connections are defined by header position, not by the
module's GPIO numbers**.

Deliberately excluded to keep this revision simple: **no HDMI, no Ethernet, no
PCIe/M.2, no CSI/DSI** — no impedance-controlled interfaces except USB 2.0.

## 2. References (source of truth)

1. **CM5 datasheet** (RP-008180-DS) — pinout, power, §4.1.1 carrier connector part.
2. **CM5IO board datasheet + published KiCad design files** (raspberrypi.com) —
   **copy the CM5IO subcircuits** for: USB-C power input (including CC wiring and
   ESD), rpiboot USB-C device port, microSD, fan connector, RTC battery, LEDs,
   and jumpers. This board is essentially "CM5IO minus video/network/PCIe, plus
   a XIAO socket."
3. **RP1 peripherals datasheet** — GPIO alternate functions (uart2 on GPIO4–7).
4. **Seeed XIAO ESP32-S3 schematic v1.1 + XIAO series package/footprint drawing**
   (wiki.seeedstudio.com) — socket footprint (two 1×7 rows, 2.54mm pitch, rows
   15.25mm apart) and the bottom-side test-pad positions.

## 3. Requirements

### 3.1 CM5 socket
- Two 100-pin board-to-board receptacles, carrier-side part per CM5 datasheet
  §4.1.1 (Amphenol 10164227-1001A1RLF, 1.5mm stack). Note: this changed from the
  CM4's Hirose DF40 — do not reuse a CM4 footprint.
- GPIO_VREF (pin 78) tied to CM5_3.3V for 3.3V GPIO levels. Antenna keep-out per
  CM5 datasheet §4.1.2.

### 3.2 Power input & staging

Stage map (every rail and what hangs on it):

```
USB-C power receptacle (5.1V/5A PSU, e.g. official 27W)
 ├─ CC1/CC2 ──► CM5 pins 94/96 directly (CM5 negotiates PD itself; add the
 │              CC discharge resistor per the CM5IO reference — no external
 │              PD sink controller anywhere)
 └─ VBUS ──► +5V plane
       ├─► CM5 5V inputs: pins 77, 79, 81, 83, 85, 87 — connect ALL six
       ├─► fan 5V, through a switch enabled by VBUS_EN (pin 111)
       └─► XIAO branch: load switch (EN = CM5_3.3V, pins 84/86)
                         → Schottky or ideal diode → module 5V pin

CM5_3.3V (pins 84/86, output) ──► Qwiic 3.3V, XIAO load-switch enable,
                                  GPIO_VREF (pin 78)
CM5_1.8V ──► unused
VBAT (pin 76) ◄── CR2032 holder
```

Electrical requirements, in order of importance:

- **Voltage window: 4.75–5.25V at the module's 5V pins.** With a 5.1V supply
  that leaves a total series budget of ≈70mΩ at 5A for the receptacle, any
  protection, and the copper. Consequence: **no series diode, no polyfuse, no
  current-sense resistor in the CM5 5V path.** The CM5IO reference connects
  VBUS to the 5V plane directly and so should this board. If input protection
  is desired, use a parallel low-clamp TVS (stand-off ≥5.25V) — never a series
  element.
- **All six 5V pins and every GND pin connected** (~0.83A per pin at full load).
  5V as polygon pours, ideally mirrored on two layers with via stitching.
- **Bulk capacitance in the CM5IO class**: on the order of 100µF near the CM5
  5V pins plus distributed 10µF/100nF. Do not go to millifarad-scale bulk —
  USB-C hot-plug inrush.
- **CM5_3.3V budget is 600mA total (300mA per pin)** and the rail powers down
  when the CM5 is off. Loads here: Qwiic connector (assume up to 300mA for
  user peripherals), the XIAO load-switch enable (negligible), GPIO_VREF.
- **Power-up sequencing** (CM5 datasheet §3.1: no CM5 pin may be driven before
  its 5V rail is up) is satisfied structurally: the XIAO only receives power
  after CM5_3.3V rises, so it cannot drive the UART pins of an unpowered CM5.
  Preserve this property — do not re-source the XIAO's power from raw 5V.
- **XIAO branch**: load switch rated ≥1A; a plain Schottky is acceptable as the
  ORing element on this branch (the module tolerates the drop — it runs from a
  3.7V battery normally). The drop-sensitive rail is the CM5's, not the XIAO's.
- Power button: momentary switch **to GND** on PWR_Button (pin 92). That pin is
  internally pulled to **5V** through 10k — no external pull, and it must never
  be driven by a 3.3V push-pull output.
- Status LEDs on LED_nPWR (pin 95) / LED_nACT per CM5IO (note the datasheet
  requires buffering LED_nPWR).
- Three additional software-controlled status LEDs, each a low-current LED +
  series resistor (~2mA at 3.3V) driven directly by a CM5 GPIO, grouped
  together on the board edge with silkscreen labels:
    GPIO23 -> green  "OS"    (Linux heartbeat)
    GPIO24 -> blue   "LINK"  (UART session to the coprocessor)
    GPIO25 -> red    "ERR"   (fault indicator)
  Active-high (GPIO high = lit) so they default off at power-up while the
  GPIOs are still inputs. No buffers or drivers needed — these are ordinary
  GPIO indicator LEDs. This supersedes the earlier note that GPIO23/24 are
  unused.
- No battery in this revision.

**rpiboot port power — deliberate deviation from the CM5IO:** the CM5IO uses a
single USB-C for both PSU power and rpiboot data, which means a host PC powers
the whole board during eMMC flashing (their datasheet recommends a powered hub
for weak ports). This board instead keeps the PSU port and a **separate USB-C
device port** for rpiboot (D+/D− to CM5 pins 105/103, USB_OTG_ID pin 101 per
CM5IO), with the device port's **VBUS left unconnected** — flashing then runs
on full PSU power and the two ports can be plugged simultaneously without
back-feeding. The module has no VBUS-sense pin, so enumeration works without
device-port VBUS.

### 3.3 XIAO socket and interconnect
- Socket: two 1×7 female headers, 2.54mm pitch, **rows 15.25mm apart** (verify
  against Seeed's package drawing). Orient so the module's USB-C end has a clear
  mechanical corridor — a mating USB-C plug overmold (~12mm) must fit, and the
  XIAO's u.FL whip antenna needs clearance. Wire by header position (pinout
  below is the XIAO ESP32-S3; QT Py modules have identical positions).
- **UART link** (33Ω series resistors in both lines):
  - CM5 GPIO4 (uart2 TXD) → module "RX" position (XIAO GPIO44)
  - CM5 GPIO5 (uart2 RXD) ← module "TX" position (XIAO GPIO43)
  - Leave CM5 GPIO6/7 unconnected — reserved for uart2 CTS/RTS in a future rev.
- **5V feed to the module's 5V pin, tracking CM5 power state without software
  control**: high-side load switch whose enable is driven by the **CM5_3.3V rail
  (pins 84/86)** — not by a Linux-controlled GPIO. CM5_3.3V is only up while the
  CM5 is powered on, so the module powers up and down with the CM5 automatically,
  and no software on the CM5 can toggle it. Follow the load switch with an
  **ideal-diode/ORing stage**. The ORing is mandatory: the XIAO's 5V pin is its
  raw USB VBUS net with no isolation diode, and a cable may be plugged into the
  module's own USB-C while socketed.
- **No USB data connection between CM5 and module — and no USB connector on
  the carrier for the module.** The XIAO has its own USB-C receptacle mounted
  on the module itself; it arrives with the module and sits above the socket
  when installed. The operator plugs a cable into it directly for flashing and
  debug. The carrier contributes nothing electrical here — no connector, no
  pads, no routed USB nets to the socket. Its only obligation is mechanical:
  keep the plug-insertion corridor at the module's USB-C end clear (a mating
  plug overmold is ~12mm long, ~6.5mm wide), with no tall components in the
  way. The board's only USB-C receptacles are the two in §3.2/§3.4 (power,
  rpiboot) — both on the CM5 side of the design.
- **No reset line to the module.** Do not connect anything to the module's
  bottom-side pads.
- **No I2C and no SPI between CM5 and module** — this is intentional (the module
  firmware is bus master on its I2C pins; SPI conflicts with the Sense
  expansion's SD card). Do not add them.
- CM5 GPIO23/24, freed by the above, are reused for status LEDs (see §3.2).

### 3.4 CM5 housekeeping (all per CM5IO reference)
- USB-C device port for rpiboot eMMC flashing (VBUS unconnected — see §3.2) +
  **nRPIBOOT (pin 93) jumper**.
- microSD slot (used by CM5 Lite variants only).
- Fan: 4-pin JST-SH, CM5IO pinout. Fan_PWM = pin 19 (open-collector, no external
  pull-up needed), Fan_Tacho = pin 16. Power the fan's 5V from the VBUS_EN-gated
  rail (pin 111) so the fan stops when the CM5 shuts down (datasheet §2.11).
- CR2032 holder to VBAT (pin 76). Primary cell — not recharged.
- EEPROM_nWP (pin 20) write-protect jumper.
- 3-pin UART console header on GPIO14/15. (Do **not** route GPIO14/15 to the
  XIAO — that UART is the default Linux console and its boot output would be
  interpreted as garbage input by the module firmware.)
- One Qwiic/STEMMA-QT connector on CM5 GPIO2/3.

### 3.5 Layout constraints
- 4 layers, solid inner ground plane; USB pairs over unbroken ground.
- Fine-pitch capability (~0.2mm) needed only for the CM5 connector fanout.
- Board ~70×60mm or as needed; four M2.5 mounting holes.
- Two 2.4GHz radios on this board (CM5 wireless + XIAO): maximize physical
  separation between the CM5 antenna region and the XIAO antenna area.

## 4. Known traps (why some of the above is the way it is)

- **VBUS back-feed** is the reason for the ideal diode: a laptop plugged into
  the socketed module's USB-C would otherwise back-feed the carrier's 5V rail
  through the module's 5V pin.
- **The isolation model has a cost, accepted deliberately:** if the module
  firmware hangs, recovery is physical (the module's own reset button, or
  unplugging carrier power) — the CM5 cannot reset or power-cycle it. The
  module is socketed and its USB-C is accessible, so this is fine for this
  application. Do not "helpfully" add a reset or power-control line back.
- **Why UART is safe but USB would not be:** the ESP32-S3's ROM download mode
  over UART needs a BOOT/GPIO0 strap the module does not expose externally,
  whereas its native USB can enter download mode with no strap at all. That
  asymmetry is why the link is UART-only.
- A supply that only offers 5V/3A still boots the CM5 fine — Linux caps USB
  peripheral current and warns. (`PSU_MAX_CURRENT=5000` in the bootloader
  EEPROM config suppresses the warning at the user's risk.)
- CM4 carriers are only "mostly compatible" — pins 16/19/76/92/94/96/99/100
  changed between CM4 and CM5. Don't copy CM4 reference circuits for those.
- The module's UART TX position spews the ESP32 ROM boot log at 115200 on every
  reset; the CM5-side software tolerates it. Nothing to do in hardware, just
  don't "clean it up" with odd terminations.

## 5. Deliverables & process

1. **Schematic first, as a review milestone** — I sign off before layout starts.
2. Layout, with a mid-layout screenshot check of the rpiboot USB pair routing
   and the 5V pours.
3. Final package: native CAD source (KiCad preferred), Gerbers + drill, BOM with
   manufacturer part numbers, pick-and-place file, 3D STEP, DRC/ERC reports.
4. Footprint verification note for the three risky footprints: the Amphenol CM5
   connector, the XIAO socket (15.25mm row spacing), and the USB-C receptacles —
   each checked against the manufacturer drawing.

Please quote fixed-price with the schematic milestone broken out, and flag
anything in §3 you believe is a mistake — pushback is welcome, but §3.3's
"deliberate" items have firmware-side reasons behind them.
