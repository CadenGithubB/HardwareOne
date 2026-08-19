# Hardware One — Logo Design Brief

## 1. One-sentence description

Hardware One is open-source firmware that turns cheap $10 ESP32 microcontrollers
into nodes of a single private, router-free system — each chip built for a
different job, all speaking the same protocol.

## 2. What the product actually is

You compile and flash each chip to fit a role: a sensor appliance, a headless
mesh relay, a handheld with a tiny OLED screen and a thumb gamepad, a camera node,
or a pocket companion that drives a pair of smart glasses. No two devices need the
same feature set. They find each other over a custom peer-to-peer radio protocol
with no WiFi router in between, pool what each one can do, and become one system
you monitor from a single dashboard.

The unifying idea, stated by the project itself: **one command system, every way in.**
The same commands with the same permission checks reach the device over a USB cable,
a browser, the physical screen and gamepad, Bluetooth, your voice, or another node
on the mesh. There is no "main" interface — they are all doorways into the same core.

## 3. The three ideas worth putting in a logo

Ranked. A good mark probably carries one or two of these, not all three.

1. **Many small parts, one system.** Scattered, cheap, individually unimpressive
   chips that only become interesting in aggregate. Mesh, constellation, swarm,
   assembly — but the parts stay visibly distinct, not merged into a blob.
2. **One core, many doorways.** A single center reached from many directions.
   Radial, convergent, hub-and-spoke — the emphasis on the *center holding*, not
   on the spokes.
3. **Modularity as the point.** Every feature is a flag you flip on or off before
   you compile. Two devices running this firmware may share almost no functionality.
   The identity is the substrate, not the feature list.

## 4. Tone

**Is:** engineered, self-hosted, precise, understated. Solo-built and hand-tuned.
Documentation-heavy and honest — the release notes admit what previous notes got
wrong. Hobbyist in scale, professional in rigor.

**Is not:** consumer-slick, startup-friendly, AI-branded, cute, retro-futurist,
military/tactical, or "smart home."

Reference points in feel — not in form: Wireshark, Home Assistant, Tailscale,
PlatformIO, Meshtastic. Utilities that people trust with infrastructure and that
never shouted.

Words a designer could work from: *lattice, node, substrate, plane, mesh, quiet,
addressable, low-power, discrete, converge.*

## 5. Where the mark has to work

- **GitHub README at 140px wide**, light and dark themes. This is the only place
  the mark is actually consumed today. The project ships separate black and white
  SVGs and swaps them on `prefers-color-scheme`, so the mark **must work as a pure
  single-color silhouette — no gradients, no fixed background.** That constraint is
  real and binding; small-size legibility currently is not.
- **Repository social card** (1280×640) and docs headers, if we add them.
- **Not served by the devices.** The firmware answers `/favicon.ico` and the
  touch-icon paths with 204 No Content on purpose, to avoid spending flash on an
  asset nobody needs. `assets/favicon.ico` and `assets/icon-16.png` … `icon-512.png`
  exist in the repo but are referenced from nowhere.

> **Open question for the client, not the designer:** if we ever want a real browser
> favicon, a GitHub org avatar, or a 128×64 one-bit OLED splash on the device, then
> the mark does need to survive coarse pixels, and that should be decided *before*
> design starts rather than retrofitted. As the project stands, none of those exist.

## 6. Constraints

- **Strictly monochrome. No color at all — not as an accent, not as a second
  stage.** The mark must be pure black on transparent, and pure white on
  transparent, and nothing else. No greys, no gradients, no tints, no baked-in
  background plate. If the design needs a color to read, it is the wrong design.
- Must have a wordmark-free standalone glyph. "Hardware One" is two words; a
  monogram route (H1) is available but not required.
- Interior detail is affordable at 140px in a way it would not be in a favicon.
  Don't reflexively over-simplify; the mark has room to carry an idea.

## 7. Deliverables

Two SVGs of the same glyph, identical in every respect except fill:

| File | Fill | Background | Used when |
| ---- | ---- | ---------- | --------- |
| `logo-black.svg` | `#000000` | transparent | reader is in light mode |
| `logo-white.svg` | `#FFFFFF` | transparent | reader is in dark mode |

These are not two designs — they are one design and its inversion. The README
selects between them automatically via `prefers-color-scheme`, so the mark has to
look equally deliberate as black-on-white and as white-on-black. A shape that
only works one way round is not finished.

This is a real constraint on form, not just on export. Heavy solid masses invert
cleanly — they read as the same object in either polarity. Delicate figure-ground
play often does not: a mark that relies on thin knocked-out gaps, on counters
doing the work, or on the eye reading the negative space as the subject will
frequently look sharp in one polarity and muddy or unbalanced in the other. Worth
testing the inversion early rather than at delivery, because discovering it late
usually means a redraw rather than a fix.

Practical notes for the designer:

- Fill the paths with a flat color value, not `currentColor` and not a CSS class.
  The two files are swapped as whole assets, not restyled.
- Outlines/strokes should be converted to filled paths so the two versions cannot
  drift apart.
- Ship the source file (AI/Figma/SVG) alongside, so the inversion can be
  regenerated if the mark is ever revised.

## 8. What we have now, and why it's being replaced

The current mark is a generic stock robot silhouette — a placeholder, not a design
decision. It says "robotics toy." The project is not about robots, is not
cute, and has nothing to do with humanoid form. Please do not iterate on it.

## 9. Clichés to avoid

WiFi arcs, radio waves radiating from a tower, circuit-board trace patterns,
literal microchips with pins, brains, hexagon grids, isometric cubes, gradient
meshes, and anything that reads as generic IoT stock art.

## 10. Scale, for context on ambition

Roughly 340,000 lines of C++ across ~370 source files, over 1,000 documented
commands, four role tiers of authentication, signed over-the-air updates with a
recovery path, and support for a couple dozen sensors across four board families.
Single author, version 0.99.x, licensed noncommercial.
