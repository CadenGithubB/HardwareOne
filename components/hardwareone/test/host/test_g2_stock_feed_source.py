#!/usr/bin/env python3
"""The G2 stock widget feed stays inside its budgets.

Source contract for G2_StockFeed.cpp / G2_StockFeedCore.h (nothing is
compiled; comments and string literals are blanked first):

- no task, no raw libc allocation, no internal-RAM static buffers; the one
  allocation is ps_alloc with AllocPolicy::RequirePsram;
- none of the slow reads the sampler is kept away from (C6 memory RPC, fresh
  filesystem stats, WiFi.RSSI());
- the G2 owner lap ticks it exactly once, directly after g2HealthPushTick();
- the wire boundary re-validates every payload against the whitelist before
  sendPbFragmented, and only on the right temple;
- the dispatcher hands it sid 0x01 envelopes and still runs handleEnvelope;
- the feed is off by default (settings constructor and settings row).

--component points the suite at another component directory.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_system_metrics_source import blank_comments_and_strings, line_of  # noqa: E402

COMPONENT = HERE.parents[1]
FEED_FILES = ("G2_StockFeed.cpp", "G2_StockFeed.h", "G2_StockFeedCore.h")

BANNED = {
    r"\bxTaskCreate\w*\s*\(": "new task",
    r"(?<![\w.])(?:malloc|calloc|realloc)\s*\(": "raw allocation",
    r"\bheap_caps_malloc\w*\s*\(": "raw allocation",
    r"\bnew\s+(?!\()": "heap new (use ps_alloc + placement new)",
    r"(?<!EXT_RAM_BSS_ATTR )\bstatic\s+char\s+\w+\s*\[":
        "internal-RAM static buffer (use the PSRAM block or EXT_RAM_BSS_ATTR)",
    r"\bradioHalQueryMemory\s*\(": "C6 memory RPC",
    r"\bRSSI\s*\(": "WiFi.RSSI()",
    r"\bVFS::getStats\s*\(": "fresh VFS stat",
    r"\besp_littlefs_info\s*\(": "fresh LittleFS stat",
}


def text(path: Path) -> str:
    return blank_comments_and_strings(path.read_text(encoding="utf-8"))


def function_body(code: str, signature: str) -> str:
    start = code.find(signature)
    if start < 0:
        return ""
    brace = code.find("{", start)
    depth, j = 0, brace
    while j < len(code):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return code[brace:j + 1]
        j += 1
    return ""


def check(component: Path) -> list[str]:
    errors: list[str] = []
    for name in FEED_FILES:
        path = component / name
        if not path.is_file():
            errors.append(f"{name}: missing")
            continue
        code = text(path)
        for pattern, why in BANNED.items():
            for m in re.finditer(pattern, code):
                errors.append(f"{name}:{line_of(code, m.start())}: {why}: {m.group(0).strip()}")
        for m in re.finditer(r"\bps_alloc\s*\(", code):
            stmt = code[m.end(): code.find(";", m.end())]
            if "RequirePsram" not in stmt:
                errors.append(f"{name}:{line_of(code, m.start())}: ps_alloc without "
                              "AllocPolicy::RequirePsram")

    glasses = text(component / "G2_Glasses.cpp")
    ticks = [m.start() for m in re.finditer(r"\bg2StockFeedTick\s*\(\s*\)\s*;", glasses)]
    if len(ticks) != 1:
        errors.append(f"G2_Glasses.cpp: g2StockFeedTick() called {len(ticks)} times, want 1")
    else:
        health = glasses.rfind("g2HealthPushTick();", 0, ticks[0])
        if health < 0 or glasses[health + len("g2HealthPushTick();"):ticks[0]].strip():
            errors.append("G2_Glasses.cpp: g2StockFeedTick() does not follow g2HealthPushTick() "
                          "directly in the owner lap")

    owner = function_body(glasses, "static void heartbeatWorkerTask(")
    if not re.search(r"\bg2StockFeedOwnerWaitMs\s*\(\s*\)", owner):
        errors.append("G2_Glasses.cpp: the owner lap no longer caps its wait with "
                      "g2StockFeedOwnerWaitMs()")

    send = function_body(glasses, "bool g2StockFeedWireSend(")
    if not send:
        errors.append("G2_Glasses.cpp: g2StockFeedWireSend is missing")
    else:
        validate = send.find("validateMessagePb(")
        write = send.find("sendPbFragmented(")
        if validate < 0 or write < 0 or validate > write:
            errors.append("G2_Glasses.cpp: g2StockFeedWireSend does not re-validate before writing")
        if not re.search(r"sendPbFragmented\(\s*gR\s*,", send):
            errors.append("G2_Glasses.cpp: g2StockFeedWireSend writes to a temple other than gR")

    dispatch = function_body(glasses, "static void g2DispatchCompleteEnvelope(")
    rx = dispatch.find("g2StockFeedOnRx(")
    handle = dispatch.find("handleEnvelope(")
    if rx < 0 or handle < 0 or rx > handle:
        errors.append("G2_Glasses.cpp: g2DispatchCompleteEnvelope must hand sid 0x01 to "
                      "g2StockFeedOnRx and then still run handleEnvelope")

    settings = (component / "System_Settings.h").read_text(encoding="utf-8")
    if not re.search(r"\bg2StockFeed\s*\(\s*false\s*\)", settings):
        errors.append("System_Settings.h: g2StockFeed is not off by default")
    rows = (component / "System_SensorLogging.cpp").read_text(encoding="utf-8")
    if not re.search(r'\{\s*"g2StockFeed"\s*,\s*SETTING_BOOL\s*,\s*&gSettings\.g2StockFeed\s*,\s*0\s*,', rows):
        errors.append("System_SensorLogging.cpp: the g2StockFeed row is not off by default")

    cmake = (component / "CMakeLists.txt").read_text(encoding="utf-8")
    if not re.search(r"^\s+G2_StockFeed\.cpp\s*$", cmake, re.MULTILINE):
        errors.append("CMakeLists.txt: G2_StockFeed.cpp is not in the source list")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    errors = check(args.component.resolve())
    for e in errors:
        print(e, file=sys.stderr)
    if errors:
        print(f"g2_stock_feed_source: {len(errors)} problem(s)", file=sys.stderr)
        return 1
    print("g2_stock_feed_source: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
