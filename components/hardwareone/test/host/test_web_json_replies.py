#!/usr/bin/env python3
"""Hand-built web JSON replies and response-header storage.

Compiles the production handleAutomationsExport, handleIconGet,
handleFilesCreate, handleMicRecordingsList and sseSendNotice bodies (plus the
shared webJsonString / sendJsonError and the recordings-list builder where the
tree has them) into web_json_replies_harness.cpp with real ArduinoJson and a
local Arduino String stub. The fake httpd keeps header pointers and reads them
at httpd_resp_send, so a header value that dies first is an ASan
use-after-free (run with --sanitize). Every JSON payload the harness prints is
parsed here with Python's strict json module and compared with the text that
went in: file names with backslashes, commas, colons, quotes and control
characters; command errors with quotes and newlines; SSE notices.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --case <name>.
Not covered: the map feature, GPS track and LLM status replies (same helper,
no harness), and the producer side of the recordings list.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"
CASES = ("export", "icon", "files_create", "recordings", "notice")

ARDUINO_STUB = r'''
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  String& operator=(const char* text) { text_ = text ? text : ""; return *this; }
  char operator[](size_t i) const { return i < text_.size() ? text_[i] : '\0'; }
  char charAt(size_t i) const { return (*this)[i]; }
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.size(); }
  void reserve(size_t n) { text_.reserve(n); }
  bool concat(const char* text) { text_ += text ? text : ""; return true; }
  int indexOf(char c, size_t from = 0) const { auto p = text_.find(c, from); return p == std::string::npos ? -1 : int(p); }
  int indexOf(const char* s, size_t from = 0) const { auto p = text_.find(s, from); return p == std::string::npos ? -1 : int(p); }
  int lastIndexOf(char c) const { auto p = text_.rfind(c); return p == std::string::npos ? -1 : int(p); }
  String substring(size_t from) const { return from >= text_.size() ? std::string() : text_.substr(from); }
  String substring(size_t from, size_t to) const {
    if (from >= text_.size() || to <= from) return String();
    return text_.substr(from, std::min(to, text_.size()) - from);
  }
  bool startsWith(const String& p) const { return text_.rfind(p.text_, 0) == 0; }
  bool endsWith(const String& s) const {
    return text_.size() >= s.text_.size() && text_.compare(text_.size() - s.text_.size(), s.text_.size(), s.text_) == 0;
  }
  void replace(const char* from, const char* to) {
    const std::string f(from), t(to);
    if (f.empty()) return;
    for (size_t p = text_.find(f); p != std::string::npos; p = text_.find(f, p + t.size())) text_.replace(p, f.size(), t);
  }
  String& operator+=(const String& o) { text_ += o.text_; return *this; }
  String& operator+=(const char* o) { text_ += o ? o : ""; return *this; }
  String& operator+=(char c) { text_ += c; return *this; }
  String& operator+=(int v) { text_ += std::to_string(v); return *this; }
  String& operator+=(unsigned long v) { text_ += std::to_string(v); return *this; }
  friend String operator+(const String& a, const String& b) { return a.text_ + b.text_; }
  friend bool operator==(const String& a, const String& b) { return a.text_ == b.text_; }
  friend bool operator!=(const String& a, const String& b) { return a.text_ != b.text_; }
 private:
  std::string text_;
};
'''


def optional(source: str, marker: str) -> list[str]:
    if source.count(marker) == 0:
        return []
    require(source.count(marker) == 1, f"one definition: {marker}")
    return [extract_block(source, marker)]


def required(source: str, marker: str) -> str:
    require(source.count(marker) == 1, f"one definition: {marker}")
    return extract_block(source, marker)


def production() -> str:
    utils_h = (COMPONENT / "WebServer_Utils.h").read_text()
    utils_cpp = (COMPONENT / "WebServer_Utils.cpp").read_text()
    server = (COMPONENT / "WebServer_Server.cpp").read_text()
    sensors = (COMPONENT / "WebPage_Sensors.cpp").read_text()
    events = (COMPONENT / "WebServer_Events.cpp").read_text()
    filesystem = (COMPONENT / "System_Filesystem.cpp").read_text()
    pieces: list[str] = []
    pieces += optional(utils_h, "inline String webJsonString(const String& text)")
    pieces += optional(utils_cpp, "String webJsonString(const char* text)")
    pieces += optional(utils_cpp, "esp_err_t sendJsonError(")
    pieces.append(required(filesystem, "String quotePath("))
    pieces += optional(server, "static void automationExportDisposition(")
    pieces.append(required(server, "esp_err_t handleAutomationsExport("))
    pieces.append(required(server, "esp_err_t handleIconGet("))
    pieces.append(required(server, "esp_err_t handleFilesCreate("))
    pieces += optional(sensors, "static String buildRecordingsListJson(")
    pieces.append(required(sensors, "esp_err_t handleMicRecordingsList("))
    pieces.append(required(events, "bool sseSendNotice("))
    return "\n\n".join(pieces)


def unescape(text: str) -> str:
    out, i = [], 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            out.append({"\\": "\\", "n": "\n", "r": "\r", "t": "\t"}[text[i + 1]])
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def payloads(stdout: str) -> dict[str, list[str]]:
    found: dict[str, list[str]] = {}
    for line in stdout.splitlines():
        tag, _, rest = line.partition("\t")
        found.setdefault(tag, []).append(unescape(rest))
    return found


def strict(payload: str):
    return json.loads(payload)


def validate(case: str, found: dict[str, list[str]]) -> list[str]:
    problems: list[str] = []

    def expect(condition: bool, message: str) -> None:
        if not condition:
            problems.append(message)

    try:
        if case == "files_create":
            error = 'Error: Cannot create "/a\\b"\nsecond line\t\x01 end'
            for tag in ("CREATE_FOLDER_ERROR", "CREATE_FILE_ERROR"):
                reply = strict(found[tag][0])
                expect(reply == {"success": False, "error": error}, f"{tag}: {reply!r}")
        elif case == "recordings":
            reply = strict(found["RECORDINGS"][0])
            expected = [("plain.wav", 10), ("take\\1.wav", 20), ("a,b.wav", 30), ("x:y.wav", 40),
                        ('q"uote.wav', 5), ("ctl\x01.wav", 6), ("zero.wav", 0), ("lead,0.wav", 12)]
            expect(reply.get("count") == len(expected), f"recordings count: {reply.get('count')}")
            expect([(f["name"], f["size"]) for f in reply.get("files", [])] == expected,
                   f"recordings files: {reply.get('files')!r}")
            expect(strict(found["RECORDINGS_EMPTY"][0]) == {"count": 0, "files": []}, "empty recordings")
        elif case == "notice":
            frame = found["NOTICE_FRAME"][0]
            prefix = "event: notice\ndata: "
            expect(frame.startswith(prefix) and frame.endswith("}\n\n"), f"notice frame: {frame!r}")
            data = frame[len(prefix):-2]
            expect("\n" not in data, "notice data stays on one line")
            expect(strict(data) == {"msg": 'say "hi"\\ok\nnext\ttab'}, f"notice payload: {data!r}")
    except (KeyError, IndexError) as missing:
        problems.append(f"missing payload {missing}")
    except json.JSONDecodeError as invalid:
        problems.append(f"invalid JSON: {invalid}")
    for line in found.get("FAIL", []):
        problems.append(line)
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", choices=CASES)
    args = parser.parse_args()

    harness = (HERE / "web_json_replies_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    harness = harness.replace("// INSERT_PRODUCTION", production())
    failed = []
    with tempfile.TemporaryDirectory(prefix="hw1-web-json-replies-") as temporary:
        directory = Path(temporary)
        (directory / "Arduino.h").write_text(ARDUINO_STUB)
        unit, binary = directory / "web_json_replies.cpp", directory / "web_json_replies"
        unit.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
                   "-I", str(directory), "-I", str(JSON_INCLUDE), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        for case in ([args.case] if args.case else CASES):
            result = subprocess.run([str(binary), case], capture_output=True, text=True,
                                    errors="replace", timeout=60)
            problems = validate(case, payloads(result.stdout))
            if result.returncode != 0:
                tail = "\n".join(result.stderr.strip().splitlines()[:6])
                problems.append(f"harness exit {result.returncode}" + (f"\n{tail}" if tail else ""))
            if problems:
                failed.append(case)
                print(f"FAIL {case}:")
                for problem in problems:
                    print(f"  {problem}")
            else:
                print(f"ok   {case}")
    if failed:
        print(f"web json replies: {len(failed)} of {len(CASES) if not args.case else 1} cases failed")
        return 1
    print("web json replies: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
