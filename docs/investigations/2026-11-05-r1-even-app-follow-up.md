# Deferred: compare R1 connection behavior with the Even app

Status: **deferred at the user's request, 2026-11-05. Do not start this
investigation until the user explicitly asks to resume it.**

The user wants a future investigation into how the stock Even app maintains
its R1 connection and how its behavior differs from Hardware One. This note
records the work; it does not authorize starting it now.

## Current decision

The R1 runs stock firmware, which Hardware One must accommodate. Occasional
disconnects are acceptable for now. The current reliability requirement is
bounded, automatic recovery without a crash, stuck setup, or unnecessary loss
of the glasses and phone links. An uninterrupted R1 connection is not a
completion requirement for the current fixes.

The user remained essentially stationary during testing; the phone and glasses
worked nearby around a corner. Do not attribute the R1 failures to walking or
range changes without evidence. R1 supervision timeouts were observed, but
their underlying cause is not established. The tested ring reports firmware
2.3.2.0008. Current evidence and deployed fixes are recorded in
[the fix progress](2026-11-04-p4x-fix-progress.md).

## Future investigation, only after resumption

Compare the stock app and Hardware One under matching conditions: the same
ring/firmware, glasses, phone, device placement and competing Bluetooth peers.
Record the app version and complete connection topology.

Determine which differences actually affect stability or recovery:

- Discovery, address types, pairing/bonding and security setup.
- Connection intervals, latency, supervision timeout and parameter updates.
- Setup order, subscriptions, periodic traffic and idle behavior.
- Coordination between the ring, both glasses arms and the phone.
- Disconnect detection, cleanup, retry timing and return to a usable session.

Use captured behavior to justify small changes, with before/after tests.
Do not assume a missing heartbeat, a particular radio parameter, or stock-app
superiority is already proven.

## Resume condition

Wait for the user's explicit instruction to investigate the Even app. The
current reliability work must not silently expand into that comparison.
