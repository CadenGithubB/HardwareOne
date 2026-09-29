#!/usr/bin/env python3
"""Compile/run the real SDMMC lifecycle with fault-injected SDK calls."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
host = Path(__file__).resolve().parent
component = host.parent.parent
with tempfile.TemporaryDirectory(prefix="hw1-sdmmc-") as tmp:
    exe = Path(tmp) / "lifecycle"
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run([args.cxx, *flags, "-I" + str(host / "sdmmc_stubs"),
                    str(host / "test_sdmmc_lifecycle.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    # A disabled card must compile to an empty backend even if the board
    # requests SDMMC. No Arduino/SDK include path is supplied on purpose.
    subprocess.run([args.cxx, *flags, "-DSYSTEM_BUILDCONFIG_H",
                    "-DENABLE_SDMMC_CARD=1", "-DENABLE_SD_CARD=0", "-c",
                    str(component / "HAL_SDCard.cpp"),
                    "-o", str(Path(tmp) / "disabled.o")], check=True)
print("SDMMC lifecycle, failure cleanup, removal, format, and disabled-build tests passed")
