#!/usr/bin/env python3
"""Execute the production OLED ESP-NOW unpair adapter and discovery-screen input.

The unpair and pair-window owner (System_ESPNow's `espnowunpair` and
`espnowpairmode` commands), the OLED command executor with its admin gate, and
the toast are host doubles. The cases check that the OLED only submits
`espnowunpair <MAC>` / `espnowpairmode 120|off` under the display identity,
shows a short refusal without leaving the screen, and never calls the owner's
peer-delete, registry-remove or pair-window functions itself. Owner behavior
(registry save, crypto teardown, events, authorization) is not exercised here.
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
HELPER = "static bool oledEspNowRunOwnerCommand("
OWNER_INTERNALS = ("espnowDeletePeerRuntime(", "removeEspNowDevice(",
                   "espnowPairModeOpen(", "espnowPairModeClose(")


def declaration(source: str, name: str) -> str:
    match = re.search(r"(?m)^static[^\n]*\b" + name + r"\b[^\n]*;[^\n]*$", source)
    require(match is not None, f"missing file-static declaration: {name}")
    return match[0]


def generated(component: Path) -> str:
    oled = (component / "OLED_ESPNow.cpp").read_text()
    header = (component / "OLED_ESPNow.h").read_text()
    display = (component / "OLED_Display.h").read_text()
    types = [extract_block(header, marker) + ";" for marker in (
        "enum OLEDEspNowView", "enum OLEDEspNowMode", "struct OLEDEspNowState")]
    types.append(extract_block(display, "struct NavEvents") + ";")
    # The shared refusal helper exists only after the fix; a tree without it
    # still compiles here and is judged by the behavioral cases.
    production = [extract_block(oled, HELPER)] if HELPER in oled else []
    production += [extract_block(oled, marker) for marker in (
        "void oledEspNowBackToList(", "void oledEspNowUnpairDevice(")]
    handler = extract_block(oled, "bool oledEspNowHandleInput(")
    arm = extract_block(handler, "case ESPNOW_VIEW_PAIRING:")
    harness = (HERE / "oled_espnow_ownership_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_DECLARATIONS", declaration(oled, "sPairingSel")),
                         ("// INSERT_PRODUCTION", "\n\n".join(production)),
                         ("// INSERT_PAIRING_ARM", arm)):
        require(harness.count(marker) == 1, f"missing/duplicate marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1],
                        help="production component directory (overlay for base runs)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-parameter", "-Wno-unused-function"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-oled-espnow-own-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(generated(args.component))
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                       check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)
    # Static guards, after the behavioral run (which is the evidence on a tree
    # that still edits the owner's state directly).
    oled = (args.component / "OLED_ESPNow.cpp").read_text()
    require(HELPER in oled, "OLED_ESPNow.cpp lacks the shared owner-command helper")
    for symbol in OWNER_INTERNALS:
        require(symbol not in oled, f"OLED_ESPNow.cpp still calls {symbol}")
    config = extract_block(oled, "bool oledEspNowHandleDeviceConfigInput(")
    require("oledEspNowUnpairDevice();" in config,
            "the device-config menu no longer reaches oledEspNowUnpairDevice")
    print("OLED ESP-NOW ownership: unpair and discovery go through the owner's commands")


if __name__ == "__main__":
    main()
