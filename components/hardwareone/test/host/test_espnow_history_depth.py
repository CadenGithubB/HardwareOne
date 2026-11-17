#!/usr/bin/env python3
"""The ESP-NOW message depth follows CONFIG_SPIRAM on every target (RT-3, D-45).

MESSAGES_PER_DEVICE (System_ESPNow.h) and kWebMessagesBufCount
(WebPage_ESPNow.cpp) are chosen by a preprocessor test. This suite takes each
#if/#else/#endif block out of the production file, unchanged, and preprocesses
and compiles it under the option sets the supported targets really have:

- ESP32-P4 with PSRAM: CONFIG_SPIRAM only. ESP-IDF 5.5.5 has no legacy alias
  for it (sdkconfig.rename.esp32 and .esp32s3 are the only files that define
  CONFIG_SPIRAM_SUPPORT / CONFIG_ESP32S3_SPIRAM_SUPPORT), so a test of the
  aliases leaves the P4 at the no-PSRAM depth: 16 records and 15 per poll.
- esp32 and esp32s3 with PSRAM: CONFIG_SPIRAM plus the target's alias.
- no PSRAM: none of them.

The depths are 250/50 with PSRAM and 16/15 without, the classic boards'
values before and after. The source is also read for: no legacy alias in any
preprocessor test of the component, and one boot line that prints the depth
where the history is allocated.

Not covered: the size of the blocks on a device (a 64-bit host lays the
record out differently) and where the allocator places them.

--component DIR reads another tree (the tree before this change fails the P4
depth, the alias and the boot-line checks).
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

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent

LEGACY = ("CONFIG_SPIRAM_SUPPORT", "CONFIG_ESP32_SPIRAM_SUPPORT", "CONFIG_ESP32S2_SPIRAM_SUPPORT",
          "CONFIG_ESP32S3_SPIRAM_SUPPORT")
# (label, -D options, expected MESSAGES_PER_DEVICE, expected kWebMessagesBufCount)
TARGETS = (
    ("esp32p4 with PSRAM", ["-DCONFIG_SPIRAM=1"], 250, 50),
    ("esp32 with PSRAM", ["-DCONFIG_SPIRAM=1", "-DCONFIG_SPIRAM_SUPPORT=1",
                          "-DCONFIG_ESP32_SPIRAM_SUPPORT=1"], 250, 50),
    ("esp32s3 with PSRAM", ["-DCONFIG_SPIRAM=1", "-DCONFIG_ESP32S3_SPIRAM_SUPPORT=1"], 250, 50),
    ("no PSRAM", [], 16, 15),
)


def conditional(source: str, anchor: str, name: str) -> str:
    """The #if ... #endif block that holds `anchor`, as written."""
    assert source.count(anchor) == 2, f"{name}: expected two branches of {anchor!r}"
    at = source.index(anchor)
    begin = source.rindex("\n#if", 0, at) + 1
    end = source.index("#endif", at) + len("#endif")
    block = source[begin:end]
    assert block.count("#if") == 1 and block.count("#else") == 1, f"{name}: one #if/#else"
    return block


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    assert args.cxx, "a host C++17 compiler is required"
    component = args.component.resolve()
    header = (component / "System_ESPNow.h").read_text()
    web = (component / "WebPage_ESPNow.cpp").read_text()
    espnow = (component / "System_ESPNow.cpp").read_text()

    failures = total = 0

    def check(name: str, ok: bool, detail: str = "") -> None:
        nonlocal failures, total
        total += 1
        if ok:
            print(f"PASS {name}")
        else:
            failures += 1
            print(f"FAIL {name}{': ' + detail if detail else ''}")

    unit = ("#include <cstdio>\n"
            + conditional(header, "#define MESSAGES_PER_DEVICE ", "System_ESPNow.h") + "\n"
            + conditional(web, "static constexpr int kWebMessagesBufCount = ", "WebPage_ESPNow.cpp")
            + "\nint main() { std::printf(\"%d %d\\n\", (int)MESSAGES_PER_DEVICE, "
              "kWebMessagesBufCount); return 0; }\n")
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-history-depth-") as temporary:
        directory = Path(temporary)
        (directory / "depth.cpp").write_text(unit)
        for index, (label, options, depth, per_poll) in enumerate(TARGETS):
            binary = directory / f"depth{index}"
            subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *options,
                            str(directory / "depth.cpp"), "-o", str(binary)],
                           check=True, timeout=120)
            got = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30).stdout.split()
            check(f"{label}: {depth} records per ring, {per_poll} per web poll",
                  got == [str(depth), str(per_poll)], "got " + " and ".join(got))

    # No preprocessor test anywhere in the component names a legacy alias.
    users = []
    for path in sorted(list(component.glob("*.h")) + list(component.glob("*.cpp"))):
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if re.match(r"\s*#\s*(if|elif|ifdef|ifndef)\b", line) and any(n in line for n in LEGACY):
                users.append(f"{path.name}:{number}")
    check("no preprocessor test names a legacy PSRAM alias", not users, ", ".join(users))

    # The trailing space skips the forward declaration, which ends in "();".
    init = extract_block(espnow, "static size_t peerHistoryStoreInit() ")
    lines = re.findall(r'BROADCAST_PRINTF\("(\[ESP-NOW\] Message history depth: [^"]*)",\s*'
                       r'\(int\)MESSAGES_PER_DEVICE\b', init)
    check("ESP-NOW start prints the depth once", len(lines) == 1,
          f"{len(lines)} depth line(s) in peerHistoryStoreInit")

    print(f"espnow history depth: {total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
