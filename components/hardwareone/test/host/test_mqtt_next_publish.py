#!/usr/bin/env python3
"""`mqttstatus` prints the time to the next publish, 0 when it is already due.

The line was `(lastPublishTime + interval) - millis()`. The command runs on the
command task while the main loop owns the stamp: after the due time and before
the loop had published, the unsigned subtraction wrapped and the line read
about 4294967000 ms. mqttMsUntilNextPublish (System_MQTT.cpp) is compiled and
run: counting down, due, overdue, and across the millis() wrap. Source rules:
cmd_mqttstatus prints that function's result, and copies the stamp before it
reads the clock.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases against another components/hardwareone directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
OWNER = "static uint32_t mqttMsUntilNextPublish(uint32_t lastPublishMs, uint32_t intervalMs, uint32_t nowMs)"

HARNESS = r"""
#include <cstdint>
#include <cstdio>

// INSERT_PRODUCTION

static int failures = 0, checks = 0;
static void expect(uint32_t last, uint32_t interval, uint32_t now, uint32_t want, const char* what) {
  ++checks;
  const uint32_t got = mqttMsUntilNextPublish(last, interval, now);
  if (got == want) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s: got %lu ms, expected %lu ms\n", what,
               static_cast<unsigned long>(got), static_cast<unsigned long>(want));
}

int main() {
  expect(100000, 10000, 100000, 10000, "just published");
  expect(100000, 10000, 104000, 6000, "4 s after a publish");
  expect(100000, 10000, 109999, 1, "1 ms before it is due");
  expect(100000, 10000, 110000, 0, "due now");
  // The fault: the command lands after the due time, before the loop publishes.
  expect(100000, 10000, 110300, 0, "300 ms overdue");
  expect(100000, 10000, 170000, 0, "a minute overdue (the client was busy)");
  // Never published since boot (stamp 0) on a device that has been up a while.
  expect(0, 10000, 3600000, 0, "no publish yet");
  // Across the millis() wrap.
  expect(0xFFFFF000u, 10000, 0xFFFFFF00u, 10000 - 0xF00, "before the wrap");
  expect(0xFFFFF000u, 10000, 0x00000800u, 10000 - 0x1800, "after the wrap, still counting down");
  expect(0xFFFFF000u, 10000, 0x00002000u, 0, "after the wrap, overdue");
  expect(5000, 300000, 5000, 300000, "the longest interval");
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d next-publish checks failed\n", failures, checks);
    return 1;
  }
  std::printf("MQTT next publish: %d checks passed\n", checks);
  return 0;
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    mqtt = (args.component.resolve() / "System_MQTT.cpp").read_text()
    require(mqtt.count(OWNER) == 1, "System_MQTT.cpp has mqttMsUntilNextPublish, the owner of the 'Next Publish' value")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-mqtt-next-publish-") as directory:
        source = Path(directory) / "next_publish.cpp"
        binary = Path(directory) / "next_publish"
        source.write_text(HARNESS.replace("// INSERT_PRODUCTION", extract_block(mqtt, OWNER)))
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=60)
    status = extract_block(mqtt, "const char* cmd_mqttstatus(const String& argsInput)")
    require("- millis()" not in status, "cmd_mqttstatus subtracts millis() from nothing itself")
    copied = re.search(r"const uint32_t lastPublishMs = \(uint32_t\)lastPublishTime;", status)
    printed = re.search(r"nextPublish = mqttMsUntilNextPublish\(\s*lastPublishMs,[^;]*millis\(\)\);", status)
    require(copied is not None and printed is not None and copied.start() < printed.start(),
            "cmd_mqttstatus copies the stamp, then reads the clock, and prints the owner's result")
    require('BROADCAST_PRINTF("Next Publish: %lu ms", nextPublish);' in status, "the reply line is unchanged")
    print("mqttstatus prints 0 for a publish that is already due", flush=True)


if __name__ == "__main__":
    main()
