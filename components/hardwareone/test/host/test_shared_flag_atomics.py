#!/usr/bin/env python3
"""A flag or counter that more than one task changes changes in one atomic step.

`x |= bit`, `x &= ~bit` and `++x` on a plain or volatile integer are a load, a
change and a store. Two tasks that do this to the same variable lose one of the
two changes when one is scheduled between the other's load and store. The
variables below were changed that way from several tasks; each is now a
std::atomic and each change is one atomic operation.

Output lanes (gOutputFlags, System_Debug.h): command handlers on cmd_exec open
and close lanes (`log start`, outble, outg2, outserial, the HTTP server start)
while the main loop closes the BLE and WEB lanes at a session end or server
stop, and applySettings rewrites the SERIAL lane from both tasks.

The production definition of each variable and the production statements that
change it are compiled into shared_flag_atomics_harness.cpp and run against
each other on real threads; the harness states the invariant each run checks.
Source contract: one definition and one declaration per variable, both atomic,
and no statement that stores a value computed from an earlier read.

Options: --cxx <compiler>, --sanitize (accepted; the threads run without a
sanitizer because the pre-fix data race is the thing under test), --component
<dir> to run the same cases against another components/hardwareone directory.
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

LANES = ("SERIAL", "WEB", "FILE", "BLE", "G2")


def firmware_sources(component: Path) -> list[Path]:
    found = []
    for pattern in ("*.cpp", "*.h"):
        for path in component.rglob(pattern):
            parts = path.relative_to(component).parts
            if "test" in parts or path.name.startswith("._"):
                continue
            found.append(path)
    return sorted(found)


def code_lines(text: str):
    """Source lines with their // comment removed (none of these lines holds a string with //)."""
    for number, line in enumerate(text.splitlines(), 1):
        yield number, line.split("//", 1)[0]


def one_line(text: str, pattern: str, what: str) -> str:
    found = re.findall(pattern, text, re.M)
    require(len(found) == 1, f"{what}: expected one, found {len(found)}")
    return found[0]


def between(text: str, start: str, end: str, what: str) -> str:
    require(text.count(start) == 1, f"{what}: one start marker")
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


# ---- Output lanes -----------------------------------------------------------------

def output_lane_parts(component: Path, sources: dict[Path, str]) -> dict[str, str]:
    debug_header = sources[component / "System_Debug.h"]
    route_bits = "\n".join(re.findall(r"^#define MSG_ROUTE_\w+\s+0x[0-9A-Fa-f]+", debug_header, re.M))
    require(route_bits.count("#define") == 8, "the eight MSG_ROUTE_* values of System_Debug.h")
    definition = one_line(sources[component / "HardwareOne.cpp"],
                          r"^[\w:<> ]*\bgOutputFlags\b[^;\n]*;", "the definition of gOutputFlags")
    apply_settings = extract_block(sources[component / "System_Settings.cpp"], "void applySettings(")
    lane = between(apply_settings, "// Apply the one persisted output lane (SERIAL)",
                   "// Apply debug settings to runtime flags", "the output lane step of applySettings")
    steps: dict[str, str] = {}
    for path, text in sources.items():
        for _, line in code_lines(text):
            for statement, operator, lane_name in re.findall(
                    r"(gOutputFlags\s*(\|=|&=)\s*~?MSG_ROUTE_(\w+)\s*;)", line):
                opens = operator == "|="
                require(opens != ("~" in statement), f"{path.name}: unexpected lane change `{statement}`")
                steps.setdefault(("laneOpen_" if opens else "laneClose_") + lane_name, statement)
    for name in LANES:
        for prefix in ("laneOpen_", "laneClose_"):
            require(prefix + name in steps, f"a production statement for {prefix}{name}")
    functions = "\n".join(f"static void {name}() {{ {statement} }}" for name, statement in sorted(steps.items()))
    return {"// INSERT_ROUTE_BITS": route_bits, "// INSERT_OUTPUT_FLAGS_DEFINITION": definition,
            "// INSERT_APPLY_SETTINGS_LANE": lane, "// INSERT_LANE_STEPS": functions}


def check_output_lane_sources(component: Path, sources: dict[Path, str]) -> None:
    declarations = []
    for path, text in sources.items():
        for number, line in code_lines(text):
            if "gOutputFlags" not in line:
                continue
            where = f"{path.relative_to(component)}:{number}"
            if re.search(r"\b(?:uint32_t|std::atomic<[^>]*>)\s+gOutputFlags\b", line):
                declarations.append((where, line.strip()))
                continue
            require(not re.search(r"\bgOutputFlags\s*=(?!=)", line),
                    f"{where}: gOutputFlags is stored from an earlier read; use |=, &= or a compare-exchange")
            require(not re.search(r"gOutputFlags\s*(\^=|\+=|-=|\+\+|--)", line),
                    f"{where}: unexpected change of gOutputFlags")
    require(sorted(text for _, text in declarations) ==
            ["extern std::atomic<uint32_t> gOutputFlags;", "std::atomic<uint32_t> gOutputFlags{MSG_ROUTE_SERIAL};"],
            f"gOutputFlags has one atomic definition and one atomic declaration, found {declarations}")
    apply_settings = extract_block(sources[component / "System_Settings.cpp"], "void applySettings(")
    require(apply_settings.count("gOutputFlags.compare_exchange_weak(") == 1,
            "applySettings rewrites the SERIAL lane with one compare-exchange loop")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    sources = {path: path.read_text(encoding="utf-8", errors="replace") for path in firmware_sources(component)}

    harness = (HERE / "shared_flag_atomics_harness.cpp").read_text()
    parts = {}
    parts.update(output_lane_parts(component, sources))
    for marker, body in parts.items():
        require(harness.count(marker) == 1, f"one {marker} in the harness")
        harness = harness.replace(marker, body)
    require("// INSERT_" not in harness, "every insertion marker of the harness is filled")

    flags = ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-shared-flag-atomics-") as directory:
        source = Path(directory) / "shared_flag_atomics.cpp"
        binary = Path(directory) / "shared_flag_atomics"
        source.write_text(harness)
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        if subprocess.run([str(binary)], timeout=300).returncode != 0:
            failures.append("the harness reported lost updates or a non-atomic type")
    for check in (check_output_lane_sources,):
        try:
            check(component, sources)
        except AssertionError as error:
            failures.append(str(error))
    require(not failures, "\n".join(failures))
    print("shared flags and counters change in one atomic step each", flush=True)


if __name__ == "__main__":
    main()
