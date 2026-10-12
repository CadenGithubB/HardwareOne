#!/usr/bin/env python3
"""Exercise the actual streaming upload handler with injected boundary failures.

Only Arduino/HTTP/auth/storage/allocation boundaries are mocked. The handler and
filesystem guard are extracted unchanged; PollPauseGuard is the production header.
--baseline accepts a saved WebServer_Server.cpp for before/after reproductions.
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--case", default="all", choices=("all", "invalid-open", "disconnect", "binary", "nested", "missing-content", "timeout", "replace"))
    parser.add_argument("--maps", default="both", choices=("both", "on", "off"))
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++23 compiler is required")
    source = (args.baseline or COMPONENT / "WebServer_Server.cpp").read_text()
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    guard = extract_block((COMPONENT / "System_Mutex.h").read_text(), "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()", "void fsLock(", "void fsUnlock("))
    handler = extract_block(source, "esp_err_t handleFileUpload(")
    harness = (HERE / "upload_lifecycle_harness.cpp").read_text()
    for marker, replacement in {"// INSERT_FS_GUARD": guard, "// INSERT_UPLOAD_HANDLER": handler}.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="hw1-upload-lifecycle-") as directory:
        generated = Path(directory) / "test.cpp"
        generated.write_text(harness)
        for maps in ((0, 1) if args.maps == "both" else (int(args.maps == "on"),)):
            binary = Path(directory) / f"test-{maps}"
            command = [args.cxx, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
                       "-Wno-unused-but-set-variable", f"-DENABLE_WEB_MAPS={maps}",
                       "-I", str(COMPONENT), str(generated), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(binary), args.case], check=True, timeout=30)


if __name__ == "__main__":
    main()
