#!/usr/bin/env python3
"""Exercise production saved-network menu selection with host UI substitutes.

The owner is faked as a changing bounded snapshot source. Production menu,
selection, and command-submission code must retain the displayed entry identity
across those changes. Owner storage/JSON behavior is tested separately.

A second unit compiles the G2 Network page's row bindings, its WiFi, ESP-NOW,
HTTP(S), Bluetooth, G2 and R1 menus and their tap handlers: a tap must run the
action and toggle direction of the row the lens showed, even when the live
state behind the labels changed in between. Each of its cases runs in its own
process; the ENABLE_ESPNOW / ENABLE_HTTP_SERVER / ENABLE_WIFI off variants are
compile-checked.
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# G2 Network functions under test, in source order; located by name so a
# changed signature still matches, skipping forward declarations.
NETWORK_ROW_FUNCTIONS = (
    "bluetoothClientRoleForUi", "showWiFiMenu", "showEspNowMenu",
    "showBluetoothG2Menu", "showBluetoothR1Menu", "showBluetoothMenu",
    "handleWiFiTap", "espnowNameCommit", "espnowNameCancel",
    "beginEspNowNameEntry", "espnowNameThenOpenCommit", "handleEspNowTap",
    "bluetoothToggleAutoStart", "handleBluetoothG2Tap", "handleBluetoothR1Tap",
    "handleBluetoothTap", "showHttpMenu", "handleHttpTap",
)
DISABLED_VARIANTS = (
    ["-DENABLE_ESPNOW=0"],
    ["-DENABLE_HTTP_SERVER=0"],
    ["-DENABLE_WIFI=0", "-DENABLE_ESPNOW=0", "-DENABLE_HTTP_SERVER=0"],
)


def declarations(source, names):
    return "\n".join(
        re.search(r"(?m)^(?:EXT_RAM_BSS_ATTR )?static[^\n]*\b" + name +
                  r"\b[^\n]*;[^\n]*$", source)[0]
        for name in names
    )


def statement(source, marker):
    start = source.index(marker)
    return source[start:source.index(";", start) + 1]


def region(source, begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]


def definition(source, name):
    pattern = r"(?m)^(?:static[ \t]+)?[A-Za-z_][\w:<>* \t&]*?\b" + re.escape(name) + r"\("
    for match in re.finditer(pattern, source):
        brace = source.find("{", match.end())
        semicolon = source.find(";", match.end())
        if brace != -1 and (semicolon == -1 or brace < semicolon):
            return extract_block(source[match.start():], match.group(0))
    raise AssertionError(f"production definition not found: {name}")


def network_rows_unit(component):
    page = (component / "G2_Page_Network.cpp").read_text()
    glasses = (component / "G2_Glasses.h").read_text()
    bridge = (component / "G2_HijackCmd.h").read_text()
    entry = (component / "G2_Page_TextEntry.h").read_text()
    types = [
        extract_block(glasses, "enum G2HijackPage") + ";",
        extract_block(bridge, "struct G2CmdCookie") + ";",
        statement(bridge, "typedef void (*G2HijackCmdCallback)"),
    ] + [statement(entry, f"typedef void (*TextEntry{kind}Fn)")
         for kind in ("Commit", "Cancel", "Abandon")]
    types.append(extract_block(entry, "struct TextEntryConfig") + ";")
    # Local state (sub-mode, overlays, row bindings) and WiFi's action enum.
    state = (region(page, "enum NetworkSubMode", "// Info text") + "\n" +
             region(page, "enum WifiMenuAction", "static void showWiFiMenu() {"))
    functions = "\n\n".join(definition(page, name) for name in NETWORK_ROW_FUNCTIONS)
    harness = (HERE / "g2_network_rows_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n".join(types)),
                         ("// INSERT_STATE", state),
                         ("// INSERT_FUNCTIONS", functions)):
        assert harness.count(marker) == 1, f"missing/duplicate marker: {marker}"
        harness = harness.replace(marker, body)
    return harness


def run_network_rows(args, component, temp, flags):
    unit = Path(temp) / "network_rows.cpp"
    unit.write_text(network_rows_unit(component))
    failed = []
    for variant in DISABLED_VARIANTS:
        check = subprocess.run([args.cxx, *flags, "-fsyntax-only", *variant, str(unit)],
                               timeout=120, capture_output=True, text=True)
        if check.returncode != 0:
            failed.append(" ".join(variant))
            sys.stdout.write(f"compile {' '.join(variant)}: FAILED\n{check.stderr}")
    executable = Path(temp) / "network_rows"
    subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                   check=True, timeout=120)
    cases = subprocess.run([str(executable), "--list"], check=True, timeout=30,
                           capture_output=True, text=True).stdout.split()
    for case in cases:
        run = subprocess.run([str(executable), case], timeout=60,
                             capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        if run.returncode != 0:
            failed.append(case)
            sys.stdout.write(f"{case}: FAILED\n{run.stderr}")
    sys.stdout.flush()
    if failed:
        raise SystemExit(f"G2 Network row bindings: {len(failed)} failed: {', '.join(failed)}")
    print(f"G2 Network row bindings: {len(cases)} cases and "
          f"{len(DISABLED_VARIANTS)} disabled-feature builds passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="production component directory (e.g. a base tree)")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    component = args.component.resolve()

    oled = (component / "OLED_Mode_Network.cpp").read_text()
    g2 = (component / "G2_Page_Network.cpp").read_text()
    header = (component / "network/wifi/System_WiFiNetworks.h").read_text()
    types = "\n".join(extract_block(header, marker) + ";" for marker in [
        "struct SavedWifiNetwork", "struct WifiNetworkSnapshot",
    ]) + "\n" + extract_block(g2, "enum NetworkSubMode") + ";"
    state = declarations(oled, [
        "sSavedNetScroll", "sSavedNetInitialized", "sSavedNetSnapshot",
        "sSavedNetSnapshotValid", "sPendingRemoveId", "sRemovePromptBuf",
    ]) + "\n" + declarations(g2, [
        "gNetSub", "gWifiSavedSel", "gWifiSavedSnapshot",
    ] + (["gNetRowCount"]  # row bindings setNetSub clears (absent on older trees)
         if re.search(r"(?m)^static[^\n]*\bgNetRowCount\b", g2) else []))
    functions = "\n\n".join(extract_block(oled, marker) for marker in [
        "static void populateSavedNetScroll(",
        "static void savedNetworksOnEnter(",
        "static void displaySavedNetworksRendered(",
        "static bool networkWifiListInputHandler(",
        "static void onWifiRemoveConfirmed(",
        "static bool networkWifiRemoveInputHandler(",
        "void oledNetworkModeResetSessionState(",
    ]) + "\n\n" + "\n\n".join(extract_block(g2, marker) for marker in [
        "static inline void setNetSub(",
        "static void showWiFiSavedList(",
        "static void showWiFiSavedActionMenu(",
        "static void handleWiFiSavedTap(",
        "static void handleWiFiSavedActionTap(",
    ])
    # Actual entry hooks ensure direct OLED navigation captures the same model
    # as selecting the submenu; the tests invoke these production hooks below.
    for mode in ["OLED_NETWORK_WIFI_LIST", "OLED_NETWORK_WIFI_REMOVE"]:
        assert re.search(r"\{\s*" + mode + r"\b[^}]*savedNetworksOnEnter\s*\}", oled)
    assert "populateSavedNetScroll" not in extract_block(
        oled, "static void displaySavedNetworksRendered(")

    harness = (HERE / "wifi_saved_ui_harness.cpp").read_text()
    with tempfile.TemporaryDirectory(prefix="hw1-wifi-saved-ui-") as temp:
        unit = Path(temp) / "ui.cpp"
        unit.write_text(harness.replace("// INSERT_PRODUCTION_TYPES", types)
                       .replace("// INSERT_PRODUCTION_STATE", state)
                       .replace("// INSERT_PRODUCTION_FUNCTIONS", functions))
        executable = Path(temp) / "ui"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-Wno-unused-variable", str(unit),
                   "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=60)
        print("saved-network UI: production snapshot/selection cases passed")

        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-Wno-unused-function", "-Wno-unused-variable"]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        run_network_rows(args, component, temp, flags)


if __name__ == "__main__":
    main()
