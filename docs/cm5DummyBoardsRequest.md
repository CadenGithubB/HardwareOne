# Quote brief — CM5 dummy + XIAO dummy

I need two small test PCBs that plug into my carrier **instead of** a real
Raspberry Pi CM5 and a real Seeed XIAO ESP32-S3. They let me check power,
LEDs, and two UART wires before I risk the real modules.

This is a simple hardware-only job: schematic, layout, and manufacturing
files. Quote that, and stop there.

## Do not include

- firmware, microcontroller, screen, or USB-serial (no CH340, CP2102, or
  similar)
- USB-C on either dummy
- a 1.8 V regulator or any 1.8 V supply
- a bench 5 V inject header on the XIAO dummy
- USB-PD, or any voltage above 5 V
- HDMI, Ethernet, or high-speed routing
- lab certification or an automated tester

If something below is missing and you would normally add it, ask first.

## How power actually flows

The **carrier** pushes 5 V into the dummy. The dummy does not make 5 V.

On the CM5 dummy, 5 V arrives on six pins, then through six jumpers that
ship **open**, then onto a common rail. A regulator on that rail can make
3.3 V after I flip a switch that ships **off**. That 3.3 V goes out only
on CM5 pins 84 and 86, so the carrier can wake up.

On the XIAO dummy, 5 V comes only from the carrier, on the XIAO 5 V pin.
After the CM5 dummy’s 3.3 V switch is on, a yellow LED on that pin should
light. The XIAO dummy does not make 5 V or 3.3 V.

## Board A — CM5 dummy

Same 55 × 40 mm outline and mounting holes as a real CM5.

- Two Amphenol `10164228-1001A1RLF` 100-pin **headers** on the **bottom**.
- The carrier uses matching 1.5 mm-stack receptacles. There is no room
  under the dummy for parts. Everything else goes on top.
- Copy header position and rotation from the official Raspberry Pi CM5
  STEP model and the Amphenol drawing. The two header centre-lines are
  **34.00 mm apart**, near opposite long edges, not clustered in the
  middle.
- Use the official CM5 pinout. Put pads on the board for all 200 pins so
  none are silently dropped.
- Tie all official CM5 GND pins together.

**5 V in (from the carrier)**

Pins 77, 79, 81, 83, 85, 87. Do **not** hard-wire them together.

Each of those six pins gets its own labelled test point and its own
removable 2-pin jumper to a common `5V_DUMMY` rail. All six jumpers ship
**open**. I will meter each pin first, then fit the jumpers. That finds a
dead 5 V contact that tying the pins together would hide.

Yellow LED on `5V_DUMMY`. Labelled test points for `5V_DUMMY` and GND.

**3.3 V out (dummy pretends to be a CM5)**

Simple 5 V to 3.3 V regulator, at least 500 mA, with a real ON/OFF switch
that physically disconnects the output from pins 84 and 86 when off. Switch
ships **off**. Green LED on 3.3 V **after** the switch. Labelled 3.3 V
test point.

**Meter pads only — do not power these**

- GPIO_VREF, pin 78. Leave a pad. Do not tie it to the dummy regulator.
  After 3.3 V is on, this pin should come back as 3.3 V through the
  carrier. If it does not, the carrier VREF link is wrong.
- 1.8 V pins 88 and 90. Leave pads. The dummy does not make 1.8 V. These
  pins should sit at 0 V. If they have voltage, the carrier is driving
  them by mistake.

**USB-C attach (5 V only)**

Two separate 5.1 kΩ resistors: CC1 pin 94 to GND, and CC2 pin 96 to GND.
Do not join CC1 and CC2. This only asks a USB-C supply for normal 5 V.
No PD controller.

**DIP switch (all off)**

| What it tests | CM5 pin | What the switch does |
|---|---:|---|
| PWR LED | 95 | to GND through 1 kΩ |
| ACT LED | 21 | to GND through 1 kΩ |
| OS LED | GPIO23, pin 47 | to 3.3 V through 1 kΩ |
| LINK LED | GPIO24, pin 45 | to 3.3 V through 1 kΩ |
| ERR LED | GPIO25, pin 41 | to 3.3 V through 1 kΩ |
| USB VBUS enable | 111 | to 3.3 V through 1 kΩ |
| SD power enable | 75 | to 3.3 V through 1 kΩ |

Keep those signals separate. Do not gang them.

**UART check**

No USB-serial on this board.

Two momentary buttons, normally open, powered from dummy 3.3 V after the
switch:

- `TX` puts 3.3 V through about 1 kΩ onto GPIO4 / pin 54
- `RX` puts 3.3 V through about 1 kΩ onto GPIO5 / pin 34

Put a small LED next to each button so I can see the button is actually
driving that pin.

Also a 3-pin header, no power pin, labelled `3.3 V UART ONLY`:

- GND
- GPIO4 / CM5 TX, pin 54
- GPIO5 / CM5 RX, pin 34

Optional 2-pin header: `5V_DUMMY` and GND, for a load if I want one.

## Board B — XIAO dummy

A small board that plugs into the carrier’s XIAO ESP32-S3 **socket**.

- Official Seeed XIAO ESP32-S3 footprint.
- Two 1×7 male headers, 2.54 mm pitch, **15.24 mm** row spacing.
- Male pins point **down** into the socket.
- Mark `USB END` and `PIN 1` on both sides so the board cannot be flipped
  or mirrored. There is **no USB-C connector**. The mark is only so it
  matches a real XIAO.
- Outline can be 21 × 17.8 mm, or a few millimetres larger if the parts
  need room. The **pin positions** must still match Seeed. Print 1:1 and
  overlay a real XIAO before fab.
- All parts on top.

**Power**

- Yellow LED and test point on the XIAO 5 V pin. This 5 V comes from the
  carrier. After Board A’s 3.3 V switch is on, this LED should light.
- Red LED and test point on the XIAO 3.3 V pin. The dummy does **not**
  make 3.3 V. This LED should stay **off**. If it lights, the carrier is
  putting 3.3 V on a pin it should leave alone.

No inject header. No second 5 V supply.

**UART LEDs**

Two LEDs to GND through about 1 kΩ. Do not connect them to the XIAO 3.3 V
pin.

- On D7 / RX / GPIO44, label `FROM CM5 TX`
- On D6 / TX / GPIO43, label `TO CM5 RX`

With both dummies plugged in and Board A’s 3.3 V switch on:

- Press `TX`: only `FROM CM5 TX` lights
- Press `RX`: only `TO CM5 RX` lights
- The other LED lights: the two UART wires are swapped
- Neither lights: a UART wire is open

All other XIAO pins stay unconnected.

## What I should be able to do

1. With jumpers open, meter each of the six CM5 5 V pins (about 4.75–5.25 V),
   then fit the jumpers and see the yellow 5 V LED.
2. Turn on 3.3 V. Green LED on. Pin 78 reads about 3.3 V. Pins 88/90 stay
   at 0 V.
3. Flip DIP switches and see the matching carrier LEDs / enables.
4. Press TX / RX and see the matching Board B UART LED.
5. See Board B’s yellow 5 V LED on, and its red 3.3 V LED off.

## Labels I want on the silk

Board A: `5V`, `3V3`, `3V3 SWITCH`, jumper pin numbers `77 79 81 83 85 87`,
`TX`, `RX`, `3.3 V UART ONLY`, `GPIO_VREF`, `1.8V CHECK` on pins 88/90,
`CC1`, `CC2`.

Board B: `USB END`, `PIN 1`, `5V`, `3V3` (and `MUST STAY OFF` near the red
LED), `FROM CM5 TX`, `TO CM5 RX`.

## Deliverables

- KiCad preferred
- schematic for my approval **before** layout
- Gerbers, drill, BOM with manufacturer part numbers, pick-and-place, 3D STEP
- basic ERC/DRC
- assembly note listing every switch/jumper that ships off or open

Please quote two simple prototype boards. Extra diagnostics, a display, an
MCU, or formal qualification are outside this quote.
