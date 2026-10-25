#!/usr/bin/env python3
"""Keep the G2 Network page's background tasks behind the lens redraw fence.

The WiFi "Pending..." watchdog, the R1 "connecting..." watchdog and the WiFi
scan worker run on their own tasks. They may publish state and queue redraws
that the lens applier drops after any navigation, but must never draw, so a held
note (or any other page) is not covered by a Network menu. The page's state,
renderers, tasks and tap handlers, the redraw helper, the lens job types, the
WiFi scan record types and the lens worker's Redraw arm are compiled from
production sources; RTOS, radio, ring, command and display calls are host
boundaries driven by a fake clock. Each case runs in its own process.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from test_g2_status_corner import link_status_unit
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# Production functions, in source order, located by name so a changed return
# type still matches; forward declarations are skipped.
FUNCTIONS = (
    "showWiFiMenu", "showWiFiScanningPlaceholder", "showScanResults",
    "showBluetoothR1Menu", "showBluetoothPhoneMenu",
    None,  # the WiFi scan block goes here
    "wifiPendingWatchdogTask", "onWifiMenuRefreshDone", "onWifiConnectRefreshDone",
    "ringPendingWatchdogTask", "onBluetoothR1MenuRefreshDone",
    "onBluetoothPhoneMenuRefreshDone",
    "handleWiFiTap", "handleBluetoothR1Tap", "handleBluetoothPhoneTap",
)


def declaration(source: str, marker: str) -> str:
    start = source.index(marker)
    return source[start:source.index(";", start) + 1]


def region(source: str, begin: str, end: str) -> str:
    start = source.index(begin)
    return source[start:source.index(end, start)]


def definition(source: str, name: str) -> str:
    pattern = r"(?m)^(?:static[ \t]+)?[A-Za-z_][\w:<>* \t&]*?\b" + re.escape(name) + r"\("
    for match in re.finditer(pattern, source):
        brace = source.find("{", match.end())
        semicolon = source.find(";", match.end())
        if brace != -1 and (semicolon == -1 or brace < semicolon):
            return extract_block(source[match.start():], match.group(0))
    raise AssertionError(f"production definition not found: {name}")


def generated(component: Path) -> str:
    header = (component / "G2_HijackCmd.h").read_text()
    bridge = (component / "G2_HijackCmd.cpp").read_text()
    glasses = (component / "G2_Glasses.cpp").read_text()
    public = (component / "G2_Glasses.h").read_text()
    wifi = (component / "System_WiFi.h").read_text()
    page = (component / "G2_Page_Network.cpp").read_text()

    types = [extract_block(public, "enum G2HijackPage") + ";"]
    types += [extract_block(header, marker) + ";" for marker in (
        "struct G2CmdCookie", "struct RedrawSpec", "enum class LensJobKind",
        "struct LensUiJob", "enum class G2LensEnqueueWait")]
    types += [declaration(header, marker) for marker in (
        "bool g2EnqueueLensJob(", "bool g2EnqueueRedraw(")]
    types += [extract_block(wifi, marker) + ";" for marker in (
        "struct WifiScanRecord", "enum class WifiScanStatus", "struct WifiScanResult")]
    types.append(declaration(wifi, "using WifiScanVisitor"))
    # The Phone menu's words come from bleLinkStatus(), compiled as shipped over
    # the harness's role inputs and fake pipe/peer/console (`linkIn`).
    link_types, link_status = link_status_unit(component)
    types.append(link_types)

    state = region(page, "enum NetworkSubMode", "// Info text")
    state += "\n" + region(page, "enum WifiMenuAction", "static void showWiFiMenu() {")
    functions = []
    for name in FUNCTIONS:
        if name is None:
            start = page.index("static portMUX_TYPE s_netScanMux")
            functions.append(page[start:page.index("#endif  // ENABLE_WIFI", start)])
        else:
            functions.append(definition(page, name))
    worker = extract_block(glasses, "static void pageSwapWorkerLoop(")

    harness = (HERE / "g2_network_async_redraw_harness.cpp").read_text()
    replacements = (
        ("// INSERT_TYPES", "\n\n".join(types)),
        ("// INSERT_LINK_STATUS", link_status),
        ("// INSERT_REDRAW_HELPER", extract_block(bridge, "bool g2EnqueueRedraw(")),
        ("// INSERT_STATE", state),
        ("// INSERT_FUNCTIONS", "\n\n".join(functions)),
        ("// INSERT_REDRAW_DISPATCH", extract_block(worker, "case LensJobKind::Redraw:")),
    )
    for marker, body in replacements:
        require(harness.count(marker) == 1, f"missing/duplicate marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="production component directory (e.g. a base tree)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-function", "-Wno-unused-variable"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-g2-network-async-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(generated(args.component.resolve()))
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                       check=True, timeout=180)
        cases = subprocess.run([str(executable), "--list"], check=True, timeout=30,
                               capture_output=True, text=True).stdout.split()
        failed = []
        for case in cases:
            run = subprocess.run([str(executable), case], timeout=60,
                                 capture_output=True, text=True)
            sys.stdout.write(run.stdout)
            if run.returncode != 0:
                failed.append(case)
                sys.stdout.write(f"{case}: FAILED\n{run.stderr}")
        sys.stdout.flush()
        require(not failed, f"{len(failed)}/{len(cases)} cases failed: {', '.join(failed)}")
    print(f"G2 Network background redraw fence: {len(cases)} cases passed")


if __name__ == "__main__":
    main()
