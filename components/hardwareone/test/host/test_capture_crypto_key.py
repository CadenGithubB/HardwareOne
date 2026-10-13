#!/usr/bin/env python3
"""Exercise the real capture-key loader against scripted NVS and libsodium fakes.

Only ESP_ERR_NVS_NOT_FOUND (namespace or blob missing) may mint a new key. Any
other read-only open failure, or an unreadable stored blob, must fail closed
without calling nvs_set_blob. Sealing, opening and the PSRAM scratch are not
covered; the NVS fake models return codes, not flash wear or encryption.
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


def generate() -> str:
    source = (COMPONENT / "System_CaptureCrypto.cpp").read_text()
    marker = "bool captureCryptoEnsureKey("
    require(source.count(marker) == 1, "one captureCryptoEnsureKey definition")
    loader = extract_block(source, marker)
    harness = (HERE / "capture_crypto_key_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    return harness.replace("// INSERT_PRODUCTION", loader)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-capture-key-") as directory:
        work = Path(directory)
        source, binary = work / "test.cpp", work / "test"
        source.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
