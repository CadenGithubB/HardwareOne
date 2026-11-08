#!/usr/bin/env python3
"""Report whether an ESP-IDF tree carries the FreeRTOS PIE context-switch fix.

Usage: python3 -B check_idf_pie.py --idf "$IDF_PATH"

ESP-IDF before 6.0 saves PIE registers q0-q2 and q4-q7 on a RISC-V context
switch but not q3, and packs XACC so that its upper byte is overwritten.
Upstream b25cb29 (tools/p4/idf-patches/0001-...) fixes both in
components/freertos/FreeRTOS-Kernel/portable/riscv/portasm.S. Speaker-label
builds (ENABLE_STT_SPEAKER_LABELS, the p4x_eye profile) need it; Waveshare,
S3 and classic-ESP32 builds do not. The fix is recognised by the instructions
it adds, not by a commit hash, so a locally patched 5.5.5 and an upstream
6.0+ tree both pass. The IDF tree is only read. This prints one state line
and always exits 0: it reports, it does not decide which board is built.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent
PATCH = ROOT / "idf-patches/0001-freertos-fix-xesppie-registers-save-restore.patch"
PORTASM = Path("components/freertos/FreeRTOS-Kernel/portable/riscv/portasm.S")
# What b25cb29 adds to portasm.S: q3 in pie_save_regs and pie_restore_regs,
# and XACC stored/loaded without advancing the frame pointer (the packing fix).
MARKERS = {
    "q3 save": re.compile(r"^\s*esp\.vst\.128\.ip\s+q3\s*,", re.M),
    "q3 restore": re.compile(r"^\s*esp\.vld\.128\.ip\s+q3\s*,", re.M),
    "XACC store": re.compile(r"^\s*esp\.st\.u\.xacc\.ip\s+\\frame\s*,\s*0\s*$", re.M),
    "XACC load": re.compile(r"^\s*esp\.ld\.xacc\.ip\s+\\frame\s*,\s*0\s*$", re.M),
}


def remedy(idf: Path) -> str:
    return (f"apply it with: git -C {idf} apply {PATCH} "
            "(tools/p4/idf-patches/README.md); only ENABLE_STT_SPEAKER_LABELS builds (p4x_eye) need it")


def pie_state(idf: Path) -> tuple[str, str]:
    """Return (state, line): state is present, missing, partial or unknown."""
    source = idf / PORTASM
    label = "ESP-IDF PIE q3 context-switch fix (upstream b25cb29)"
    try:
        text = source.read_text(errors="replace")
    except OSError:
        return "unknown", f"{label}: cannot check, {source} not found; {remedy(idf)}"
    found = [name for name, pattern in MARKERS.items() if pattern.search(text)]
    if len(found) == len(MARKERS):
        return "present", f"{label}: present in {idf}"
    if not found:
        return "missing", f"{label}: MISSING from {idf}; {remedy(idf)}"
    absent = ", ".join(name for name in MARKERS if name not in found)
    return "partial", (f"{label}: PARTIAL in {idf} (no {absent}); restore {PORTASM} with "
                       f"git -C {idf} checkout -- {PORTASM}, then {remedy(idf)}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--idf", type=Path, default=os.environ.get("IDF_PATH"),
                        help="the ESP-IDF tree to read; defaults to IDF_PATH")
    args = parser.parse_args()
    if args.idf is None:
        parser.exit(1, "check_idf_pie: provide --idf or export IDF_PATH\n")
    print(pie_state(args.idf)[1])


if __name__ == "__main__":
    main()
