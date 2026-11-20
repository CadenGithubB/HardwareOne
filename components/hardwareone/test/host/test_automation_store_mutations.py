#!/usr/bin/env python3
"""Every change to automations.json is one locked read-modify-write by numeric id.

The store's mutation owner (mutateAutomationsJson in System_Automation.cpp) and
the commands and writebacks on it are compiled from the firmware and run on the
scheduler harness's fake store with the real ArduinoJson:

- enable, disable and delete act on the automation with exactly that id (12 and
  1234 beside 123456), found in the parsed document, not by a text offset;
- a quote or brace inside a name, command or condition survives a toggle, and
  an entry stored without an "enabled" flag is refused instead of toggling the
  next automation's flag;
- "id" stays the first member of every stored object and the text id lookup
  the readers use (automationIdPosInJson) still finds it after a mutation;
- an add is one write (no separate empty-store write), an edit through id=
  needs the creator or an admin, and a dry run writes nothing;
- two mutations started at the same moment both land (the second waits for the
  filesystem lock instead of writing from a stale read);
- a document that does not parse, has no "automations" array or is in a newer
  format is never written, and a malformed or unreadable stored file is left
  as it is;
- `userdelete` disables every automation the deleted user created (exact
  name), reports the count, leaves other users' automations on, and still
  deletes the user when automations.json cannot be read;
- the scheduler claims a pass only after a filesystem try-lock: with the lock
  held by another task the tick returns at once and consumes nothing (the
  refresh flag, the pending events and the due cache stay), and the pass runs
  on the first lap after the lock is free.

Source rules: no command or writeback in System_Automation.cpp splices the
stored text or writes the file itself; the only writer is automationsStoreWrite.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --thread-sanitize.
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

from test_automation_scheduler import generated
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def definition(source: str, marker: str) -> str:
    """The block of a definition; a forward declaration of the same name is skipped."""
    return extract_block(source[source.index(marker + " {"):], marker)


def between(source: str, start: str, end: str) -> str:
    require(source.count(start) == 1, f"marker is not unique: {start!r}")
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def unit(component: Path) -> str:
    owner = (component / "System_Automation.cpp").read_text()
    header = (component / "System_Automation.h").read_text()
    user = (component / "System_User.cpp").read_text()
    base = generated(component)
    require(base.count("\nint main() {") == 1, "the scheduler harness has one main")
    base = base.replace("\nint main() {", "\nstatic void schedulerHarnessCases() {")
    declaration = re.search(r"^bool sanitizeAutomationsJson\(String& jsonRef, [^;]*\);$", header, re.M)
    require(declaration is not None, "System_Automation.h declares sanitizeAutomationsJson with counts")
    store_api = re.search(r"^bool automationsSanitizeStore\([^;]*\);$", header, re.M)
    require(store_api is not None, "System_Automation.h declares automationsSanitizeStore")
    owned_api = re.search(r"^int automationDisableOwnedBy\([^;]*\);$", header, re.M)
    require(owned_api is not None, "System_Automation.h declares automationDisableOwnedBy")
    production = "\n\n".join([
        extract_block(header, "struct AutomationSanitizeCounts") + ";",
        declaration.group(0), store_api.group(0), owned_api.group(0),
        "bool readAutomationsJson(String& json);",
        definition(owner, "static int automationIdPosInJson(const String& json, unsigned long id)"),
        definition(owner, "bool sanitizeAutomationsJson(String& jsonRef, AutomationSanitizeCounts* counts)"),
        definition(owner, "bool automationsSanitizeStore(AutomationSanitizeCounts* counts, bool* changedOut)"),
        definition(owner, "static const char* automationsReadErrorReply()"),
        # The add entry point of the store: its context and mutation.
        between(owner, "// automationMutateErrorReply and automationCallerMayChange are defined with",
                "const char* cmd_automation_add(const String& argsInput) {"),
        # enable/disable, the owner-removed sweep and delete, with their helpers.
        between(owner, "// The reply of enable, disable, delete and run to an id= that is not an id.",
                "const char* cmd_automation_run(const String& argsInput) {"),
        # The next-run writebacks of the commands: one trigger's next run, and
        # `automation recompute`.
        between(owner, "struct AutomationNextAtContext {", "// Legacy shim: writes nextAt into the first schedulable trigger."),
        between(owner, "struct AutomationRecomputeContext {", "// The min nextAt across the triggers of the automation"),
    ])
    cases = (HERE / "automation_store_mutations_cases.cpp").read_text()
    for marker, replacement in {"// INSERT_STORE_PRODUCTION": production,
                                "// INSERT_USER_DELETE": definition(
                                    user, "static const char* doUserDelete(const String& username)")}.items():
        require(cases.count(marker) == 1, f"exactly one insertion marker: {marker}")
        cases = cases.replace(marker, replacement)
    return base + "\n" + cases


def code_only(source: str) -> str:
    return re.sub(r"//[^\n]*", "", source)


def source_rules(component: Path) -> list[str]:
    owner = (component / "System_Automation.cpp").read_text()
    failures = []
    code = code_only(owner)
    if code.count("writeTextAtomic(AUTOMATIONS_JSON_FILE") != 1:
        failures.append("automations.json has one writer (automationsStoreWrite)")
    writer = code_only(definition(owner, "static bool automationsStoreWrite(const String& json, bool notify, bool verified)"))
    if "writeTextAtomic(AUTOMATIONS_JSON_FILE" not in writer or "automationsNewerFileVersion(json)" not in writer \
            or "FsLockGuard" not in writer or "deserializeJson(" not in writer:
        failures.append("automationsStoreWrite refuses a newer or unparseable document and writes under the lock")
    elif writer.index("automationsNewerFileVersion(json)") > writer.index("writeTextAtomic("):
        failures.append("the version refusal comes before the write")
    mutate = code_only(definition(owner, "static AutomationsMutateResult mutateAutomationsJson(const char* tag, "
                                         "AutomationsMutator mutator,\n"
                                         "                                                     void* context, "
                                         "uint8_t flags,\n"
                                         "                                                     String& errorOut)"))
    for needed in ("FsLockGuard guard(tag);", "deserializeJson(doc, json)", "doc.overflowed()", "measureJsonPretty(doc)",
                   "serializeJsonPretty(doc, json)", "deserializeJson(verify, json)", "automationsStoreWrite("):
        if needed not in mutate:
            failures.append(f"mutateAutomationsJson: {needed}")
    for command in ("const char* cmd_automation_add(const String& argsInput)",
                    "const char* cmd_automation_enable_disable(const String& argsInput, bool enable)",
                    "const char* cmd_automation_delete(const String& argsInput)",
                    "bool updateAutomationTriggerNextAt(long automationId, int triggerIdx, time_t newNextAt)",
                    "static void rescheduleAfterFire(long id, const char* automationJson, time_t firedAt, bool notify)",
                    "static void automationCarryUptimeSchedule(time_t now)",
                    "static int automationRecomputeAllClockTriggers(time_t now, int* outSkipped)",
                    "int automationDisableOwnedBy(const String& username)"):
        body = code_only(definition(owner, command))
        name = command.split("(")[0].split()[-1]
        if body.count("mutateAutomationsJson(") != 1:
            failures.append(f"{name} goes through mutateAutomationsJson once")
        for forbidden in ("json.substring(", "writeAutomationsJsonAtomic(", "writeTextAtomic(", "readAutomationsJson(",
                          "automationIdPosInJson(", "serializeJsonPretty("):
            if forbidden in body:
                failures.append(f"{name} still edits or writes the stored text itself: {forbidden}")
    # The pass is claimed only after the filesystem try-lock, and the miss
    # path returns before anything is consumed.
    tick = code_only(definition(owner, "void automationTick()"))
    probe = tick.find("if (!automationPassFilesystemFree())")
    if probe < 0 or tick.count("sAutomationRefreshPending.exchange(false)") != 1 or \
            probe > tick.index("sAutomationRefreshPending.exchange(false)") or \
            probe > tick.index("gAutomationsCacheValid = false;") or probe > tick.index("schedulerTickMinute();"):
        failures.append("automationTick tries the filesystem lock before it claims a pass")
    free = code_only(definition(owner, "static bool automationPassFilesystemFree()"))
    if 'FsLockGuard probe("automation.pass", 0);' not in free or "isFsLockedByCurrentTask()" not in free:
        failures.append("the pass probe is a try-lock that also accepts this task's own hold")
    # userdelete disables the removed user's automations after the delete.
    user = (component / "System_User.cpp").read_text()
    delete = code_only(definition(user, "static const char* doUserDelete(const String& username)"))
    if delete.count("automationDisableOwnedBy(username)") != 1 or \
            delete.index("deleteUserInternal(username, err)") > delete.index("automationDisableOwnedBy(username)"):
        failures.append("userdelete disables the user's automations after a successful delete")
    disabled_stub = header_stub(component)
    if "inline int automationDisableOwnedBy(const String&) { return 0; }" not in disabled_stub:
        failures.append("a build without automations has an automationDisableOwnedBy stub")
    return failures


def header_stub(component: Path) -> str:
    header = (component / "System_Automation.h").read_text()
    return header[header.index("#else // !ENABLE_AUTOMATION"):]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    sanitizer = parser.add_mutually_exclusive_group()
    sanitizer.add_argument("--sanitize", action="store_true")
    sanitizer.add_argument("--thread-sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
             "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
             "-Wno-sign-compare", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
             "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
             "-DARDUINOJSON_ENABLE_PROGMEM=0", "-I", str(HERE / "web_batch_stubs"), "-I", str(component),
             "-I", str(COMPONENT.parent / "hardwareone_libs/ArduinoJson/src")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    if args.thread_sanitize:
        flags += ["-fsanitize=thread", "-fno-omit-frame-pointer", "-g"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-automation-store-") as directory:
        source = Path(directory) / "automation_store_mutations.cpp"
        binary = Path(directory) / "automation_store_mutations"
        source.write_text(unit(component))
        if subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], timeout=240).returncode != 0:
            failures.append("the automation store harness does not compile")
        elif subprocess.run([str(binary)], timeout=120).returncode != 0:
            failures.append("the automation store harness failed")
    failures += source_rules(component)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        sys.exit(1)
    print("automations.json: every change is one locked read-modify-write by numeric id", flush=True)


if __name__ == "__main__":
    main()
