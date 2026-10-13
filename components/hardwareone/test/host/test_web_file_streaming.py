#!/usr/bin/env python3
"""Lock discipline of the web file streaming paths.

Compiles the shipping streamFileChunks helper, handleFileRead,
streamHtmlEscapedChecked and the GPS track listing tail of handleGPSTracksAPI
against an httpd double that records whether the filesystem mutex or a sensor
poll pause was held inside any chunk send, and that fails one send to prove
each loop stops there. handleFileView is checked at source level: its non-JSON
branches must stream through the helper, keep no function-scope lock and resume
polling once before any transfer. The File/VFS stand-in is harness-local and
tiny; persistence has its own suites.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

LISTING_MARKER = "  // No query params - list available GPS log files"


HELPER_MARKER = "static esp_err_t streamFileChunks("
ESCAPER_MARKER = "esp_err_t streamHtmlEscapedChecked("


def production() -> str:
    """The production unit: handleFileRead and the GPS listing always, plus the
    streaming helper and the checked escaper when the sources have them. A tree
    without them still compiles, so its behavioural failures are what the
    harness reports (the source-level checks run afterwards)."""
    server = (COMPONENT / "WebServer_Server.cpp").read_text()
    utils = (COMPONENT / "WebServer_Utils.cpp").read_text()
    maps = (COMPONENT / "WebPage_Maps.cpp").read_text()

    read_handler = extract_block(server, "esp_err_t handleFileRead(")
    policy = extract_block(server, "static esp_err_t setFileResponsePolicy(")

    helper_present = HELPER_MARKER in server and ESCAPER_MARKER in utils
    parts = [f"#define HW1_STREAM_HELPER_PRESENT {1 if helper_present else 0}", policy]
    if helper_present:
        require(server.count(HELPER_MARKER) == 1, "streamFileChunks marker must be unique")
        require(utils.count(ESCAPER_MARKER) == 1, "streamHtmlEscapedChecked marker must be unique")
        parts.append(extract_block(utils, ESCAPER_MARKER))
        parts.append(extract_block(server, HELPER_MARKER))
    else:
        print("note: streamFileChunks / streamHtmlEscapedChecked not found; compiling the handlers alone",
              flush=True)
    parts.append(read_handler)

    # GPS listing: the tail of handleGPSTracksAPI after the query handling,
    # wrapped as its own function so the harness can drive it.
    tracks = extract_block(maps, "esp_err_t handleGPSTracksAPI(")
    require(LISTING_MARKER in tracks, "GPS track listing marker moved")
    parts.append("static esp_err_t gpsTrackListing(httpd_req_t* req, AuthContext& ctx) {\n"
                 + tracks[tracks.index(LISTING_MARKER):])

    return "\n\n".join(parts)


def check_sources() -> None:
    """Source-level checks on the parts the harness cannot drive: handleFileView's
    JSON and non-JSON branches and the shape of handleFileRead."""
    server = (COMPONENT / "WebServer_Server.cpp").read_text()
    utils = (COMPONENT / "WebServer_Utils.cpp").read_text()

    require(HELPER_MARKER in server,
            "WebServer_Server.cpp must own a streamFileChunks helper that locks per read")
    require(ESCAPER_MARKER in utils,
            "WebServer_Utils.cpp must provide the stop-on-error HTML escaper")

    read_handler = extract_block(server, "esp_err_t handleFileRead(")

    # handleFileRead: the lock covers the open only, polling resumes before the
    # transfer, and the body goes out through the helper.
    require("streamFileChunks(req, f," in read_handler, "handleFileRead must stream through the helper")
    require("\n  FsLockGuard " not in read_handler,
            "handleFileRead must not hold a function-scope filesystem lock")
    require(read_handler.index("pollResume();", read_handler.index("VFS::openGuarded"))
            < read_handler.index("streamFileChunks(req, f,"),
            "handleFileRead must resume polling before streaming")

    # handleFileView: no function-scope lock on the non-JSON path, every
    # chunked branch uses the helper, the viewer's escaper is the checked one,
    # and polling resumes exactly once before any transfer.
    view = extract_block(server, "esp_err_t handleFileView(")
    non_json = view[view.index("// Default: non-JSON"):]
    require('FsLockGuard fsLock("handleFileView")' not in non_json,
            "handleFileView must not hold the filesystem lock for the whole non-JSON path")
    require(non_json.count("streamFileChunks(req, file, viewBuf, VIEW_BUF_SIZE") == 4,
            "handleFileView: image, chunked audio, .hwmap and text branches must stream through the helper")
    require("streamHtmlEscaped(req, viewBuf" not in non_json and "streamHtmlEscaped(req, lineStr" not in non_json,
            "handleFileView must escape through the stop-on-error variant")
    require("streamHtmlEscapedChecked(req, lineStr" in non_json, "sealed-capture lines must stop on a send error")
    require(non_json.count("pollResume();") == 1, "handleFileView non-JSON path must resume polling exactly once")
    require(non_json.index("pollResume();") < non_json.index("httpd_resp_set_type"),
            "handleFileView must resume polling before the first response byte")
    for loop in re.finditer(r"while \(true\) \{(.*?)\n  \}", non_json, re.S):
        body = loop.group(1)
        if "httpd_resp_send_chunk" in body or "streamHtmlEscapedChecked" in body:
            require("FsLockGuard" in body, "every remaining send loop in handleFileView must lock per read")
            require("if (sendErr != ESP_OK) break;" in body, "every send loop must stop on a send error")

    # After every transfer the handler's result follows the send: ESP_FAIL after
    # a failed send is what httpd uses to close the socket, so a keep-alive
    # session is never reused behind an unterminated chunked body.
    transfers = [m.end() for m in re.finditer(r"streamFileChunks\(req, file, viewBuf, VIEW_BUF_SIZE", non_json)]
    transfers += [m.end() for m in re.finditer(r'httpd_resp_send\(req, buf, fileSize\)|httpd_resp_send\(req, "", 0\)', non_json)]
    require(len(transfers) == 6, "handleFileView: four chunked transfers and two single-shot audio sends")
    for at in transfers:
        tail = non_json[non_json.index("return ", at):]
        require(tail.startswith("return sendErr == ESP_OK ? ESP_OK : ESP_FAIL;"),
                "handleFileView must return ESP_FAIL after a failed send")
    require("sendErr = raw ? httpd_resp_send_chunk(req, NULL, 0) : streamViewerTail(req);" in non_json,
            "the text viewer's tail and terminator must report their send result")

    # The JSON viewer: polling resumes after the open, each send is checked and
    # ends the transfer, the escaper is the checked one, no terminator follows
    # a failure, and both the raw and the pretty path return ESP_FAIL after it.
    json_branch = view[view.index("if (isJson) {"):view.index("// Default: non-JSON")]
    require(json_branch.count("pollResume();") == 2,
            "JSON view: polling resumes once after the open (plus the allocation-failure path)")
    require(json_branch.index("pollResume();  // sensor polling") < json_branch.index('fsLock("file.view.json.open")'),
            "JSON view must resume polling before the transfer")
    require("sendErr = httpd_resp_send_chunk(req, gFileReadBuf, bytesRead);" in json_branch,
            "raw JSON view must check every chunk send")
    require("streamHtmlEscaped(req, gFileOutBuf" not in json_branch and
            "streamHtmlEscapedChecked(req, gFileOutBuf, outLen)" in json_branch,
            "pretty JSON view must escape through the stop-on-error variant")
    require(json_branch.count("if (sendErr != ESP_OK) break;") >= 3,
            "every JSON send loop must stop on a send error")
    require("if (sendErr == ESP_OK) sendErr = httpd_resp_send_chunk(req, NULL, 0);" in json_branch and
            "if (sendErr == ESP_OK) sendErr = streamViewerTail(req);" in json_branch,
            "no JSON terminator after a failed send")
    require(json_branch.count("return sendErr == ESP_OK ? ESP_OK : ESP_FAIL;") == 2,
            "both JSON paths must return ESP_FAIL after a failed send")
    tail_helper = extract_block(server, "static esp_err_t streamViewerTail(")
    require("if (err != ESP_OK) return err;" in tail_helper and "return httpd_resp_send_chunk(req, NULL, 0);" in tail_helper,
            "streamViewerTail must stop at a failed tail chunk and return the terminator's result")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    harness = (HERE / "web_file_streaming_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION_HERE") == 1, "missing production marker")
    unit = harness.replace("// INSERT_PRODUCTION_HERE", production())
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(HERE / "web_batch_stubs")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-web-file-streaming-") as directory:
        source = Path(directory) / "streaming.cpp"
        binary = Path(directory) / "streaming"
        source.write_text(unit)
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=60)
    check_sources()
    print("web file streaming lock discipline passed", flush=True)


if __name__ == "__main__":
    main()
