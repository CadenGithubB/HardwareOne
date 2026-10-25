#!/usr/bin/env python3
"""Run OLED_Mode_Bluetooth.cpp's screens and link watcher on the host.

The whole file is compiled as shipped, with the real BLE_LinkStatus.cpp behind
it, against fakes for the live state (oled_bluetooth_link_watch_harness.cpp):
every ENABLE_G2_GLASSES x ENABLE_BLE_PHONE_PIPE build, on the 64 px OLED (5
content rows) and the 128 px P4 canvas (13). Checks:
- the BT status screen shows Role (+ Saved), Glasses/Ring, Pipe and Console in
  BLE_LinkStatusCore.h's words, with no State: row, for off, the glasses, the
  phone pipe and the app console; the console's detail stops at the footer;
- the Phone screen's "PIPE [OK]/[..]/[!!]/[--]/[off]" row, the pipe's words and
  the Console row; its taps run what the drawn rows said;
- the main menu's Advertising/Disconnect rows come from bleConsoleView() with
  gBLEState freed, and the Phone >> marker follows the pipe;
- one main-menu row per role in the build (App console, Glasses, Phone pipe)
  where Start/Stop was, the running role marked " *" (Glasses+pipe marks
  Glasses); each tap opens its confirm (lines, default) and Yes runs the drawn
  row's command: openconsole/closeconsole, openg2/closeg2, openphone/closephone.
  The app console is reachable from every role;
- nothing wraps past 21 columns or reaches the footer;
- the link watcher marks the display once per change (role, saved mode, pipe
  phase or text, pairing, auto-reconnect, console state, clients, advertising,
  glasses, ring) and never when idle;
- Phone >> New code and pair asks first (default No), then runs
  `phone pair new` asynchronously; the code screen draws the code large under
  the time left and the outcome once the window ends; B closes the window; the
  watcher closes a window whose screen was left (or never came up within 2 s)
  and redraws the countdown once a second; the code never reaches a toast, a
  command or a confirm. It draws the code only while the display session that
  issued it is live (else it wipes its copy and closes the window), and the
  countdown never passes 59:59. The fake's PhonePairView fields are pinned to
  BLE_PhonePipe.h's.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
# Copied beside the harness, so their own includes find the empty stand-ins.
REAL = ("OLED_Mode_Bluetooth.cpp", "BLE_LinkStatus.cpp", "BLE_LinkStatus.h", "BLE_LinkStatusCore.h",
        "OLED_Layout.h", "BLE_PhonePairCode.h")
SYSTEM = {"cstdarg", "cstddef", "cstdint", "cstdio", "cstring"}
HEIGHTS = (43, 107)  # content rows: 5 on a 64 px OLED, 13 on the P4 canvas


def stand_ins(sources: list[str]) -> set[str]:
    """Every header the copied files include that is not copied or a C++ header."""
    names = set()
    for text in sources:
        names.update(re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.M))
    return {n for n in names if n not in REAL and n not in SYSTEM}


def pair_view_fields(text: str) -> list[str]:
    """PhonePairView's member names, in order."""
    body = re.sub(r"//[^\n]*", "", extract_block(text, "struct PhonePairView"))
    return re.findall(r"(\w+)\s*=", body)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sanitize = (["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                 "-fno-omit-frame-pointer", "-g"] if args.sanitize else [])
    # The harness fakes the owner's view of the window: same members, same order.
    real = pair_view_fields((args.component / "BLE_PhonePipe.h").read_text())
    fake = pair_view_fields((HERE / "oled_bluetooth_link_watch_harness.cpp").read_text())
    require(real == fake and "epoch" in real, f"PhonePairView: header {real} vs harness {fake}")
    total = 0
    with tempfile.TemporaryDirectory(prefix="hw1-oled-bt-") as directory:
        work = Path(directory)
        texts = []
        for name in REAL:
            text = (args.component / name).read_text()
            (work / name).write_text(text)
            texts.append(text)
        for name in stand_ins(texts):
            (work / name).write_text("#pragma once\n")
        unit = work / "harness.cpp"
        shutil.copy(HERE / "oled_bluetooth_link_watch_harness.cpp", unit)
        for height in HEIGHTS:
            for g2 in (0, 1):
                for pipe in (0, 1):
                    binary = work / f"oled-bt-{height}-{g2}{pipe}"
                    subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    "-Wno-unused-function", "-Wno-missing-field-initializers",
                                    *sanitize, "-I", str(work),
                                    f"-DENABLE_G2_GLASSES={g2}", f"-DENABLE_BLE_PHONE_PIPE={pipe}",
                                    f"-DOLED_CONTENT_HEIGHT={height}",
                                    str(unit), "-o", str(binary)], check=True, timeout=180)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                    require(result.returncode == 0,
                            f"height={height} g2={g2} pipe={pipe}: {result.stderr.strip()}")
                    total += int(result.stdout.strip())
    print(f"OLED Bluetooth: status, Phone and menu rows in the link words; watcher marks once per "
          f"change; New code and pair, the code screen and its window watch; 8 builds ({total} checks)")


if __name__ == "__main__":
    main()
