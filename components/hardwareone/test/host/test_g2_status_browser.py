#!/usr/bin/env python3
"""Exercise the shipping Bluetooth page's status rendering and controls in Node.

Only browser boundaries are mocked. An optional --baseline header compares
successful rendering against saved production, including the former text parser.
The dashboard's Bluetooth card (test_dashboard_bluetooth_browser.py) runs too.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
FUNCTIONS = (
    "el", "setText", "applyMode", "applyState", "stateFromInfo",
    "g2OverallState", "renderG2", "renderRing", "esc", "renderConnections",
    "showStatusText", "refresh",
)
# The link status (role pill, saved mode, role switch, app console row);
# optional only in a --baseline header that predates them.
LINK_FUNCTIONS = ("viewMode", "renderRoleSwitch", "renderLink")


# BT_PHONE_JS(R"JS(...)JS") splices phone relay pipe script into the page's raw
# literal only in ENABLE_BLE_PHONE_PIPE builds; elsewhere it expands to "".
PHONE_FRAGMENT = re.compile(r'\)JS"\s*BT_PHONE_JS\(R"JS\((.*?)\)JS"\)\s*R"JS\(', re.DOTALL)
# Adjacent raw literals (`)JS"` then `R"JS(`) are one string, as C++ concatenates them.
LITERAL_SPLIT = re.compile(r'\)JS"\s*R"JS\(')


def expand_phone(source: str, phone: bool) -> str:
    expanded = PHONE_FRAGMENT.sub(lambda match: match.group(1) if phone else "", source)
    return LITERAL_SPLIT.sub("", expanded)


def browser_functions(header: Path, baseline: bool = False) -> str:
    source = expand_phone(header.read_text(), phone=False)
    names = FUNCTIONS
    if baseline and re.search(r"^  function parseG2Status\(", source, re.MULTILINE):
        names += ("parseG2Status",)
    if re.search(r"^  function updateControlAvailability\(", source, re.MULTILINE):
        names += ("updateControlAvailability",)
    for name in LINK_FUNCTIONS:
        if not baseline or re.search(rf"^  function {name}\(", source, re.MULTILINE):
            names += (name,)
    return "\n\n".join(page_function(source, name) for name in names)


def page_function(source: str, name: str) -> str:
    # These functions are top-level members of the page IIFE. Use their
    # indentation boundary rather than treating JS regexes as C++ literals.
    marker = re.search(rf"^  function {name}\([^\n]*", source, re.MULTILINE)
    if not marker:
        raise AssertionError(f"missing production function: {name}")
    if marker.group().rstrip().endswith("}"):
        return marker.group()
    closing = re.search(r"^  }$", source[marker.end():], re.MULTILINE)
    if not closing:
        raise AssertionError(f"unterminated production function: {name}")
    return source[marker.start():marker.end() + closing.end()]


def core_strings(core: str, function: str, enum: str) -> dict:
    """Enumerator -> string of one BLE_LinkStatusCore.h switch, its default included."""
    body = re.search(rf"^inline const char\* {function}\({enum} \w\) {{\n(.*?)\n}}",
                     core, re.MULTILINE | re.DOTALL)
    if not body:
        raise AssertionError(f"missing hw1link::{function}")
    table = dict(re.findall(rf'case {enum}::(\w+): return "([^"]*)";', body.group(1)))
    fallback = re.search(r'^  return "([^"]*)";$', body.group(1), re.MULTILINE)
    for name in re.findall(rf"case {enum}::(\w+): break;", body.group(1)):
        table[name] = fallback.group(1)
    return table


def check_saved_text(script: str, reader: str, where: str, component: Path) -> None:
    """The saved mode's name is the device's savedText (bleLinkStatusJson writes
    hw1link::roleText of the saved role), shown as is: no page keeps its own
    table of role ids to names."""
    writer = (component / "BLE_LinkStatus.cpp").read_text()
    if not re.search(r'out\["savedText"\]\s*=\s*hw1link::roleText\(s\.saved\);', writer):
        raise AssertionError("bleLinkStatusJson does not write savedText as hw1link::roleText")
    if reader not in script:
        raise AssertionError(f"{where} does not show {reader}")
    ids = core_strings((component / "BLE_LinkStatusCore.h").read_text(), "roleId", "Role")
    alternatives = "|".join(re.escape(role) for role in sorted(set(ids.values())))
    table = re.search(rf"'(?:{alternatives})'\s*:\s*'", script)
    if table:
        raise AssertionError(f"{where} keeps a role-id-to-text table: {table.group()}")


def check_link_words(header: Path, core: Path) -> None:
    """The page names roles and console states by the ids BLE_LinkStatusCore.h
    publishes and shows the saved mode as the device's savedText (Node not needed)."""
    source = expand_phone(header.read_text(), phone=True)
    words = core.read_text()
    role_id = core_strings(words, "roleId", "Role")
    for name in ("viewMode", "renderRoleSwitch"):
        roles = set(re.findall(r"role [!=]== '([\w-]+)'", page_function(source, name)))
        if not roles or not roles <= set(role_id.values()):
            raise AssertionError(f"{name} names unknown roles: {roles - set(role_id.values())}")
    check_saved_text(source, "link.savedText", "Bluetooth page", header.parent)
    if "link.savedText" not in page_function(source, "renderLink"):
        raise AssertionError("renderLink does not show link.savedText")
    consoles = {**core_strings(words, "consoleText", "Console"),
                **dict(re.findall(r'case Console::(\w+): return "([^"]*)";',
                                  re.search(r"consoleId\(Console c\) \{(.*?)\n\}", words,
                                            re.DOTALL).group(1)))}
    named = set(re.findall(r"link\.console !== '([\w-]+)'", page_function(source, "renderLink")))
    if named != {consoles["NotHere"], consoles["Off"]}:
        raise AssertionError(f"renderLink console ids {named} are not not-here/off")
    print("Bluetooth page link words match BLE_LinkStatusCore.h")


def browser_page(header: Path) -> dict:
    """Extract the entire shipping IIFE and the corresponding static markup."""
    raw = header.read_text()
    if "BT_PHONE_JS(" in raw and not PHONE_FRAGMENT.search(raw):
        raise AssertionError("unrecognized BT_PHONE_JS fragment layout")
    def script(phone: bool) -> str:
        found = re.search(r'R"JS\(\s*<script>(.*?)</script>\s*\)JS"',
                          expand_phone(raw, phone), re.DOTALL)
        if not found:
            raise AssertionError("missing production Bluetooth page script")
        return found.group(1)
    source = raw
    content = source[source.index("inline void streamBluetoothInner"):source.index("  // JavaScript")]
    def markup(g2: bool, sensors: bool = True, microphone: bool = True,
               phone: bool = False) -> str:
        features = {"ENABLE_BLUETOOTH": True, "ENABLE_G2_GLASSES": g2,
                    "ENABLE_WEB_SENSORS": sensors, "ENABLE_MICROPHONE": microphone,
                    "ENABLE_BLE_PHONE_PIPE": phone}
        active = [True]
        lines = []
        for line in content.splitlines():
            directive = line.strip()
            if directive.startswith("#if "):
                # "A && !B" or "A || B": && groups joined by ||.
                condition = directive[4:]
                groups = [[term.strip() for term in group.split("&&")]
                          for group in condition.split("||")]
                if any(term.lstrip("!") not in features for group in groups for term in group):
                    raise AssertionError(f"unhandled page feature guard: {condition}")
                value = any(all(not features[term[1:]] if term.startswith("!")
                                else features[term] for term in group) for group in groups)
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
        "script": script(False),
        "phoneScript": script(True),
        "markup": {str(enabled).lower(): markup(enabled) for enabled in (False, True)},
        # Phone relay pipe builds, with and without the glasses (p4-phone-only).
        "phoneMarkup": {str(enabled).lower(): markup(enabled, phone=True)
                        for enabled in (False, True)},
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
    check_link_words(COMPONENT / "WebPage_Bluetooth.h", COMPONENT / "BLE_LinkStatusCore.h")
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
    # The dashboard's Bluetooth card shows the same link words; run it with the
    # page it summarizes so this registered suite covers both.
    subprocess.run([sys.executable, str(HERE / "test_dashboard_bluetooth_browser.py"),
                    "--node", args.node], check=True)


if __name__ == "__main__":
    main()
