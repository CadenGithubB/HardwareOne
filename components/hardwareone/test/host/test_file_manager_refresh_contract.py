#!/usr/bin/env python3
"""The web file manager (createFileManager in WebServer_Utils.h) refreshes the
page's storage figures once per folder load.

On the Files page config.onRefresh asks /api/files/stats, the storage figure:
on the P4X-EYE a LittleFS walk of about 1.3 s that holds the single web server
task, when the shared reading is older than 10 s or marked stale. Every folder load, including the reload after an action, ends
in the explorer's onNavigate, which calls onRefresh. Actions used to call it a
second time right after loadExplorer(), so every new folder, new file, upload
and Refresh cost two walks. This guard pins the single call.
"""
from __future__ import annotations

from pathlib import Path
import re
import sys

COMPONENT = Path(__file__).resolve().parents[2]


def js_function(source: str, marker: str) -> str:
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:pos + 1]
    raise AssertionError(f"unterminated function: {marker}")


def main() -> None:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else COMPONENT / "WebServer_Utils.h"
    source = path.read_text(encoding="utf-8")
    manager = js_function(source, "window.createFileManager = function")
    calls = manager.count("config.onRefresh(")
    assert calls == 1, f"createFileManager calls config.onRefresh {calls} times; one, in onNavigate, is enough"
    explorer = js_function(manager, "function loadExplorer()")
    navigate = js_function(explorer, "onNavigate: function(path, dirPerms)")
    assert "if (config.onRefresh) config.onRefresh(currentPath);" in navigate, "onNavigate keeps the refresh"
    # Each action still reloads the folder, which is what runs that refresh.
    for action in ("CreateFolder", "CreateFile", "Refresh"):
        body = js_function(manager, f"window[managerId + '{action}'] = function()")
        assert "loadExplorer();" in body, f"{action} reloads the folder"
    upload = js_function(manager, "window[managerId + 'UploadFile'] = function()")
    done = js_function(upload, "onDone: function(ok, msg)")
    assert re.search(r"if \(ok\) loadExplorer\(\);", done), "a finished upload reloads the folder"
    print("file manager refresh contract passed")


if __name__ == "__main__":
    main()
