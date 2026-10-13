#!/usr/bin/env python3
"""G2 page and hijack-bridge public API: no entry points without a caller.

The Files, Settings, Network and Sensors pages are reached through their
interactive list (g2Show*Menu / g2*HandleTap) and the g2Build*Info text the
page registry renders. The text-dump wrappers g2ShowFilesPage,
g2ShowSettingsPage, g2ShowNetworkPage and g2ShowSensorList, the no-op
g2FilesTick, the hijack bridge's g2CurrentCmdSeq and the FSM's
hijackEventName have no caller, and g2ShowSensorsMenu has no use for a copy of
its rows (gSensorsListCache / gSensorsListCount): none of them may exist.

1. Header probe: G2_Page_Files.h, G2_Page_Settings.h, G2_Page_Network.h,
   G2_Page_Sensors.h and G2_HijackFsm.h are compiled with G2 enabled and with
   each feature gate off (the board/feature header pattern of
   test_g2_disabled_header.py). A probe namespace offers variadic fallbacks
   returning `Absent`; every removed name must resolve to that fallback, so
   neither an enabled declaration nor a disabled inline stub may remain. The
   kept API keeps its signatures, and the disabled builds link and run its
   stubs.
2. Source contract: no production .cpp/.h under the component (test/ excluded;
   comments and string literals stripped) declares, defines or uses a removed
   name; the owning files do not mention one even in a comment, and the Files
   header does not claim a hijack-tick caller.

Production sources come from --component (default: this component), so the
suite replays against a pre-fix tree. The board header comes from this
repository's boards/ folder.
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
COMPONENT = HERE.parents[1]
REPO = HERE.parents[3]

# Names that have no production caller (E10). targetNetSub, g2KickLivePageRefresh
# and G2_HIJACK_PAGE_RING live in G2_Glasses.cpp/.h and are not covered here.
REMOVED_CALLS = {
    # name: probe call expression
    "g2ShowFilesPage": "g2ShowFilesPage()",
    "g2FilesTick": "g2FilesTick()",
    "g2ShowSettingsPage": "g2ShowSettingsPage()",
    "g2ShowNetworkPage": "g2ShowNetworkPage()",
    "g2ShowSensorList": "g2ShowSensorList()",
    "hijackEventName": "hijackEventName(HijackEvent::HijackEnter)",
}
REMOVED = sorted(set(REMOVED_CALLS) | {"g2CurrentCmdSeq", "gSensorsListCache", "gSensorsListCount"})

# The modules that own the removed names: no mention at all, comments included.
OWNERS = [
    "G2_Page_Files.h", "G2_Page_Files.cpp",
    "G2_Page_Settings.h", "G2_Page_Settings.cpp",
    "G2_Page_Network.h", "G2_Page_Network.cpp",
    "G2_Page_Sensors.h", "G2_Page_Sensors.cpp",
    "G2_HijackCmd.h", "G2_HijackCmd.cpp",
    "G2_HijackFsm.h", "G2_HijackFsm.cpp",
]

PROBE = r'''
#include "G2_Page_Files.h"
#include "G2_Page_Settings.h"
#include "G2_Page_Network.h"
#include "G2_Page_Sensors.h"
#include "G2_HijackFsm.h"
#include <cstring>
#include <type_traits>

static_assert(bool(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES) == bool(EXPECT_G2),
              "feature gates took effect");

// A removed name resolves to these fallbacks; a remaining declaration or
// inline stub is an exact non-template match and wins overload resolution.
namespace removed {
struct Absent {};
@FALLBACKS@
}  // namespace removed
using namespace removed;
@ABSENT@

// The kept API: same signatures enabled and disabled.
static_assert(std::is_same_v<decltype(&g2BuildFilesInfo), void (*)(char*, size_t)>);
static_assert(std::is_same_v<decltype(&g2ShowFilesMenu), void (*)()>);
static_assert(std::is_same_v<decltype(&g2FilesHandleTap), void (*)(uint32_t)>);
static_assert(std::is_same_v<decltype(&g2BuildSettingsInfo), void (*)(char*, size_t)>);
static_assert(std::is_same_v<decltype(&g2ShowSettingsMenu), void (*)()>);
static_assert(std::is_same_v<decltype(&g2SettingsHandleTap), void (*)(uint32_t)>);
static_assert(std::is_same_v<decltype(&g2BuildNetworkInfo), void (*)(char*, size_t)>);
static_assert(std::is_same_v<decltype(&g2ShowNetworkMenu), void (*)()>);
static_assert(std::is_same_v<decltype(&g2NetworkHandleTap), void (*)(uint32_t)>);
static_assert(std::is_same_v<decltype(&g2BuildSensorList), void (*)(char*, size_t)>);
static_assert(std::is_same_v<decltype(&g2ShowSensorsMenu), void (*)()>);
static_assert(std::is_same_v<decltype(&g2SensorsHandleTap), void (*)(uint32_t)>);
static_assert(std::is_same_v<decltype(&hijackStateName), const char* (*)(HijackState)>);
static_assert(std::is_same_v<decltype(&hijackFsmState), HijackState (*)()>);

int main() {
  char text[8];
  int failures = 0;
  void (*const builders[])(char*, size_t) = {
      g2BuildFilesInfo, g2BuildSettingsInfo, g2BuildNetworkInfo, g2BuildSensorList};
  for (auto build : builders) {
    std::memset(text, '!', sizeof(text));
    build(text, sizeof(text));
    if (!EXPECT_G2 && text[0] != '\0') ++failures;  // disabled stubs: empty text
  }
  g2ShowFilesMenu();
  g2ShowSettingsMenu();
  g2ShowNetworkMenu();
  g2ShowSensorsMenu();
  g2FilesHandleTap(1);
  g2SettingsHandleTap(1);
  g2NetworkHandleTap(1);
  g2SensorsHandleTap(1);
  if (!EXPECT_G2 && std::strcmp(hijackStateName(hijackFsmState()), "Idle") != 0) ++failures;
  return failures;
}
'''

GATES = (
    # (label, ENABLE_BLUETOOTH, ENABLE_G2_GLASSES)
    ("G2 enabled", 1, 1),
    ("ENABLE_G2_GLASSES=0", 1, 0),
    ("ENABLE_BLUETOOTH=0", 0, 1),
)

LEXEMES = re.compile(r'''
    (?:u8|u|U|L)?R"(?P<delim>[^()\\\s]{0,16})\(.*?\)(?P=delim)"   # raw string
  | //[^\n]*                                                     # line comment
  | /\*.*?\*/                                                    # block comment
  | (?:u8|u|U|L)?"(?:\\.|[^"\\\n])*"                             # string literal
  | (?:u8|u|U|L)?'(?:\\.|[^'\\\n])*'                             # character literal
''', re.S | re.X)


def probe_source() -> str:
    fallbacks = "\n".join(f"template <typename... T> Absent {name}(T&&...);"
                          for name in REMOVED_CALLS)
    absent = "\n".join(
        f"static_assert(std::is_same_v<decltype({call}), removed::Absent>,\n"
        f"              \"{name} is not declared (no caller)\");"
        for name, call in REMOVED_CALLS.items())
    return PROBE.replace("@FALLBACKS@", fallbacks).replace("@ABSENT@", absent)


def header_probe(component: Path, cxx: str, sanitize: bool) -> list[str]:
    failures = []
    board = REPO / "boards" / "xiao_s3.features.h"
    with tempfile.TemporaryDirectory(prefix="hw1-g2-page-api-") as temp:
        directory = Path(temp)
        (directory / "Arduino.h").write_text("#include <stdint.h>\n#include <stddef.h>\n")
        (directory / "sdkconfig.h").write_text(
            '#define CONFIG_IDF_TARGET "esp32s3"\n#define CONFIG_IDF_TARGET_ESP32S3 1\n'
            '#define CONFIG_BT_ENABLED 1\n#define CONFIG_BT_BLUEDROID_ENABLED 1\n')
        unit = directory / "probe.cpp"
        unit.write_text(probe_source())
        for label, bluetooth, glasses in GATES:
            enabled = int(bluetooth and glasses)
            profile = directory / f"features_{bluetooth}{glasses}.h"
            profile.write_text(
                f"#undef ENABLE_BLUETOOTH\n#define ENABLE_BLUETOOTH {bluetooth}\n"
                f"#undef ENABLE_G2_GLASSES\n#define ENABLE_G2_GLASSES {glasses}\n")
            output = directory / (f"probe_{bluetooth}{glasses}" + (".o" if enabled else ""))
            command = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-I", str(component), "-I", str(directory),
                       "-DARDUINO_XIAO_ESP32S3_DEV=1", f"-DEXPECT_G2={enabled}",
                       f'-DHW1_BOARD_CONFIG_HEADER="{board}"',
                       f'-DHW1_FEATURE_CONFIG_HEADER="{profile}"']
            if sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            # Enabled: the page modules are not linked, so stop at an object file.
            command += (["-c"] if enabled else []) + [str(unit), "-o", str(output)]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if compiled.returncode != 0:
                asserts = sorted(set(re.findall(r"\w+ is not declared \(no caller\)",
                                                compiled.stderr)))
                if asserts:
                    failures += [f"{label}: {message}" for message in asserts]
                else:
                    failures.append(f"{label}: probe failed to compile:\n{compiled.stderr}")
                continue
            if not enabled:
                ran = subprocess.run([str(output)], capture_output=True, text=True, timeout=60)
                if ran.returncode != 0:
                    failures.append(f"{label}: disabled stubs misbehaved "
                                    f"(exit {ran.returncode})\n{ran.stderr}")
    return failures


def production_files(component: Path):
    for path in sorted(component.rglob("*")):
        if path.suffix not in (".cpp", ".h") or not path.is_file():
            continue
        relative = path.relative_to(component)
        if relative.parts[0] == "test" or relative.name.startswith("._"):
            continue
        yield relative, path


def source_contract(component: Path) -> list[str]:
    failures = []
    for relative, path in production_files(component):
        text = path.read_text(errors="replace")
        if not any(name in text for name in REMOVED):
            continue
        code = LEXEMES.sub(" ", text)
        for name in REMOVED:
            if re.search(rf"\b{name}\b", code):
                failures.append(f"{relative}: code still names {name}")
    for owner in OWNERS:
        text = (component / owner).read_text(errors="replace")
        for name in REMOVED:
            if re.search(rf"\b{name}\b", text) and \
                    not re.search(rf"\b{name}\b", LEXEMES.sub(" ", text)):
                failures.append(f"{owner}: comment or string still names {name}")
    if "Called by the hijack tick" in (component / "G2_Page_Files.h").read_text():
        failures.append("G2_Page_Files.h: comment claims a hijack-tick caller")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory to read production sources from")
    args = parser.parse_args()
    if not args.cxx:
        sys.exit("a host C++17 compiler is required")
    component = args.component.resolve()
    failures = header_probe(component, args.cxx, args.sanitize)
    failures += source_contract(component)
    if failures:
        print("G2 page API contract FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        sys.exit(1)
    print(f"G2 page API contract: header probe ({len(GATES)} gate states) and "
          f"source contract ({len(REMOVED)} names) passed")


if __name__ == "__main__":
    main()
