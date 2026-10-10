#!/usr/bin/env python3
"""Execute production G2 startup admission and manual reopen ordering.

Substitute transport, lease, lifecycle and CREATE publication boundaries; extract
the admission, commands, bootstrap, lease wait and launch-cancel sender unchanged.
This checks host control flow, not glasses firmware acceptance or visible output.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    source = (args.component / "G2_Glasses.cpp").read_text()
    # These functions also have forward declarations. Start after the last
    # declaration so extract_block reads the actual implementation.
    send_source = source[source.index("// Internal: send Cmd=18 MenuStartUpFailed"):]
    blocks = [extract_block(send_source, "static bool sendMenuFailedAndSettle(")]
    blocks += [extract_block(source, marker) for marker in (
        "static bool hijackBootstrapWaitForLease(",
        "static void hijackBootstrapBody(",
        "static void hijackReopenBody(",
        "static bool handleMenuStartUp(",
        "static const char* cmd_g2hijacktest(",
        "static const char* cmd_g2reopen(")]
    harness = (HERE / "g2_reopen_harness.cpp").read_text()
    assert harness.count("// INSERT_PRODUCTION") == 1
    harness = harness.replace("// INSERT_PRODUCTION", "\n\n".join(blocks))
    compiler = args.cxx
    if not compiler:
        raise RuntimeError("A C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory(prefix="hw1-g2-reopen-") as directory:
        unit = Path(directory) / "test.cpp"
        exe = Path(directory) / "test"
        unit.write_text(harness)
        subprocess.run([compiler, *flags, str(unit), "-o", str(exe)],
                       check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == "__main__":
    main()
