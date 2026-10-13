#!/usr/bin/env python3
"""Exercise production Health logging request routing and web session delivery.

Shipping model actions, worker dispatch, OLED input, HTTP handler and unified
web executor are compiled with boundary mocks. Logger mutations and the G2
command bridge's authorization machinery have separate owner tests. The ring
controls run the production ring/health command rows and handlers through a
registry executor double, so web, OLED and CLI refusals and audit records can
be compared.
"""
from __future__ import annotations

import argparse
import re
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_mqtt_boolean_commands import extract_macro
from test_web_batch_handlers import extract_block, require, web_command_types

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def optional(source: str, marker: str) -> list[str]:
    """A definition the fixed sources add; absent on 0.99.97.4."""
    return [extract_block(source, marker)] if marker in source else []


def generated(component: Path = COMPONENT, source_dir: Path | None = None,
              baseline: Path | None = None) -> str:
    def read(name: str) -> str:
        overlay = source_dir / name if source_dir is not None else None
        return (overlay if overlay is not None and overlay.exists() else component / name).read_text()

    health = read("G2_Health.cpp")
    public = read("G2_Health.h")
    glasses = read("G2_Glasses.cpp")
    web = read("WebPage_R1_Health.cpp")
    utils = read("WebServer_Utils.h")
    if baseline is not None:
        command_path = baseline / "System_Utils.before.cpp" if baseline.is_dir() else baseline
        command = command_path.read_text()
    else:
        command = read("System_Utils.cpp")
    has_helper = "CommandContext makeWebCommandContext(" in command
    web_executor = []
    if has_helper:
        web_executor.append(extract_block(command, "CommandContext makeWebCommandContext("))
    web_executor.append(extract_block(command, "bool executeUnifiedWebCommand("))
    oled = read("OLED_Mode_R1_Health.cpp")
    ring = read("G2_Ring.cpp")
    sensor_logging = read("System_SensorLogging.cpp")
    types = [web_command_types(), extract_block(public, "enum G2HealthMetric") + ";",
             extract_block(read("System_CommandRegistry.h"), "struct CommandEntry") + ";",
             extract_block(public, "enum G2HealthNav") + ";",
             extract_block(read("G2_Glasses.h"), "enum G2HijackPage") + ";",
             extract_block(read("G2_HijackCmd.h"), "struct G2CmdCookie") + ";",
             extract_block(oled, "enum HealthAction") + ";"]
    auth_start = utils.index("#define WEB_AUTH_OR_RETURN(req, ctx)")
    auth_end = utils.index("\n\n", auth_start)
    production = [utils[auth_start:auth_end],
                  *[re.search(pattern, health, re.M)[0] for pattern in (
                      r"^static bool     sLoggingCmdPending[^\n]+", r"^static char     sLoggingUiMsg[^\n]+",
                      r"^static uint32_t sLoggingUiMs[^\n]+",
                      r"^static constexpr uint32_t kLoggingUiHoldMs[^\n]+")],
                  extract_block(health, "void g2HealthOpen("),
                  extract_block(health, "void g2HealthAction("),
                  extract_block(health, "bool g2HealthConsumeLoggingToggleRequest("),
                  extract_block(health, "void g2HealthSetPageActive("),
                  extract_block(health, "void g2HealthNoteLoggingCommandQueued("),
                  extract_block(health, "void g2HealthNoteLoggingCommandResult("),
                  extract_block(health, "void g2HealthNoteLoggingCommandRefused("),
                  extract_block(health, "const char* g2HealthLoggingStatusLine("),
                  extract_block(glasses, "static void onHealthLoggingCmdDone("),
                  "static void drainLoggingRequest() {\n" +
                  extract_block(glasses, "if (g2HealthConsumeLoggingToggleRequest())") + "\n}",
                  *web_executor,
                  extract_block(web, "static bool readHealthActionBody("),
                  extract_block(web, "static String healthFormValue("),
                  extract_block(web, "static esp_err_t healthActionReply("),
                  *optional(web, "static esp_err_t healthCommandReply("),
                  extract_block(web, "esp_err_t handleR1HealthAction("),
                  extract_macro(read("System_Utils.h"), "RETURN_VALID_IF_VALIDATE_CSTR"),
                  *optional(ring, "static const char* cmd_ringcontrol("),
                  extract_block(ring, "extern const CommandEntry g2RingCommands[] =") + ";",
                  # healthstatus is a set-1 handler: only its force-history branch is exercised.
                  "static const char* cmd_healthstatus(const String& argsInput) {\n"
                  "  CommandArgs a(argsInput);\n"
                  "  String sub = a.has(0) ? a.arg(0) : String();\n"
                  "  sub.toLowerCase();\n" +
                  extract_block(sensor_logging, 'if (sub == "force-history")') +
                  "\n  return \"Error: healthstatus form not modelled by this harness\";\n}",
                  extract_block(sensor_logging, "const CommandEntry healthCommands[] =") + ";",
                  *optional(oled, "static const char* healthNextDesiredToken("),
                  *optional(oled, "static void healthRunCommand("),
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
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="components/hardwareone directory to read production sources from")
    parser.add_argument("--source-dir", type=Path,
                        help="directory whose files overlay --component file by file")
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
        unit.write_text(generated(args.component, args.source_dir, args.baseline))
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
