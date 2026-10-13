#!/usr/bin/env python3
"""Source guard: camera images reach the web only one frame per request.

esp_httpd runs every handler on one task, so a handler that loops capturing
and sending frames (an MJPEG stream) holds every other page, poll, SSE and
login until its viewer disconnects. This checks that no stream endpoint is
registered, that no multipart/x-mixed-replace response exists in the web
sources, and that every httpd handler which captures a camera frame does so
once, without a loop.

It reads source text only; it does not compile or run the handlers.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HANDLER = re.compile(r"^(?:static\s+)?esp_err_t\s+(\w+)\(httpd_req_t\s*\*\s*\w+\)\s*\{", re.M)
LOOP = re.compile(r"\bwhile\s*\(|\bfor\s*\(|\bdo\s*\{")


def web_sources() -> list[Path]:
    sources = []
    for pattern in ("WebPage_*.cpp", "WebPage_*.h", "WebServer_*.cpp", "WebServer_*.h", "System_Camera*Web*.h",
                    "System_Camera*Web*.cpp"):
        sources.extend(sorted(COMPONENT.glob(pattern)))
    require(bool(sources), "web sources found")
    return sources


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", help="accepted for a uniform runner interface; unused")
    parser.add_argument("--sanitize", action="store_true", help="accepted for a uniform runner interface; unused")
    parser.parse_args()

    capturing = 0
    for path in web_sources():
        text = path.read_text()
        require("/camera/stream" not in text, f"{path.name} still names a camera stream endpoint")
        require("multipart/x-mixed-replace" not in text, f"{path.name} still sends a multipart stream")
        require(not re.search(r"esp_err_t\s+handleCameraStream\([^)]*\)\s*\{", text),
                f"{path.name} still defines handleCameraStream")
        for match in HANDLER.finditer(text):
            body = extract_block(text[match.start():], match.group(0)[:-1].rstrip())
            # Calls only: `extern uint8_t* captureFrame(...)` declarations do not count.
            calls = body.count("captureFrame(") - len(re.findall(r"\*\s*captureFrame\(", body))
            if calls == 0:
                continue
            capturing += 1
            require(calls == 1, f"{path.name}:{match.group(1)} captures more than one frame per request")
            require(not LOOP.search(body), f"{path.name}:{match.group(1)} loops on the httpd task while capturing")

    sensors = (COMPONENT / "WebPage_Sensors.cpp").read_text()
    registration = extract_block(sensors, "void registerSensorHandlers(httpd_handle_t server)")
    require('"/api/sensors/camera/frame"' in registration, "the single-frame endpoint stays registered")
    require(capturing >= 1, "the frame handler was found and checked")
    print(f"camera web frame-only guard: {capturing} capturing handler(s) checked")


if __name__ == "__main__":
    main()
