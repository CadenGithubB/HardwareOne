#!/usr/bin/env python3
"""Source contract: the C6 companion's reboot escalation uses the shared reboot owner.

dispatch(HardReboot) in System_RadioCompanion.cpp must restart through
rebootDevice() (System_Utils.cpp), which finalizes an active microphone
recording, records the reboot reason for the next boot, writes the REBOOT audit
line and flushes the event log. A direct esp_restart() skips all of that. The
escalation keeps its own companion event, C6 log line and the RTC marker that
stops a second automatic reboot. Source checks only; the restart itself is a
device check.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re

from test_web_batch_handlers import require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def case_body(source: str, label: str) -> str:
    start = source.index(label)
    end = source.index("case CompanionAction::", start + len(label))
    return source[start:end]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", help="accepted for the shared ctest arguments; unused")
    parser.add_argument("--sanitize", action="store_true", help="accepted; unused")
    parser.parse_args()

    companion = strip_comments((COMPONENT / "System_RadioCompanion.cpp").read_text())
    require("esp_restart(" not in companion,
            "System_RadioCompanion.cpp must not call esp_restart() directly")
    require("ESP.restart(" not in companion,
            "System_RadioCompanion.cpp must not call ESP.restart() directly")

    label = "case CompanionAction::HardReboot:"
    require(companion.count(label) == 1, "one HardReboot dispatch case")
    body = case_body(companion, label)
    reboot = body.find('rebootDevice("c6-companion",')
    require(reboot >= 0, "the escalation reboots through rebootDevice with the c6-companion reason")
    require(re.search(r'rebootDevice\("c6-companion",\s*detail,\s*750\)', body) is not None,
            "the escalation keeps its 750 ms flush delay and passes the outage detail")
    marker = body.find("gCompanionRebootMarker = kRebootMarker;")
    require(0 <= marker < reboot, "the RTC marker is set before the restart")
    for kept in ("systemEventPost(SYSEVT_COMPANION_REBOOT", 'logSystemEvent("C6", "rebooting:'):
        position = body.find(kept)
        require(0 <= position < reboot, f"the escalation still reports before restarting: {kept}")

    utils_header = (COMPONENT / "System_Utils.h").read_text()
    require("void rebootDevice(const char* reason, const char* auditDetail, uint32_t flushDelayMs);"
            in utils_header, "rebootDevice keeps the signature the escalation calls")
    print("companion reboot contract: escalation restarts through rebootDevice")


if __name__ == "__main__":
    main()
