"""System_SdPathCore.h for harnesses that splice normalizeFsPath.

normalizeFsPath (System_Filesystem.cpp) refuses a card path in a spelling FatFs
would resolve to another name (B11); the rule is sdpath::check in
System_SdPathCore.h. A harness that compiles the extracted normalizeFsPath gets
the header's text in front of it through these helpers. The text is wrapped in
an include guard of its own, so a unit that splices it twice still compiles.
"""
from __future__ import annotations

from pathlib import Path


def sd_path_core(component: Path) -> str:
    text = (component / "System_SdPathCore.h").read_text().replace("#pragma once", "")
    return ("#ifndef HW1_SD_PATH_CORE_SPLICED\n#define HW1_SD_PATH_CORE_SPLICED\n"
            + text + "\n#endif  // HW1_SD_PATH_CORE_SPLICED\n")


def with_sd_path_core(block: str, component: Path) -> str:
    """`block` (the extracted normalizeFsPath, or code that holds it) with the core in front."""
    if "sdpath::" not in block:
        return block  # a tree from before B11 (a --baseline run)
    return sd_path_core(component) + "\n" + block
