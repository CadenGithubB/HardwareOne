#!/usr/bin/env python3
"""The web guest gate lets a guest view every page and refuses what changes state.

Owner decision B6: guests view interfaces only. The harness compiles,
unchanged, the guest gate of WebServer_Utils.cpp (webGuestSendForbidden,
webGuestApiAllowed, webResolveGuestRole, webGuestMutationAllowed,
webGuestAccessAllowed) over a fake httpd request and a fake roster, and checks
that for a guest:

- every HTML page opens, /cli included (the page answers with its own
  view-only notice), and the read-only status GET endpoints the pages load
  their data from pass;
- POST /api/cli passes, so the command chokepoint decides (its read-only status
  lines run, every other line is the guest refusal; remote_command_authority's
  guest_read_only_surface case covers that half);
- every other POST, /api/cli/batch included, and the file, log and capture
  GETs are refused with a 403 whose JSON carries code "guest_forbidden" and the
  sentence the pages show ("Guest accounts are view-only");
- the private reads (owner, guest privacy 1-3, 2026-11-22: the ESP-NOW message
  history, the ring's health status, the LLM chat turns and streamed answer)
  are refused with a 403 whose JSON carries code "guest_private" and "Not
  available to guests", which the pages show as their own state;
- an account the roster cannot resolve is refused; a user is stopped only by
  the admin-only reads (the ESP-NOW message history: 403, code
  "admin_required"), and an administrator never.

--component points the suite at another copy of the production sources.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block, require  # noqa: E402

MARKERS = (
    "static void webGuestSendForbidden(",
    "static void webGuestSendPrivate(",
    "static void webSendAdminRequired(",
    "static bool webApiAdminOnly(",
    "static bool webGuestApiPrivate(",
    "static void webRequestPath(",
    "static bool webGuestApiAllowed(",
    "static bool webResolveGuestRole(",
    "bool webGuestMutationAllowed(",
    "bool webGuestAccessAllowed(",
)

PRELUDE = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Harness-local String: what the gate calls, over std::string.
class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  unsigned length() const { return static_cast<unsigned>(text_.length()); }
  const char* c_str() const { return text_.c_str(); }
  bool operator==(const char* other) const { return text_ == (other ? other : ""); }
  String& operator=(const char* text) { text_ = text ? text : ""; return *this; }
  const std::string& str() const { return text_; }
 private:
  std::string text_;
};

enum httpd_method { HTTP_GET = 1, HTTP_HEAD = 2, HTTP_POST = 3, HTTP_OPTIONS = 6, HTTP_PUT = 4 };
struct httpd_req_t { int method = HTTP_GET; char uri[512] = {}; };
struct Reply { std::string status, type, body; int sends = 0; };
static Reply gReply;
static int httpd_resp_set_status(httpd_req_t*, const char* s) { gReply.status = s; return 0; }
static int httpd_resp_set_type(httpd_req_t*, const char* t) { gReply.type = t; return 0; }
static int httpd_resp_sendstr(httpd_req_t*, const char* b) { gReply.body = b; gReply.sends++; return 0; }

struct AuthContext { String user; };
static bool hasNamedUser(const AuthContext& ctx) { return ctx.user.length() > 0; }

// Roster: gina a guest, alice a user, root an admin, owner the superadmin.
static const char* kRoleGuest = "guest";
static bool getUserAuthorizationRole(const String& user, String& role) {
  const std::string& u = user.str();
  if (u == "gina") { role = "guest"; return true; }
  if (u == "alice") { role = "user"; return true; }
  if (u == "root" || u == "owner") { role = "admin"; return true; }
  return false;
}
static bool isSuperAdminUser(const String& user) { return user.str() == "owner"; }
static bool isAdminUser(const String& user) { return user.str() == "root" || user.str() == "owner"; }
struct FsLockGuard { bool held = true; explicit FsLockGuard(const char*) {} };
static bool isFsLockedByCurrentTask() { return false; }
'''

DRIVER = r'''
static unsigned checks = 0;
static void checked(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "check failed: %s (status '%s' body '%s')\n", what.c_str(),
               gReply.status.c_str(), gReply.body.c_str());
  std::exit(1);
}
static bool gate(const char* user, int method, const char* uri) {
  gReply = Reply{};
  httpd_req_t req;
  req.method = method;
  std::snprintf(req.uri, sizeof(req.uri), "%s", uri);
  AuthContext ctx;
  ctx.user = user;
  return webGuestAccessAllowed(&req, ctx);
}
static std::string label(const char* user, int method, const char* uri) {
  return std::string(user) + (method == HTTP_POST ? " POST " : " GET ") + uri;
}
static void expectPass(const char* user, int method, const char* uri) {
  const bool ok = gate(user, method, uri);
  checked(ok && gReply.sends == 0, label(user, method, uri) + " passes");
}
static void expectRefusedAs(const char* kind, const char* user, int method, const char* uri) {
  const bool ok = gate(user, method, uri);
  checked(!ok && gReply.sends == 1 && gReply.status == "403 Forbidden" &&
              gReply.type == "application/json",
          label(user, method, uri) + " refused with one 403");
  std::printf("BODY %s %s\n", kind, gReply.body.c_str());
}
static void expectRefused(const char* user, int method, const char* uri) {
  expectRefusedAs("guest_forbidden", user, method, uri);
}

int main() {
  // Every page opens for a guest; /cli answers with its own notice.
  const char* pages[] = {"/dashboard", "/cli", "/sensors", "/espnow", "/bond", "/bluetooth",
                         "/settings", "/files", "/logging", "/maps", "/llm", "/automations",
                         "/power", "/battery", "/speech", "/mqtt", "/r1-health", "/games",
                         "/logout", "/dashboard?tab=1"};
  for (const char* page : pages) expectPass("gina", HTTP_GET, page);
  // The read-only status endpoints the pages load their data from.
  const char* reads[] = {"/api/system", "/api/sensors/status", "/api/sensors?sensor=gps&ts=1",
                         "/api/espnow/status", "/api/ble/status", "/api/logging/status",
                         "/api/automations", "/api/power/status", "/api/battery/status",
                         "/api/speech/status", "/api/mqtt/status", "/api/settings",
                         "/api/settings/schema", "/api/events", "/api/bond/status",
                         "/api/llm/status", "/api/gps/tracks", "/api/waypoints"};
  for (const char* uri : reads) expectPass("gina", HTTP_GET, uri);
  // The command line: the executor's guest surface decides.
  expectPass("gina", HTTP_POST, "/api/cli");
  expectPass("gina", HTTP_POST, "/api/cli?x=1");
  // Writes, batches, files, logs, captures and admin reads stay refused.
  const char* refusedPosts[] = {"/api/cli/batch", "/api/bond/cli/batch", "/api/bond/exec",
                                "/api/files/upload", "/api/files/write", "/api/user/settings",
                                "/api/llm/load", "/api/waypoints", "/api/health/action",
                                "/api/transcription", "/api/clix", "/api/cli/"};
  for (const char* uri : refusedPosts) expectRefused("gina", HTTP_POST, uri);
  expectRefused("gina", HTTP_PUT, "/api/cli");
  const char* refusedGets[] = {"/api/files/list?path=/maps", "/api/files/view?name=battery.csv",
                               "/api/cli/logs", "/api/sensors/camera/frame", "/api/recordings",
                               "/api/videos", "/api/debug/flags", "/api/admin/sessions",
                               "/api/transcription", "/api/bond/fs/list", "/api/cli"};
  for (const char* uri : refusedGets) expectRefused("gina", HTTP_GET, uri);
  // Private reads (owner, guest privacy 1-3): not for a guest, said as such.
  const char* privateReads[] = {"/api/espnow/messages", "/api/espnow/messages?since=0",
                                "/api/health/status", "/api/llm/chat/turns",
                                "/api/llm/result?session=3&offset=0"};
  for (const char* uri : privateReads) expectRefusedAs("guest_private", "gina", HTTP_GET, uri);
  // The pages that load them still open, and the neighbouring reads pass.
  expectPass("gina", HTTP_GET, "/r1-health");
  expectPass("gina", HTTP_GET, "/api/llm/status");
  expectPass("gina", HTTP_GET, "/api/espnow/metadata");
  // The ESP-NOW message history is admin-only: a user is refused, the other
  // private reads are a user's as before, and an administrator reads all.
  expectRefusedAs("admin_required", "alice", HTTP_GET, "/api/espnow/messages?since=4&mac=AA");
  expectRefusedAs("admin_required", "alice", HTTP_GET, "/api/espnow/messages");
  expectPass("alice", HTTP_GET, "/api/health/status");
  expectPass("alice", HTTP_GET, "/api/llm/chat/turns");
  expectPass("alice", HTTP_GET, "/api/espnow/status");
  expectPass("root", HTTP_GET, "/api/espnow/messages?since=0");
  expectPass("owner", HTTP_GET, "/api/espnow/messages");
  // An account the roster cannot resolve is refused, even for a page.
  expectRefused("ghost", HTTP_GET, "/dashboard");
  // Users and administrators are never stopped here.
  const char* others[] = {"alice", "root", "owner"};
  for (const char* user : others) {
    expectPass(user, HTTP_POST, "/api/cli/batch");
    expectPass(user, HTTP_POST, "/api/files/upload");
    expectPass(user, HTTP_GET, "/api/files/list?path=/");
  }
  // No signed-in name (require-auth off): the gate does not apply.
  expectPass("", HTTP_POST, "/api/files/upload");
  // A query that selects a state change on a read endpoint.
  {
    gReply = Reply{};
    httpd_req_t req;
    req.method = HTTP_GET;
    std::snprintf(req.uri, sizeof(req.uri), "/api/gps/tracks?live=save");
    AuthContext ctx;
    ctx.user = "gina";
    checked(!webGuestMutationAllowed(&req, ctx) && gReply.status == "403 Forbidden",
            "guest live-track save refused");
    ctx.user = "alice";
    gReply = Reply{};
    checked(webGuestMutationAllowed(&req, ctx) && gReply.sends == 0, "user live-track save passes");
  }
  std::printf("%u checks\n", checks);
  return 0;
}
'''


def generate(utils: str) -> str:
    pieces = []
    for marker in MARKERS:
        require(utils.count(marker) == 1, f"one {marker} in WebServer_Utils.cpp")
        pieces.append(extract_block(utils, marker))
    return PRELUDE + "\n\n".join(pieces) + "\n" + DRIVER


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++")
                        or shutil.which("c++"))
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    unit_text = generate((args.component / "WebServer_Utils.cpp").read_text())
    with tempfile.TemporaryDirectory(prefix="hw1-web-guest-gate-") as directory:
        unit = Path(directory) / "web_guest_gate.cpp"
        binary = Path(directory) / "web_guest_gate"
        unit.write_text(unit_text)
        compiled = subprocess.run(
            [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-Wno-unused-parameter", str(unit), "-o", str(binary)],
            capture_output=True, text=True, timeout=240)
        if compiled.returncode != 0:
            print(compiled.stderr[-4000:], file=sys.stderr)
            raise SystemExit("web_guest_gate: compile failed")
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        if run.returncode != 0:
            print(run.stdout[-2000:], (run.stderr or "").strip(), file=sys.stderr)
            raise SystemExit("web_guest_gate: FAILED")
    sentences = {
        "guest_forbidden": "Guest accounts are view-only",
        "guest_private": "Not available to guests",
        "admin_required": "Admin access required",
    }
    seen = set()
    for line in run.stdout.splitlines():
        if not line.startswith("BODY "):
            continue
        kind, body = line[5:].split(" ", 1)
        seen.add(kind)
        reply = json.loads(body)
        require(reply.get("success") is False and reply.get("code") == kind and
                reply.get("error") == sentences[kind] and reply.get("message") == sentences[kind],
                f"the {kind} refusal names its code and the sentence a page shows: {body}")
    require(seen == set(sentences), f"every refusal kind was checked: {sorted(seen)}")
    print(f"web_guest_gate: {run.stdout.splitlines()[-1]}; a guest views every page, "
          "POST /api/cli reaches the command guest surface, everything else is the refusal; "
          "private reads are not a guest's, the ESP-NOW history is an administrator's")


if __name__ == "__main__":
    main()
