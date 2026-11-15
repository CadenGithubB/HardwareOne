"""System_PsramJson.h as source text for a harness that splices production code.

The header's #include and #pragma lines are dropped: the harness supplies
ArduinoJson, AllocPref, ps_alloc and ps_free. A tree from before the header
existed gives an empty text, so a test can still be pointed at it.
"""
from __future__ import annotations

from pathlib import Path
import re


def psram_json_text(component: Path) -> str:
    header = Path(component) / "System_PsramJson.h"
    if not header.exists():
        return ""
    return re.sub(r"^#(?:include|pragma)[^\n]*\n", "", header.read_text(), flags=re.M)
