#!/usr/bin/env python3
"""One session cookie read per authenticated web request.

WEB_AUTH_OR_RETURN builds the request's AuthContext with makeWebAuthCtx, which
reads the session cookie into ctx.sid. tgRequireAuth then called isAuthed,
which read the cookie again, and read it a third time into ctx.sid; four
handlers read it a fourth time. Each read is a new String, one heap block for
a 32-character session id. isAuthed now takes the sid the caller read, and
tgRequireAuth and the handlers use ctx.sid.

The production prologue (WEB_AUTH_OR_RETURN, makeWebAuthCtx, tgRequireAuth
and AuthContext) runs on the real Arduino String with its heap calls counted:
one cookie read, two String blocks for a cookie session, the same sid reaching
isAuthed and the handler, the same auth log arguments, the same refusal.
Source checks: the sid form of isAuthed and the four handlers read no cookie,
and every direct tgRequireAuth caller builds its context with makeWebAuthCtx
(tgRequireAuth relies on ctx.sid).

Options: --cxx <compiler>, --sanitize, --component <dir> to run against
another components/hardwareone directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from arduino_string_host import arduino_string_counted
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HANDLERS = (
    "esp_err_t handleSessionsList(httpd_req_t* req)",
    "esp_err_t handleSensorsStatusWithUpdates(httpd_req_t* req)",
    "esp_err_t handleCliCommand(httpd_req_t* req)",
    "esp_err_t handleCliBatch(httpd_req_t* req)",
)


def macro(source: str, name: str) -> str:
    start = source.index(f"#define {name}(")
    lines = []
    for line in source[start:].splitlines():
        lines.append(line)
        if not line.rstrip().endswith("\\"):
            break
    return "\n".join(lines)


def generate(component: Path) -> str:
    user_h = (component / "System_User.h").read_text()
    server_h = (component / "WebServer_Server.h").read_text()
    utils_h = (component / "WebServer_Utils.h").read_text()
    user = (component / "System_User.cpp").read_text()
    pieces = [
        extract_block(user_h, "enum CommandSource {") + ";",
        extract_block(user_h, "struct AuthContext {") + ";",
        extract_block(server_h, "inline AuthContext makeWebAuthCtx(httpd_req_t* req)"),
        "bool webGuestAccessAllowed(httpd_req_t* req, const AuthContext& ctx);",
        macro(utils_h, "WEB_AUTH_OR_RETURN"),
        extract_block(user, "bool tgRequireAuth(AuthContext& ctx)"),
    ]
    harness = (HERE / "web_auth_cookie_reads_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION"
    require(harness.count(marker) == 1, "one insertion marker")
    return harness.replace(marker, "\n\n".join(pieces))


def source_contract(component: Path) -> None:
    server = (component / "WebServer_Server.cpp").read_text()
    marker = "bool isAuthed(httpd_req_t* req, const String& sid, String& outUser)"
    require(server.count(marker + " {") == 1, "WebServer_Server.cpp defines isAuthed(req, sid, outUser) once")
    require("getCookieSid(" not in extract_block(server, marker),
            "isAuthed(req, sid, outUser) uses the sid it is given")
    for handler in HANDLERS:
        require("getCookieSid(" not in extract_block(server, handler),
                f"{handler.split('(')[0].split()[-1]} uses ctx.sid, not a second cookie read")
    user = (component / "System_User.cpp").read_text()
    auth = extract_block(user, "bool tgRequireAuth(AuthContext& ctx)")
    require("getCookieSid(" not in auth and "isAuthed(req, ctx.sid, userTmp)" in auth,
            "tgRequireAuth hands isAuthed the ctx.sid that makeWebAuthCtx read")
    # tgRequireAuth relies on ctx.sid: each direct caller builds ctx with
    # makeWebAuthCtx just before (the macros do so in WebServer_Utils.h).
    call = re.compile(r"\btgRequireAuth\(ctx\)")
    for path in sorted(component.glob("*.cpp")):
        text = path.read_text()
        for match in call.finditer(text):
            before = text[max(0, match.start() - 600):match.start()]
            require("AuthContext ctx = makeWebAuthCtx(req);" in before,
                    f"{path.name}: tgRequireAuth(ctx) without makeWebAuthCtx(req) just before")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="hw1-web-auth-cookie-") as directory:
        d = Path(directory)
        includes, sources = arduino_string_counted(d, sanitize)
        unit, binary = d / "test.cpp", d / "test"
        unit.write_text(generate(args.component))
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-sign-compare",
                        *sanitize, *includes, str(unit), *sources, "-o", str(binary)],
                       check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=30)
    source_contract(args.component)
    print("web auth cookie reads source contract passed")


if __name__ == "__main__":
    main()
