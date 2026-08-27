#!/usr/bin/env python3
"""Pre-flight serial-port guard for the guarded HardwareOne cable-flash targets.

Why this exists
---------------
`idf.py -p <port> ota0-flash` silently ignores `-p`.

The port is a *global* click option carrying `'envvar': 'ESPPORT'`
(esp-idf/tools/idf_py_actions/serial_ext.py), and the built-in flash actions
forward it explicitly:

    run_target(target_name, args, {'ESPBAUD': ..., 'ESPPORT': args.port})

But every HardwareOne cable target (`ota0-flash`, `factory-flash`,
`migration-flash`, `littlefs-flash`, and the `encrypted-*` variants) is a plain
CMake custom target, so idf.py dispatches it through `fallback_target()`
(esp-idf/tools/idf_py_actions/core_ext.py), which calls `run_target(target_name,
args)` with no extra environment at all. `args.port` is parsed, accepted, and
dropped on the floor. esptool then falls back to scanning for a port and picks
whichever one answers first.

With a single board attached that is harmless. With two, it flashes whichever
enumerates first - which is a silent, and quite expensive, way to write firmware
onto the wrong device.

What this does
--------------
Runs immediately before any guarded flash and reports the port that will
actually be written. It refuses only when the choice is genuinely ambiguous:

  ESPPORT set          -> use it, print it, proceed
  exactly one candidate-> print it, proceed (the common bench case)
  zero candidates      -> fail: nothing to flash
  two or more          -> fail: list them and print the exact command to re-run

A "candidate" is any serial port reporting a USB vendor ID. That filter exists
to drop the macOS pseudo-ports (`/dev/cu.debug-console`,
`/dev/cu.Bluetooth-Incoming-Port`) which report no VID and are the reason
esptool's own "Found 2 serial ports" line is untrustworthy as a board count. It
deliberately does not allowlist specific vendor IDs, so an unusual USB-serial
bridge is still seen rather than silently excluded.

If pyserial cannot be imported this warns and allows the flash: esptool needs
pyserial too, so it will produce the real error a moment later, and blocking a
legitimate flash over a missing diagnostic dependency would be worse than the
problem being guarded against.
"""

from __future__ import annotations

import os
import sys


def _fail(message: str) -> int:
    sys.stderr.write("\n" + message.rstrip() + "\n\n")
    return 1


def main() -> int:
    target = sys.argv[1] if len(sys.argv) > 1 else "<flash target>"

    port = (os.environ.get("ESPPORT") or "").strip()
    if port:
        # Trusting ESPPORT blindly defeated the entire point of this guard. A
        # copy-pasted placeholder -- ESPPORT=/dev/cu.usbserial-XXXX -- was
        # accepted and the guard printed "writing hw1-layout-program to
        # /dev/cu.usbserial-XXXX" for a device that does not exist. The one
        # line an operator reads to confirm where a destructive one-time
        # migration is about to go must not be able to name a nonexistent
        # port with total confidence.
        if not os.path.exists(port):
            hint = ""
            try:
                import serial.tools.list_ports as _lp
                found = [p.device for p in sorted(_lp.comports(), key=lambda p: p.device)
                         if p.vid is not None]
                if found:
                    hint = "\n\nAttached USB serial devices:\n" + "\n".join(
                        f"    ESPPORT={d}" for d in found)
            except Exception:
                pass
            return _fail(
                f"Refusing to run {target}: ESPPORT={port} does not exist.\n"
                "Nothing was written. If that looks like a placeholder from a "
                "pasted command, substitute the real port." + hint
            )
        print(f"[flash-port] ESPPORT={port} -> writing {target} to {port}")
        return 0

    try:
        import serial.tools.list_ports as list_ports
    except Exception as exc:  # pragma: no cover - depends on host env
        sys.stderr.write(
            f"[flash-port] WARNING: cannot enumerate serial ports ({exc}); "
            "skipping the port guard and letting esptool choose.\n"
        )
        return 0

    candidates = [p for p in sorted(list_ports.comports(), key=lambda p: p.device)
                  if p.vid is not None]

    if not candidates:
        return _fail(
            f"No USB serial device found, so {target} has nothing to write.\n"
            "Connect the board, or name the port explicitly:\n"
            f"    ESPPORT=/dev/cu.usbmodemXXXX idf.py ... {target}"
        )

    if len(candidates) == 1:
        chosen = candidates[0]
        label = chosen.manufacturer or chosen.description or "unknown device"
        print(f"[flash-port] {target} -> {chosen.device} ({label})")
        return 0

    lines = []
    for p in candidates:
        vid_pid = f"{p.vid:04x}:{p.pid:04x}" if p.pid is not None else f"{p.vid:04x}:????"
        label = p.description or p.manufacturer or "unknown device"
        lines.append(f"    ESPPORT={p.device:<28} # {vid_pid}  {label}")

    return _fail(
        f"Refusing to run {target}: {len(candidates)} USB serial devices are attached "
        "and nothing said which one to write.\n\n"
        "NOTE: idf.py's -p/--port flag does NOT reach this target. It is a plain CMake\n"
        "custom target, so idf.py drops the port and esptool picks whichever device\n"
        "answers first. Set ESPPORT instead - prefix it onto the same command:\n\n"
        + "\n".join(lines)
    )


if __name__ == "__main__":
    sys.exit(main())
