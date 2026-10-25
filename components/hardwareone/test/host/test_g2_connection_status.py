#!/usr/bin/env python3
"""Execute production G2 connection snapshot and legacy text formatting.

The owner state names, topology read, public status types and text formatter are
extracted from shipping sources. Only hardware state carriers and critical
section primitives are host boundaries. --baseline compares text byte-for-byte
with saved pre-change source; it does not claim whole-state atomicity.
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


def generated(component: Path, baseline: bool) -> str:
    owner = (component / "G2_Glasses.cpp").read_text()
    header = (component / "G2_Glasses.h").read_text()
    runtime = "struct G2ClientRuntime" in owner
    typed = "bool g2ConnectionStatusSnapshot(" in owner
    repair = "static const char* g2RepairDescribe(" in owner
    types = [extract_block(header, "enum G2State") + ";"]
    if runtime:
        types.append(extract_block(owner, "struct G2ClientRuntime") + ";")
    else:
        types += [extract_block(header, marker) + ";" for marker in
                  ("enum G2Eye", "enum G2EventType")]
        types += ["typedef void (*G2EventCallback)(G2EventType event);",
                  extract_block(header, "struct G2ClientState") + ";"]
    # Keep the actual declarations for the fields read by production. The rest
    # of the BLE driver's large temple state is irrelevant to this read API.
    temple = extract_block(owner, "struct G2Temple")
    fields = []
    for name in ("connected", "mtu", "packetsSent", "packetsReceived",
                 "connectionGeneration", "sessionReadyGeneration"):
        declaration = re.search(r"^\s+\w+\s+" + name + r"\s*;", temple, re.M)
        require(declaration is not None, f"missing production temple field: {name}")
        fields.append(declaration.group(0).strip())
    types.append("struct G2Temple { " + " ".join(fields) + " };")
    definitions = []
    if typed or runtime:
        types += [extract_block(owner, "struct G2LinkSnapshot") + ";"]
        definitions.append(extract_block(owner, "static G2LinkSnapshot g2LinkSnapshot("))
    if runtime:
        types += [extract_block(owner, "struct G2ClientStatusSnapshot") + ";"]
        definitions += [extract_block(owner, marker) for marker in
                        ("static G2ClientRuntime g2ClientRuntimeSnapshot(",
                         "static G2ClientStatusSnapshot g2ClientStatusSnapshotLocked(",
                         "static G2ClientStatusSnapshot g2ClientStatusSnapshot(",
                         "static const char* g2ClientStateName(")]
    definitions += [extract_block(owner, "G2State getG2State("),
                    extract_block(owner, "const char* getG2StateString(")]
    if repair:
        definitions.append(extract_block(owner, "static const char* g2RepairDescribe("))
    if typed:
        types += [extract_block(header, marker) + ";" for marker in
                  ("struct G2TempleConnectionStatus", "struct G2ConnectionStatus")]
        definitions += [extract_block(owner, "bool g2ConnectionStatusSnapshot("),
                        extract_block(owner, "void formatG2Status(")]
    definitions.append(extract_block(owner, "void getG2Status("))
    types.insert(0, f"#define TEST_RUNTIME {int(runtime)}\n#define TEST_TYPED {int(typed)}\n"
                    f"#define TEST_REPAIR {int(repair)}")
    harness = (HERE / "g2_connection_status_harness.cpp").read_text()
    return harness.replace("// INSERT_TYPES", "\n".join(types)).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-function", "-Wno-unused-variable"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-g2-status-") as directory:
        work = Path(directory)

        def run(name: str, component: Path, baseline: bool) -> str:
            source = work / (name + ".cpp")
            source.write_text(generated(component, baseline))
            subprocess.run([args.cxx, *flags, f"-DTEST_BASELINE={int(baseline)}", "-I", str(COMPONENT),
                            str(source), "-o", str(work / name)], check=True, timeout=120)
            return subprocess.check_output([str(work / name)], text=True, timeout=60)

        output = run("current", COMPONENT, False)
        if args.baseline:
            old = run("baseline", args.baseline, True)
            require(output == old, "G2 status text differs from characterized production")
            print("G2 connection status: text matches pre-change production byte-for-byte")
    print("G2 connection status: state, topology, repair, snapshot and text boundaries passed")


if __name__ == "__main__":
    main()
