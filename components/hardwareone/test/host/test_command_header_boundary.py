#!/usr/bin/env python3
"""Compile the real command interfaces without any ESP-IDF include paths.

Registry metadata must work with an incomplete String; the parser interface
needs only the real Arduino String declaration. Dependency files also prevent
either interface from importing unrelated project headers as the tree grows.
This checks the declaration boundary and constructor security guard, not device
command execution or authorization (covered by the command regression tests).
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[2]
ARDUINO_CORE = COMPONENT.parent / "arduino" / "cores" / "esp32"
# The boundary is about project headers. The Arduino core may be a symlink
# into another checkout, so its resolved location counts as inside.
REPOSITORY_ROOTS = {COMPONENT.parents[1].resolve(), (COMPONENT.parent / "arduino").resolve()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")

    with tempfile.TemporaryDirectory(prefix="hw1-command-boundary-") as temp:
        work = Path(temp)

        def compile_case(name: str, source: str, allowed: set[str],
                         *, string_header: bool = False) -> None:
            unit = work / f"{name}.cpp"
            dependency_file = work / f"{name}.d"
            executable = work / name
            unit.write_text(source, encoding="utf-8")
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-pedantic", "-I", str(COMPONENT)]
            if string_header:
                # The actual String header is portable. Arduino.h would drag in
                # the SDK and fail this compile; no platform stubs hide that.
                command += ["-I", str(ARDUINO_CORE)]
            command += ["-MMD", "-MF", str(dependency_file), str(unit),
                        "-o", str(executable)]
            subprocess.run(command, check=True)
            dependencies = dependency_file.read_text().replace("\\\n", " ")
            paths = {Path(path).resolve() for path in shlex.split(
                dependencies.split(":", 1)[1])}
            expected = {unit.resolve(), *((COMPONENT / path).resolve()
                                         for path in allowed)}
            # -MMD leaves out system headers, but Apple clang still lists files
            # such as the SDK's SDKSettings.json. Only unexpected paths inside
            # the repository change the boundary; everything outside is ignored.
            paths = {path for path in paths if path in expected or any(
                root == path or root in path.parents for root in REPOSITORY_ROOTS)}
            assert paths == expected, (
                f"{name} header boundary changed: "
                f"unexpected={paths - expected}; missing={expected - paths}"
            )
            subprocess.run([str(executable)], check=True)

        compile_case("registry", r'''
#include "System_CommandRegistry.h"
#include "System_CommandRegistry.h"
#include <assert.h>
#include <type_traits>

// Defining String after the include proves metadata only needs its name.
class String {};
static const char* reply(const String&) { return "OK"; }
using Handler = const char* (*)(const String&);

constexpr CommandEntry entries[] = {
    {"public", "help", false, reply},
    {"admin", "help", true, reply, "usage"},
    {"super", "help", true, reply, "usage", true},
};
static_assert(!entries[0].requiresAdmin && !entries[0].requiresSuperAdmin);
static_assert(entries[0].usage == nullptr);
static_assert(entries[1].requiresAdmin && !entries[1].requiresSuperAdmin);
static_assert(entries[2].requiresAdmin && entries[2].requiresSuperAdmin);
static_assert(std::is_standard_layout<CommandEntry>::value);
static_assert(std::is_trivially_copyable<CommandEntry>::value);
// A legacy phrase in the final slot must never silently become super-admin.
static_assert(!std::is_constructible<CommandEntry, const char*, const char*,
              bool, Handler, const char*, const char*>::value);
static_assert(std::is_constructible<CommandEntry, const char*, const char*,
              bool, Handler, const char*, bool>::value);
static_assert(CMD_MODULE_SENSOR == 0x01 && CMD_MODULE_CORE == 0x02);
// Nothing read the admin and network module bits, so they are gone; access
// control lives in each command's requiresAdmin/requiresSuperAdmin columns.
#if defined(CMD_MODULE_ADMIN) || defined(CMD_MODULE_NETWORK)
#error "CMD_MODULE_ADMIN/CMD_MODULE_NETWORK are retired module flags"
#endif

int main() {
  const size_t count = sizeof(entries) / sizeof(entries[0]);
  const CommandModule module = {"test", nullptr, nullptr, entries, &count,
                                CMD_MODULE_CORE, nullptr};
  const CommandModule empty = {"empty", nullptr, nullptr, nullptr, nullptr,
                               CMD_MODULE_SENSOR, nullptr};
  assert(module.commandCount() == 3);
  assert(empty.commandCount() == 0);
  String args;
  assert(module.commands[0].handler(args)[0] == 'O');
}
''', {"System_CommandRegistry.h"})

        compile_case("ota_declarations", r'''
#include "System_OTA.h"
int main() { return 0; }
''', {"System_OTA.h", "System_CommandRegistry.h"})

        compile_case("command_declarations", r'''
#include "System_Command.h"
#include "System_Command.h"
#include <assert.h>
#include <type_traits>

static_assert(sizeof(CommandArgs) >= sizeof(String));
static_assert(std::is_same<decltype(resolveCommand(*(const String*)nullptr)),
                           CommandResolution>::value);
int main() {
  CommandResolution resolution;
  assert(!resolution);
  assert(resolution.matchedLength == 0 && resolution.lineOffset == 0);
  assert(resolution.registryOrdinal == static_cast<size_t>(-1));
}
''', {"System_Command.h", "System_CommandRegistry.h",
      "../arduino/cores/esp32/WString.h", "../arduino/cores/esp32/pgmspace.h"},
             string_header=True)

    print("command header boundary: passed (registry, OTA, parser)")


if __name__ == "__main__":
    main()
