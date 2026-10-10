#!/usr/bin/env python3
"""Exercise the shipping Bluetooth page's status rendering and controls in Node.

Only browser boundaries are mocked. An optional --baseline header compares
successful rendering against saved production, including the former text parser.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
FUNCTIONS = (
    "el", "setText", "applyMode", "applyState", "stateFromInfo",
    "g2OverallState", "renderG2", "renderRing", "esc", "renderConnections",
    "showStatusText", "refresh",
)


def browser_functions(header: Path, baseline: bool = False) -> str:
    source = header.read_text()
    names = FUNCTIONS
    if baseline and re.search(r"^  function parseG2Status\(", source, re.MULTILINE):
        names += ("parseG2Status",)
    if re.search(r"^  function updateControlAvailability\(", source, re.MULTILINE):
        names += ("updateControlAvailability",)
    functions = []
    for name in names:
        # These functions are top-level members of the page IIFE. Use their
        # indentation boundary rather than treating JS regexes as C++ literals.
        marker = re.search(rf"^  function {name}\([^\n]*", source, re.MULTILINE)
        if not marker:
            raise AssertionError(f"missing production function: {name}")
        if marker.group().rstrip().endswith("}"):
            functions.append(marker.group())
        else:
            closing = re.search(r"^  }$", source[marker.end():], re.MULTILINE)
            if not closing:
                raise AssertionError(f"unterminated production function: {name}")
            functions.append(source[marker.start():marker.end() + closing.end()])
    return "\n\n".join(functions)


def browser_page(header: Path) -> dict:
    """Extract the entire shipping IIFE and the corresponding static markup."""
    source = header.read_text()
    script = re.search(r'R"JS\(\s*<script>(.*?)</script>\s*\)JS"', source, re.DOTALL)
    if not script:
        raise AssertionError("missing production Bluetooth page script")
    content = source[source.index("inline void streamBluetoothInner"):source.index("  // JavaScript")]
    def markup(g2: bool, sensors: bool = True, microphone: bool = True) -> str:
        features = {"ENABLE_BLUETOOTH": True, "ENABLE_G2_GLASSES": g2,
                    "ENABLE_WEB_SENSORS": sensors, "ENABLE_MICROPHONE": microphone}
        active = [True]
        lines = []
        for line in content.splitlines():
            directive = line.strip()
            if directive.startswith("#if "):
                condition = directive[4:]
                terms = [term.strip() for term in condition.split("&&")]
                if any(term.lstrip("!") not in features for term in terms):
                    raise AssertionError(f"unhandled page feature guard: {condition}")
                value = all(not features[term[1:]] if term.startswith("!")
                            else features[term] for term in terms)
                active.append(active[-1] and value)
            elif directive == "#else":
                active[-1] = active[-2] and not active[-1]
            elif directive.startswith("#endif"):
                active.pop()
            elif active[-1]:
                lines.append(line)
        chunks = re.finditer(
            r'httpd_resp_send_chunk\(req,\s*(?:R"HTML\((.*?)\)HTML"|("(?:\\.|[^"\\])*"))',
            "\n".join(lines), re.DOTALL)
        return "\n".join(match.group(1) if match.group(1) is not None
                         else json.loads(match.group(2)) for match in chunks)

    return {
        "script": script.group(1),
        "markup": {str(enabled).lower(): markup(enabled) for enabled in (False, True)},
        "featureMarkup": [
            {"g2": g2, "sensors": sensors, "microphone": microphone,
             "html": markup(g2, sensors, microphone)}
            for g2 in (False, True) for sensors in (False, True) for microphone in (False, True)
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node", default=os.environ.get("NODE") or shutil.which("node"))
    parser.add_argument("--baseline", type=Path,
                        help="Pre-change WebPage_Bluetooth.h or its directory")
    args = parser.parse_args()
    if not args.node:
        raise SystemExit("Pass --node /path/to/node for JavaScript behavior tests")

    with tempfile.TemporaryDirectory(prefix="hw1-g2-status-browser-") as tmp:
        current = Path(tmp) / "current.js"
        current.write_text(browser_functions(COMPONENT / "WebPage_Bluetooth.h"))
        cmd = [args.node, str(HERE / "g2_status_browser_test.js"), str(current)]
        subprocess.run([args.node, "--check", str(current)], check=True)
        if args.baseline:
            source = (args.baseline / "WebPage_Bluetooth.h"
                      if args.baseline.is_dir() else args.baseline)
            baseline = Path(tmp) / "baseline.js"
            baseline.write_text(browser_functions(source, baseline=True))
            cmd.append(str(baseline))
        subprocess.run(cmd, check=True)
        page = Path(tmp) / "page.json"
        page.write_text(json.dumps(browser_page(COMPONENT / "WebPage_Bluetooth.h")))
        subprocess.run([args.node, str(HERE / "bluetooth_controls_browser_test.js"),
                        str(page)], check=True)


if __name__ == "__main__":
    main()
