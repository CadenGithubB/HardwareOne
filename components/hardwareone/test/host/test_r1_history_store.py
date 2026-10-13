#!/usr/bin/env python3
"""Exercise complete production R1 history storage with controlled failure boundaries.

Compiles the complete store body/header plus the real FsLockGuard, Arduino
String and Print. VFS, semaphores, crypto and time are observable test doubles.
The current run also compiles the Health history model that drives the store
(the pending-sweep load/merge/commit path of G2_Health.cpp) to check how a
stored day that cannot be read is handled end to end.
No physical flash durability, cryptographic implementation or thread scheduler
is simulated. Optional saved-baseline runs compare normal bytes and demonstrate
previously unchecked completion/lock ownership failures. --component runs the
current expectations against another tree's production sources.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
CORE = COMPONENT.parent / "arduino/cores/esp32"


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def model(root: Path) -> tuple[str, str]:
    """Public types and the production Health history model that drives the store."""
    strip = re.compile(r'^#include [<"](?:System_BuildConfig|Arduino|R1_HealthHistoryStore|'
                       r'System_R1_Protocol)\.h[>"]\n', re.M)
    # Header constants the model does not use stay quiet, as they would in a header.
    types = ("#define ENABLE_BLUETOOTH 1\n#define ENABLE_G2_GLASSES 1\n"
             "#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored \"-Wunused-const-variable\"\n" +
             "\n".join(strip.sub("", (root / name).read_text())
                       for name in ("System_R1_Protocol.h", "G2_Health.h")) +
             "\n#pragma GCC diagnostic pop\n")
    health = (root / "G2_Health.cpp").read_text()
    parts = [between(health, "EXT_RAM_BSS_ATTR static R1HealthHistoryDay sHistoryDay;", "class SeriesLock"),
             between(health, "static uint32_t markHistoryChangedLocked()",
                     "bool g2HealthHistoryPeerCustodySelfTest("),
             between(health, "static bool validateCommonDaily(", "static void rebuildOneTrendLocked("),
             *(extract_block(health, marker) for marker in (
                 "static void historyResetForPeerSwitchLocked(", "void g2HealthHistoryFetchStarted(",
                 "void g2HealthHistoryFetchFinished(", "static bool historyKeyMatchesLocked(",
                 "static void processPendingHistoryMainTask(", "void g2HealthHistoryCommitTick("))]
    return types, "\n\n".join(parts)


def generate(overlay: Path | None, component: Path = COMPONENT) -> str:
    root = overlay or component
    source = (root / "R1_HealthHistoryStore.cpp").read_text()
    types = (root / "R1_HealthHistoryStore.h").read_text().replace('#include "System_BuildConfig.h"', '')
    crypto = (COMPONENT / "System_CaptureCrypto.h").read_text()
    types += "\n" + "\n".join(line for line in crypto.splitlines() if line.startswith(("#define CAPCRYPT_MAGIC_LINE", "#define CAPCRYPT_MAX_SEALED")))
    production = source[source.index("namespace {"):source.rindex("#endif  // ENABLE_R1_HEALTH")]
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    locks = extract_block((COMPONENT / "System_Mutex.h").read_text(), "struct FsLockGuard") + ";\n"
    locks += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)\n",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    # A saved baseline keeps its original scope: the store alone.
    model_types, model_code = ("", "") if overlay else model(root)
    harness = (HERE / "r1_history_store_harness.cpp").read_text()
    for marker in ("// INSERT_TYPES", "// INSERT_LOCKS", "// INSERT_PRODUCTION", "// INSERT_MODEL"):
        require(harness.count(marker) == 1, f"exactly one {marker} required")
    return (f"#define BASELINE {int(overlay is not None)}\n" + harness
            .replace("// INSERT_TYPES", types + "\n" + model_types)
            .replace("// INSERT_LOCKS", locks)
            .replace("// INSERT_PRODUCTION", production)
            .replace("// INSERT_MODEL", model_code))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="saved component sources; also runs current")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="components/hardwareone directory the current run reads production sources from")
    parser.add_argument("--evidence", type=Path, help="retain generated source and output")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-r1-history-") as temp:
        work = args.evidence.resolve() if args.evidence else Path(temp)
        work.mkdir(parents=True, exist_ok=True)
        string = (CORE / "WString.cpp").read_text().replace('#include "Arduino.h"', '#include <cstdio>')
        string = string.replace('#include "esp32-hal-log.h"', '#define log_e(...) ((void)0)\n#define log_w(...) ((void)0)')
        (work / "WString.cpp").write_text(string)
        (work / "Print.cpp").write_text((CORE / "Print.cpp").read_text().replace('#include "Arduino.h"', '#include <cstdarg>'))
        conversions = (CORE / "stdlib_noniso.c").read_text().replace('#include "esp_system.h"', '')
        conversions += "\nchar *itoa(int v,char*s,int b){return ltoa(v,s,b);}\nchar *utoa(unsigned int v,char*s,int b){return ultoa(v,s,b);}\n"
        (work / "stdlib_noniso.c").write_text(conversions)
        sanitize = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                     "-fno-omit-frame-pointer", "-g"] if args.sanitize else [])
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-Wno-unused-parameter", "-I", str(CORE), "-I", str(COMPONENT), *sanitize]
        subprocess.run([shutil.which("cc"), "-std=c11", *sanitize, "-I", str(CORE),
                        "-c", str(work / "stdlib_noniso.c"), "-o", str(work / "conversions.o")], check=True)
        for source in ("WString", "Print"):
            subprocess.run([args.cxx, *flags, "-Wno-sign-compare", "-c", str(work / f"{source}.cpp"),
                            "-o", str(work / f"{source}.o")], check=True)
        transcripts = {}
        for label, source in ([('baseline', args.baseline)] if args.baseline else []) + [('current', None)]:
            unit, binary = work / f"{label}.cpp", work / label
            unit.write_text(generate(source, args.component.resolve()))
            subprocess.run([args.cxx, *flags, str(unit), str(work / "WString.o"), str(work / "Print.o"),
                            str(work / "conversions.o"), "-o", str(binary)], check=True, timeout=120)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30,
                                    env={**os.environ, "TZ": "UTC"})
            (work / f"{label}-transcript.txt").write_text(result.stdout)
            (work / f"{label}-assertions.log").write_text(result.stderr)
            require(result.returncode == 0, f"{label}: {result.stderr}")
            transcripts[label] = result.stdout
            print(f"{label}: {result.stderr.strip()}")
        if args.baseline:
            require(transcripts['baseline'] == transcripts['current'], "normal output differs from saved baseline")
            print("Normal R1 history bytes and paths match baseline; completion/locking corrections pass separately.")


if __name__ == "__main__":
    main()
