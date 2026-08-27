# Even Realities R1 Ring — How It Actually Works

**A behavioral overview for engineers.** Black-box observations only: no vendor code,
no protocol listings, no source. Scoped to ring firmware **2.2.0.0011** — this device
drifts between versions, so treat every claim as version-bound.

---

## What it is

A finger-worn BLE health sensor: PPG-derived heart rate, HRV and SpO2, skin
temperature, step/calorie activity, sleep, plus wear detection and battery. It is
built as an accessory to the Even Realities G2 glasses and the vendor's phone app.
There is no published protocol specification.

## Topology

Three parties want the ring: the phone app, the glasses' right temple (which acts as
a bridge and drives the glasses' built-in health widget), and — if you're building
something — your own host. **The ring accepts one central at a time.** Observed
repeatedly: while any one central holds the link, everyone else's connect attempts
simply time out. Nearly every surprising thing about integrating with this device
falls out of that one property.

## Connecting

One custom GATT service, one write characteristic, one notify characteristic. Two
practical gotchas: it advertises with a *random static* address, so a directed
connect fails with a ~30 s timeout unless your stack is explicitly told the address
type; and the default MTU is small enough to fragment the larger responses, so
negotiate up.

Session setup is a short application-layer handshake — an auth step, a host-to-ring
time set, and a step that tells the ring to advertise for the glasses. Notably the
auth step involves **no key exchange and no server-issued token**; earlier community
guesses about a remote-issued pairing key are empirically wrong. After it acks, the
ring starts responding and emits an unsolicited status frame (battery + wear) about
every 30 seconds.

## Getting data off it

**It is a pull device, not a push device.** The ring samples on its own schedule
while worn — roughly every several minutes — and caches the most recent value per
metric. Your queries read that cache: the reply comes back in well under a second,
but the value may be minutes old. Polling faster than about 30 seconds gains you
nothing but power draw. An on-demand measurement mode exists, but it was not
reliably reproducible in testing and is not fully characterized.

Daily aggregates work for heart rate and activity. For HRV, SpO2, temperature and
sleep the ring either refused the request or answered inconsistently. Long
aggregate responses can span multiple notifications with a partial record at the
boundary — you have to stitch them.

## The clock problem

The host sets the ring's time during setup, and the ring will **never report its
clock back**. You cannot measure its drift. Combined with cached reads, this means a
freshly measured sample and a ten-minute-old cache re-read are indistinguishable
unless you record a per-metric timestamp *and* track whether that timestamp came
from the ring or from your own receipt clock. If you're logging this data for
graphing, design for that up front — retrofitting sample provenance is painful.

## What doesn't work

- **Driving the ring↔glasses bridge from a third-party host.** Every stage up to the
  final bond succeeds: the glasses accept the trigger, find the ring, and attempt to
  connect — then fail terminally and retry forever. The most likely explanation is
  that the temple needs a per-ring shared secret that the vendor's own app
  provisions. We found no evidence anyone has completed this path third-party.
  The workaround is to read the ring yourself and draw the numbers to the lens with
  your own display commands; don't plan around the glasses' native health widget.
- **Touch and gesture events.** Physical taps produce nothing on the ring's host
  link. They appear to route to the glasses, if anywhere.
- Several modules named in community reverse-engineering references are simply
  silent on this firmware — possibly removed.

## Hazards

- At least one settings write **silently disables the ring's touch sensor**.
  Recovery is a physical tap gesture on the charger, not a software undo.
- There is a firmware-update entry point. Triggering it without a valid image is a
  brick.
- There is a per-device key that can be overwritten, which may permanently break the
  ring's relationship with the vendor's app.
- The rule that kept us out of trouble: **never send write/set semantics to an
  opcode you don't understand.** Reads are cheap and the ring rejects bad ones
  cleanly. Writes can be one-way doors.

## Per-device secrets

Each ring exposes a per-device key and two distinct serial numbers. The key is
partly derived from the ring's MAC with a unique remainder, and looks like
device-binding material. Treat all three as confidential — keep them out of logs,
screenshots, and any document like this one.

---

## Scope and provenance

Derived from empirical testing against hardware we own, cross-checked against
publicly available community reverse-engineering projects. **No third-party code is
reproduced or included here, and none of our own implementation is either.**
Deliberately excluded: opcode tables, frame and payload layouts, checksum
parameters, GATT identifiers, source code, and all real device identifiers. Nothing
here derives from vendor firmware extraction, decompilation, or any material under
NDA — it is black-box observation of a purchased device, at roughly the level of
detail a datasheet would provide.

Not legal advice. Not affiliated with, endorsed by, or sourced from Even Realities.

*Document revision: 2026-07-30*
