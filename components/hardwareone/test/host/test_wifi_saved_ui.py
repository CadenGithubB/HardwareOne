#!/usr/bin/env python3
"""Exercise production saved-network menu selection with host UI substitutes.

The owner is faked as a changing bounded snapshot source. Production menu,
selection, and command-submission code must retain the displayed entry identity
across those changes. Owner storage/JSON behavior is tested separately.
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def declarations(source, names):
    return "\n".join(
        re.search(r"(?m)^(?:EXT_RAM_BSS_ATTR )?static[^\n]*\b" + name +
                  r"\b[^\n]*;[^\n]*$", source)[0]
        for name in names
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")

    oled = (COMPONENT / "OLED_Mode_Network.cpp").read_text()
    g2 = (COMPONENT / "G2_Page_Network.cpp").read_text()
    header = (COMPONENT / "System_WiFiNetworks.h").read_text()
    types = "\n".join(extract_block(header, marker) + ";" for marker in [
        "struct SavedWifiNetwork", "struct WifiNetworkSnapshot",
    ]) + "\n" + extract_block(g2, "enum NetworkSubMode") + ";"
    state = declarations(oled, [
        "sSavedNetScroll", "sSavedNetInitialized", "sSavedNetSnapshot",
        "sSavedNetSnapshotValid", "sPendingRemoveId", "sRemovePromptBuf",
    ]) + "\n" + declarations(g2, [
        "gNetSub", "gWifiSavedSel", "gWifiSavedSnapshot",
    ])
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


if __name__ == "__main__":
    main()
