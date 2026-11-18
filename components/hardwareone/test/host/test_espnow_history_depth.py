#!/usr/bin/env python3
"""The ESP-NOW message-history budget follows CONFIG_SPIRAM on every target (RT-3, D-45).

The history keeps, per peer, a received and a sent byte ring whose size comes
from the setting espnowhistorykb (0 = default). The defaults, board maximum,
total cap and on-device view size (ESPNOW_HISTORY_KB_DEFAULT / _KB_MAX /
_TOTAL_KB / _VIEW_MAX, System_ESPNow.h) and the web poll count
(kWebMessagesBufCount, WebPage_ESPNow.cpp) are chosen by a preprocessor test.
This suite takes each #if/#else/#endif block out of the production file,
unchanged, and preprocesses and compiles it under the option sets the
supported targets really have:

- ESP32-P4 with PSRAM: CONFIG_SPIRAM only. ESP-IDF 5.5.5 has no legacy alias
  for it (sdkconfig.rename.esp32 and .esp32s3 are the only files that define
  CONFIG_SPIRAM_SUPPORT / CONFIG_ESP32S3_SPIRAM_SUPPORT), so a test of the
  aliases would leave the P4 at the no-PSRAM budget.
- esp32 and esp32s3 with PSRAM: CONFIG_SPIRAM plus the target's alias.
- no PSRAM: none of them.

The values are 16/32/1024/512 KB-default/max/total-KB/view and 50 per poll
with PSRAM, 2/4/64/32 and 15 without; the _NOPSRAM values (used at runtime
when a PSRAM build finds no PSRAM) are 2/4/64 everywhere. The source is also
read for: no legacy alias in any preprocessor test of the component, and one
start line in peerHistoryStoreInit that says the budget, decided from the PSRAM
found at runtime, with no allocation there (blocks come with a peer's first
message; the depth line of the fixed rings is gone).

Not covered: the size of the blocks on a device and where the allocator places
them (test_espnow_message_store.py runs the store against doubles).

--component DIR reads another tree.
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
# (label, -D options, expected KB_DEFAULT KB_MAX TOTAL_KB VIEW_MAX, expected kWebMessagesBufCount)
PSRAM = (16, 32, 1024, 512)
NO_PSRAM = (2, 4, 64, 32)
TARGETS = (
    ("esp32p4 with PSRAM", ["-DCONFIG_SPIRAM=1"], PSRAM, 50),
    ("esp32 with PSRAM", ["-DCONFIG_SPIRAM=1", "-DCONFIG_SPIRAM_SUPPORT=1",
                          "-DCONFIG_ESP32_SPIRAM_SUPPORT=1"], PSRAM, 50),
    ("esp32s3 with PSRAM", ["-DCONFIG_SPIRAM=1", "-DCONFIG_ESP32S3_SPIRAM_SUPPORT=1"], PSRAM, 50),
    ("no PSRAM", [], NO_PSRAM, 15),
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

    nopsram = "\n".join(line for line in header.splitlines()
                         if re.match(r"#define ESPNOW_HISTORY_\w+_NOPSRAM\b", line))
    unit = ("#include <cstdio>\n" + nopsram + "\n"
            + conditional(header, "  #define ESPNOW_HISTORY_KB_DEFAULT ", "System_ESPNow.h") + "\n"
            + conditional(web, "static constexpr int kWebMessagesBufCount = ", "WebPage_ESPNow.cpp")
            + "\nint main() { std::printf(\"%d %d %d %d %d %d %d %d\\n\", (int)ESPNOW_HISTORY_KB_DEFAULT, "
              "(int)ESPNOW_HISTORY_KB_MAX, (int)ESPNOW_HISTORY_TOTAL_KB, (int)ESPNOW_HISTORY_VIEW_MAX, "
              "kWebMessagesBufCount, (int)ESPNOW_HISTORY_KB_DEFAULT_NOPSRAM, (int)ESPNOW_HISTORY_KB_MAX_NOPSRAM, "
              "(int)ESPNOW_HISTORY_TOTAL_KB_NOPSRAM); return 0; }\n")
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-history-depth-") as temporary:
        directory = Path(temporary)
        (directory / "depth.cpp").write_text(unit)
        for index, (label, options, budget, per_poll) in enumerate(TARGETS):
            binary = directory / f"depth{index}"
            subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *options,
                            str(directory / "depth.cpp"), "-o", str(binary)],
                           check=True, timeout=120)
            got = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30).stdout.split()
            want = [str(v) for v in (*budget, per_poll, *NO_PSRAM[:3])]
            check(f"{label}: {budget[0]} KB per ring by default, at most {budget[1]} KB, {budget[2]} KB total, "
                  f"{budget[3]} refs per view, {per_poll} per web poll; no-PSRAM values 2/4/64",
                  got == want, "got " + " ".join(got))

    # No preprocessor test anywhere in the component names a legacy alias.
    users = []
    for path in sorted(list(component.glob("*.h")) + list(component.glob("*.cpp"))):
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if re.match(r"\s*#\s*(if|elif|ifdef|ifndef)\b", line) and any(n in line for n in LEGACY):
                users.append(f"{path.name}:{number}")
    check("no preprocessor test names a legacy PSRAM alias", not users, ", ".join(users))

    # The trailing space skips the forward declaration, which ends in ");".
    init = extract_block(espnow, "static void peerHistoryStoreInit(uint8_t settingKb) ")
    lines = re.findall(r'BROADCAST_PRINTF\("\[ESP-NOW\] Message history: [^"]*"', init)
    check("ESP-NOW start says the history budget once", len(lines) == 1,
          f"{len(lines)} start line(s) in peerHistoryStoreInit")
    check("no depth line of the fixed rings is left", "Message history depth" not in espnow)
    check("ESP-NOW start allocates no history", "ps_alloc" not in init)
    check("the budget follows the PSRAM found at runtime", "psramAvailableRuntime()" in init)
    check("the old fixed depth is gone", not re.search(r"\bMESSAGES_PER_DEVICE\b", header + web + espnow))

    print(f"espnow history depth: {total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
