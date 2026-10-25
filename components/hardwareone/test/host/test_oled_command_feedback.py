#!/usr/bin/env python3
"""Execute the OLED action helper and its callers against a scripted executor.

Splices in the production OLED action helpers (OLED_Utils.cpp oledRunCommand,
its reply-to-toast mapping oledCommandReplyText, and oledRunCommandAsync with
its frame-loop delivery) and real callers: the
Automations toggle/delete actions with their footer toast
(OLED_Mode_Automations.cpp), the Quick Settings radio, Wi-Fi, Bluetooth and HTTP
rows (OLED_SettingsEditor.cpp) and the Network menu's HTTP confirm callbacks
(OLED_Mode_Network.cpp). The OLED command executor, the global toast, the
Automations rows and the radio/BLE/HTTP state are doubles.

The cases check that a refused or failed command toasts its reply ("Admin
required" for an admin denial, otherwise the reply's first line without its
error token, "Failed" when empty) and that the caller's success text appears
only when the command succeeded. The Quick Settings rows submit without
waiting: the executor double finishes each command only when a case says so,
and the reply is delivered by the display-task poll (one request at a time, a
refused queue toasts, a reply from a replaced display session is dropped). Static checks cover the call sites the harness
does not run: the Logging and Network screens, the Quick Settings section and the
unavailable-page / Web Stats actions (OLED_Utils.cpp handleOLEDActionButton) no
longer fire commands whose reply is dropped, and their success lines are
conditional; the Bluetooth-unavailable page's X runs openble (the app console),
not the role-aware Quick Settings toggle. `--component <dir>` runs the same cases against saved sources (the
base tree has no helper and shows success texts for refused commands).
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

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block, require  # noqa: E402

RUN_COMMAND = "bool oledRunCommand("
REPLY_TEXT = "void oledCommandReplyText("
REPORT_REPLY = "static bool oledReportCommandReply("
# oledRunCommandAsync: its one-request mailbox, the executor callback, the
# submit and the display-task delivery, in source order.
ASYNC_STATE = "namespace {\nstruct OledCommandReply {"
ASYNC = ("static void oledCommandReplyArrived(", "bool oledRunCommandAsync(",
         "static void oledDeliverCommandReply(")
QUICK_RUNNER = ("static void quickCommandDone(", "static void runQuickCommand(")
TOGGLE_COMMAND = "const char* oledBluetoothToggleCommand("
AUTOMATIONS = ("static void autoShowToast(", "static void autoToggleSelected(",
               "static void autoDeleteConfirmed(")
QUICK_SETTINGS = ("static void setQuickStatus(", "static void toggleQuickRadio(",
                  "static void toggleQuickWiFi(", "static void bluetoothToggleConfirmedQuick(",
                  "static void httpToggleConfirmedQuick(")
NETWORK = ("static void httpStartConfirmedNetwork(", "static void httpStopConfirmedNetwork(")


def functions(source: str, name: str, markers: tuple[str, ...]) -> str:
    parts = []
    for marker in markers:
        require(source.count(marker) == 1, f"{name}: marker missing or not unique: {marker}")
        parts.append(extract_block(source, marker))
    return "\n\n".join(parts)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"(?m)(^|\s)//[^\n]*", r"\1", text)


def fire_and_forget_calls(text: str) -> list[str]:
    return [" ".join(call.split())
            for call in re.findall(r"\bexecuteOLEDCommand\(([^;]*)\);", strip_comments(text))]


def static_checks(read) -> list[str]:
    failures = []
    for name in ("OLED_Mode_Automations.cpp", "OLED_Mode_Logging.cpp", "OLED_Mode_Network.cpp"):
        calls = fire_and_forget_calls(read(name))
        if calls:
            failures.append(f"{name} drops the reply of {len(calls)} user action(s): "
                            + ", ".join(calls))
    editor = read("OLED_SettingsEditor.cpp")
    start = editor.index("// Quick Settings - Dynamic item registry")
    quick = editor[start:editor.index("static void initQuickItems(", start)]
    calls = fire_and_forget_calls(quick)
    if calls:
        failures.append("Quick Settings drops the reply of: " + ", ".join(calls))
    waits = re.findall(r"\boledRunCommand\(([^;]*)\)", strip_comments(quick))
    if waits:
        failures.append("Quick Settings waits on cmd_exec_task in the frame loop for: " +
                        ", ".join(" ".join(w.split()) for w in waits))
    utils = read("OLED_Utils.cpp")
    if ASYNC[1] in utils and "oledDeliverCommandReply();" not in strip_comments(
            extract_block(utils, "void updateOLEDDisplay(")):
        failures.append("updateOLEDDisplay never delivers oledRunCommandAsync replies")

    action = strip_comments(extract_block(read("OLED_Utils.cpp"), "void handleOLEDActionButton("))
    for command in ('"automation system enable"', '"openhttp"', '"closehttp"', '"openble"'):
        if f"executeOLEDCommand({command})" in action:
            failures.append(f"handleOLEDActionButton drops the reply of {command}")
    for match in re.finditer(r'broadcastOutput\("\[OLED\][^"]*"\)', action):
        statement_start = max(action.rfind(";", 0, match.start()), action.rfind("}", 0, match.start()))
        if "if (oledRunCommand(" not in action[statement_start + 1:match.start()]:
            failures.append(f"handleOLEDActionButton prints {match.group(0)} whatever the command answered")
    # The Bluetooth-unavailable page's X starts the app console, as it did before
    # the role rows; Quick Settings alone keeps the role-aware toggle.
    bluetooth = re.search(r'unavailableOLEDTitle == "Bluetooth"\)\s*\{(.*?)\}\s*else if', action, re.S)
    if not bluetooth:
        failures.append("handleOLEDActionButton has no Bluetooth-unavailable action")
    elif ('oledRunCommand("openble")' not in bluetooth.group(1)
          or "oledBluetoothToggleCommand" in bluetooth.group(1)):
        failures.append("the Bluetooth-unavailable page's X does not run openble (the app console)")
    return failures


def generated(read) -> tuple[str, bool, bool, bool]:
    utils = read("OLED_Utils.cpp")
    have_helper = RUN_COMMAND in utils
    have_reply_text = REPLY_TEXT in utils
    have_async = all(marker in utils for marker in (ASYNC_STATE, *ASYNC))
    helpers = [extract_block(utils, marker)
               for marker in (REPLY_TEXT, REPORT_REPLY, RUN_COMMAND) if marker in utils]
    if have_async:
        start = utils.index(ASYNC_STATE)
        helpers.append(utils[start:utils.index(ASYNC[0], start)])
        helpers.extend(extract_block(utils, marker) for marker in ASYNC)
    editor = read("OLED_SettingsEditor.cpp")
    quick = [m for m in QUICK_RUNNER if m in editor] + list(QUICK_SETTINGS[1:])
    harness = (HERE / "oled_command_feedback_harness.cpp").read_text()
    inserts = (
        ("// INSERT_RUN_COMMAND", "\n\n".join(helpers)),
        ("// INSERT_AUTOMATIONS", functions(read("OLED_Mode_Automations.cpp"),
                                            "OLED_Mode_Automations.cpp", AUTOMATIONS)),
        # The shared role-aware Bluetooth toggle command, before its Quick Settings caller.
        ("// INSERT_QUICK_SETTINGS", "\n\n".join(
            [extract_block(utils, TOGGLE_COMMAND)] if TOGGLE_COMMAND in utils else []) + "\n\n" +
            functions(editor, "OLED_SettingsEditor.cpp", (QUICK_SETTINGS[0], *quick))),
        ("// INSERT_NETWORK", functions(read("OLED_Mode_Network.cpp"), "OLED_Mode_Network.cpp", NETWORK)),
    )
    for marker, body in inserts:
        require(harness.count(marker) == 1, f"harness marker missing/duplicated: {marker}")
        harness = harness.replace(marker, body)
    return harness, have_helper, have_reply_text, have_async


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1],
                        help="production component directory (point at a saved tree for a base run)")
    parser.add_argument("--source-dir", type=Path,
                        help="overlay directory; a file present here wins over --component")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    def read(name: str) -> str:
        overlay = args.source_dir / name if args.source_dir else None
        return (overlay if overlay and overlay.exists() else args.component / name).read_text()

    unit_text, have_helper, have_reply_text, have_async = generated(read)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function",
             f"-DHW1_HAVE_RUN_COMMAND={1 if have_helper else 0}",
             f"-DHW1_HAVE_REPLY_TEXT={1 if have_reply_text else 0}",
             f"-DHW1_HAVE_ASYNC_COMMAND={1 if have_async else 0}"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-oled-command-feedback-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(unit_text)
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)], check=True, timeout=300)
        run = subprocess.run([str(executable)], timeout=60)
    failures = static_checks(read)
    if not have_reply_text:
        failures.insert(0, "OLED_Utils.cpp has no oledCommandReplyText(): OLED screens have no shared "
                           "reply-to-toast mapping (mapping cases not compiled)")
    if not have_async:
        failures.insert(0, "OLED_Utils.cpp has no oledRunCommandAsync(): long OLED actions block "
                           "the frame loop (async cases not compiled)")
    if not have_helper:
        failures.insert(0, "OLED_Utils.cpp has no oledRunCommand(): OLED actions have no shared "
                           "way to report a refused command (helper cases not compiled)")
    for failure in failures:
        print(f"FAIL [static] {failure}", file=sys.stderr)
    if run.returncode != 0 or failures:
        raise SystemExit("oled_command_feedback: FAILED")
    print("oled_command_feedback: refusals toasted, success text only on success, no dropped replies, "
          "quick rows never wait")


if __name__ == "__main__":
    main()
