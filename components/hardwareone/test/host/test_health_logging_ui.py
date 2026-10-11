#!/usr/bin/env python3
"""Exercise production Health logging request routing and web session delivery.

Shipping model actions, worker dispatch, OLED input, HTTP handler and unified
web executor are compiled with boundary mocks. Logger mutations and the G2
command bridge's authorization machinery have separate owner tests.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require, web_command_types

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generated(baseline: Path | None = None) -> str:
    health = (COMPONENT / "G2_Health.cpp").read_text()
    public = (COMPONENT / "G2_Health.h").read_text()
    glasses = (COMPONENT / "G2_Glasses.cpp").read_text()
    web = (COMPONENT / "WebPage_R1_Health.cpp").read_text()
    utils = (COMPONENT / "WebServer_Utils.h").read_text()
    command_path = COMPONENT / "System_Utils.cpp"
    if baseline is not None:
        command_path = baseline / "System_Utils.before.cpp" if baseline.is_dir() else baseline
    command = command_path.read_text()
    has_helper = "CommandContext makeWebCommandContext(" in command
    web_executor = []
    if has_helper:
        web_executor.append(extract_block(command, "CommandContext makeWebCommandContext("))
    web_executor.append(extract_block(command, "bool executeUnifiedWebCommand("))
    oled = (COMPONENT / "OLED_Mode_R1_Health.cpp").read_text()
    types = [web_command_types(), extract_block(public, "enum G2HealthMetric") + ";",
             extract_block(public, "enum G2HealthNav") + ";",
             extract_block((COMPONENT / "G2_Glasses.h").read_text(), "enum G2HijackPage") + ";",
             extract_block((COMPONENT / "G2_HijackCmd.h").read_text(), "struct G2CmdCookie") + ";",
             extract_block(oled, "enum HealthAction") + ";"]
    auth_start = utils.index("#define WEB_AUTH_OR_RETURN(req, ctx)")
    auth_end = utils.index("\n\n", auth_start)
    production = [utils[auth_start:auth_end],
                  extract_block(health, "void g2HealthOpen("),
                  extract_block(health, "void g2HealthAction("),
                  extract_block(health, "bool g2HealthConsumeLoggingToggleRequest("),
                  extract_block(health, "void g2HealthSetPageActive("),
                  extract_block(glasses, "static void onHealthLoggingCmdDone("),
                  "static void drainLoggingRequest() {\n" +
                  extract_block(glasses, "if (g2HealthConsumeLoggingToggleRequest())") + "\n}",
                  *web_executor,
                  extract_block(web, "static bool readHealthActionBody("),
                  extract_block(web, "static String healthFormValue("),
                  extract_block(web, "static esp_err_t healthActionReply("),
                  extract_block(web, "esp_err_t handleR1HealthAction("),
                  extract_block(oled, "static bool healthInputHandler(")]
    production.append("namespace disabled {\n" + extract_block(
        public, "inline bool g2HealthConsumeLoggingToggleRequest(") + "\n}")
    require("healthLoggingSet(" not in health and "healthLoggingSet(" not in web,
            "Health UI regained a direct logger mutation")
    require("gSensorLogMask" not in health, "Health model regained mutable mask access")
    harness = (HERE / "health_logging_ui_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_PRODUCTION", "\n\n".join(production))):
        require(harness.count(marker) == 1, f"bad harness marker: {marker}")
        harness = harness.replace(marker, body)
    return f"#define HAVE_WEB_CONTEXT_HELPER {int(has_helper)}\n" + harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path,
                        help="Old System_Utils.cpp file, or evidence directory containing System_Utils.before.cpp")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-health-logging-ui-") as directory:
        unit, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(generated(args.baseline))
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
