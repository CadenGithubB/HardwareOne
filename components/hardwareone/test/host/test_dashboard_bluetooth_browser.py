#!/usr/bin/env python3
"""Run the shipping dashboard Bluetooth card in Node against `status json`.

The card's markup and its two update scripts (the connectivity script and the
relay pipe script of ENABLE_BLE_PHONE_PIPE builds) are extracted from
WebPage_Dashboard.h; only the DOM and the dashboard's Dash/hw helpers are fake.
Without Node, the card is checked to show the device's savedText and to keep no
table of role names of its own.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_g2_status_browser import check_saved_text

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
CHUNK = re.compile(r"httpd_resp_send_chunk\(req,(.*?),\s*HTTPD_RESP_USE_STRLEN\);", re.DOTALL)
LITERAL = re.compile(r'"(?:\\.|[^"\\])*"')
PIPE_GUARD = "#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE"


def literals(chunk: str, phone: bool) -> str:
    """Concatenated C string literals of one chunk, after its #if lines."""
    active = [True]
    parts = []
    for line in chunk.splitlines():
        directive = line.strip()
        if directive.startswith("#if "):
            if directive[4:] != "ENABLE_BLE_PHONE_PIPE":
                raise AssertionError(f"unhandled card feature guard: {directive}")
            active.append(active[-1] and phone)
        elif directive.startswith("#endif"):
            active.pop()
        elif active[-1] and not directive.startswith("//"):
            parts += [json.loads(match) for match in LITERAL.findall(line)]
    return "".join(parts)


def one_chunk(source: str, marker: str) -> re.Match:
    found = [match for match in CHUNK.finditer(source) if marker in match.group(1)]
    if len(found) != 1:
        raise AssertionError(f"expected one dashboard chunk with {marker}, found {len(found)}")
    return found[0]


def script(chunk: str, phone: bool) -> str:
    text = literals(chunk, phone)
    if not (text.startswith("<script>") and text.endswith("</script>")):
        raise AssertionError("dashboard update chunk is not one script")
    return text[len("<script>"):-len("</script>")]


def dashboard(header: Path) -> dict:
    source = header.read_text()
    card = one_chunk(source, "id='conn-bt-card'").group(1)
    connectivity = one_chunk(source, "_origUpdateSystem").group(1)
    pipe = one_chunk(source, "_phoneUpdateSystem")
    directives = [line for line in source[:pipe.start()].splitlines() if line.startswith("#")]
    if directives[-1] != PIPE_GUARD:
        raise AssertionError("relay pipe script must sit under " + PIPE_GUARD)
    return {
        "markup": literals(card, phone=False),
        "phoneMarkup": literals(card, phone=True),
        "connectivity": script(connectivity, phone=False),
        "pipe": script(pipe.group(1), phone=True),
    }


def check_role_words(page: dict, component: Path) -> None:
    check_saved_text(page["connectivity"] + page["pipe"], "bt.savedText", "dashboard card", component)
    print("Dashboard Bluetooth card shows the device's savedText")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node", default=os.environ.get("NODE") or shutil.which("node"))
    args = parser.parse_args()
    page = dashboard(COMPONENT / "WebPage_Dashboard.h")
    check_role_words(page, COMPONENT)
    if not args.node:
        raise SystemExit("Pass --node /path/to/node for JavaScript behavior tests")
    with tempfile.TemporaryDirectory(prefix="hw1-dashboard-bt-browser-") as tmp:
        data = Path(tmp) / "dashboard.json"
        data.write_text(json.dumps(page))
        for name in ("connectivity", "pipe"):
            js = Path(tmp) / f"{name}.js"
            js.write_text(page[name])
            subprocess.run([args.node, "--check", str(js)], check=True)
        subprocess.run([args.node, str(HERE / "dashboard_bluetooth_browser_test.js"), str(data)],
                       check=True)


if __name__ == "__main__":
    main()
