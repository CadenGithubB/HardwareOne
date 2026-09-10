#!/usr/bin/env python3
"""Exercise the shipping file-response policy and both handler entry guards.

HTTP header storage is mocked. Source guards check placement before streaming;
the optional loopback fixture uses the compiled policy for real browser checks.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HARNESS = r'''
#include <cassert>
#include <iostream>
#include <map>
#include <string>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t HEADER_ERROR = -17;
struct httpd_req_t {
  int calls = 0;
  int failAt = 0;
  int bodies = 0;
  bool svg = false;
  std::map<std::string, std::string> headers;
};
int resumes = 0;
void pollResume() { ++resumes; }
struct TestPath {
  bool svg;
  bool endsWith(const char* suffix) const {
    assert(std::string(suffix) == ".svg");
    return svg;
  }
};
esp_err_t httpd_resp_set_hdr(httpd_req_t* req, const char* key, const char* value) {
  if (++req->calls == req->failAt) return HEADER_ERROR;
  req->headers[key] = value;
  return ESP_OK;
}
#define DEBUG_STORAGEF(...) ((void)0)
// INSERT_PRODUCTION_HERE
int main() {
  for (auto handler : {handleFileRead, handleFileView}) {
    for (bool svg : {false, true}) {
      httpd_req_t req;
      req.svg = svg;
      resumes = 0;
      assert(handler(&req) == ESP_OK);
      assert(req.calls == 3 && req.headers.size() == 3 && req.bodies == 1);
      assert(resumes == 0);
      const bool retainsOrigin = req.headers.at("Content-Security-Policy")
          .find("allow-same-origin") != std::string::npos;
      assert(retainsOrigin == (handler == handleFileRead || !svg));
      for (int failure = 1; failure <= 3; ++failure) {
        httpd_req_t failed;
        failed.svg = svg;
        failed.failAt = failure;
        resumes = 0;
        assert(handler(&failed) == HEADER_ERROR);
        assert(failed.calls == failure && failed.bodies == 0);
        assert(resumes == (handler == handleFileView ? 1 : 0));
      }
    }
  }
  for (bool isolate : {false, true}) {
    httpd_req_t req;
    assert(setFileResponsePolicy(&req, isolate) == ESP_OK);
    for (const auto& header : req.headers)
      std::cout << isolate << '\t' << header.first << '\t' << header.second << '\n';
  }
}
'''


def compile_and_check(cxx: str, sanitize: bool) -> dict[str, dict[str, str]]:
    source = (COMPONENT / "WebServer_Server.cpp").read_text()
    definitions = [extract_block(source, "static esp_err_t setFileResponsePolicy(")]
    # Only these two untrusted-file endpoints should get this policy. Applying
    # it globally would disable trusted firmware UI scripts.
    require(source.count("setFileResponsePolicy(") == 3,
            "review the scope of the file policy: expected helper and two callers")
    for name in ("handleFileRead", "handleFileView"):
        handler = extract_block(source, f"esp_err_t {name}(")
        if name == "handleFileRead":
            prefix, body = handler.split("pollPause();", 1)
            require("setFileResponsePolicy(req, false)" in prefix,
                    "raw text must retain its origin but disable scripting")
            require("if (policyErr != ESP_OK) return policyErr;" in prefix,
                    "file read must fail closed before polling")
            require("httpd_resp_" not in prefix and "VFS::" not in prefix,
                    "file read accessed data or responded before policy enforcement")
        else:
            before, guarded = handler.split("  path = decoded;", 1)
            guard, body = guarded.split("  if (isAdminOnlyPath(path)", 1)
            require('setFileResponsePolicy(req, path.endsWith(".svg"))' in guard,
                    "SVG isolation must use the decoded path, independent of mode=raw")
            require("if (policyErr != ESP_OK) {\n    pollResume();\n    return policyErr;" in guard,
                    "file view must fail closed and resume polling")
            for marker in ("VFS::", "streamViewerHead(", "httpd_resp_send_chunk(",
                           "httpd_query_key_value(query, \"mode\""):
                require(marker not in before, f"file view reaches {marker} before policy")
            prefix = ("esp_err_t handleFileView(httpd_req_t* req) {\n"
                      "TestPath path{req->svg};\n" + guard)
        for header in ("Content-Security-Policy", "X-Content-Type-Options", "Cache-Control"):
            require(header not in body, f"{name}: must not override {header} in a branch")
        # Execute the actual policy guard; a sentinel stands in for the existing
        # auth/format/streaming body. This does not emulate those firmware paths.
        definitions.append(prefix + "++req->bodies; return ESP_OK; }")
    harness = HARNESS.replace("// INSERT_PRODUCTION_HERE", "\n\n".join(definitions))
    with tempfile.TemporaryDirectory(prefix="hw1-file-policy-") as temp:
        generated = Path(temp) / "policy.cpp"
        executable = Path(temp) / "policy"
        generated.write_text(harness)
        command = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   str(generated), "-o", str(executable)]
        if sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        output = subprocess.check_output([str(executable)], text=True)
    policies = {"0": {}, "1": {}}
    for line in output.splitlines():
        mode, key, value = line.split("\t", 2)
        policies[mode][key] = value
    for mode, headers in policies.items():
        check_headers(headers, isolate_origin=mode == "1")
    return policies


def check_headers(headers: dict[str, str], isolate_origin: bool) -> None:
    directives = {}
    for directive in headers["Content-Security-Policy"].split(";"):
        key, *values = directive.split()
        require(key not in directives, f"duplicate CSP directive: {key}")
        directives[key] = set(values)
    sandbox = {"allow-downloads"}
    if not isolate_origin:
        sandbox.add("allow-same-origin")
    require(directives["sandbox"] == sandbox,
            "SVG must have an opaque origin; all file responses must disable scripts/forms")
    for directive in ("default-src", "script-src", "base-uri", "form-action"):
        require(directives[directive] == {"'none'"}, f"unsafe {directive}")
    require(directives["style-src"] == {"'unsafe-inline'"}, "static preview styling")
    require(directives["img-src"] == {"data:"}, "allow embedded images only")
    require(directives["media-src"] == {"'self'"}, "native local audio playback")
    require(headers["X-Content-Type-Options"] == "nosniff", "prevent MIME sniffing")
    require(headers["Cache-Control"] == "no-store", "do not cache private file previews")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--serve-fixture", action="store_true",
                        help="serve synthetic browser cases on loopback after host checks")
    parser.add_argument("--port", type=int, default=18765)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    headers = compile_and_check(args.cxx, args.sanitize)
    print("file-response policy and fail-closed entry guards passed", flush=True)
    if args.serve_fixture:
        from web_file_response_fixture import serve
        serve(headers, args.port)


if __name__ == "__main__":
    main()
