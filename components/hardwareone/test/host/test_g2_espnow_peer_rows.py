#!/usr/bin/env python3
"""The lens "Peers" rows count and list this device's peers, never this device.

A peer is every ESP-NOW registry entry except this device's own. Its state is
the one every surface shows (System_ESPNow_PeerStateCore.h, owner decision
2026-11-20): online with a mesh heartbeat within 30 s AND an ACTIVE encrypted
session, connecting with the heartbeat alone, offline without a recent
heartbeat. The lens prints online/paired as "up" and the connecting peers
after it ("ESPNow: 2/3 up +1 conn"); it used to count a session alone as up.
The ESP-NOW app and the Network page once printed the registry size (one high:
the registry holds this device too) or the number of peers with message
history, and both peer lists had a row for this device (in the app, a row
whose detail page offered Ping, Send and Forget on this device itself).

Part one compiles the production g2EspNowPeerCounts, g2FormatPeerCounts,
g2BuildNetworkInfo and showEspNowDevices (G2_Page_Network.cpp), the row table,
g2BuildEspNowAppInfo, showMainMenu, showPeersMenu, showStatsMenu and
handlePeersTap (G2_Page_ESPNow.cpp) and the accessors espnowPeerState /
espnowPeerCounts (System_ESPNow.h) against doubles of the registry, the
heartbeat table, the session test and the lens list page
(g2_espnow_peer_rows_harness.cpp): the count rule (connecting is never
online, a session without a heartbeat is offline, this device and unpaired
peers are not counted), the lens wording, the rows that print it, the two
lists without this device, and a tap selecting the peer its row named, also
after the registry changed under the list. It is built again with
ENABLE_ESPNOW=0, where no registry double exists.

Part two reads sources (comments stripped), for what part one cannot run and
for the other screen rules that landed with it:
- neither lens page reads peerHistoryCount, and every "Peers" row prints the
  g2FormatPeerCounts text;
- the lens status line (G2_Glasses.cpp) prints g2FormatPeerCounts of
  g2EspNowPeerCounts, and no G2_*.cpp file counts peers by espnowSessionUp or
  the heartbeat table itself (the rule is espnowPeerCounts);
- g2EspNowPeerCounts sits outside the two blocks of G2_Page_Network.cpp that
  the Network host suites lift as local state (they have no registry);
- the lens RTC row and RTC readout convert with rtcLocalTime (screens show
  local time; the chip keeps UTC);
- the OLED G2 status screen draws from g2ConnectionStatusSnapshot, not from
  the `g2status` line;
- the OLED ESP-NOW name and metadata editors take their limits from
  devicename::kMaxLen and the metadata frame's fields, not from numbers;
- the OLED ESP-NOW device list, mesh status and bond picker do not query the
  radio driver for this device's address on a repaint (espnowSelfStaMac is
  cached while ESP-NOW runs).

--component points the suite at another component directory.
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

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

LEXEMES = re.compile(r'''
    //[^\n]*                          # line comment
  | /\*.*?\*/                         # block comment
  | "(?:\\.|[^"\\\n])*"               # string literal
  | '(?:\\.|[^'\\\n])*'               # character literal
''', re.S | re.X)


def code_of(text: str) -> str:
    """The text without comments (string and character literals kept)."""
    return LEXEMES.sub(lambda m: " " if m[0].startswith(("//", "/*")) else m[0], text)


def definition(source: str, name: str) -> str:
    """The first definition of `name` (forward declarations skipped)."""
    pattern = r"(?m)^(?:static[ \t]+)?(?:inline[ \t]+)?[A-Za-z_][\w:<>* \t&]*?\b" + re.escape(name) + r"\("
    for match in re.finditer(pattern, source):
        brace = source.find("{", match.end())
        semicolon = source.find(";", match.end())
        if brace != -1 and (semicolon == -1 or brace < semicolon):
            return extract_block(source[match.start():], match.group(0))
    raise AssertionError(f"production definition not found: {name}")


def line_of(source: str, pattern: str) -> str:
    match = re.search(pattern, source, re.M)
    require(match is not None, f"production line not found: {pattern}")
    return match[0]


def unit(component: Path) -> str:
    network = (component / "G2_Page_Network.cpp").read_text()
    app = (component / "G2_Page_ESPNow.cpp").read_text()
    espnow = (component / "System_ESPNow.h").read_text()
    production = [
        extract_block(app, "enum EspNowAppSub") + ";",
        line_of(app, r"^static EspNowAppSub gSub = [^;\n]*;"),
        definition(app, "setSub"),
        definition(app, "espnBackRow"),
        line_of(app, r"^static int gSelectedPeer = -1;"),
        "#if ENABLE_ESPNOW",
        line_of(app, r"^static EXT_RAM_BSS_ATTR uint8_t gPeerRowMac\[16\]\[6\];"),
        line_of(app, r"^static uint8_t gPeerRowCount = 0;"),
        "#endif",
        extract_block(app, "enum class EspNowAppPhase") + ";",
        definition(app, "currentPhase"),
        definition(app, "phaseLabel"),
        "#if ENABLE_ESPNOW",
        extract_block(espnow, "inline EspNowPeerState espnowPeerState("),
        extract_block(espnow, "inline EspNowPeerCounts espnowPeerCounts("),
        "#endif",
        definition(network, "g2EspNowPeerCounts"),
        definition(network, "g2FormatPeerCounts"),
        definition(network, "g2BuildNetworkInfo"),
        definition(network, "showEspNowDevices"),
        definition(app, "g2BuildEspNowAppInfo"),
        definition(app, "showMainMenu"),
        definition(app, "showPeersMenu"),
        definition(app, "showStatsMenu"),
        definition(app, "handlePeersTap"),
    ]
    harness = (HERE / "g2_espnow_peer_rows_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "missing/duplicate marker: // INSERT_PRODUCTION")
    return harness.replace("// INSERT_PRODUCTION", "\n\n".join(production))


def source_failures(component: Path) -> list[str]:
    failures: list[str] = []
    network_text = (component / "G2_Page_Network.cpp").read_text()
    pages = {"G2_Page_Network.cpp": network_text,
             "G2_Page_ESPNow.cpp": (component / "G2_Page_ESPNow.cpp").read_text()}
    wanted = {"Peers: %s\\n", "Peers: %s", "Peers %s\\n", "Peers (%s) >>"}
    seen = set()
    for name, text in pages.items():
        code = code_of(text)
        if "peerHistoryCount" in code:
            failures.append(f"{name} reads peerHistoryCount (peers with message history, not peers)")
        for literal in re.findall(r'"(Peers[^"\n]*%[^"\n]*)"', code):
            if literal in wanted:
                seen.add(literal)
            else:
                failures.append(f'{name} prints "{literal}": a Peers row prints the g2FormatPeerCounts text')
    if seen != wanted:
        failures.append(f"lens Peers rows found: {sorted(seen)}; expected the five rows printing "
                        "g2FormatPeerCounts")
    glasses = code_of((component / "G2_Glasses.cpp").read_text())
    status = re.search(r'"ESPNow: (%[^"\n]*)\\n"', glasses)
    if status is None or status[1] != "%s" or \
            "g2FormatPeerCounts(g2EspNowPeerCounts()" not in glasses:
        failures.append("G2_Glasses.cpp: the status line does not print g2FormatPeerCounts of "
                        "g2EspNowPeerCounts() (\"ESPNow: %s\")")
    for path in sorted(component.glob("G2_*.cpp")):
        code = code_of(path.read_text())
        if re.search(r"\bespnowSessionUp\(|\bisMeshPeerAlive\(|\bmeshPeerLinkOf\(|\bgMeshPeers\b", code):
            failures.append(f"{path.name} reads the session or heartbeat table; a peer's state comes "
                            "from espnowPeerState / espnowPeerCounts")
    # The two blocks the Network host suites lift as the page's local state.
    lifted = (network_text[network_text.index("enum NetworkSubMode"):network_text.index("// Info text")] +
              network_text[network_text.index("enum WifiMenuAction"):
                           network_text.index("static void showWifiMenu() {")])
    for name in ("g2EspNowPeerCounts", "g2FormatPeerCounts", "espnowPeerCounts", "espnowSessionUp",
                 "isSelfMac"):
        if name in code_of(lifted):
            failures.append(f"G2_Page_Network.cpp: {name} is inside the local-state block the Network "
                            "host suites lift (they have no ESP-NOW registry)")

    sensors = (component / "G2_Page_Sensors.cpp").read_text()
    for name in ("rtcG2FormatValue", "g2BuildRtcReadout"):
        body = code_of(definition(sensors, name))
        if body.count("rtcLocalTime(&rtc.dateTime)") != 1 or re.search(r"rtc\.dateTime\s*[.;]", body):
            failures.append(f"G2_Page_Sensors.cpp {name} does not show the RTC through rtcLocalTime "
                            "(screens show local time)")

    bluetooth = (component / "OLED_Mode_Bluetooth.cpp").read_text()
    detail = code_of(definition(bluetooth, "displayG2StatusDetail"))
    if "getG2Status(" in code_of(bluetooth) or detail.count("g2ConnectionStatusSnapshot(") != 1:
        failures.append("OLED_Mode_Bluetooth.cpp: the G2 status screen draws its rows from one "
                        "g2ConnectionStatusSnapshot, not from the g2status line")

    oled = (component / "OLED_ESPNow.cpp").read_text()
    settings = code_of(definition(oled, "oledEspNowHandleSettingsInput"))
    numbers = sorted(int(n) for n in re.findall(r"\bmaxLen\s*=\s*(\d+)\s*;", settings))
    if numbers != [17, 17, 32, 32]:  # the default, the passphrase and the two MAC fields
        failures.append(f"OLED_ESPNow.cpp settings editors: maxLen literals {numbers}; the name and "
                        "metadata editors take theirs from devicename::kMaxLen / V4PayloadMetadata")
    for limit in ("devicename::kMaxLen", "sizeof(V4PayloadMetadata::room) - 1",
                  "sizeof(V4PayloadMetadata::zone) - 1", "sizeof(V4PayloadMetadata::friendlyName) - 1",
                  "sizeof(V4PayloadMetadata::tags) - 1"):
        if not re.search(r"maxLen\s*=\s*\(int\)\(?" + re.escape(limit) + r"\)?\s*;", settings):
            failures.append(f"OLED_ESPNow.cpp settings editors: no editor is limited by {limit}")
    if "Pretty" in code_of(oled):
        failures.append("OLED_ESPNow.cpp: the friendly name is called 'Friendly Name' on every screen")

    for name in ("OLED_ESPNow.cpp", "OLED_Mode_Network.cpp", "OLED_Mode_Remote.cpp"):
        if "esp_wifi_get_mac(" in code_of((component / name).read_text()):
            failures.append(f"{name} queries the radio driver for this device's address on a screen "
                            "path (use espnowSelfStaMac, cached while ESP-NOW runs)")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    # Without ESP-NOW the lifted page state (gSelectedPeer) has no reader left in this unit.
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function",
             "-Wno-unused-variable", "-I", str(args.component)]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                  "-fno-omit-frame-pointer", "-g"]
    total = 0
    with tempfile.TemporaryDirectory(prefix="hw1-g2-peer-rows-") as directory:
        work = Path(directory)
        source = work / "peer_rows.cpp"
        source.write_text(unit(args.component))
        for espnow in (1, 0):
            binary = work / f"peer-rows-{espnow}"
            subprocess.run([args.cxx, *flags, f"-DENABLE_ESPNOW={espnow}", str(source), "-o", str(binary)],
                           check=True, timeout=120)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            if result.returncode != 0:
                sys.stdout.write(result.stderr)
                print(f"g2_espnow_peer_rows: ENABLE_ESPNOW={espnow} failed")
                return 1
            total += int(result.stdout.strip())

    failures = source_failures(args.component)
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        print(f"g2_espnow_peer_rows: {len(failures)} source failure(s)")
        return 1
    print(f"g2_espnow_peer_rows: online/connecting/paired counts, both peer lists without this device, taps by the "
          f"row's peer ({total} checks, 2 builds); screen source rules hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
