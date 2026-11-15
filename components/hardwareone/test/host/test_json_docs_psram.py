#!/usr/bin/env python3
"""Every JSON document in the firmware sources is made on an explicit allocator.

A plain `JsonDocument doc;` takes ArduinoJson's default allocator, which is
malloc: under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL its 1 KB pool and its copied
strings come from internal RAM. Firmware code writes `PSRAM_JSON_DOC(doc)`
(System_MemUtil.h) instead, or passes an allocator of its own.

Checks, on the source text of the component (test/ excluded):
1. No document is declared without an allocator. The one exception is the
   `doc_` member of hw1http::Stream (System_LLMHttpCore.h): the firmware builds
   it with the budgeted PSRAM allocator (System_LLMHttp.cpp), only host tests
   reach its default constructor.
2. The eleven documents that were plain until the 2026-11-14 heap sweep
   (census-json-docs-01 and -02) are PSRAM_JSON_DOC(doc), and each of their
   files includes System_MemUtil.h itself.

The harnesses that run these functions (quicknote_service, oled_transcription,
camera_oled_preview, espnow_identity_writes, transcription_ui_adapter) define
PSRAM_JSON_DOC on a counting allocator (psram_json_doc_host.h) and check that
the reply document is made on it and released.

--component points the test at another copy of components/hardwareone.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

PLAIN = re.compile(r"\bJsonDocument\s+(\w+)\s*(?:;|\{\s*\}|\(\s*\))")
ALLOWED = {("System_LLMHttpCore.h", "doc_")}

# file -> (function opening, number of PSRAM_JSON_DOC(doc) in it)
SWEPT = {
    "System_TranscriptionUI.cpp": [("const char* commandSession(", 1), ("const char* commandFiles(", 1)],
    "System_QuickNote.cpp": [("const char* commandNote(", 1)],
    "OLED_Mode_Transcription.cpp": [("static void acceptReply(", 1)],
    "System_STT.cpp": [("static const char* cmd_stt(", 2)],
    "OLED_Mode_Camera.cpp": [("const char* cmd_oledcameraread(", 1)],
    "System_ESPNow_Identity.cpp": [("bool writeIdentityFile(", 1), ("bool readIdentityFile(", 1),
                                   ("bool writePeerIdentityFile(", 1), ("bool readPeerIdentityFile(", 1)],
}


def code_only(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m[0]), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()
    failures = []

    for path in sorted(component.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp", ".c") or "test" in path.relative_to(component).parts:
            continue
        text = code_only(path.read_text(errors="replace"))
        relative = str(path.relative_to(component))
        for match in PLAIN.finditer(text):
            if (relative, match[1]) in ALLOWED:
                continue
            line = text.count("\n", 0, match.start()) + 1
            failures.append(f"{relative}:{line}: `{match[0]}` takes the default (internal malloc) "
                            "allocator; write PSRAM_JSON_DOC(name)")

    for name, functions in SWEPT.items():
        source = (component / name).read_text()
        if '#include "System_MemUtil.h"' not in source:
            failures.append(f"{name}: does not include System_MemUtil.h, which defines PSRAM_JSON_DOC")
        for opening, count in functions:
            if source.count(opening) != 1:
                failures.append(f"{name}: expected one `{opening}`, found {source.count(opening)}")
                continue
            body = code_only(extract_block(source, opening))
            found = body.count("PSRAM_JSON_DOC(doc);")
            if found != count:
                failures.append(f"{name}: {opening.split('(')[0].split()[-1]} has {found} PSRAM_JSON_DOC(doc), "
                                f"expected {count}")

    if failures:
        print("\n".join(f"FAIL: {failure}" for failure in failures), file=sys.stderr)
        return 1
    print("json documents: none on the default allocator outside hw1http::Stream; "
          "the eleven swept documents are PSRAM_JSON_DOC")
    return 0


if __name__ == "__main__":
    sys.exit(main())
