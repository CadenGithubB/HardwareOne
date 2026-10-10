#!/usr/bin/env python3
"""Compile the production command catalogue against minimal link/provider fixtures.

Readable snapshots were captured by compiling the pre-extraction table. They
pin module order, command/count bindings, flags and callback behavior. Feature
selections here are resolved macro inputs, not a second board/dependency model.
An optional original Utils source repeats the pre-extraction comparison.
"""

import argparse
import difflib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
STUBS = {
    "System_BuildConfig.h": "",
    "System_RadioCompanion.h": "#define HW1_RADIO_COMPANION FIXTURE_RADIO_COMPANION\nbool radioCompanionOnline();",
    "System_I2C.h": "bool isSensorConnected(const char* name);",
    "System_VFS.h": "namespace VFS { bool isSDAvailable(); }",
    "HAL_Input.h": "extern bool gInputConnected;",
    "i2csensor_ano_encoder.h": "extern bool gAnoEncoderConnected;",
    "System_Camera_DVP.h": "extern bool cameraConnected;",
    "HAL_Audio.h": "bool audioAnySourceAvailable();",
}


def baseline_source(path, symbols):
    source = path.read_text()
    start = source.index("static constexpr CommandModule gCommandModules[] = {")
    end = source.index("const CommandModule* getCommandModules(", start)
    declarations = "\n".join(f"extern const CommandEntry {symbol}[];\n"
                             f"extern const size_t {symbol}Count;" for symbol in symbols)
    return ('#include "System_CommandRegistry.h"\n' +
            "\n".join(f'#include "{name}"' for name in STUBS) + "\n" +
            declarations + "\n" + source[start:end] +
            extract_block(source[end:], "const CommandModule* getCommandModules("))


def compile_catalogue(work, cxx, source, symbols, gates, enabled, sanitize):
    # Copy the production translation unit intact so only its adjacent platform
    # headers are substituted. The real public metadata header is unchanged.
    (work / "registry.cpp").write_text(source)
    shutil.copyfile(COMPONENT / "System_CommandRegistry.h", work / "System_CommandRegistry.h")
    for name, declarations in STUBS.items():
        (work / name).write_text("#pragma once\n" + declarations + "\n")
    (work / "catalogue_fixture_rows.inc").write_text("\n".join(
        f"COMMAND_FIXTURE({symbol}, {i + 1})" for i, symbol in enumerate(symbols)))
    executable = work / "catalogue"
    command = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(work)]
    for gate in sorted(gates):
        macro = "FIXTURE_RADIO_COMPANION" if gate == "HW1_RADIO_COMPANION" else gate
        command.append(f"-D{macro}={int(gate in enabled)}")
    if sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    command += [str(work / "registry.cpp"), str(HERE / "command_catalogue_harness.cpp"),
                "-o", str(executable)]
    subprocess.run(command, check=True, timeout=120)
    return subprocess.run([str(executable)], check=True, text=True,
                          capture_output=True, timeout=60).stdout.splitlines()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="pre-extraction System_Utils.cpp")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    snapshot = json.loads((HERE / "command_catalogue_expected.json").read_text())
    rows = {row.split("|")[0]: row for row in snapshot["rows"]}
    assert len(rows) == len(snapshot["rows"]), "duplicate snapshot modules"
    symbols = sorted({row.split("|")[1] for row in rows.values()})
    source = (COMPONENT / "System_CommandRegistry.cpp").read_text()
    gates = set(re.findall(r"\b(?:ENABLE_\w+|HW1_RADIO_COMPANION)\b", source))
    covered = {flag for profile in snapshot["profiles"].values() for flag in profile["enabled"]}
    assert gates == covered, f"update catalogue selections for changed gates: {gates ^ covered}"
    assert set(rows) == {name for profile in snapshot["profiles"].values()
                         for name in profile["modules"].split()}, "uncovered snapshot modules"
    sources = [("current", source)]
    if args.baseline:
        sources.append(("baseline", baseline_source(args.baseline, symbols)))
    with tempfile.TemporaryDirectory(prefix="hw1-command-catalogue-") as temp:
        for label, unit in sources:
            for name, profile in snapshot["profiles"].items():
                expected = [rows[module] for module in profile["modules"].split()]
                actual = compile_catalogue(Path(temp), args.cxx, unit, symbols, gates,
                                           profile["enabled"], args.sanitize)
                assert actual == expected, "\n".join(difflib.unified_diff(
                    expected, actual, fromfile=f"snapshot/{name}", tofile=f"{label}/{name}"))
            print(f"command catalogue: {label} passed {len(snapshot['profiles'])} feature selections")


if __name__ == "__main__":
    main()
