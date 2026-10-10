#!/usr/bin/env python3
"""Execute G2 lifecycle publication and real init/deinit admission with host boundaries.

BLE, workers, heap samples, timers and temple allocation are substituted;
production lifecycle claims, initialization/rollback/teardown control flow,
runtime state, combined status snapshots, memory checker and G2 threshold are
extracted unchanged. Optional baseline compares unchanged lifecycle outcomes
and reproduces the older memory-gate ordering. Concurrency exercises production
publication and read helpers with real host mutexes, without claiming telemetry
atomicity or hardware allocation fidelity.
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


def generated(component: Path, *, baseline: bool = False) -> str:
    source = (component / "G2_Glasses.cpp").read_text()
    header = (component / "G2_Glasses.h").read_text()
    memory = (component / "System_MemoryMonitor.cpp").read_text()
    memory_header = (component / "System_MemoryMonitor.h").read_text()
    requirement = re.search(r'\{\s*"g2client"\s*,[^}]+\}', memory)
    require(requirement is not None, "production G2 memory registry entry is present")
    init = extract_block(source, "bool initG2Client(")
    # Current production must pass the new admission contract. Saved sources
    # may predate the placement fix; reproduce that refusal as a regression
    # baseline while still comparing the unchanged lifecycle outcomes.
    post_normalization = not baseline or init.index("checkMemoryAvailable(") > init.index(
        "templeRuntimeQuiescedForInit(")
    runtime = "struct G2ClientRuntime" in source
    types = [f"#define TEST_RUNTIME {int(runtime)}",
             f"#define TEST_POST_NORMALIZATION {int(post_normalization)}",
             extract_block(memory_header, "struct MemoryRequirement") + ";",
             "static const MemoryRequirement gMemoryRequirements[] = {" + requirement[0] + "};",
             "static const size_t gMemoryRequirementsCount = 1;",
             extract_block(header, "enum G2State") + ";",
             extract_block(source, "struct G2LinkSnapshot") + ";",
             extract_block(source, "enum class G2WorkerInitState") + ";"]
    types += [extract_block(header, marker) + ";" for marker in
              ("struct G2TempleConnectionStatus", "struct G2ConnectionStatus")]
    if runtime:
        types += [extract_block(source, marker) + ";" for marker in
                  ("struct G2ClientRuntime", "struct G2ClientStatusSnapshot")]
    else:
        types += [extract_block(header, marker) + ";" for marker in
                  ("enum G2Eye", "enum G2EventType")]
        types += ["typedef void (*G2EventCallback)(G2EventType event);",
                  extract_block(header, "struct G2ClientState") + ";"]
    definitions = [extract_block(memory, marker) for marker in (
        "const MemoryRequirement* getMemoryRequirement(", "bool checkMemoryAvailable(")]
    definitions += [extract_block(source, "static G2LinkSnapshot g2LinkSnapshot(")]
    if runtime:
        definitions += [extract_block(source, marker) for marker in (
            "static G2ClientRuntime g2ClientRuntimeSnapshot(",
            "static void g2ClientRuntimeReset(",
            "static void g2ClientSetState(",
            "static void g2ClientConnected(",
            "static G2ClientStatusSnapshot g2ClientStatusSnapshotLocked(",
                         "static G2ClientStatusSnapshot g2ClientStatusSnapshot(",
            "static const char* g2ClientStateName(")]
    definitions += [extract_block(source, marker) for marker in (
        "static bool g2RoleAdmissionOpen(",
        "static void g2ControlStartBlockAcquire(",
        "static void g2ControlStartBlockRelease(",
        "static bool g2ClientInitBegin(",
        "static void g2ClientInitDone(",
        "static bool g2ClientInitIdle(")]
    definitions += [extract_block(source, marker) + ";" for marker in (
        "struct G2ClientInitClaim", "class G2BleRoleTransitionClaim")]
    definitions += [extract_block(source, marker) for marker in (
        "bool initG2Client(", "bool deinitG2Client(",
        "bool isG2ClientInitialized(", "G2State getG2State(",
        "const char* getG2StateString(", "bool g2ConnectionStatusSnapshot(",
        "static bool pageSwapRuntimeSnapshot(", "static bool tapRuntimeSnapshot(",
        "static void g2UiSubmitDone(")]
    harness = (HERE / "g2_client_lifecycle_harness.cpp").read_text()
    return harness.replace("// INSERT_TYPES", "\n".join(types)).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--thread-sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    require(not (args.sanitize and args.thread_sanitize), "select one sanitizer mode")
    flags = ["-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter"]
    if args.sanitize or args.thread_sanitize:
        flags += ["-fsanitize=" + ("thread" if args.thread_sanitize else "address,undefined"),
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-g2-lifecycle-") as directory:
        work = Path(directory)

        def run(name: str, component: Path) -> str:
            path = work / (name + ".cpp")
            path.write_text(generated(component, baseline=name == "baseline"))
            subprocess.run([args.cxx, *flags, str(path), "-o", str(work / name)],
                           check=True, timeout=120)
            return subprocess.check_output([str(work / name)], text=True, timeout=120)

        current = run("current", COMPONENT)
        if args.baseline:
            baseline = run("baseline", args.baseline)
            require(current == baseline, "init/rollback/deferred teardown outcomes changed")
            print("G2 lifecycle: init/rollback/deferred teardown outcomes match saved production")
            print("G2 memory admission: saved production checked with its original gate placement")
    print("G2 memory admission: production threshold, normalization, retained workers and low-memory refusal passed")
    print("G2 lifecycle: production transitions, guarded publication and concurrent readers passed")


if __name__ == "__main__":
    main()
