#!/usr/bin/env python3
"""Feed temple-forwarded ring frames to the production forwarded-telemetry parser.

g2RingNoteForwardedTelemetry and its local protobuf helpers (G2_Ring.cpp) run
with the shared g2Pb* readers (System_G2_Protocol.cpp), the production
R1TelemetryCache layout and the G2HealthMetric enum. The telemetry mutex, the
direct-link ownership check, the Health live series and millis() are doubles.
No G2 transport, temple bridge or ring is simulated.

Each case runs in its own process under a timeout, with the frame in an
exact-size heap buffer, so a parser that never returns or reads past the frame
fails by name (use --sanitize for the read-past-the-buffer check).
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
CASE_TIMEOUT_S = 30


def optional(source: str, marker: str) -> list[str]:
    """A definition the fixed sources add; absent on f4cc542."""
    return [extract_block(source, marker)] if marker in source else []


def definition(source: str, signature: str) -> str:
    """Extract the definition of `signature`, skipping forward declarations."""
    start = 0
    while True:
        index = source.index(signature, start)
        close = source.index(")", index)
        if source[close + 1:].lstrip().startswith("{"):
            return extract_block(source[index:], signature)
        start = index + len(signature)


def generated(component: Path, source_dir: Path | None) -> str:
    def read(name: str) -> str:
        overlay = source_dir / name if source_dir is not None else None
        return (overlay if overlay is not None and overlay.exists() else component / name).read_text()

    ring = read("G2_Ring.cpp")
    protocol = read("System_G2_Protocol.cpp")
    protocol_h = read("System_G2_Protocol.h")
    health_h = read("G2_Health.h")

    types = [extract_block(protocol_h, "enum G2PbWire") + ";",
             extract_block(health_h, "enum G2HealthMetric") + ";",
             extract_block(ring, "struct R1TelemetryCache") + ";"]
    protocol_parts = [definition(protocol, signature) for signature in (
        "bool g2PbReadVarint(", "bool g2PbReadTag(", "bool g2PbSkipField(")]
    production = [*optional(ring, "static bool ringPbReadDelimited("),
                  *optional(ring, "static bool ringPbSkipField("),
                  definition(ring, "void g2RingNoteForwardedTelemetry(")]

    harness = (HERE / "ring_forwarded_telemetry_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", types), ("// INSERT_PROTOCOL", protocol_parts),
                         ("// INSERT_PRODUCTION", production)):
        require(harness.count(marker) == 1, f"bad harness marker: {marker}")
        harness = harness.replace(marker, "\n\n".join(body))
    return harness


def run_case(binary: Path, name: str) -> str | None:
    """Return None on success, otherwise a failure summary naming the defect."""
    try:
        result = subprocess.run([str(binary), name], capture_output=True, text=True,
                                timeout=CASE_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return (f"{name}: g2RingNoteForwardedTelemetry did not return within "
                f"{CASE_TIMEOUT_S} s (parser hang)")
    print(result.stdout, end="")
    if result.returncode == 0:
        return None
    lines = [line for line in (result.stdout + result.stderr).splitlines() if line.strip()]
    asan = [line for line in lines if "ERROR: AddressSanitizer" in line or "SUMMARY:" in line]
    if asan:
        print(result.stderr, end="")
        return f"{name}: read past the frame buffer: " + " | ".join(asan)
    fails = [line for line in lines if line.startswith("FAIL")]
    return f"{name}: " + (" | ".join(fails) if fails else f"exit code {result.returncode}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="components/hardwareone directory to read production sources from")
    parser.add_argument("--source-dir", type=Path,
                        help="directory whose files overlay --component file by file")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-ring-forwarded-telemetry-") as directory:
        work = Path(directory)
        (work / "test.cpp").write_text(generated(args.component, args.source_dir))
        binary = work / "test"
        subprocess.run([args.cxx, *flags, str(work / "test.cpp"), "-o", str(binary)],
                       check=True, timeout=180)
        cases = subprocess.run([str(binary), "--list"], capture_output=True, text=True,
                               check=True, timeout=30).stdout.split()
        require(len(cases) > 0, "the harness lists no cases")
        failures = [failure for failure in (run_case(binary, name) for name in cases) if failure]
        for failure in failures:
            print(f"FAILED {failure}")
        require(not failures, f"{len(failures)} of {len(cases)} forwarded-telemetry cases failed")
    print(f"ring forwarded telemetry: {len(cases)} cases passed")


if __name__ == "__main__":
    main()
