#!/usr/bin/env python3
"""The lens words of a refused command (G2_ReplyNotice.h).

A G2 page that runs a command shows why the firmware refused it: "Needs an
admin" for the admin refusals (the logging one included), "Needs a super
admin" for the super-admin refusal, otherwise the reason's first sentence
("This recording belongs to alice"). A completed command has no notice. The
unit compiles the pure header against the real System_CommandReply.h replies.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

UNIT = r'''
#include <cstdio>
#include <cstring>
#include "G2_ReplyNotice.h"

static unsigned checks = 0, failures = 0;
static void expect(bool ok, const char* reply, const char* want) {
  char out[kG2NoticeMax];
  const bool refused = g2ReplyNoticeText(ok, reply, out, sizeof(out));
  ++checks;
  const bool pass = want ? (refused && std::strcmp(out, want) == 0)
                         : (!refused && out[0] == '\0');
  if (!pass) {
    ++failures;
    std::fprintf(stderr, "FAIL ok=%d reply='%s': got %d '%s', want '%s'\n",
                 (int)ok, reply ? reply : "(null)", (int)refused, out, want ? want : "(none)");
  }
}

int main() {
  // Completed commands: no notice.
  expect(true, "OK: LED set to red", nullptr);
  expect(true, "SUCCESS: Logging started", nullptr);
  expect(true, "{\"schema\":1,\"ok\":true}", nullptr);
  // The executor's privilege refusals.
  expect(false, "Error: Admin access required for command 'openmic'. Contact an administrator.",
         "Needs an admin");
  expect(false, kCommandReplyLoggingAdminRequired, "Needs an admin");
  expect(false, "Error: Super-admin access required for command 'espnowunpair'.",
         "Needs a super admin");
  // Other failures: the first sentence of the reason.
  expect(false, "Error: This recording belongs to alice. Only alice or an administrator can stop it.",
         "This recording belongs to alice");
  expect(false, "Error: This recording was started by the device (the glasses or another screen). "
                "An administrator can stop it.",
         "This recording was started by the device");
  expect(false, "Error: Insufficient memory for ESP-NOW (need ~40KB DRAM + ~320KB PSRAM)",
         "Insufficient memory for ESP-NOW");
  expect(false, "Error: Recording not started: SD card not writable. Check sdinfo and retry.",
         "Recording not started: SD card not writable");
  expect(false, kCommandReplyNotADecimal, "Not a number");
  expect(false, "Usage: g2 list push", "Usage: g2 list push");
  // ok=true with a failure lead is still a refusal; an empty failure is named.
  expect(true, "Error: Not permitted", "Not permitted");
  expect(false, "", "Command failed");
  expect(false, nullptr, "Command failed");
  expect(false, "Error: ", "Command failed");
  // The predicate and a short buffer.
  ++checks; if (g2ReplyRefused(true, "OK: done") || !g2ReplyRefused(false, "Error: x")) ++failures;
  char small[8];
  ++checks;
  if (!g2ReplyNoticeText(false, "Error: Admin access required", small, sizeof(small)) ||
      std::strcmp(small, "Needs a") != 0) ++failures;
  if (failures) {
    std::fprintf(stderr, "G2 reply notice: %u of %u checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("G2 reply notice: %u checks passed\n", checks);
  return 0;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(args.component.resolve())]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-g2-reply-notice-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(UNIT)
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)], check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
