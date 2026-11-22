#!/usr/bin/env python3
"""Build and run the PendingCommand harness against the production file.

System_PendingCommand.cpp is compiled unchanged. Only its boundaries are
doubles, written to a temporary folder ahead of the component on the include
path: the Arduino String, the command types, the debug macros, the allocator and
the executor's submitCommandAsync / broadcastOutput. The command limits and
reply texts are the real headers. The serial console and the UART host link
must also go through PendingCommand rather than wait in submitAndExecuteSync;
the source contract at the end checks that.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

STUBS = {
    "Arduino.h": r"""
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
class String {
 public:
  String() = default;
  String(const char* t) : s_(t ? t : "") {}
  String& operator=(const char* t) { s_ = t ? t : ""; return *this; }
  const char* c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }
 private:
  std::string s_;
};
""",
    "System_CommandTypes.h": r"""
#pragma once
#include <Arduino.h>
struct AuthContext { String user; };
struct CommandContext { AuthContext auth; };
struct Command { String line; CommandContext ctx; };
typedef void (*ExecAsyncCallback)(bool ok, const char* result, void* userData);
""",
    "System_Debug.h": r"""
#pragma once
#define DEBUG_CMD_FLOWF(...) do {} while (0)
#define ERROR_COMMANDF(...) do {} while (0)
""",
    "System_MemUtil.h": r"""
#pragma once
#include <cstddef>
enum class AllocPolicy { PreferPsram };
void* ps_alloc(size_t size, AllocPolicy policy, const char* tag);
void ps_free(void* p);
""",
    "System_Utils.h": r"""
#pragma once
#include "System_CommandTypes.h"
bool submitCommandAsync(const Command& cmd, ExecAsyncCallback cb, void* userData);
bool commandQueueFull();
void broadcastOutput(const String& line);
""",
}


def source_contract() -> None:
    """Serial and the UART link hand their commands to PendingCommand."""
    main = (COMPONENT / "HardwareOne.cpp").read_text()
    uart = (COMPONENT / "System_UartLink.cpp").read_text()
    for name, text in (("HardwareOne.cpp (serial console)", main),
                       ("System_UartLink.cpp", uart)):
        code = re.sub(r"//[^\n]*", "", text)
        assert "PendingCommand" in code, f"{name} does not use PendingCommand"
    uart_code = re.sub(r"//[^\n]*", "", uart)
    assert "submitAndExecuteSync(" not in uart_code, "the UART link still waits in submitAndExecuteSync"
    serial = main[main.index("Serial auth gate"):main.index("uartLinkTick();")]
    assert "submitAndExecuteSync(" not in re.sub(r"//[^\n]*", "", serial), \
        "the serial console still waits in submitAndExecuteSync"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--skip-source-contract", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hw1-pending-") as tmp:
        stub = Path(tmp) / "stubs"
        stub.mkdir()
        for name, text in STUBS.items():
            (stub / name).write_text(text)
        # Quoted includes search the including file's folder first, so the
        # production pair is compiled from a byte-identical copy beside the
        # doubles; the real limits and reply headers come from -I COMPONENT.
        for name in ("System_PendingCommand.h", "System_PendingCommand.cpp"):
            shutil.copyfile(COMPONENT / name, stub / name)
        exe = Path(tmp) / "pending"
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I", str(stub), "-I", str(COMPONENT)]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run([args.cxx, *flags,
                        str(stub / "System_PendingCommand.cpp"),
                        str(HERE / "pending_command_harness.cpp"),
                        "-o", str(exe)], check=True, timeout=180)
        subprocess.run([str(exe)], check=True, timeout=120)
    if not args.skip_source_contract:
        source_contract()
        print("pending command: serial and UART link use it (source contract)")


if __name__ == "__main__":
    main()
