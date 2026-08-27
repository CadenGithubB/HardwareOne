# Design Brief: CM5 + XIAO Carrier Bring-Up Dummies (reviewed v2)

**Scope:** Schematic and PCB layout for two small test boards that plug into a
CM5/XIAO carrier in place of the real modules. They must expose wiring and power
faults without sacrificing a CM5 or XIAO. They do not run Linux or ESP-IDF.

**Quantity:** Prototype run, five of each board, suitable for JLCPCB/PCBWay.

**Design can start before the carrier PCB is finished.** The official CM5 pinout,
CM5 mechanical envelope, XIAO footprint, UART pins, and status-LED GPIOs are
already fixed. Before either PCB is ordered, compare the final carrier netlist
against the net contract in this brief.

## 1. Sources and coordination

Use these as the source of truth:

1. Raspberry Pi CM5 datasheet **RP-008180-DS, release 3 / DS7**:
   pinout §4.2, mechanical §4.1.1, and power sequencing §3.
2. Official Raspberry Pi `rpi-cm5` STEP model from `CM5-step.zip`.
3. Manufacturer drawings for the Amphenol BergStak parts.
4. Official Seeed XIAO ESP32-S3 DIP footprint, symbol, and package drawing.
5. The final carrier netlist. Do not copy known mistakes from an earlier carrier
   PDF.

The carrier uses two Amphenol `10164227-1001A1RLF` receptacles for a 1.5 mm
stack. Board A uses two `10164228-1001A1RLF` **headers**. The same header mates
with either the 1.5 mm `...-1001...` or 4.0 mm `...-1004...` carrier
receptacle; stack height is selected by the **carrier receptacle**, not by
changing the dummy header.

Confirm header stock before layout. A Hirose
`DF40C-100DP-0.4V(51)` may be used for low-current fit/logic testing after
checking both connector drawings. Use the Amphenol header for higher-current
load testing. If Hirose is used, limit total dummy input current to 1.5 A and
limit `J_LOAD` to 1.0 A.

There is no official module-side KiCad header footprint in the supplied CM5IO
package. Create Board A from the official CM5 STEP plus the
`10164228-1001A1RLF` land-pattern drawing. Record both connector centres,
rotations, and pin-1 orientation in a footprint-verification note. Do not reuse
the carrier receptacle pad geometry. The official STEP places the two connector
centre-lines **34.00 mm apart**, near opposite long edges of the module.

---

## 2. Board A — CM5 dummy

### 2.1 Mechanical

| Item | Requirement |
|---|---|
| Outline | 55.00 × 40.00 mm, matching CM5 |
| PCB thickness | 1.2 mm (inside the CM5 1.24 mm ±10% specification) |
| Layers | 4 |
| Holes | Four M2.5 holes, centres 3.5 mm from the adjacent edges; copy the official STEP |
| Connectors | Two 100-pin module-side headers on the **bottom**, centre-lines 34.00 mm apart |
| Other components | **Top only**; a 1.5 mm stack has no usable clearance below |
| Finish | ENIG |
| Marking | Pin 1, connector numbers, orientation, and `NOT A CM5 — TEST DUMMY` on both sides |

Print the PCB 1:1 and overlay a real CM5 before fabrication.

### 2.2 Verified CM5 power and ground pins

**5 V inputs:** 77, 79, 81, 83, 85, 87.

**GND:** 1, 2, 7, 8, 13, 14, 22, 23, 32, 33, 42, 43, 52, 53,
59, 60, 65, 66, 71, 74, 98, 107, 108, 113, 114, 119, 120, 125,
126, 131, 132, 137, 138, 144, 150, 155, 156, 161, 162, 167, 168,
173, 174, 179, 180, 185, 186, 191, 192, 197, 198.

Connect every listed GND pin to a solid ground plane. All other unused pins
remain electrically NC; do not ground unused signal pins.

### 2.3 Safe connection of the six 5 V pins

Do not hard-short the six carrier contacts together before they have been
checked.

For each 5 V pin, provide:

`CM5 contact → labelled test point → 0.1 Ω, 1%, ≥0.25 W shunt → removable 2-pin link → 5V_DUMMY`

The six links ship **open**. On first power, measure all six contacts separately
and require 4.75–5.25 V on each. Power down, discharge, and only then fit all six
links. The shunts allow a voltage-drop/current-sharing check under load; a
missing contact should not be hidden by the other five.

`5V_DUMMY` feeds the indicators, 3.3 V emulator, and `J_LOAD`. Keep the
carrier-plus-dummy capacitance directly visible on raw Type-C VBUS within the
applicable USB-C limit (10 µF maximum for this passive-`Rd` implementation),
including tolerance and effective capacitance. Use 100 nF local decouplers and
no directly connected 100 µF capacitor. Any larger bulk must be DNP or behind
controlled inrush.

### 2.4 5 V indicators

Provide:

- yellow `5V PRESENT`;
- red `5V LOW`;
- red `5V HIGH`;
- `TP_5V` and `TP_GND` for a DMM/scope.

Use a dedicated precision window supervisor such as TPS3700, or an equivalent
fully calculated circuit. Do not use the earlier unqualified LM339/TL431
resistor values.

Requirements:

- worst-case low trip must lie from 4.75 to 4.90 V;
- worst-case high trip must lie from 5.10 to 5.25 V;
- use 0.1% divider resistors and a reference/supervisor specified over
  temperature;
- include hysteresis so LEDs do not chatter;
- submit a worst-case calculation including reference error, divider tolerance,
  comparator error, and temperature;
- every LED has its own series resistor, sized for approximately 1–2 mA.

`5V PRESENT` alone is not a pass. A DMM/scope measurement is the final authority.

### 2.5 3.3 V emulator and physical isolation

A real CM5 receives 5 V and emits 3.3 V on pins 84/86. Board A emulates only
that behavior:

`5V_DUMMY → efficient 3.3 V buck (≥1 A) → active 450–550 mA current limiter → SW1 physical disconnect → pins 84 and 86`

Requirements:

- use a buck or another thermally justified regulator; **do not use AP2112K**
  for this load;
- the current limiter must survive an output short indefinitely; a resettable
  PTC is not the sole current limiter;
- `SW1 3V3 EMU` is a two-pole mechanical ON/OFF switch rated ≥1 A that
  positively disconnects both outputs; use one pole for pin 84 and one for
  pin 86;
- with SW1 off, both connector pins are physically open from the regulator:
  no output-discharge resistor and no reverse-current path may clamp or hide
  carrier backfeed;
- SW1 ships and powers up **off**;
- connector-side voltage must be 3.135–3.465 V from no load through 400 mA;
- provide `TP_3V3_84` and `TP_3V3_86`.

Before switching SW1 on, both pins 84 and 86 must measure below 0.20 V. Any
pre-existing voltage is a fail.

Provide a green `3V3 OUT OK` window indicator. It may light only when the
connector-side rail is within 3.135–3.465 V; include its worst-case threshold
calculation with the 5 V supervisor calculation.

This dummy does not reproduce the CM5 1.8 V rail and does not reproduce
`PMIC_Enable` shutdown behavior. Those limitations must appear on the
silkscreen/README.

### 2.6 GPIO_VREF and 1.8 V checks

`GPIO_VREF` is CM5 pin 78. **Do not connect it directly to the dummy regulator.**
The intended test path is:

`dummy pins 84/86 → carrier CM5_3.3V copper → carrier GPIO_VREF tie → dummy pin 78`

Fit a 1 kΩ load from pin 78 to GND so leakage cannot create a false pass. Use a
precision window detector for the green `VREF OK` indicator; it may light only
when pin 78 is within 3.135–3.465 V. Provide `TP_VREF` and include the detector
in the worst-case calculations.

Pins 88 and 90 are CM5 1.8 V **outputs**. The dummy must not generate 1.8 V.
Sense the two pins independently and at high impedance; do not tie them
together. Provide `TP_1V8_88` and `TP_1V8_90`. A shared red `1V8 BACKFEED`
indicator is acceptable only if its input leakage is ≤1 µA and it is guaranteed
on when either pin is ≥0.20 V. A simple LED/BJT/MOSFET threshold is not
sufficient.

### 2.7 Control-pin emulation

| Pin | Official signal | Dummy requirement |
|---|---|---|
| 75 | `SD_PWR_ON` | Independent 3.3 V source driver through 1 kΩ |
| 76 | `VBAT` | Test point only |
| 92 | `PWR_Button` | 5 V-domain active-low input: weak 10 kΩ pull-up to 5 V; assert only by contact/open-drain to GND |
| 93 | `nRPIBOOT` | Weak 10 kΩ pull-up to emulated 3.3 V |
| 94 | `CC1` | Independent 5.1 kΩ `Rd` to GND, default-on removable link, test point |
| 96 | `CC2` | Separate 5.1 kΩ `Rd` to GND, default-on removable link, test point |
| 99 | `PMIC_Enable` | Weak 100 kΩ pull-up to 5 V and test point; informational only |
| 101 | `USB_OTG_ID` | Weak 10–100 kΩ pull-up to emulated 3.3 V and test point; informational only |
| 103 | USB2 D− | Test point only |
| 105 | USB2 D+ | Test point only |
| 111 | `VBUS_EN` | Independent 3.3 V source driver through 1 kΩ |

CC1 and CC2 must never be tied together. Passive `Rd` requests **5 V attach
only**; it does not negotiate or prove 5 A. Test both cable orientations. Use a
bench/current-limited source for load testing unless the Type-C source current
advertisement has been measured.

For button/jumper indication, each branch has a separate series resistor:

- 5 V → resistor → yellow `PWR BUTTON` LED → pin 92;
- emulated 3.3 V → resistor → yellow `RPIBOOT` LED → pin 93.

### 2.8 Carrier status-LED test

Verified pins:

| Pin | Signal | Test action |
|---|---|---|
| 15 | `Ethernet_nLED3` | Pull low; optional if the carrier fits this LED |
| 17 | `Ethernet_nLED2` | Pull low; optional if the carrier fits this LED |
| 21 | `LED_nACT` | Pull low |
| 95 | `LED_nPWR` | Pull low |
| 47 | GPIO23 / OS | Drive high |
| 45 | GPIO24 / LINK | Drive high |
| 41 | GPIO25 / ERR | Drive high |

Do not call pins 15/17 yellow or green in the dummy net names; those colors are
carrier-specific. Use the official `nLED3` and `nLED2` names.

Implementation requirements:

- pins 15, 17, 21, and 95 each get an **independent** N-MOS open-drain stage;
- GPIO23/24/25 each get an independent 3.3 V source/buffer stage;
- put 1 kΩ in each connector-pin drive path so an unexpected opposite-rail
  fault is current-limited;
- each channel has its own DIP switch;
- one momentary `LAMP TEST` signal is diode-ORed into the seven **driver
  inputs**;
- never connect or OR the seven CM5 connector pins together;
- all drivers are inactive while SW1 is off.

The common button should light PWR, ACT, OS, LINK, and ERR together, plus the
two Ethernet indicators if they are fitted on the carrier. Individual DIP
switches isolate a failed channel.

Use one default-off DIP-10: `VBUS_EN`, `SD_PWR_ON`, `nLED3`, `nLED2`, `nACT`,
`nPWR`, `OS`, `LINK`, `ERR`, and one NC spare. `LAMP TEST` affects only the
seven LED channels, not `VBUS_EN` or `SD_PWR_ON`.

This test verifies continuity and polarity only. It does not replace final
carrier review or validation with a real CM5.

### 2.9 UART buttons, probe header, and load header

- Two momentary, normally-open buttons powered from dummy 3.3 V after SW1:
  `TX` drives GPIO4 (pin 54) through 1 kΩ; `RX` drives GPIO5 (pin 34) through
  1 kΩ. Put a small LED next to each button so the local drive is visible.
- `J_U2`: GND, GPIO4 (CM5 uart2 TX, pin 54), GPIO5 (CM5 uart2 RX, pin 34).
  Probe header only. No VCC pin. No USB-serial converter on Board A.
- `J_CON`: GND, GPIO14 (console TX, pin 55), GPIO15 (console RX, pin 51).
  No VCC pin.
- Mark `3.3 V UART ONLY — NO RS-232 / NO 5 V TTL`.
- `J_LOAD`: 5V_DUMMY and GND for an external electronic load.

With an Amphenol header, total dummy input current may be tested to 2 A if
connector temperature and each 5 V branch current are monitored. With a Hirose
header, limit `J_LOAD` to 1 A and total dummy current to 1.5 A.

---

## 3. Board B — XIAO dummy

### 3.1 Mechanical and non-mirrored pin map

Use the official Seeed XIAO ESP32-S3 DIP footprint and a 21.00 × 17.80 mm
mating outline. Header pitch is 2.54 mm; row spacing is exactly 15.24 mm
(0.600 inch).

Top/component view, moving away from the USB end:

- one row, pins 1–7: D0, D1, D2, D3, D4, D5, D6/TX/GPIO43;
- opposite row, pins 14–8: 5V, GND, 3V3, D10, D9, D8, D7/RX/GPIO44.

Pins 1/14 are nearest the USB end of a real XIAO; pins 7/8 are farthest. Male
mating pins protrude from the bottom; all parts are on top. There is no USB-C
on this dummy. Mark `PIN 1`, `USB END`, `5V`, `D6/TX`, and `D7/RX` on both
silkscreens. No mirroring or 180° rotation. Print 1:1 and overlay a real XIAO
before fabrication.

### 3.2 Power checks

- XIAO 5V position: `TP_XIAO_5V` plus yellow LED, its own 2.2 kΩ resistor,
  and a removable `5V LED ENABLE` link so the indicator current can be excluded
  from reverse-leakage measurements.
- XIAO 3V3 position: high-impedance `TP_XIAO_3V3` plus red LED and its own
  2.2 kΩ resistor. The LED is supplemental; pass requires a DMM reading
  ≤0.20 V.
- Add `J_XLOAD` for a defined external load on the XIAO 5 V position.
- All remaining XIAO positions are NC except GND, TX, and RX.

Keep `XIAO_5V` and `XIAO_3V3` as separate nets. Do not add USB-C or a
USB-serial converter on Board B.

### 3.3 UART check LEDs

No laptop serial port. Continuity and swap are checked with Board A’s buttons
and two LEDs on Board B:

- green LED on XIAO D7/RX/GPIO44 through 1 kΩ to GND, labelled `FROM CM5 TX`;
- green LED on XIAO D6/TX/GPIO43 through 1 kΩ to GND, labelled `TO CM5 RX`.

These LEDs must not connect to the XIAO 3.3 V pin. With both dummies seated and
SW1 on:

- press Board A `TX`: only `FROM CM5 TX` lights;
- press Board A `RX`: only `TO CM5 RX` lights;
- the wrong LED lighting means the two UART wires are swapped;
- neither lighting means an open UART path.

### 3.4 Direct VBUS/backfeed test

Do not use laptop USB VBUS for the fault test, and do not put a Schottky diode
in the acceptance path.

Provide:

- `J_INJECT_5V` and GND for a current-limited bench source;
- default-off `INJECT 5V` switch;
- fuse protection plus an openable ammeter link or Kelvin shunt/test points
  that can resolve 0.1 mA with ±0.1 mA or better accuracy;
- direct switched connection to the XIAO 5 V position.

This reproduces the real XIAO’s raw USB-VBUS connection at its 5 V pin. An
optional Schottky-protected “safe” path may be added, but it is not the
acceptance test.

---

## 4. Acceptance procedure

### 4.1 Safety and first power

1. Insert/remove either dummy only with carrier power off, the carrier USB-C
   cable unplugged, all removable links open, SW1 off, INJECT off, and rails
   below 0.20 V.
2. Under those conditions, seat both dummies. Fit Board A’s independent CC
   `Rd` links; keep all six 5 V links open.
   Before applying power, use continuity/resistance checks to verify each
   exposed 5 V contact reaches the intended carrier 5 V net and is not shorted
   to GND, 3.3 V, or a signal. Also verify GPIO4→XIAO D7/RX and
   GPIO5←XIAO D6/TX through the documented series resistance, and verify
   GPIO14/GPIO15 are open to both XIAO UART contacts.
3. Apply current-limited 5.00 V to the carrier (50–100 mA limit). Measure every
   CM5 5 V contact independently; each must be 4.75–5.25 V and none may be
   shorted to GND, 3.3 V, or a signal.
4. Power off and discharge. Fit all six 5 V links.
5. Reapply 5.00 V at a 200 mA limit. `5V PRESENT` is on; LOW and HIGH are off.
   Pins 84/86, 88/90, pin 78, XIAO 5V, and XIAO 3V3 must all be ≤0.20 V.
6. Test both orientations of the carrier USB-C power cable. Passive `Rd` is
   only a 5 V attach test.

### 4.2 Load and 3.3 V emulation

7. Raise the bench limit appropriately and apply a 1 A load at `J_LOAD`.
   Require ≥4.75 V at `TP_5V`, no branch below 10% or above 25% of measured
   total input current, and no abnormal connector/shunt heating. A 2 A
   **total-current** test is Amphenol-only.
8. Remove `J_LOAD`; return to a conservative current limit.
9. Confirm pins 84 and 86 are each ≤0.20 V, then switch SW1 on.
10. Require 3.135–3.465 V on pins 84/86 and pin 78. `VREF OK` is on.
11. The XIAO 5 V position must rise; record its drop from carrier 5 V under a
    defined 100 mA load. XIAO 3V3 must remain ≤0.20 V. Disconnect `J_XLOAD`
    after recording the result.
12. Switch SW1 off. Pins 84/86, VREF, and XIAO 5 V must fall below 0.20 V.

### 4.3 LEDs and control pins

13. With SW1 on, hold `LAMP TEST`. Carrier PWR, ACT, OS, LINK, and ERR LEDs
    must light; optional Ethernet indicators light if fitted.
14. Use each DIP channel individually to identify wiring/polarity faults.
15. Verify `PWR_Button`: released 4.75–5.25 V, pressed <0.4 V, yellow LED on
    only while pressed.
16. Verify `nRPIBOOT`: open ≈3.3 V, strapped <0.4 V, yellow LED on while
    strapped.
17. Exercise `VBUS_EN` and `SD_PWR_ON` individually and measure their carrier
    outputs. `PMIC_Enable` is informational and not part of emulator pass/fail.

### 4.4 UART

18. With both dummies still seated, reapply carrier 5 V, switch SW1 on, and
    confirm VREF/XIAO power checks first.
19. Press Board A `TX`. Only Board B `FROM CM5 TX` lights. The local TX button
    LED on Board A also lights.
20. Press Board A `RX`. Only Board B `TO CM5 RX` lights. The local RX button
    LED on Board A also lights.
21. If the opposite Board B LED lights, the two UART wires are swapped. If
    neither lights, the path is open. The console header must stay dark /
    unconnected to the XIAO UART pins.
22. Release both buttons before switching SW1 off or removing carrier power.

### 4.5 XIAO VBUS reverse-isolation

25. Carrier off and discharged; `J_XLOAD` disconnected; Board B
    `5V LED ENABLE` open.
26. Apply 5.25 V at `J_INJECT_5V` with a 50–100 mA bench limit and switch
    INJECT on.
27. Confirm 5.20–5.25 V at `TP_XIAO_5V`; this prevents an open fuse/switch/link
    from creating a false pass. After five seconds, carrier main 5 V and XIAO
    3V3 must remain ≤0.20 V and injection current into the carrier must be
    <1 mA. Measure at the specified ammeter link/shunt; with the LED link open
    there is no indicator-current subtraction.
28. Repeat with carrier and injection both active, in both connection orders,
    while injection remains at 5.25 V. Require `TP_XIAO_5V` to remain
    5.20–5.25 V, carrier 5 V to remain 4.75–5.25 V, and injection current to
    remain <1 mA after five seconds.
29. Finish with INJECT off, injection removed, SW1 off, carrier off, and all
    rails discharged. Open the six 5 V links and both CC links; leave
    `5V LED ENABLE` open before removing either dummy.

---

## 5. Deliverables

1. KiCad preferred; Board A and Board B in one project as two PCBs.
2. Schematic sign-off milestone before layout.
3. All 200 CM5 connector pins represented so NC pads are not omitted.
4. Connector/header land patterns made from manufacturer drawings.
5. Footprint-verification report:
   connector centres, rotations, pin 1, STEP overlay, and XIAO 1:1 overlay.
6. Worst-case calculations for voltage supervisors, regulator thermal/current
   limit, LED currents, shunts, and raw VBUS capacitance.
7. ERC/DRC reports, native CAD, Gerbers/drills, BOM with manufacturer part
   numbers, pick-and-place, and 3D STEP.
8. Assembly notes listing every default-open/default-off link and switch.

Quote fixed-price with schematic and layout milestones separated. Flag any
requirement believed to be incorrect. Do not add an MCU/OLED, a USB-serial
converter, a 5 V UART, or a USB-C sink capable of requesting 9/12/15/20 V.
