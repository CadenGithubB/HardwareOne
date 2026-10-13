#!/usr/bin/env python3
"""Source guard: retired auth, session, SSE, help, registry and mutex helpers stay gone.

These helpers had no caller, and several would leak or mint credentials if a
caller came back: a second per-transport admission policy in tgRequireAuth, an
unauthenticated session publisher (authSuccessUnified), header and cookie
readers that broadcast the session cookie, a session-minting /login/setsession
route, a login-success page that wrote the session id into its HTML, and a
`[revoke]` notice path that could never be delivered. With that path gone the
per-session notice queue had no producer and no client (no page fetches
/api/notice or listens for an SSE `notice` event), so the queue, its handler,
route, guest-gate entry and SSE drain are retired too, as are the unused
CMD_MODULE_ADMIN/CMD_MODULE_NETWORK module bits and streamDebugRecord. This
checks, over the firmware sources (comments ignored), that none of the retired
names is defined, declared or called; that the three never-used page headers
are gone; that neither /api/notice nor an SSE `notice` frame is named; that
tgRequireAuth is defined once, for web requests only; that a revoke queues no
notice; that /api/sessions and /api/admin/sessions share one handler; and that
the stale three-argument streamBeginHtml prototype is gone.

No name is exempt: the HTTP-off no-op stubs System_SensorStubs.h kept for
authSuccessUnified and the notice queue went after the merge. It reads source
text only; it does not compile anything. `--component` points it at another
tree's component directory (its repository is two levels up).
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
REPO = COMPONENT.parents[1]

REMOVED = (
    # Second admission policy and unused auth/session helpers (System_User, WebServer_Server)
    "authSuccessUnified", "authSuccessDebug", "isTransportAdmin",
    "serialTransportInputEpochIsCurrent", "usernameExistsInUsersJson",
    "localDisplayBootAuthBypassActive",
    # Registry and help helpers (System_Utils, System_Command, System_CLI, HardwareOne)
    "commandRequiresAdmin", "commandRequiresSuperAdmin", "dispatchCommand",
    "isHelpModeCommand", "renderHelpSystem", "renderHelpSettings",
    "renderHelpAutomations", "renderHelpEspnow", "renderHelpWifi",
    "renderHelpModuleByName", "exitHelpAndExecute",
    # Mutex helpers (System_Mutex)
    "I2cLockGuard", "i2cLock", "i2cUnlock", "FileTransferGuard", "gFileTransferMutex",
    # Web auth, session and SSE helpers (WebServer_*, WebPage_*, HardwareOne)
    "broadcastNoticeToAllSessions", "sendSSEBurstToSession", "sseSessionAliveAndRefresh",
    "sseSendLogs", "sseDebug", "sseHeartbeat", "sendAuthRequired", "redirectToLogin",
    "getLogoutReasonForAuthPage", "streamAuthRequiredInner", "getHeaderValue",
    "getCookieValue", "streamChunkBuf", "streamNav", "streamContentGeneric",
    "renderTwoFieldForm", "streamLoginSuccessContent", "handleMapSelectAPI",
    "handleMapUnloadAPI", "buildUserSessionsJson", "handleLoginSetSession", "gSessUser",
    "handleAdminSessionsList",
    # Per-session SSE notice queue and /api/notice (WebServer_*, HardwareOne)
    "sseEnqueueNotice", "sseDequeueNotice", "webSessionDequeueSseNotice", "sseSendNotice",
    "handleNotice", "noticeQueue", "nqHead", "nqTail", "nqCount", "nqDropped",
    "NOTICE_QUEUE_SIZE", "NOTICE_MAX_LEN",
    # Unread module flags (System_CommandRegistry) and the uncalled stream recorder (System_Debug)
    "CMD_MODULE_ADMIN", "CMD_MODULE_NETWORK", "streamDebugRecord",
    # The streaming summary counters only streamDebugRecord ever wrote.
    "gStreamTotalBytes", "gStreamMaxChunk", "gStreamHitMaxChunk",
)


def definition(source: str, marker: str) -> tuple[int, str]:
    """(count, body) for the definitions of `marker`: occurrences followed by
    a body, so forward declarations and calls are skipped. The body is the
    last definition's, extracted from where it starts."""
    starts = [m.start() for m in re.finditer(re.escape(marker), source)
              if source[m.end():].lstrip().startswith("{")]
    if not starts:
        return 0, ""
    return len(starts), extract_block(source[starts[-1]:], marker)

GONE_FILES = ("WebPage_Register.h", "WebPage_LoginRequired.h", "WebPage_LoginSuccess.h")

RAW_OPEN = re.compile(r'(?<![A-Za-z0-9_])(?:u8|u|U|L)?R"([^ ()\\\t\n]{0,16})\(')


def strip_comments(text: str) -> str:
    """Drop // and /* */ comments; keep string, character and raw-string
    contents so the line structure and identifiers in code survive."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        raw = RAW_OPEN.match(text, i)
        if raw:
            end = text.find(")" + raw.group(1) + '"', raw.end())
            end = n if end < 0 else end + len(raw.group(1)) + 2
            out.append(text[i:end])
            i = end
            continue
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            seg = text[i:n if j < 0 else j + 2]
            out.append("\n" * seg.count("\n"))
            i = n if j < 0 else j + 2
            continue
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def firmware_sources() -> list[Path]:
    roots = [COMPONENT, REPO / "main", REPO / "updater", REPO / "components" / "hw1_ota_protocol"]
    found: list[Path] = []
    for root in roots:
        if not root.is_dir():
            continue
        for pattern in ("*.cpp", "*.h", "*.c", "*.hpp", "*.ino"):
            for path in root.rglob(pattern):
                if "test" in path.relative_to(root).parts:
                    continue
                found.append(path)
    return sorted(found)


def main() -> None:
    global COMPONENT, REPO
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", help="accepted for a uniform runner interface; unused")
    parser.add_argument("--sanitize", action="store_true", help="accepted for a uniform runner interface; unused")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="production component directory (default: this checkout)")
    args = parser.parse_args()
    COMPONENT = args.component.resolve()
    REPO = COMPONENT.parents[1]

    failures: list[str] = []
    sources = firmware_sources()
    if len(sources) < 50:
        failures.append(f"only {len(sources)} firmware sources found under {COMPONENT}")
    names = re.compile(r"\b(" + "|".join(REMOVED) + r")\b")
    code: dict[str, str] = {}
    for path in sources:
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        code[path.name] = text
        for lineno, line in enumerate(text.splitlines(), 1):
            for name in sorted(set(names.findall(line))):
                failures.append(f"{path.relative_to(REPO)}:{lineno} still names {name}")
        if "/login/setsession" in text:
            failures.append(f"{path.name} still names the /login/setsession route")
        if "[revoke]" in text:
            failures.append(f"{path.name} still builds a [revoke] notice")
        if "/api/notice" in text:
            failures.append(f"{path.name} still names the /api/notice route")
        if "event: notice" in text:
            failures.append(f"{path.name} still writes an SSE notice frame")

    for name in GONE_FILES:
        if (COMPONENT / name).exists():
            failures.append(f"{name} still exists (never included)")

    user = code.get("System_User.cpp", "")
    marker = "bool tgRequireAuth(AuthContext& ctx) {"
    if user.count(marker) != 1:
        failures.append(f"System_User.cpp defines tgRequireAuth {user.count(marker)} times, want 1 (web only)")
    else:
        _, auth = definition(user, "bool tgRequireAuth(AuthContext& ctx)")
        if "if (ctx.transport != SOURCE_WEB) return false;" not in auth:
            failures.append("tgRequireAuth must refuse every context that is not a web request")
        for source in ("SOURCE_SERIAL", "SOURCE_UART", "SOURCE_LOCAL_DISPLAY", "SOURCE_G2_GLASSES"):
            if source in auth:
                failures.append(f"tgRequireAuth still has a {source} admission branch")
        before = user[:user.index(marker)]
        if before.rfind("#if ENABLE_HTTP_SERVER") < before.rfind("#endif"):
            failures.append("tgRequireAuth must be compiled only with ENABLE_HTTP_SERVER")

    server = code.get("WebServer_Server.cpp", "")
    revoke_marker = "void enqueueTargetedRevokeForSessionIdx(int idx, const String& reasonMsg)"
    revoke_count, revoke = definition(server, revoke_marker)
    if revoke_count != 1:
        failures.append("enqueueTargetedRevokeForSessionIdx definition not found once")
    else:
        if "sseEnqueueNotice" in revoke:
            failures.append("a revoke still queues a notice the revoked session can never fetch")
        if "revoked = true" not in revoke:
            failures.append("a revoke must still mark the slot revoked")
    if server.count("esp_err_t handleSessionsList(httpd_req_t* req) {") != 1:
        failures.append("want exactly one sessions handler")
    for uri in ('"/api/sessions"', '"/api/admin/sessions"'):
        if not re.search(r"\{ \.uri = " + re.escape(uri) + r", \.method = HTTP_GET, \.handler = handleSessionsList,",
                         server):
            failures.append(f"{uri} must be served by handleSessionsList")
    header = code.get("WebServer_Server.h", "")
    if re.search(r"void\s+streamBeginHtml\(\s*httpd_req_t\s*\*\s*req\s*,\s*const String&\s*title", header):
        failures.append("WebServer_Server.h still declares the undefined 3-argument streamBeginHtml")

    if failures:
        print("dead auth helper guard FAILED:")
        for failure in failures:
            print("  " + failure)
        raise SystemExit(1)
    print(f"dead auth helper guard: {len(REMOVED)} retired names absent from {len(sources)} sources")


if __name__ == "__main__":
    main()
