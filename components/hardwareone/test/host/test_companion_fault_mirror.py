#!/usr/bin/env python3
"""The C6 console mirror is off unless a fault or `c6console on` wants it; faults are counted for good.

UP-14 (2026-11-20). The mirror of the companion's own console held about 6 KB
of internal SRAM on every boot (a 2048 B UART buffer and a 4096 B task stack,
HAL_Radio.cpp). It is now off by default and not started at boot. It starts
when a companion fault is seen (an outage, a failed start or recovery, a boot
attempt that failed, a reboot made for the companion) and stops once the
companion has been healthy for ten minutes; a saved `c6console on` keeps it.
The owner also wanted to read how often the companion faults: the number of
outages and of reboots made for the companion, and the time of the last one,
are settings rows that survive restarts.

The harness compiles the shipping serviceFaults, consoleMirrorProc,
noteLifetimeFault, faultCountsSaveProc, mirrorReason, mirrorText, dispatch,
radioCompanionBootInit and cmd_c6console (System_RadioCompanion.cpp) against
the real Radio_CompanionCore.h and recording fakes. It checks what happens and
on which task: the mirror is started and stopped only on the command executor
(a stop waits for the mirror task, and the HAL call has no lock), a mirror
start is queued ahead of the recovery it should show, a start that fails is
tried once per arming, the counters are plain assignments (no setSetting, so
no setting-changed event per fault), the settings file is written only from
the executor and never in the boot or the tick that ends in a reboot, and the
reboot is counted at the next boot from the RTC marker.

Source checks cover what the harness does not compile: the defaults (the
struct initialiser and the settings row must both be off, because the row's
default is what a boot applies), the read-only counter rows, the status keys,
and radioHalPrepare, which starts the mirror before its first attempt.

B13 (2026-11-21): the mirror now runs on every boot. radioHalPrepare starts
it; the boot window (bootDebugInProgress) counts as the setting being on, so
the policy stops it on the first tick after setup() ends unless the setting or
a fault wants it. A failed first attempt is read through radioHalBootRetried.
The policy's timing is in test_radio_companion_core.cpp.
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


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def one(source: str, marker: str) -> str:
    require(source.count(marker) == 1, f"expected one definition of {marker!r} in System_RadioCompanion.cpp")
    return extract_block(source, marker)


def constant(source: str, name: str) -> str:
    match = re.search(r"constexpr (?:uint32_t|uint8_t|int|size_t) " + name + r" = [^;]+;", source)
    require(match is not None, f"System_RadioCompanion.cpp defines {name}")
    return match.group(0)


def source_checks(src: Path) -> None:
    companion = (src / "System_RadioCompanion.cpp").read_text()
    plain = strip_comments(companion)
    hal = strip_comments((src / "HAL_Radio.cpp").read_text())
    settings_h = (src / "System_Settings.h").read_text()

    # Defaults: the row default is what applyRegisteredDefaults() stamps at boot.
    require(re.search(r'\{ "c6ConsoleMirror",\s*SETTING_BOOL,\s*&gSettings\.c6ConsoleMirror,\s*0,.*"c6console" \},',
                      plain) is not None, "the c6ConsoleMirror row defaults to off and stays bound to c6console")
    require("c6ConsoleMirror(false)," in settings_h and "c6ConsoleMirror(true)" not in settings_h,
            "the struct default of c6ConsoleMirror is off")
    for key, field in (("c6OutageCount", "c6OutageCount"), ("c6RebootCount", "c6RebootCount"),
                       ("c6LastFaultEpochSec", "c6LastFaultEpochSec")):
        row = re.search(r'\{ "' + key + r'",\s*SETTING_U32,\s*&gSettings\.' + field +
                        r',\s*0, 0, nullptr, 0, 0,\s*"[^"]+",\s*nullptr, false, nullptr, nullptr, true \},', plain)
        require(row is not None, f"{key} is a read-only U32 row with default 0 and no command")
        require(f"{field}(0)," in settings_h and re.search(r"\buint32_t " + field + r";", settings_h) is not None,
                f"{field} is a uint32_t settings field that starts at 0")
        require(not re.search(r"setSetting\(\s*gSettings\." + field, plain),
                f"{field} is assigned plainly, never through setSetting")
    require(plain.count("requestSettingsPersist(") == 1 and "writeSettingsJson(" not in plain,
            "the companion saves settings in one place, through the ordinary request")
    require("requestSettingsPersist(" in one(companion, "void faultCountsSaveProc(void*)"),
            "that place is the executor procedure")

    # Every mirror call outside the HAL sits in the two executor functions.
    calls = plain.count("radioHalConsoleMirror(")
    in_proc = strip_comments(one(companion, "void consoleMirrorProc(void* arg)")).count("radioHalConsoleMirror(")
    in_cmd = strip_comments(one(companion, "const char* cmd_c6console(const String& argsInput)")).count(
        "radioHalConsoleMirror(")
    require(in_proc == 2 and in_cmd == 1 and calls == 3,
            f"the mirror is started and stopped only by consoleMirrorProc and c6console ({calls} calls)")
    boot = strip_comments(one(companion, "void radioCompanionBootInit()"))
    require("radioHalConsoleMirror(" not in boot and "requestSettingsPersist" not in boot
            and "setSetting(" not in boot, "boot init starts no mirror itself and writes no settings")
    require(boot.index("m.onBootResult(") < boot.index("serviceFaults(") and "noteLifetimeFault(1, 1);" in boot,
            "boot init feeds the boot result, then the fault service; the marker counts one outage and one reboot")

    # The reboot case neither counts nor queues: dispatch skips the service for it.
    dispatch = strip_comments(one(companion, "void dispatch(CompanionAction action, uint32_t now)"))
    guard = "if (action != CompanionAction::HardReboot) serviceFaults(now, action == CompanionAction::None);"
    require(guard in dispatch and dispatch.index(guard) < dispatch.index("switch (action)"),
            "dispatch runs the fault service first, except before a reboot, and saves only on a quiet tick")
    reboot = dispatch[dispatch.index("case CompanionAction::HardReboot:"):dispatch.index("case CompanionAction::Hold:")]
    for forbidden in ("requestSettingsPersist", "noteLifetimeFault", "serviceFaults", "submitDeferredToCmdExec",
                      "setSetting("):
        require(forbidden not in reboot, f"the reboot case must not call {forbidden}")

    # The HAL: the mirror runs for the boot (B13), started before the first
    # attempt; a failed first attempt is reported as a fault, not by the mirror.
    prepare = extract_block(hal, "esp_err_t radioHalPrepare()")
    require(prepare.count("radioHalConsoleMirror(true);") == 1, "radioHalPrepare has one mirror start")
    require(prepare.index("radioHalConsoleMirror(true);") < prepare.index("for (int attempt = 1;"),
            "radioHalPrepare starts the mirror before its first attempt")
    loop = prepare[prepare.index("for (int attempt = 1;"):]
    retry = loop[loop.index("if (attempt > 1) {"):loop.index("err = radioHalStartTransport();")]
    require("sBootRetried = true;" in retry, "a retry is recorded for radioHalBootRetried")
    require("radioHalBootRetried()" in boot and "radioHalConsoleMirrorOn()" not in boot,
            "boot init arms the mirror for a failed boot attempt, not for the boot's own mirror")
    service = strip_comments(one(companion, "void serviceFaults(uint32_t now, bool quiet)"))
    require("gSettings.c6ConsoleMirror || bootDebugInProgress()" in service,
            "the boot window counts as the setting being on until setup() ends")
    require(re.search(r"bool radioHalConsoleMirrorSupported\(\) \{ return true; \}", hal) is not None and
            re.search(r"bool radioHalConsoleMirrorSupported\(\) \{ return false; \}", hal) is not None,
            "the HAL says whether this assembly has the companion console")

    # Status: the new keys are in the full reply only (c6status json), so
    # `status json` and /api/system keep their companion object.
    status = strip_comments(one(companion, "void fillStatus(JsonObject out, bool full)"))
    full_only = status[status.index("if (!full) return;"):]
    for key in ('rec["outages"]', 'rec["lifetime_outages"]', 'rec["lifetime_reboots"]',
                'rec["last_fault_epoch_ms"]', 'out["console_mirror_reason"]'):
        require(key in full_only and key not in status[:status.index("if (!full) return;")],
                f"{key} is reported by c6status json only")
    require('out["console_mirror"] = radioHalConsoleMirrorOn();' in status[:status.index("if (!full) return;")],
            "console_mirror stays a boolean in the short status")
    text = strip_comments(one(companion, "const char* cmd_c6status(const String& argsInput)"))
    for part in ('"  Lifetime: %" PRIu32 " outages, %" PRIu32 " restarts of this device for the companion; last fault: %s"',
                 '"  Console mirror: %s", mirrorText()'):
        require(part in text, f"c6status prints {part}")
    # No new direct ESP-IDF log call (test_logging_routes.py counts them per file).
    for name in ("void serviceFaults(uint32_t now, bool quiet)", "void consoleMirrorProc(void* arg)",
                 "void noteLifetimeFault(uint32_t outages, uint32_t reboots)", "void faultCountsSaveProc(void*)"):
        require("ESP_LOG" not in one(companion, name), f"{name} logs through logSystemEvent")
    # One name: the command row and the settings row both say c6console.
    require(len(re.findall(r'\{ "c6console",', plain)) == 1, "one c6console command row")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-dir", "--component", dest="source_dir", type=Path, default=COMPONENT)
    args = parser.parse_args()
    src = args.source_dir
    source_checks(src)

    companion = (src / "System_RadioCompanion.cpp").read_text()
    blocks = {
        "/*@CONSTANTS@*/": "\n".join([constant(companion, "kMirrorHealthyMs"), constant(companion, "kRebootMarker")]),
        "/*@SERVICE@*/": "\n\n".join(one(companion, marker) for marker in (
            "bool mirrorArmed()",
            "const char* mirrorReason()",
            "const char* mirrorText()",
            "void consoleMirrorProc(void* arg)",
            "void noteLifetimeFault(uint32_t outages, uint32_t reboots)",
            "void faultCountsSaveProc(void*)",
            "void serviceFaults(uint32_t now, bool quiet)",
        )),
        "/*@DISPATCH@*/": one(companion, "void dispatch(CompanionAction action, uint32_t now)"),
        "/*@BOOT@*/": one(companion, "void radioCompanionBootInit()"),
        "/*@CONSOLE@*/": one(companion, "const char* cmd_c6console(const String& argsInput)"),
    }
    harness = (HERE / "companion_fault_mirror_harness.cpp").read_text()
    for token, code in blocks.items():
        require(harness.count(token) == 1, token)
        harness = harness.replace(token, code)

    with tempfile.TemporaryDirectory(prefix="hw1-companion-fault-mirror-") as tmp:
        unit = Path(tmp) / "test" / "host" / "harness.cpp"
        unit.parent.mkdir(parents=True)
        shutil.copy(src / "Radio_CompanionCore.h", Path(tmp) / "Radio_CompanionCore.h")  # ../../ from test/host
        exe = Path(tmp) / "test_bin"
        unit.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                   "-Wno-unused-parameter", "-Wno-unused-const-variable", str(unit), "-o", str(exe)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True)
        run = subprocess.run([str(exe)])
    sys.exit(run.returncode)


if __name__ == "__main__":
    main()
