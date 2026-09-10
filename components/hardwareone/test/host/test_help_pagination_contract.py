#!/usr/bin/env python3
"""Source-contract guards for transport-safe, registry-neutral help paging."""

import re
from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[2]
REPO = COMPONENT.parents[1]


def brace_block(source: str, opening_brace: int) -> str:
    """Return one balanced C++ brace block, ignoring comments and literals."""

    depth = 0
    pos = opening_brace
    state = "code"
    while pos < len(source):
        current = source[pos]
        following = source[pos + 1] if pos + 1 < len(source) else ""

        if state == "line_comment":
            if current == "\n":
                state = "code"
        elif state == "block_comment":
            if current == "*" and following == "/":
                state = "code"
                pos += 1
        elif state == "string":
            if current == "\\":
                pos += 1
            elif current == '"':
                state = "code"
        elif state == "character":
            if current == "\\":
                pos += 1
            elif current == "'":
                state = "code"
        else:
            if current == "/" and following == "/":
                state = "line_comment"
                pos += 1
            elif current == "/" and following == "*":
                state = "block_comment"
                pos += 1
            elif current == '"':
                state = "string"
            elif current == "'":
                state = "character"
            elif current == "{":
                depth += 1
            elif current == "}":
                depth -= 1
                if depth == 0:
                    return source[opening_brace : pos + 1]
        pos += 1
    raise AssertionError("unterminated C++ brace block")


cli = (COMPONENT / "System_CLI.cpp").read_text(encoding="utf-8")
pager = (COMPONENT / "System_HelpPagerCore.h").read_text(encoding="utf-8")
limits = (COMPONENT / "System_CommandLimits.h").read_text(encoding="utf-8")
debug = (COMPONENT / "System_Debug.h").read_text(encoding="utf-8")
guide = (REPO / "docs" / "USERGUIDE.md").read_text(encoding="utf-8")
command_reference = (REPO / "docs" / "COMMAND_REFERENCE.md").read_text(
    encoding="utf-8"
)
utils = (COMPONENT / "System_Utils.cpp").read_text(encoding="utf-8")

# Page planning is a dependency-light step before the fan-out pipeline. The CLI
# consumes it directly and derives its budgets from the two production transport
# envelopes instead of growing another independent magic-number limit.
assert '#include "System_HelpPagerCore.h"' in cli
assert "hw1_help_pager::" in cli
assert "CMD_RESULT_MAX" in cli
assert "DEBUG_MSG_SIZE" in cli
assert re.search(r"\bCMD_RESULT_MAX\b\s*=", limits)
assert re.search(r"#define\s+DEBUG_MSG_SIZE\b", debug)

for forbidden_dependency in (
    "Arduino.h",
    "broadcastOutput",
    "CaptureBufState",
    "WebMirrorBuf",
    "httpd_req_t",
    "Serial.",
):
    assert forbidden_dependency not in pager

# The renderer still feeds bounded lines through the ordinary fan-out. It must
# not install a private capture buffer or write the web mirror directly; those
# would make paging transport-specific and bypass normal serial/BLE/G2 routing.
assert re.search(
    r"hw1_help_pager::Placement[\s\S]{0,1600}"
    r"\bbroadcastOutput\(\s*length\s*\?\s*line\s*:\s*\" \"\s*\)",
    cli,
)
assert "helpLineCaptureBytes(length)" in cli
assert "setCaptureBuffer(" not in cli
assert "currentCaptureState(" not in cli
assert ".assignFrom(" not in cli

# The explicit p<N> form works for stateless callers, while the same token and
# next/prev work inside an owned help mode. Keep both firmware help text and the
# user guide discoverable.
assert cli.count("p<N>") >= 2
assert "parsePageToken(" in cli
assert "`help <module> p<N>`" in guide
assert "`next`, `prev`, or `p<N>`" in guide
assert "help espnow p2" in command_reference

# Planning and replay use one immutable view of dynamic sensor/Wi-Fi state.
# Interactive page navigation retains that view for the selected module, so a
# live status change cannot move a boundary and repeat/skip a command.
assert "struct HelpRenderSnapshot" in cli
assert "captureHelpRenderSnapshot" in cli
assert "HelpRenderSnapshot snapshot" in cli
assert "walkModulePageBody(module, snapshot" in cli
assert "snapshot.wifiNetworkCount" in cli

navigation_start = cli.index("bool handleHelpNavigation(")
navigation_open = cli.index("{", navigation_start)
help_navigation = brace_block(cli, navigation_open)
assert '"next"' in help_navigation
assert '"prev"' in help_navigation
assert "parsePageToken(" in help_navigation
assert "looksLikePageToken" not in help_navigation
assert "findCommand(lc)" in help_navigation
assert help_navigation.index("findCommand(lc)") < help_navigation.index(
    "parsePageToken("
)

mode_input_start = cli.index("static CLIModeInputResult helpMode_onInput(")
mode_input_open = cli.index("{", mode_input_start)
help_mode_input = brace_block(cli, mode_input_open)
assert "addressedHelp" in help_mode_input
assert "CLI_MODE_PASSTHROUGH" in help_mode_input

command_start = cli.index("static const char* cmd_help(")
command_open = cli.index("{", command_start)
help_command = brace_block(cli, command_open)
assert "parsePageToken(" in help_command
assert "p<N>" in help_command

# next/prev are mode inputs, not global registry commands. Registering them
# would alter command discovery, generated references, and peer manifests.
array_marker = "const CommandEntry cliCommands[]"
array_start = cli.index(array_marker)
array_open = cli.index("{", array_start)
cli_command_array = brace_block(cli, array_open)
registered_names = re.findall(r'\{\s*"([^"]+)"\s*,', cli_command_array)
assert registered_names == ["help", "back", "exit", "clear"]

# A consumed mode error is still a failed command. This keeps in-mode p<N>
# errors aligned with direct `help <module> p<N>` HTTP status behavior.
assert re.search(
    r"cliModeDispatchInput\([^)]*\)\)\s*\{[\s\S]{0,300}"
    r"strncmp\(out,\s*\"Error\"",
    utils,
)

print("help pagination source guards passed")
