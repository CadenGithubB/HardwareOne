#!/usr/bin/env python3
"""Execute the real NTP transport with deterministic platform substitutions.

SNTP reply delivery, monotonic/wall time, WiFi and the existing Clock service
are fixtures; the production wait, callback, status and drain code is unchanged.
No network or real waits are used. An optional pre-extraction Utils source runs
the same cases for a one-time behavior comparison.
"""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
PLATFORM_HEADERS = (
    "Arduino.h", "WiFi.h", "esp_sntp.h", "esp_timer.h",
    "freertos/FreeRTOS.h", "freertos/portmacro.h", "System_Clock.h",
    "System_Debug.h", "System_Settings.h", "OLED_Display.h", "i2csensor_ds3231.h",
)
CASES = (
    "setup", "disconnected", "cold_reply", "warm_reply", "deadline_reply",
    "carryover", "prior_rtc", "rtc_fallback", "rtc_failed", "dark_timeout",
    "stopped_timeout", "late_reply", "drain_interleaving", "coalesced_replies",
)


def baseline_source(path):
    source = path.read_text()
    section = source[source.index("// NTP Time Synchronization (moved from .ino)"):]
    section = section[:section.index("#endif // ENABLE_WIFI") + len("#endif // ENABLE_WIFI")]
    return '#include "System_NTP.h"\n#include "ntp_transport_platform.h"\n' + section


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="pre-extraction System_Utils.cpp")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    sources = [("current", (COMPONENT / "System_NTP.cpp").read_text())]
    if args.baseline:
        sources.append(("baseline", baseline_source(args.baseline)))
    with tempfile.TemporaryDirectory(prefix="hw1-ntp-transport-") as temp:
        work = Path(temp)
        shutil.copyfile(COMPONENT / "System_NTP.h", work / "System_NTP.h")
        (work / "System_BuildConfig.h").write_text("#pragma once\n")
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(work)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        # No platform substitutes exist for this compile. The disabled source
        # and real API must link and run without Arduino/SDK/driver headers.
        (work / "ntp.cpp").write_text(sources[0][1])
        disabled = work / "disabled.cpp"
        disabled.write_text('''#include "System_NTP.h"
#include <cassert>
int main() {
  setupNTP();
  NtpSyncOutcome outcome = NtpSyncOutcome::Reply;
  assert(!syncNTPAndResolve(&outcome) && outcome == NtpSyncOutcome::Failed);
  assert(!syncNTPAndResolve());
  ntpSyncDrainTick();
  const auto status = getNtpRuntimeStatus();
  assert(status.replyCount == 0 && status.foregroundTimeoutCount == 0 && status.lateReplyCount == 0);
  assert(status.lastReplyEpoch == 0 && status.lastReplyAgeMs == 0);
  assert(!status.clientArmed && !status.awaitingLateReply);
}
''')
        executable = work / "ntp"
        subprocess.run(command + ["-DENABLE_WIFI=0", str(work / "ntp.cpp"), str(disabled),
                                  "-o", str(executable)], check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)
        for name in PLATFORM_HEADERS:
            path = work / name
            path.parent.mkdir(exist_ok=True)
            path.write_text('#pragma once\n#include "ntp_transport_platform.h"\n')
        shutil.copyfile(HERE / "ntp_transport_platform.h", work / "ntp_transport_platform.h")
        for label, source in sources:
            (work / "ntp.cpp").write_text(source)
            for rtc in (0, 1):
                subprocess.run(command + ["-DENABLE_WIFI=1", f"-DENABLE_RTC_SENSOR={rtc}",
                    str(work / "ntp.cpp"), str(HERE / "ntp_transport_harness.cpp"),
                    "-o", str(executable)], check=True, timeout=120)
                cases = CASES if rtc else ("dark_timeout", "late_reply", "cold_reply")
                for case in cases:
                    # A fresh process resets private transport state without
                    # adding test-only reset hooks to the production service.
                    subprocess.run([str(executable), case], check=True, timeout=60)
            print(f"NTP transport: {label} passed 14 scenarios and RTC-disabled paths")
        print("NTP transport: disabled API and source passed without platform headers")


if __name__ == "__main__":
    main()
