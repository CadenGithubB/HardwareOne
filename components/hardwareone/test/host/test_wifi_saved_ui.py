#!/usr/bin/env python3
"""Exercise production saved-network menu selection with host UI substitutes.

The owner is faked as a changing bounded snapshot source. Production menu,
selection, and command-submission code must retain the displayed entry identity
across those changes. Owner storage/JSON behavior is tested separately.

A second unit compiles the G2 Network page's row bindings, its WiFi, ESP-NOW,
HTTP(S), Bluetooth, G2, R1 and Phone menus and their tap handlers: a tap must run the
action and toggle direction of the row the lens showed, even when the live
state behind the labels changed in between. Each of its cases runs in its own
process; the ENABLE_ESPNOW / ENABLE_HTTP_SERVER / ENABLE_WIFI /
ENABLE_BLE_PHONE_PIPE off variants are compile-checked.

A third unit compiles the Add-WiFi forms: the OLED keyboard flow (manual Add and
scan pick, with OLED_Utils.cpp's shared reply-to-toast mapping) and the G2
join-a-scanned-AP flow. A double-quote or control character in the typed
password or a scanned SSID must be refused before the quoted `wifiadd` line is
built, with a visible message, and the owner's reply must reach the OLED toast /
G2 lens. The field rule and the line come from the saved-network owner's
structured add (System_WiFiNetworks.cpp wifiNetworkFieldProblem and
wifiAddCommandLine, compiled unchanged); a contract checks that neither form
writes the line or keeps a byte rule of its own.
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from test_g2_status_corner import link_status_unit
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# G2 Network functions under test, in source order; located by name so a
# changed signature still matches, skipping forward declarations.
NETWORK_ROW_FUNCTIONS = (
    "bluetoothClientRoleForUi", "showWiFiMenu", "showEspNowMenu",
    "showBluetoothG2Menu", "showBluetoothR1Menu", "showBluetoothPhoneMenu", "showBluetoothMenu",
    "handleWiFiTap", "espnowNameCommit", "espnowNameCancel",
    "beginEspNowNameEntry", "espnowNameThenOpenCommit", "handleEspNowTap",
    "bluetoothToggleAutoStart", "handleBluetoothG2Tap", "handleBluetoothR1Tap",
    "handleBluetoothPhoneTap", "handleBluetoothTap", "showHttpMenu", "handleHttpTap",
)
# Add-WiFi form functions; the helpers exist only on fixed trees, so a base tree
# still compiles and fails on behavior. Spliced in source order.
OLED_ADD_FUNCTIONS = ("startWifiAddSSID", "startWifiAddPass", "wifiAddReset",
                      "startWifiAddFromScan", "wifiAddKeyboardTick")
OLED_ADD_HELPERS = ("wifiAddFieldRefusal",)
# The shared reply-to-toast mapping the OLED flow toasts the owner's reply with.
OLED_REPLY_TEXT = "void oledCommandReplyText("
G2_ADD_FUNCTIONS = ("showWifiAddMsg", "onWifiAddDone", "wifiAddSubmit",
                    "wifiAddPassCommit", "wifiAddPassCancel", "handleWiFiScanTap")
G2_ADD_HELPERS = ("wifiAddTextQuotable", "showWifiAddRefusal")
DISABLED_VARIANTS = (
    ["-DENABLE_ESPNOW=0"],
    ["-DENABLE_HTTP_SERVER=0"],
    ["-DENABLE_WIFI=0", "-DENABLE_ESPNOW=0", "-DENABLE_HTTP_SERVER=0"],
    ["-DENABLE_BLE_PHONE_PIPE=0"],
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


def definition_match(source, name):
    """First definition of `name` (forward declarations skipped), or None."""
    pattern = r"(?m)^(?:static[ \t]+)?[A-Za-z_][\w:<>* \t&]*?\b" + re.escape(name) + r"\("
    for match in re.finditer(pattern, source):
        brace = source.find("{", match.end())
        semicolon = source.find(";", match.end())
        if brace != -1 and (semicolon == -1 or brace < semicolon):
            return match
    return None


def definition(source, name):
    match = definition_match(source, name)
    if match is None:
        raise AssertionError(f"production definition not found: {name}")
    return extract_block(source[match.start():], match.group(0))


def definitions_in_order(source, required, optional=()):
    """Required and present optional definitions, spliced in source order."""
    blocks = {name: definition(source, name) for name in required}
    blocks.update((name, definition(source, name)) for name in optional
                  if definition_match(source, name))
    order = sorted(blocks, key=lambda name: definition_match(source, name).start())
    return "\n\n".join(blocks[name] for name in order)


def g2_bridge_types(component):
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
    return types


def network_rows_unit(component):
    page = (component / "G2_Page_Network.cpp").read_text()
    # The Bluetooth and Phone menus' words come from bleLinkStatus(), compiled
    # as shipped over the harness's role inputs and fake pipe/peer/console.
    link_types, link_status = link_status_unit(component)
    types = g2_bridge_types(component) + [link_types]
    # Local state (sub-mode, overlays, row bindings) and WiFi's action enum.
    state = (region(page, "enum NetworkSubMode", "// Info text") + "\n" +
             region(page, "enum WifiMenuAction", "static void showWiFiMenu() {"))
    functions = "\n\n".join([link_status] +
                            [definition(page, name) for name in NETWORK_ROW_FUNCTIONS])
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


def wifi_field_owner(component):
    """The owner's field rule and structured add line (fixed trees only)."""
    header = (component / "network/wifi/System_WiFiNetworks.h").read_text()
    if "enum class WifiFieldProblem" not in header:
        return ""
    source = (component / "network/wifi/System_WiFiNetworks.cpp").read_text()
    return "\n\n".join([extract_block(header, "enum class WifiFieldProblem") + ";",
                        extract_block(source, "WifiFieldProblem wifiNetworkFieldProblem("),
                        extract_block(source, "WifiFieldProblem wifiAddCommandLine(")])


def add_form_contract(component):
    """Both forms leave the field rule and the line format to the owner."""
    failures = []
    for name, functions in (("OLED_Mode_Network.cpp", OLED_ADD_FUNCTIONS + OLED_ADD_HELPERS),
                            ("G2_Page_Network.cpp", G2_ADD_FUNCTIONS + G2_ADD_HELPERS)):
        source = (component / name).read_text()
        code = "\n".join(definition(source, function) for function in functions
                         if definition_match(source, function))
        code = re.sub(r"/\*.*?\*/|//[^\n]*", "", code, flags=re.S)
        if '"wifiadd \\"' in code or "wifiAddCommandLine(" not in code:
            failures.append(f"{name} writes the wifiadd line itself instead of wifiAddCommandLine")
        if re.search(r"0x7F|< *0x20|'\"'", code):
            failures.append(f"{name} keeps its own field byte rule instead of wifiNetworkFieldProblem")
    if failures:
        raise SystemExit("Add-WiFi forms: " + "; ".join(failures))


def add_form_unit(component):
    oled = (component / "OLED_Mode_Network.cpp").read_text()
    page = (component / "G2_Page_Network.cpp").read_text()
    utils = (component / "OLED_Utils.cpp").read_text()
    inserts = (
        ("// INSERT_WIFI_FIELD_OWNER", wifi_field_owner(component)),
        ("// INSERT_OLED_REPLY_TEXT",
         extract_block(utils, OLED_REPLY_TEXT) if OLED_REPLY_TEXT in utils else ""),
        ("// INSERT_OLED_STATE", extract_block(oled, "enum WifiAddStep") + ";\n" + declarations(oled, [
            "sWifiAddStep", "sWifiKeyboardActive", "sWifiAddConnectAfter",
            "sWifiAddSSID", "sWifiAddPass", "sWifiAddEpoch"])),
        ("// INSERT_OLED_FUNCTIONS", definitions_in_order(oled, OLED_ADD_FUNCTIONS, OLED_ADD_HELPERS)),
        ("// INSERT_G2_TYPES", "\n".join(g2_bridge_types(component) + [
            extract_block(page, "enum NetworkSubMode") + ";",
            extract_block(page, "struct CachedAp") + ";"])),
        ("// INSERT_G2_STATE", declarations(page, [
            "gNetSub", "gScanCache", "gScanCacheCount", "sWifiAddSsid", "sWifiAddMsg"])),
        ("// INSERT_G2_FUNCTIONS", definitions_in_order(page, G2_ADD_FUNCTIONS, G2_ADD_HELPERS)),
    )
    harness = (HERE / "wifi_add_form_harness.cpp").read_text()
    for marker, body in inserts:
        assert harness.count(marker) == 1, f"missing/duplicate marker: {marker}"
        harness = harness.replace(marker, body)
    return harness


def run_add_form(args, component, temp, flags):
    unit = Path(temp) / "add_form.cpp"
    unit.write_text(add_form_unit(component))
    executable = Path(temp) / "add_form"
    subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                   check=True, timeout=120)
    run = subprocess.run([str(executable)], timeout=60, capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    sys.stdout.write(run.stderr)
    sys.stdout.flush()
    if run.returncode != 0:
        raise SystemExit("Add-WiFi forms: FAILED")
    add_form_contract(component)


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
        # Both units always run, so a tree that fails one still reports the other.
        failures = []
        for unit in (run_network_rows, run_add_form):
            try:
                unit(args, component, temp, flags)
            except SystemExit as failure:
                failures.append(str(failure))
        if failures:
            raise SystemExit("; ".join(failures))


if __name__ == "__main__":
    main()
