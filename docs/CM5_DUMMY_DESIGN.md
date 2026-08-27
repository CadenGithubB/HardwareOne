# Design Brief: CM5 Dummy + XIAO Dummy (carrier bring-up, no sacrificial modules)

**Scope:** Two small PCBs that plug into the unfinished CM5 + XIAO carrier in place of the real modules. They report whether the carrier is presenting the right voltages and whether the coprocessor power path behaves. They do not run Linux or ESP-IDF.
**Quantity:** panel of 5+5, JLCPCB/PCBWay class, 4-layer dummy / 2-layer XIAO dummy.
**This brief is self-contained.** Pin numbers are from Raspberry Pi CM5 datasheet RP-008180-DS §4.2. Re-verify against that document, not third-party pinout sites.

The companion carrier brief is `docs/CM5_CARRIER_DESIGN_BRIEF.md`. This dummy is the first thing that should be inserted into that carrier.

---

## 1. What these boards are

### Board A — CM5 dummy

A 40 × 55 mm PCB that **is** a Compute Module 5 as far as the carrier’s connectors are concerned: same outline, same M2.5 holes, same two 100-pin **plugs** on the bottom. It is not a computer.

It does four jobs:

1. **Receive** every 5 V and GND pin the way a real CM5 would, so a skinny pour or a missing via shows up as sag or heat, not as a dead module.
2. **Tell you** whether 5 V is inside 4.75–5.25 V, and whether `GPIO_VREF` came back as 3.3 V (carrier tied it) or not (carrier left it floating or tied it to 1.8 V).
3. **Emulate** `CM5_3.3V` (pins 84/86) with a current-limited LDO, behind a switch that defaults **off**. That is what turns on the carrier’s XIAO load switch and Qwiic rail without a real CM5.
4. **Stimulate** the carrier nets a CM5 would drive (`VBUS_EN`, `LED_nPWR`, `LED_nACT`, `SD_PWR_ON`, GPIO23/24/25) so you can prove those circuits before Linux exists.

### Board B — XIAO dummy

A 14-pin male-header board in the Seeed XIAO / QT Py footprint. LED on 5 V, LED on 3V3 (must stay off), USB-serial on TX/RX, and a switch that injects USB VBUS onto the 5 V pin to prove the carrier’s ORing does not back-feed.

### What they are not

- Not an rpiboot device, not a fan controller, not a CM5 interposer (no real module stacks on top).
- Not a USB gadget that enumerates as a Compute Module. The dummy’s own USB-C (v1.1) is a **measurement console only**, and its VBUS must never join the carrier 5 V plane.
- They cannot test HDMI, PCIe, CSI/DSI, Ethernet, Wi-Fi, or eMMC.

v1.1 **does** present USB-C `Rd` (5.1 kΩ on CC1/CC2) so a PD PSU will source 5 V without a real CM5. It does **not** request 9/12/15/20 V. See §14.

---

## 2. References

1. CM5 datasheet RP-008180-DS — pinout §4.2, power §3, mechanical §4.1.1, GPIO_VREF rules.
2. CM5IO datasheet + KiCad — connector XY on the carrier; dummy plugs are the mating parts at the **module** coordinates, not the receptacle footprint.
3. Amphenol BergStak 0.40 mm: carrier receptacle `10164227-1001A1RLF` (1.5 mm stack) or `10164227-1004A1RLF` (4.0 mm). Dummy plug `10164228-1001A1RLF`. Raspberry Pi has used Hirose DF40 interchangeably in development; `DF40C-100DP-0.4V(51)` is an acceptable dummy-side substitute if the Amphenol header is unobtainable. Do not mix stack heights: 1.5 mm plugs only mate 1.5 mm receptacles.
4. Seeed XIAO ESP32-S3 package drawing — two 1×7 rows, 2.54 mm pitch, **15.24 mm** row spacing (0.600 in). The carrier brief says 15.25 mm; use 15.24 mm and verify both boards against the same drawing.
5. Carrier brief `docs/CM5_CARRIER_DESIGN_BRIEF.md` — XIAO 5 V is load-switch + ORing, enable = `CM5_3.3V`, UART-only isolation, `PWR_Button` is a 5 V pin.

---

## 3. Mechanical (Board A)

| Item | Value |
|---|---|
| Outline | 40.00 × 55.00 mm, identical to CM5 |
| PCB thickness | 1.2 mm (CM5 is 1.24 mm ±10%) |
| Layers | 4 (signal / GND / 5V pour / signal) |
| Mounting | Four M2.5 holes, inset 3.5 mm from each edge (3.5, 3.5), (51.5, 3.5), (3.5, 36.5), (51.5, 36.5) with origin at the 55 mm × 40 mm corner matching the official CM5 drawing |
| Connectors | Bottom side only. Two 100-pin plugs. Pins 1–100 and 101–200 per datasheet §4.2. **Copy XY from the official CM5 STEP / KiCad module, not from the CM5IO receptacle footprint.** |
| Component side | **Top only.** 1.5 mm stack has zero clearance under the board. |
| Max component height | 8 mm on top is fine for a dummy; keep the antenna-corner of a real CM5 free of anything taller than 2 mm so the dummy still fits a carrier that followed §4.1.2 keep-out. |
| Silkscreen | Bottom: `NOT A CM5 — TEST DUMMY`. Top: every LED and switch labelled as in §6. |
| Finish | ENIG preferred (0.4 mm pitch). |

**Strong recommendation to the carrier:** use the **4.0 mm** receptacles (`10164227-1004A1RLF`) on the carrier. Same footprint, 2.5 mm of probe space under the module. If the carrier stays on 1.5 mm, this dummy still works; probing the carrier under the dummy does not.

---

## 4. Connector policy (Board A)

The dummy seats all 200 pins. Most are no-connect on the dummy (pad exists, no trace). That is deliberate: an NC pin cannot fight a carrier net and cannot back-feed a high-speed pair.

### 4.1 Must-connect — power and ground

Tie **every** 5 V pin together on a pour, and **every** GND pin together on a pour. Stitch with vias. This is the whole point of the dummy as a current-sharing test.

**5 V (input, 4.75–5.25 V):** pins 77, 79, 81, 83, 85, 87.

**GND:** pins 1, 2, 7, 8, 13, 14, 22, 23, 32, 33, 42, 43, 52, 53, 59, 60, 65, 66, 71, 74, 98, 107, 108, 113, 114, 119, 120, 125, 126, 131, 132, 137, 138, 144, 150, 155, 156, 161, 162, 167, 168, 173, 174, 179, 180, 185, 186, 191, 192, 197, 198.

### 4.2 Driven by the dummy (behind switches, after 3V3-EMU is on)

| Pin | Net | Dummy action |
|---|---|---|
| 84, 86 | `CM5_3.3V` | LDO output, 3.3 V ±2%, current-limited ≈500 mA, **SW1 off by default**. This rail is also the anode supply for every carrier status LED. |
| 111 | `VBUS_EN` | DIP: 3.3 V through 100 Ω (fan 5 V switch — not an LED) |
| 95 | `LED_nPWR` | Open-drain to GND through 47 Ω. Lights carrier D2 (red PWR). Active **low**. |
| 21 | `LED_nACT` | Open-drain to GND through 47 Ω. Lights carrier D1 (green ACT). Active **low**. |
| 15 | `Ethernet_nLED3` / ETH_LEDY | Open-drain to GND through 47 Ω. Lights carrier yellow Ethernet LED if fitted. Active **low**. |
| 17 | `Ethernet_nLED2` / ETH_LEDG | Open-drain to GND through 47 Ω. Lights carrier green Ethernet LED if fitted. Active **low**. |
| 75 | `SD_PWR_ON` | DIP: 3.3 V through 100 Ω |
| 47 | GPIO23 | 3.3 V through 100 Ω. Carrier “OS” LED. Active **high**. |
| 45 | GPIO24 | 3.3 V through 100 Ω. Carrier “LINK” LED. Active **high**. |
| 41 | GPIO25 | 3.3 V through 100 Ω. Carrier “ERR” LED. Active **high**. |

**Polarity is not uniform.** PWR / ACT / Ethernet LEDs on the carrier (your schematic: +3.3 V → 1 kΩ → LED → module pin) turn on when the dummy **sinks** the pin to GND. OS / LINK / ERR turn on when the dummy **sources** 3.3 V into the GPIO. Never drive `LED_nPWR` / `LED_nACT` / Ethernet LED pins high. `PWR_Button` (92) is pulled to **5 V** — never a 3.3 V push-pull.

Carrier LEDs will not light with SW1 off: their anodes sit on +3.3 V, and that 3.3 V is the dummy LDO. Sequence: 5 V window clean → SW1 on → then lamp-test.

**SW3 `LAMP TEST`** (momentary, edge of board): while held, asserts every status LED drive at once (pins 15, 17, 21, 95 low; GPIO23/24/25 high). One press should light PWR, ACT, OS, LINK, ERR, and Ethernet Y/G if those LEDs exist on the carrier. DIP bits still exist so you can isolate a dead LED. SW3 does nothing useful until SW1 is on.

OS / LINK / ERR only exist if those three LED circuits are on the carrier (brief §3.2). The connector page you have already has D1/D2; GPIO23/24/25 LEDs live on a different sheet — they must actually be drawn and stuffed or the dummy has nothing to light.

### 4.3 Sensed, not driven

| Pin | Net | Dummy action |
|---|---|---|
| 78 | `GPIO_VREF` | Window comparator + LED. Must **not** be tied to the dummy LDO on the dummy itself. If the carrier tied VREF to `CM5_3.3V`, the LED lights after SW1 is on. If the carrier left it floating or tied it to 1.8 V, the LED stays off. That is the test. |
| 88, 90 | `CM5_1.8V` | Sense only. Dummy does **not** generate 1.8 V. A red LED here is a carrier fault (back-feed or short). |
| 92 | `PWR_Button` | 10 kΩ pull-up to 5 V (mimics CM5). Yellow LED from 5 V to this pin, 1.5 kΩ — lights when the carrier button shorts the pin to GND. |
| 93 | `nRPIBOOT` | 10 kΩ pull-up to dummy 3.3 V. Yellow LED from 3.3 V to this pin — lights when the carrier jumper is fitted. Only valid after SW1. |
| 76 | `VBAT` | Test point only. Do not put a standing LED on a CR2032. |
| 94, 96 | CC1, CC2 | Test points only. Dummy is not a PD controller. |
| 103, 105 | USB2 D− / D+ | Test points only. Dummy cannot enumerate. |
| 101 | `USB_OTG_ID` | Test point. 10 kΩ pull-up to 3.3 V (CM5 has an internal pull-up). |
| 99 | `PMIC_Enable` | 100 kΩ pull-up to 5 V (mimics CM5). Test point. Dummy has no PMIC; pulling this low will **not** drop dummy 3.3 V. Documented limitation. |
| 16, 19 | Fan tacho / PWM | Test points. |
| 20 | `EEPROM_nWP` | Test point. |
| 54, 34 | GPIO4, GPIO5 (uart2) | 3-pin header with GND. |
| 55, 51 | GPIO14, GPIO15 (console) | 3-pin header with GND. |
| 58, 56 | GPIO2, GPIO3 (Qwiic) | Test points. Do not add extra pull-ups; a real CM5 already has 1.8 kΩ to VREF. |

### 4.4 Hard no-connect

Ethernet pairs, PCIe, MIPI, HDMI, USB3, SD data/clk/cmd, `SD_VDD_OVERRIDE`, `WL_nDisable`, `BT_nDisable`, `CAM_GPIO0/1`, `SCL0`/`SDA0`, `ID_SC`/`ID_SD`, and every GPIO not listed in §4.2. Pads exist. No traces. Do not ground them. Pins 15 and 17 are **not** NC — they are LED sinks.

---

## 5. Power architecture (Board A)

```
Carrier 5V ── pins 77/79/81/83/85/87
                │
                ├─► 5V pour, 100 µF + 10 µF + 100 nF
                ├─► LED "5V" (yellow, always-on if rail is up)
                ├─► LM339 window vs TL431 2.50 V
                │     LOW  (red)  if V < 4.75 V
                │     HIGH (red)  if V > 5.25 V
                │     OK = yellow on, both reds off
                ├─► J_LOAD  2-pin 5.08 mm  (external 1–2 A dummy load)
                ├─► 10 kΩ to PWR_Button (pin 92)
                └─► AP2112K-3.3  VIN
                       EN ◄── SW1 "3V3 EMU" (default off, 100 kΩ pulldown on EN)
                       VOUT ── PTC 500 mA ── pins 84 + 86
                                  │
                                  ├─► LED "3V3" (green) via comparator, trip 3.13 V
                                  ├─► 10 kΩ pull-up for nRPIBOOT
                                  ├─► SW3 LAMP TEST + DIP: sink PWR/ACT/ETH LEDs, source OS/LINK/ERR
                                  └─► DIP high-side 100 Ω: VBUS_EN, SD_PWR_ON
```

**Sequence is a procedure, not a circuit:**

1. SW1 off. No 3.3 V may appear on pins 84/86. If it does, the carrier is back-feeding. Stop. Do not flip SW1.
2. 5 V window must be green-path (yellow on, reds off) before SW1.
3. Then SW1 on. Dummy 3.3 V rises. Carrier XIAO load switch should enable. `GPIO_VREF` LED should light.

This matches the CM5 datasheet rule that no module pin is driven before 5 V is up: the dummy cannot emit 3.3 V until 5 V exists (LDO VIN = 5 V) and the operator has confirmed the 3.3 V pins were 0 V.

LDO dissipation: (5.1 − 3.3) × 0.3 A ≈ 0.54 W if Qwiic is loaded. SOT-223 or SOT-89 with copper. The dummy is not required to source the full 600 mA CM5 budget for hours; 300 mA continuous is enough to prove the carrier.

---

## 6. Top-side UI (Board A)

Place along the 55 mm edge opposite the antenna keep-out, silkscreen exactly as labelled.

### LEDs (0603, ~2 mA)

| Ref | Colour | Legend | On means |
|---|---|---|---|
| D1 | yellow | `5V` | 5 V rail is present (even if out of window) |
| D2 | red | `5V LO` | 5 V < 4.75 V |
| D3 | red | `5V HI` | 5 V > 5.25 V |
| D4 | green | `3V3` | dummy (or back-fed) 3.3 V ≥ 3.13 V |
| D5 | green | `VREF` | pin 78 ≥ 3.00 V — carrier tied GPIO_VREF to 3.3 V |
| D6 | red | `1V8!` | pin 88/90 ≳ 1 V — **fault**, dummy does not generate this |
| D7 | yellow | `BTN` | carrier power button is holding pin 92 low |
| D8 | yellow | `BOOT` | carrier nRPIBOOT jumper is holding pin 93 low |

D1–D3 are powered from 5 V. D4–D8 are powered from dummy 3.3 V except D7 (from 5 V) and D6 (transistor on 5 V, gate/base from 1.8 V pin). D8 is meaningless until SW1 is on.

### Switches

| Ref | Type | Legend | Default |
|---|---|---|---|
| SW1 | SPDT slide | `3V3 EMU` | **OFF** |
| SW2 | 10-pos DIP | see below | all OFF |
| SW3 | momentary | `LAMP TEST` | open |

DIP bits, all off = passive dummy. SW3 ORs the LED bits so you can light everything without counting switches.

| Bit | Legend | Action | Carrier LED |
|---|---|---|---|
| 1 | `VBUS` | pin 111 ← 3.3 V via 100 Ω | none (fan rail) |
| 2 | `nPWR` | pin 95 ← GND via 47 Ω | D2 red PWR (active low) |
| 3 | `nACT` | pin 21 ← GND via 47 Ω | D1 green ACT (active low) |
| 4 | `ETHY` | pin 15 ← GND via 47 Ω | Ethernet yellow (active low) |
| 5 | `ETHG` | pin 17 ← GND via 47 Ω | Ethernet green (active low) |
| 6 | `OS` | GPIO23 ← 3.3 V via 100 Ω | green OS (active high) |
| 7 | `LINK` | GPIO24 ← 3.3 V via 100 Ω | blue LINK (active high) |
| 8 | `ERR` | GPIO25 ← 3.3 V via 100 Ω | red ERR (active high) |
| 9 | `SDPWR` | pin 75 ← 3.3 V via 100 Ω | none |
| 10 | spare | NC | — |

### Headers / test points

| Ref | Pins | Net |
|---|---|---|
| J_CON | 3 | GND, GPIO14 (console TX on a real CM5), GPIO15 (console RX) |
| J_U2 | 3 | GND, GPIO4 (uart2 TX), GPIO5 (uart2 RX) |
| J_LOAD | 2 | 5 V, GND — for a bench electronic load, 1–2 A |
| TP_5V, TP_3V3, TP_1V8, TP_VREF, TP_GND | 1.0 mm | Kelvin-ish, next to the connector they belong to |
| TP_CC1, TP_CC2, TP_DP, TP_DN, TP_OTG, TP_VBAT, TP_PMIC | 1.0 mm | |

Loopback for UART proving: a 2.54 mm jumper cap that shorts GPIO4 to GPIO5 on J_U2, and GPIO14 to GPIO15 on J_CON. With a USB-UART on the *carrier* console header you would not use the dummy header; these are for probing the connector itself.

---

## 7. Supervisor math (Board A)

One **LM339DR** (quad comparator, VCC = 5 V) and one **TL431** shunt set to 2.495 V (typical) with the datasheet 2 × 10 kΩ program if using the adjustable connection, or a 2.5 V fixed TL431A from cathode/ref tied.

Comparators are open-collector. LED + 1.5 kΩ from the local rail to the output (LED on = output low).

| Channel | + input | − input | LED on when |
|---|---|---|---|
| 5V LO | 5 V × 10.5k / (9.53k+10.5k) | 2.495 V | V5 < 4.76 V |
| 5V HI | 2.495 V | 5 V × 10.0k / (11.0k+10.0k) | V5 > 5.24 V |
| 3V3 | 3.3 V × 20.0k / (5.11k+20.0k) | 2.495 V | V3 ≥ 3.13 V  (output high when OK — invert with the remaining gate, or wire this channel so the green LED is driven from a PNP / 2N7002 inverter. Simplest: swap inputs so output goes low when V3 is good, green LED on.) |
| VREF | pin 78 × 24.9k / (5.11k+24.9k) | 2.495 V | VREF ≥ 3.01 V |

All dividers 1% 0603. Put 100 pF from each tap to GND for noise.

**1V8!:** no comparator. Pin 88 to 4.7 kΩ to the base of a 2N3904 (or 2N7002 if Vth is characterised). Collector LED to 5 V. Off at 0 V, on at 1.8 V.

Hysteresis is optional. This is a bring-up tool, not a production tester.

---

## 8. Board B — XIAO dummy

### Mechanical

Two 1×7 2.54 mm **male** pin headers, rows 15.24 mm apart, matching Seeed’s drawing. USB-C end of a real XIAO must remain a clear corridor on the carrier; keep this dummy’s USB-C on that same end.

Outline can be 21 × 17.8 mm (XIAO) or a few millimetres larger. 2-layer, 1.6 mm. All parts on the USB-C face (top). Pins solder on the bottom.

### Electrical (header position, not ESP32 GPIO numbers)

Seeed XIAO ESP32-S3 / QT Py positions:

| Position | Module name | Dummy |
|---|---|---|
| 5V | 5 V / VBUS | LED D9 yellow + 1.5 kΩ to GND. Also the ORing injection point. |
| GND | GND | pour |
| 3V3 | 3.3 V | LED D10 red + 1.5 kΩ to GND. **Must stay off** — dummy has no LDO and the carrier must not drive this pin. If it lights, the carrier is back-feeding 3V3 into the module socket. |
| RX | UART RX (XIAO GPIO44) | CH340E / CP2102 RX (through 33 Ω if not already on the carrier) |
| TX | UART TX (XIAO GPIO43) | USB-serial TX |
| D4–D10, A1–A3, 3V3, etc. | remaining | NC or test points. Do not connect I2C/SPI. |

USB-C on the dummy (or a 3-pin 2.54 mm “to USB-UART dongle” header if you want to skip the USB-C footprint): VBUS, D−, D+, GND. VBUS does **not** automatically join the 5 V pin.

**SW3 `INJECT VBUS`:** when on, dummy USB VBUS connects to the 5 V pin through a Schottky (or the same ideal-diode part family as the carrier). This simulates “operator plugged a cable into the socketed module.”

ORing test, carrier unpowered: SW3 on, dummy USB plugged into a laptop. Carrier 5 V plane must stay 0 V. If the plane rises, the carrier ORing is missing or reversed.

ORing test, carrier powered, SW1 on dummy A on: SW3 on. Carrier 5 V must not jump, dummy 5 V LED stays on, and a current meter in the dummy USB should show only the dummy’s own draw, not the CM5 rail.

UART test: USB-serial on Board B, USB-serial on carrier console (GPIO14/15) is a different UART. To test the **XIAO link**, attach a USB-serial to Board B and another to dummy A’s J_U2 (GPIO4/5), then type. Crossing is: carrier routes CM5 GPIO4 (TX) to module RX. So dummy A GPIO4 should appear on Board B RX.

---

## 9. BOM (prototype, LCSC-friendly)

Manufacturer part numbers below are the design intent. Substitutions are fine if the function is identical; do not substitute the connectors without checking stack height.

### Board A

| Qty | Ref | MPN / value | Notes |
|---|---|---|---|
| 2 | J1, J2 | Amphenol `10164228-1001A1RLF` or Hirose `DF40C-100DP-0.4V(51)` | Bottom, 1.5 mm stack. 4.0 mm dummy plugs only if the carrier uses 4.0 mm receptacles. |
| 1 | U1 | AP2112K-3.3TRG1 (600 mA) or AMS1117-3.3 SOT-223 | AMS1117 dropout is tight at 4.75 V; prefer AP2112. |
| 1 | U2 | LM339DR | Quad comparator |
| 1 | U3 | TL431A SOT-23 | 2.5 V ref |
| 1 | F1 | 500 mA 0805 PTC | LDO output |
| 1 | SW1 | SPDT slide, 2.54 mm | 3V3 EMU |
| 1 | SW2 | DIP-8 SPST | stimulus |
| 1 | Q1 | 2N3904 SOT-23 | 1V8 fault LED |
| 8 | D1–D8 | 0603 LED | colours in §6 |
| 1 | C_bulk | 100 µF 16 V 1206 | 5 V, near pins 77–87 |
| 4 | C | 10 µF 16 V 0805 | 5 V, 3.3 V, TL431, LDO |
| 6 | C | 100 nF 0402 | IC decouple |
| 4 | C | 100 pF 0402 | divider taps |
| — | R | 1% 0603 | 9.53k, 10.5k, 11.0k, 10.0k, 5.11k, 20.0k, 24.9k, 10k, 1.5k, 1k, 100, 4.7k, 100k |
| 1 | J_LOAD | 5.08 mm 2-pin | 5 V load |
| 2 | J_CON, J_U2 | 2.54 mm 1×3 | UART |
| 11 | TP | Ø1.0 mm | as §6 |
| 4 | H1–H4 | M2.5 | holes only; standoffs live on the carrier |

### Board B

| Qty | Ref | MPN / value | Notes |
|---|---|---|---|
| 2 | P1, P2 | 1×7 2.54 mm male, 15.24 mm row spacing | |
| 1 | U4 | CH340E or CP2102N | USB-serial |
| 1 | J_USB | USB-C receptacle, CC 5.1 kΩ to GND | dummy’s own cable |
| 1 | SW3 | SPDT | INJECT VBUS |
| 1 | D_or | BAT54 / SS14 | injection diode |
| 2 | D9, D10 | 0603 LED | 5 V yellow, 3V3 red |
| 2 | R | 1.5 kΩ | LED |
| 2 | R | 33 Ω | TX/RX if carrier series resistors are not yet fitted |
| 1 | ESD | USBLC6-2SC6 | USB D+/D−, same family as the carrier |

A filled CSV lives at `hardware/cm5-dummy/bom.csv`.

---

## 10. Layout rules (Board A)

1. Place J1/J2 from the official CM5 module CAD. Print 1:1 and overlay a real CM5 before ordering.
2. Bottom: connectors and silkscreen only. No copper pours under the plugs except the pads.
3. Top: 5 V pour under the LED/LDO area, via-stitched to the six 5 V pads. GND inner layer solid.
4. Keep analog (TL431, dividers, LM339) away from J_LOAD. Kelvin the 5 V window divider off a via next to pins 77/79, not off a long 5 V spur to the LEDs.
5. LDO thermal pad / tab to a copper pour with vias to inner GND.
6. DIP and SW1 along the board edge so they are reachable while the dummy is socketed next to a XIAO.
7. Do not put metal in the CM5 antenna keep-out if you want the dummy to fit a carrier that has a keep-out cutout / fence there. Electrically the dummy does not care.

Board B: treat USB D+/D− as a 90 Ω pair over GND. 33 Ω series near the header pins.

---

## 11. Bring-up procedure

Power source for the **carrier** on first use: bench 5.1 V, current limit **200 mA**, not a 5 A USB-C brick. Raise the limit only after the 5 V window is clean.

### A. Carrier empty (no dummy, no XIAO)

1. Resistance 5 V–GND on the carrier: high (not ohms). If it is a short, stop.
2. Apply 5.1 V @ 200 mA.
3. Only the 5 V plane is allowed to be alive. `CM5_3.3V`, Qwiic 3.3 V, XIAO 5 V must be 0 V. If any of those is up, there is a sneak path. Stop.

### B. Dummy A, SW1 off, all DIP off, no XIAO

4. Insert dummy with carrier **unpowered**. Never hot-plug.
5. Apply 5.1 V @ 200 mA.
6. D1 (`5V`) on. D2/D3 off. D4/D5/D6 off. If D6 (`1V8!`) is on, stop.
7. Measure TP_3V3: must be ≈0 V. If it is 5 V, the carrier shorted 5 V onto 3.3 V. Stop. Dummy LDO is still off; the dummy survived. A real CM5 would not.
8. Press the carrier power button: D7 (`BTN`) on. Fit nRPIBOOT jumper: D8 still off (no 3.3 V yet).
9. Optional: J_LOAD 1 A. D2 must stay off (still ≥4.75 V at the dummy). If D2 comes on, the carrier pour/vias cannot carry current. Fix the carrier, do not insert a CM5.

### C. Dummy A, SW1 on

10. Flip SW1. Current limit can go to 500 mA.
11. D4 (`3V3`) on. D5 (`VREF`) on. If D4 on and D5 off, the carrier did not tie `GPIO_VREF` to `CM5_3.3V`.
12. XIAO socket 5 V (empty socket) should now be ~5 V minus the ORing drop. Qwiic 3.3 V should be 3.3 V.
13. Flip SW1 off: XIAO 5 V and Qwiic must fall. That is the isolation property in the carrier brief.
14. SW3 `LAMP TEST` (SW1 still on): carrier PWR, ACT, OS, LINK, ERR, and Ethernet Y/G (if stuffed) must all light. Then DIP one bit at a time to isolate a dark LED.

### D. Dummy B in the XIAO socket

15. D9 (5 V) on after step 12. D10 (3V3) **off**. If D10 is on, stop — carrier is driving module 3V3.
16. USB-serial on dummy B ↔ USB-serial on dummy A J_U2: confirm GPIO4→module RX and GPIO5←module TX. The 33 Ω resistors are on the carrier.
17. Carrier unpowered, SW3 on, dummy B USB to a laptop: carrier 5 V plane stays 0 V.

### E. Real modules, last

18. A CM5 that already booted on an official CM5IO. Power off, swap dummy A for the CM5, current limit ~1 A, console on GPIO14/15 first.
19. Real XIAO only after dummy B passed D9/D10 and the ORing test.

---

## 12. What this dummy cannot catch

| Gap | Why | What to use instead |
|---|---|---|
| USB-C PD 5 A / PPS | Dummy only presents `Rd` (5 V class). It will not negotiate 5 A | Real CM5; a USB-C power meter on the PSU cable |
| rpiboot / eMMC | Dummy cannot enumerate as BCM USB | Real CM5 + carrier nRPIBOOT jumper, already proven on CM5IO |
| `PMIC_Enable` actually shutting down 3.3 V | Dummy LDO ignores pin 99 | Real CM5 |
| Antenna / RF | No radio | Real modules, after power is trusted |
| UART at boot (ESP32 ROM log, Linux console) | Dummy has no SoC | Real modules |
| 5 A copper at thermal steady state | Dummy load at 1–2 A is the prototype budget | Electronic load longer soak, then real CM5 idle (~400 mA) / run (~900 mA) |

---

## 13. Deliverables for layout

Same cadence as the carrier: schematic for sign-off, then layout.

1. KiCad (preferred) schematic with the CM5 dummy symbol split by function (power, GPIO, USB, NC), not 200 pins on one box if that is unreadable — but every pin must still exist on the two connector symbols so NC pads generate.
2. Footprint verification note: dummy plug XY vs official CM5 STEP, XIAO 15.24 mm row spacing vs Seeed drawing.
3. Gerbers, drill, BOM with MPNs, pick-and-place, 3D STEP, DRC/ERC.
4. Panel: Board A + Board B together is fine; they do not share nets.

Quote Board A and Board B as two schematic pages in one project. v1.1 (§14) is a third page on Board A that may be left DNP; analog LEDs and SW1 must still work with that page empty.

---

## 14. v1.1 — numbers, current, CC, and a display

The v0 board answers “is this rail roughly right?” with lamps. That is the fail-safe and it stays. What it cannot answer:

- 5.02 V vs 4.78 V (both light the same yellow LED)
- 80 mA vs 800 mA (a short that is not quite a dead short)
- Current **into** pins 84/86 with SW1 off (back-feed)
- Whether one of the six 5 V pins is unsoldered
- Whether the carrier actually wired CC1/CC2 (a PD PSU will refuse to turn on)
- What XIAO 5 V, Qwiic, and fan 5 V are doing (those nets are not on the CM5 connector)

Do **not** replace the analog LEDs with firmware. Blank-flash, a crashed RP2040, or a dead OLED must still show 5 V window and 3V3-EMU-off.

### 14.1 How to show the data (pick one primary)

| Display | What you get | Cost on the dummy | Use when |
|---|---|---|---|
| **USB CDC serial** on the dummy | Full dashboard, CSV, min/max, pass/fail on a laptop | USB-C (data only) + MCU | Always. This is the primary display. |
| **0.91" I2C OLED** (128×32) | Same numbers with no laptop | ~12 × 30 mm, DNP-able | Bench without a computer |
| **USB-C PD inline meter** on the PSU cable | Carrier **total** V and I, CC state | $0 on the dummy | Buy one regardless. Dummy INA cannot see fan/always-on 5 V loads. |
| Cheap 3-digit panel voltmeter | One rail, no current, noisy ground | Does not fit six rails | Skip. MCU is smaller and better. |
| Ribbon “faceplate” | OLED + buttons off the 40×55 outline | 10-pin JST | Only if the module top is too tight |

Primary: MCU + USB serial. OLED is stuffed if it fits, unpopulated if it does not. A $15 PD meter on the PSU cable is the display for **total** carrier current; do not try to duplicate that on the dummy.

Dummy USB-C: D+/D− to the MCU, **VBUS unconnected** (same rule as the carrier rpiboot port). Power the MCU from carrier 5 V through its own LDO (`MCU_3V3`), not from the CM5-emu LDO, and not from dummy USB VBUS. Otherwise the laptop back-feeds the carrier, or the dashboard dies while SW1 is off — which is exactly when you need it.

### 14.2 Always-on brain

- **MCU:** RP2040 (native USB) or STM32G031. Own 3.3 V LDO from the 5 V pour, always on when carrier 5 V exists. Independent of SW1.
- **INA226** #1 on the dummy’s 5 V feed (shunt after the six pins join the pour, before dummy loads). Bidirectional. Reports dummy draw. With SW1 off and DIP off this should be tens of mA (MCU + LEDs). Hundreds of mA means a sneak path **through the dummy**; a carrier 5 V–GND short still shows up as sag + PSU current, not here.
- **INA226** #2 on the CM5-emu LDO output, **bidirectional**. SW1 off: any current into the dummy is back-feed. SW1 on: Qwiic + VREF + XIAO-enable load.
- **ADS1115** (or MCU ADC + 74HC4051) for slow rails: `GPIO_VREF`, `CM5_1.8V`, `PWR_Button`, `nRPIBOOT`, CC1, CC2, and the sense header.
- **10 mΩ** between **each** 5 V pin and the pour, plus a mux to the ADC. Under J_LOAD, an open pin reads ~0 V on its kelvin tap; a good pin reads pour minus I×10 mΩ. Populate 0 Ω first if you want v0 behaviour, swap to 10 mΩ when you want the imbalance test. 10 mΩ × 5 A × 6 pins in parallel is 8 mV — inside the 4.75–5.25 V window, but do not leave 10 mΩ in if you later use this dummy as a current-sharing stand-in at 5 A for hours.
- **GND lift:** ADC between pin 1 and pin 198 under load. More than a few millivolts means a missing GND pin or a split pour.
- **CC `Rd`:** 5.1 kΩ from CC1 to GND and CC2 to GND, each behind a default-ON jumper. A PD PSU should then output 5 V. Measure CC with the ADC: ~0.4 V class is a source with `Rp` seeing `Rd`; ~0 V is open; ~5 V is a broken/miswired CC. **Do not fit a CH224K / “PD trigger”** unless its voltage-select pins are hard-tied to 5 V with no jumper that can request 9–20 V. A 20 V request will destroy the carrier. `Rd` alone is the safe test.
- **J_SENSE** 8-pin 1.27 mm, flying leads to carrier test points the dummy cannot see:

  | Pin | Net |
  |---|---|
  | 1 | GND |
  | 2 | XIAO 5 V (after ORing) |
  | 3 | XIAO 3V3 (must stay 0) |
  | 4 | Qwiic 3.3 V |
  | 5 | USB-C PSU VBUS at the receptacle |
  | 6 | Fan 5 V (after `VBUS_EN` switch) |
  | 7 | rpiboot D+ |
  | 8 | rpiboot D− |

  5 vs 2 is copper drop across the carrier. 6 only rises when DIP `VBUS` is on. 3 must stay 0. 7/8: MCU weak-pulls and watches for a host attach (rpiboot wiring) without enumerating as a CM5.

- **UART sniff:** MCU GPIO on GPIO4/5/14/15 through 100 kΩ. Idle should be 0 or floating with SW1 off; after SW1, not 5 V. Activity counters prove the 33 Ω pairs are not open. Optional TX from MCU into GPIO15 / GPIO5 to light dummy B’s USB-serial, as a directed loopback.

### 14.3 What the screen / serial should print

Refresh ~4 Hz. Latch min/max since last reset. One `PASS`/`FAIL` line.

```
5V    5.08 V   0.14 A   OK     min 5.04  max 5.11
3V3   0.00 V   0.00 A   EMU OFF
VREF  0.00 V   Δ —      —
1V8   0.00 V            OK
CC1   0.41 V   CC2 0.40 V   Rd
5Vpins  2 3 2 3 2 2 mV   OK
GND lift 1 mV           OK
XIAO  0.00 V   QWIIC 0.00 V   FAN 0.00 V
BTN 5.02  BOOT 0.00
PASS
```

After SW1: `3V3` ≈ 3.30 V, `VREF` within ~20 mV of `3V3`, `XIAO` ≈ 5 V minus ORing, `QWIIC` ≈ 3.3 V. `1V8` must stay 0.00. `EMU OFF` then those three fall.

OLED pages, button to cycle: RAILS / CURRENT / PINS / SENSE.

CSV on USB at 10 Hz for a serial plotter if a rail is hunting.

### 14.4 What else to look for (this carrier, not a generic CM)

These are the failure modes the lamps do not name.

| Hunt | Pass | Fail looks like |
|---|---|---|
| 5 V window under 1–2 A | ≥ 4.75 V at dummy kelvin | `5V LO` / serial 4.6 V — pour or connector |
| One 5 V pin open | All six kelvin taps similar under load | One tap ~0 V |
| GND open on one connector | GND lift < ~5 mV at 1 A | Tens of mV, flaky UART later |
| 5 V shorted to 3.3 V | TP_3V3 = 0 with SW1 off | 5 V on 3.3 V — **stop** |
| 3.3 V back-feed | INA2 ≈ 0 mA, SW1 off | Current into dummy on pins 84/86 |
| `GPIO_VREF` not tied | After SW1, VREF tracks 3V3 | 3V3 up, VREF 0 or 1.8 V |
| 1.8 V driven | 0 V forever on dummy | Any voltage — carrier bug |
| XIAO 5 V sequencing | Rises only after SW1 | Up with SW1 off |
| XIAO 3V3 from carrier | 0 V on dummy B and J_SENSE/3 | Red LED — isolation broken |
| ORing | Dummy B `INJECT VBUS`, carrier 5 V = 0 | Carrier plane rises |
| ORing drop | J_SENSE XIAO 5 V is 5 V minus diode | Too low for the module, or 0 if switch failed |
| Fan gating | Fan 5 V = 0 until DIP `VBUS` | Fan live with CM5 “off” |
| CC wired | PD PSU turns on 5 V; CC ~0.4 V | PSU dark, or CC at 5 V / 0 V mismatched |
| `PWR_Button` is 5 V | Serial ~5.0 V released, ~0 V pressed | 3.3 V released — someone buffered it wrong |
| UART isolation | GPIO4/5/14/15 not 5 V | 5 V on a GPIO — destroyed real CM5 |
| UART crossing | MCU TX on GPIO4 appears on dummy B RX | TX/RX swapped |
| rpiboot pair | D+/D− move when a host is plugged | Dead pair, swapped with nothing |
| Isolation model | No net from dummy A to dummy B except UART | USB D+, reset, or a GPIO on the XIAO socket |
| Dummy USB back-feed | Dummy USB VBUS unconnected | Laptop 5 V on carrier plane |

### 14.5 Board B additions (still no MCU required)

- UART RX/TX activity LEDs (high-pass into a transistor, or just a high-Z LED to 3V3 that blinks on traffic once dummy A’s emu is on).
- 2-pin analog out `XIAO_5V` / `XIAO_3V3` that jumpers to Board A `J_SENSE` 2/3 so one screen shows both sockets.
- Optional 10 mΩ + test points on the 5 V pin for inject-current during the ORing test (a DMM across the shunt is enough).

### 14.6 Firmware, if the MCU is fitted

No bootloader dance on first power: analog LEDs work unprogrammed. Firmware job is USB CDC text, OLED, INA/ADC scan, and `PASS`/`FAIL`. No USB PD stack, no 9 V request, no driving `CM5_3.3V` from firmware (SW1 stays a mechanical switch).

### 14.7 What still needs a real CM5

rpiboot enumeration, PD 5 A, `PMIC_Enable` actually dropping 3.3 V, Wi-Fi antenna, Linux console content, ESP32 ROM log. The dummy’s job is to make those tests non-suicidal.
