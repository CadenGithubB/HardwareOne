#!/usr/bin/env python3
"""No other task can switch the ESP-NOW loop off.

espnow_task runs processMeshHeartbeats every 10 ms: the receive drain first,
then the session, send-status and file sweeps, the mesh and bond heartbeats,
the deferred command and result handling and the text and stream drains. Web
handlers used to set a plain bool (gMeshActivitySuspended) around every page
render, every /api/cli command and the whole /api/cli/batch loop; while it was
set the loop returned right after the receive drain. A web command longer than
the 15 s bond timeout (the page's Send File runs through /api/cli) then cost
the bond its session, and two overlapping requests cleared the flag early.

Source contract, nothing is compiled:

- no production source of the component names the flag (code or comment);
- between the receive drain and the first sweep, processMeshHeartbeats has one
  early return and it asks only whether ESP-NOW is initialised;
- the four web handlers that held the flag name no mesh or ESP-NOW pause.

The web handlers themselves are compiled by web_batch_handlers, whose harness
no longer defines the flag, so a handler that sets it again does not build
there.

--component points the suite at another component directory (the pre-fix
tree fails every part).
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

FLAG = "gMeshActivitySuspended"
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".c", ".inc", ".js"}
DRAIN_MARK = "// 1. Drain the inbound RX ring buffer."
FIRST_SWEEP = "pendingFrameTimeoutSweep("
EXPECTED_GUARD = "if (!gEspNow || !gEspNow->initialized) return;"
WEB_HANDLERS = (
    "void streamPageWithContent(",
    "esp_err_t handlePasswordChangePage(",
    "esp_err_t handleCliCommand(",
    "esp_err_t handleCliBatch(",
)
PAUSE_NAME = re.compile(r"\b\w*(?:mesh|espnow)\w*(?:suspend|pause)\w*\b", re.IGNORECASE)


def production_sources(component: Path):
    for path in sorted(component.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(component)
        if relative.parts[0] in ("test", "build"):
            continue
        yield relative, path.read_text(encoding="utf-8", errors="replace")


def strip_comments(code: str) -> str:
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    return re.sub(r"//[^\n]*", "", code)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    # Accepted for the shared test registration arguments; nothing is compiled.
    parser.add_argument("--cxx")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()

    failures: list[str] = []

    # 1. The flag is gone from every production source.
    for relative, text in production_sources(args.component):
        for number, line in enumerate(text.splitlines(), 1):
            if FLAG in line:
                failures.append(f"{relative}:{number} still names {FLAG}: {line.strip()[:100]}")

    # 2. The loop's only early return after the receive drain is its own state.
    espnow = (args.component / "System_ESPNow.cpp").read_text(encoding="utf-8")
    loop = extract_block(espnow, "void processMeshHeartbeats(")
    drain = loop.find(DRAIN_MARK)
    sweep = loop.find(FIRST_SWEEP, drain if drain >= 0 else 0)
    if drain < 0 or sweep < 0:
        failures.append("processMeshHeartbeats: cannot locate the receive drain and the first sweep")
    else:
        between = strip_comments(loop[drain:sweep])
        guards = [" ".join(m.group(0).split())
                  for m in re.finditer(r"if\s*\([^;{}]*\)\s*return\s*;", between)]
        if guards != [EXPECTED_GUARD]:
            failures.append("processMeshHeartbeats: the early return after the receive drain is "
                            f"{guards or 'missing'}, expected only '{EXPECTED_GUARD}'")

    # 3. The web handlers that held the flag pause nothing of ESP-NOW.
    web = (args.component / "WebServer_Server.cpp").read_text(encoding="utf-8")
    for marker in WEB_HANDLERS:
        body = strip_comments(extract_block(web, marker))
        for name in sorted(set(PAUSE_NAME.findall(body))):
            failures.append(f"WebServer_Server.cpp {marker.split('(')[0].split()[-1]} uses {name}")

    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        print(f"espnow_loop_not_paused: {len(failures)} failure(s)")
        return 1
    print("PASS espnow_loop_not_paused: web requests no longer pause the ESP-NOW loop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
