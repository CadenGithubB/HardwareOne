#!/usr/bin/env python3
"""Self-test of the shared fake VFS (fake_vfs.h) that persistence suites build on.

The harness drives every policy knob, fault, the lock seam, the call log and the
VFS:: forwarders, and compiles the production writeText/writeTextAtomic and
FsLockGuard against the fake unchanged. Two further translation units check that
the header also compiles without the VFS namespace or lock double, and with the
real AuthContext/CommandSource extracted from System_User.h. The shared
web_batch_stubs String double is copied into the build directory with the
String::concat(const String&) overload writeTextAtomic needs. This is test
infrastructure: it establishes nothing about device flash or the real backends.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

BARE_UNIT = """#include <Arduino.h>
#include "fake_vfs.h"
void webBatchTestStringAppend() {}
int main() {
  auto& fs = fake_vfs::fs();
  fs.reset();
  fs.put("/a", "1");
  File f = fs.open("/a", "r", false, {});
  const bool ok = f && f.read() == '1' && fs.count("open") == 1;
  f.close();
  return ok ? 0 : 1;
}
"""

REAL_AUTH_UNIT = """#include <Arduino.h>
%s
#define FAKE_VFS_EXTERNAL_AUTH_CONTEXT
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#include "fake_vfs.h"
void webBatchTestStringAppend() {}
int main() {
  auto& fs = fake_vfs::fs();
  fs.reset();
  const AuthContext ctx = VFS::systemAuth("/sd/VIDEOS", "unit");
  const bool identity = ctx.transport == SOURCE_INTERNAL && ctx.user == "system" &&
                        ctx.path == "system:unit" && ctx.scope == "/sd/VIDEOS";
  File f = VFS::openGuarded("/sd/VIDEOS/x", "w", ctx, /*create=*/true);
  const bool wrote = f && f.print("x") == 1;
  f.close();
  const fake_vfs::Call* call = fs.last("open");
  return identity && wrote && call && call->guarded && call->authScope == "/sd/VIDEOS" ? 0 : 1;
}
"""


def string_double() -> str:
    """The shared host String with the concat(String) overload writeTextAtomic calls."""
    stub = (HERE / "web_batch_stubs" / "Arduino.h").read_text()
    anchor = "  bool concat(const char* text) {"
    require(stub.count(anchor) == 1, "one concat(const char*) in the String double")
    return stub.replace(anchor, "  bool concat(const String& other) { return concat(other.c_str()); }\n" + anchor)


def real_auth_types() -> str:
    user = (COMPONENT / "System_User.h").read_text()
    return "\n".join(extract_block(user, marker) + ";" for marker in ("enum CommandSource", "struct AuthContext"))


def generate_harness() -> str:
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    writers = "\n\n".join(extract_block(utils, marker) for marker in ("bool writeText(", "bool writeTextAtomic("))
    harness = (HERE / "fake_vfs_harness.cpp").read_text()
    for marker, replacement in {"// INSERT_FS_GUARD": guard, "// INSERT_WRITERS": writers}.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    units = {
        "fake_vfs_self_test": generate_harness(),
        "fake_vfs_bare": BARE_UNIT,
        "fake_vfs_real_auth": REAL_AUTH_UNIT % real_auth_types(),
    }
    with tempfile.TemporaryDirectory(prefix="hw1-fake-vfs-") as directory:
        (Path(directory) / "Arduino.h").write_text(string_double())
        for name, text in units.items():
            source, binary = Path(directory) / f"{name}.cpp", Path(directory) / name
            source.write_text(text)
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-parameter", "-I", directory, "-I", str(HERE), "-I", str(COMPONENT),
                       str(source), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
