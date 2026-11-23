#!/usr/bin/env python3
"""Execute the OLED action helper and its callers against a scripted executor.

Splices in the production OLED action helpers (OLED_Utils.cpp oledRunCommand,
its reply-to-toast mapping oledCommandReplyText, and the one OLED command in
flight behind oledRunCommandAsync and the background executeOledCommand, built
on System_PendingCommand, with its frame-loop delivery) and real callers: the
Automations toggle/delete actions with their shared toast
(OLED_Mode_Automations.cpp), the Quick Settings radio, Wi-Fi, Bluetooth and HTTP
rows (OLED_SettingsEditor.cpp) and the Web menu's server confirm callbacks
(OLED_Mode_Network.cpp). The OLED command executor, the global toast, the
Automations rows and the radio/BLE/HTTP state are doubles.

The cases check that a refused or failed command toasts its reply ("Needs
an admin" / "Needs a super admin" for the privilege gates, otherwise the reply's
first sentence without its error token, "Failed" when empty) and that the caller's success text appears
only when the command succeeded. The Quick Settings rows submit without
waiting: the executor double finishes each command only when a case says so,
and the reply is delivered by the display-task poll (one request at a time, a
full queue ends as a refusal after 2 s, a reply from a replaced display session is dropped).
The background executeOledCommand (OLED background commands, step 1,
2026-11-22) is run too: a press while a command runs shows "Busy" and queues
nothing, the busy flag clears when it ends, and the toast carries the
refusal's reason (admin gates included), a full queue or the 60 s timeout; a
success shows no toast (owner: failures only), except through
executeOledCommandShowResult, which toasts the reply. The value-scroll rows
(step 2: oledValueScrollStep on the production OLED_ValueScroll.h) are run on
the same slot: a step is shown at once and never waits or says "Busy", steps
while one runs coalesce into the newest value, which is sent when the slot
frees (also after another screen's command), a refusal gives the row back to
the device, the row follows the device once the last command ended, and a
display session boundary drops a waiting value so it is never sent as the new user.
Static checks cover the call sites the harness does not run:
executeOledCommand runs in the background, every OLED screen either uses
oledRunCommand's verdict or runs the action in the background (a remaining
wait says why and that step 2 moves it), the frame loop delivers the reply
whether the panel runs or not; the Quick Settings section never waits on a command; the
unavailable-page actions (OLED_Utils.cpp handleOledActionButton) print their
success lines only when the command succeeded; the
Bluetooth menu's on/off row runs openble, not the role-aware Quick Settings
toggle. `--component <dir>` runs the same cases against saved sources (the
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
from reply_words_source import reply_prelude

RUN_COMMAND = "bool oledRunCommand("
REPLY_TEXT = "void oledCommandReplyText("
REPORT_REPLY = "static bool oledReportCommandReply("
# The one OLED command in flight: its state, the busy flag, the success text,
# the start, oledRunCommandAsync, the background executeOledCommand and the
# display-task delivery, in source order.
ASYNC_STATE = "namespace {\nstruct OledCommandInFlight {"
ASYNC = ("bool oledCommandBusy(", "static void oledCommandSuccessText(",
         "static bool oledStartBackgroundCommand(", "bool oledRunCommandAsync(",
         "static void oledRunBackgroundAction(", "void executeOledCommand(",
         "void executeOledCommandShowResult(", "static void oledDeliverCommandReply(")
# The value-scroll rows (OLED background commands, step 2): their registry, the
# done callback, the frame-loop send, the step and the shown value.
VALUE_SCROLL_STATE = "namespace {\nstruct OledValueScrollRow {"
VALUE_SCROLL = ("static void oledValueScrollDone(", "static void oledValueScrollPump(",
                "void oledValueScrollStep(", "int32_t oledValueScrollShown(",
                "static void oledValueScrollSessionBoundary(")
# Every OLED screen source whose actions run commands.
OLED_SOURCES_GLOB = ("OLED_*.cpp", "i2csensor_*_oled.h", "HAL_*_OLED.h")
# A wait that stays (value scroll, a chained or completion-dependent action)
# carries this tag on its line; step 2 of the OLED background plan moves them.
WAIT_TAG = "// waits:"
QUICK_RUNNER = ("static void quickCommandDone(", "static void runQuickCommand(")
TOGGLE_COMMAND = "const char* oledBluetoothToggleCommand("
AUTOMATIONS = ("static void autoShowToast(", "static void autoToggleSelected(",
               "static void autoDeleteConfirmed(")
QUICK_SETTINGS = ("static void setQuickStatus(", "static void toggleQuickRadio(",
                  "static void toggleQuickWifi(", "static void bluetoothToggleConfirmedQuick(",
                  "static void httpToggleConfirmedQuick(")
NETWORK = ("static void webServerStartConfirmed(", "static void webServerStopConfirmed(")


def functions(source: str, name: str, markers: tuple[str, ...]) -> str:
    parts = []
    for marker in markers:
        require(source.count(marker) == 1, f"{name}: marker missing or not unique: {marker}")
        parts.append(extract_block(source, marker))
    return "\n\n".join(parts)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"(?m)(^|\s)//[^\n]*", r"\1", text)


def static_checks(read, component: Path) -> list[str]:
    failures = []
    utils = read("OLED_Utils.cpp")
    # An action that needs no verdict runs in the background and still toasts
    # a refusal.
    execute = "static void oledRunBackgroundAction("
    if "void executeOledCommand(" in utils and (execute not in utils or "oledStartBackgroundCommand(" not in
                                                strip_comments(extract_block(utils, execute))):
        failures.append("executeOledCommand does not run in the background: "
                        "every action that ignores its verdict holds the frame loop")
    if VALUE_SCROLL[1] in utils and "oledValueScrollPump();" not in strip_comments(
            extract_block(utils, "void oledUpdate(")):
        failures.append("oledUpdate never sends a value-scroll row's waiting value")
    if VALUE_SCROLL[1] in utils and "oledValueScrollSessionBoundary();" not in strip_comments(
            extract_block(utils, "static void oledResetLocalDisplaySessionTransients(")):
        failures.append("a display session boundary keeps value-scroll rows' waiting values: "
                        "the old session's step would be sent as the new user")
    failures += wait_sites(read, component)
    editor = read("OLED_SettingsEditor.cpp")
    start = editor.index("// Quick Settings - Dynamic item registry")
    quick = editor[start:editor.index("static void initQuickItems(", start)]
    # Both helpers wait for cmd_exec; the Quick Settings rows submit instead.
    waits = re.findall(r"\b(?:oledRunCommand|executeOledCommand)\(([^;]*)\)", strip_comments(quick))
    if waits:
        failures.append("Quick Settings waits on cmd_exec in the frame loop for: " +
                        ", ".join(" ".join(w.split()) for w in waits))
    if ASYNC[3] in utils and "oledDeliverCommandReply();" not in strip_comments(
            extract_block(utils, "void oledUpdate(")):
        failures.append("oledUpdate never delivers the OLED command in flight")
    # The busy mark of the OLED command in flight is the hourglass in the
    # header bar (OLED_HeaderCore.h): the header collects oledCommandBusy();
    # the old box over the content area is gone.
    if ASYNC[3] in utils and ("static void oledHeaderCollect(" not in utils or
                              "oledCommandBusy()" not in strip_comments(
                                  extract_block(utils, "static void oledHeaderCollect("))):
        failures.append("the header bar never shows the busy mark of the OLED command in flight")
    if "oledCommandBusyRender" in strip_comments(utils):
        failures.append("the content-area busy box is back: the hourglass belongs to the header bar")

    action = strip_comments(extract_block(read("OLED_Utils.cpp"), "void handleOledActionButton("))
    for command in ('"automation system enable"', '"openhttp"', '"closehttp"', '"openble"'):
        if f"executeOledCommand({command})" in action:
            failures.append(f"handleOledActionButton drops the reply of {command}")
    for match in re.finditer(r'broadcastOutput\("\[OLED\][^"]*"\)', action):
        statement_start = max(action.rfind(";", 0, match.start()), action.rfind("}", 0, match.start()))
        if "if (oledRunCommand(" not in action[statement_start + 1:match.start()]:
            failures.append(f"handleOledActionButton prints {match.group(0)} whatever the command answered")
    # Client-capable builds start their shared stack; server-only profiles
    # start the app console. Both paths must report the command outcome.
    bt_input = strip_comments(extract_block(read("OLED_Mode_Bluetooth.cpp"),
                                            "static bool bluetoothInputHandler("))
    power = re.search(r"case BT_ACT_POWER:(.*?)break;", bt_input, re.S)
    if not power:
        failures.append("the Bluetooth menu has no on/off row")
    elif any(call not in power.group(1) for call in
             ('executeOledCommand("openble")', 'executeOledCommand("openconsole")')):
        failures.append("the Bluetooth menu's on/off row must run both client and server-only "
                        "starts in the background")
    return failures


def wait_sites(read, component: Path) -> list[str]:
    """Every oledRunCommand call on an OLED screen uses its verdict or says why it waits.

    A bare statement throws the verdict away and still holds the frame loop:
    such an action belongs on the background executeOledCommand. The remaining
    waits (value scroll, chained or completion-dependent actions) carry the
    WAIT_TAG comment naming the reason, the list step 2 works through.
    """
    failures = []
    names = sorted({path.name for pattern in OLED_SOURCES_GLOB for path in component.glob(pattern)})
    verdict = re.compile(r"(?:\bif\s*\(\s*!?\s*|=\s*|&&\s*|\|\|\s*|return\s+)$")
    tagged = 0
    for name in names:
        lines = read(name).splitlines()
        for number, line in enumerate(lines, 1):
            code = line.split("//", 1)[0]
            for match in re.finditer(r"\boledRunCommand\s*\(", code):
                before = code[:match.start()]
                if re.search(r"\bbool\s+$", before):
                    continue  # the definition
                if verdict.search(before):
                    continue  # the caller reads the verdict
                if before.rstrip().endswith("(void)") and WAIT_TAG in line:
                    tagged += 1
                    continue
                failures.append(f"{name}:{number}: oledRunCommand's verdict is ignored but the "
                                "frame loop still waits; use executeOledCommand, or tag the line "
                                f"'(void)oledRunCommand(...);  {WAIT_TAG} <why>'")
    if tagged == 0 and names:
        failures.append("no tagged OLED waits found; is the source glob right?")
    return failures


def generated(read) -> tuple[str, bool, bool, bool, bool]:
    utils = read("OLED_Utils.cpp")
    have_helper = RUN_COMMAND in utils
    have_reply_text = REPLY_TEXT in utils
    have_async = all(marker in utils for marker in (ASYNC_STATE, *ASYNC))
    have_scroll = have_async and all(marker in utils for marker in (VALUE_SCROLL_STATE, *VALUE_SCROLL))
    helpers = [extract_block(utils, marker)
               for marker in (REPLY_TEXT, REPORT_REPLY, RUN_COMMAND) if marker in utils]
    if have_async:
        start = utils.index(ASYNC_STATE)
        helpers.append(utils[start:utils.index(ASYNC[0], start)])
        helpers.extend(extract_block(utils, marker) for marker in ASYNC)
    if have_scroll:
        start = utils.index(VALUE_SCROLL_STATE)
        helpers.append(utils[start:utils.index(VALUE_SCROLL[0], start)])
        helpers.extend(extract_block(utils, marker) for marker in VALUE_SCROLL)
    pending = ""
    if have_async:
        # System_PendingCommand as is, its includes replaced by the harness's doubles.
        pending = "\n".join(
            "\n".join(line for line in read(name).splitlines()
                      if not line.startswith(("#include", "#pragma once")))
            for name in ("System_PendingCommand.h", "System_PendingCommand.cpp"))
    editor = read("OLED_SettingsEditor.cpp")
    quick = [m for m in QUICK_RUNNER if m in editor] + list(QUICK_SETTINGS[1:])
    harness = (HERE / "oled_command_feedback_harness.cpp").read_text()
    inserts = (
        ("// INSERT_PENDING_COMMAND", pending),
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
    return harness, have_helper, have_reply_text, have_async, have_scroll


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

    unit_text, have_helper, have_reply_text, have_async, have_scroll = generated(read)
    flags = ["-I", str(args.component), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function",
             f"-DHW1_HAVE_RUN_COMMAND={1 if have_helper else 0}",
             f"-DHW1_HAVE_REPLY_TEXT={1 if have_reply_text else 0}",
             f"-DHW1_HAVE_ASYNC_COMMAND={1 if have_async else 0}",
             f"-DHW1_HAVE_VALUE_SCROLL={1 if have_scroll else 0}"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-oled-command-feedback-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(reply_prelude(args.component) + unit_text)
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)], check=True, timeout=300)
        run = subprocess.run([str(executable)], timeout=60)
    failures = static_checks(read, args.component)
    if not have_reply_text:
        failures.insert(0, "OLED_Utils.cpp has no oledCommandReplyText(): OLED screens have no shared "
                           "reply-to-toast mapping (mapping cases not compiled)")
    if have_async and not have_scroll:
        failures.insert(0, "OLED_Utils.cpp has no oledValueScrollStep(): value-scroll rows wait on "
                           "cmd_exec one step at a time (value-scroll cases not compiled)")
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
          "quick rows never wait, fire-and-forget actions run in the background")


if __name__ == "__main__":
    main()
