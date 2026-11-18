"""The replies of `espnowremote` (cmd_espnow_remote, System_ESPNow.cpp) as data.

The web page and the OLED remote form decide from the reply whether a remote
command left this device. Suites that test those readers take the replies from
the production source through this module, so a reworded reply fails a test
instead of a page:

    remote_block(component)        the body of cmd_espnow_remote
    refusals(block)                [(text, json or None)] for every refusal the
                                   handler returns, as Python strings
    json_ack_format(block)         the JSON acknowledgement's printf format
    render_text_acks(component, cases, cxx, workdir)
                                   the text acknowledgement for each
                                   (command, target, mac bytes, request id),
                                   printed by the production formatter
                                   espnowFormatRemoteAck compiled for the host
    ok_stamp(component)            the executor's success stamp ("OK: ")
    stamped(reply, stamp)          the reply as the executor hands it to a
                                   caller: stamped unless it is a failure by
                                   the reply rule or a structured payload
"""
from __future__ import annotations

from pathlib import Path
import re
import subprocess

from test_web_batch_handlers import extract_block, require

HANDLER = "const char* cmd_espnow_remote("
FORMATTER = "static void espnowFormatRemoteAck("
LITERAL = r'"(?:[^"\\\n]|\\.)*"'
LITERALS = r"(?:%s\s*)+" % LITERAL


def c_string(tokens: str) -> str:
    """Adjacent C string literals as the text they spell."""
    out = []
    for body in re.findall(r'"((?:[^"\\\n]|\\.)*)"', tokens):
        out.append(re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t", "r": "\r"}.get(m[1], m[1]), body))
    return "".join(out)


def remote_block(component: Path) -> str:
    return extract_block((Path(component) / "System_ESPNow.cpp").read_text(encoding="utf-8"), HANDLER)


def refusals(block: str) -> list[tuple[str, str | None]]:
    found: list[tuple[str, str | None]] = []
    for match in re.finditer(r"espnowAckErr\(\s*wantJson\s*,\s*(%s),\s*(%s)\)" % (LITERALS, LITERALS), block):
        found.append((c_string(match[1]), c_string(match[2])))
    # Refusals returned without the helper: a literal, or text built into the
    # reply buffer and returned before the command is sent.
    for match in re.finditer(r"return\s+(%s);" % LITERALS, block):
        text = c_string(match[1])
        found.append((None, text) if text.startswith("{") else (text, None))
    sent = block.index("sendAeadSync(")
    for match in re.finditer(r"snprintf\(\s*remoteBuffer\s*,\s*sizeof\(remoteBuffer\)\s*,\s*(%s)" % LITERALS,
                             block[:sent]):
        found.append((c_string(match[1]), None))
    require(len(found) >= 6, f"cmd_espnow_remote: only {len(found)} refusal replies found")
    return found


def json_ack_format(block: str) -> str:
    match = re.search(r"if \(wantJson\) \{\s*snprintf\(\s*remoteBuffer\s*,\s*sizeof\(remoteBuffer\)\s*,\s*(%s)"
                      % LITERALS, block)
    require(match is not None, "cmd_espnow_remote: JSON acknowledgement not found")
    text = c_string(match[1])
    require('"reqId":%lu' in text and '"ok":true' in text,
            f"cmd_espnow_remote: JSON acknowledgement lost ok/reqId: {text}")
    # Since 2026-11-18 a command still waiting for the session adds
    # ,"queued":true through one %s after the id; the format returned here is
    # the acknowledgement of a command that went out (the %s left empty).
    require(text.count("%s") <= 1, f"cmd_espnow_remote: JSON acknowledgement has more than the queued %s: {text}")
    return text.replace("%s", "")


def formatter_source(component: Path) -> str:
    """espnowFormatRemoteAck as it ships. A tree from before the formatter
    built the acknowledgement inline; that statement is wrapped in a function
    of the same signature so the same cases run on it (base runs)."""
    source = (Path(component) / "System_ESPNow.cpp").read_text(encoding="utf-8")
    if FORMATTER in source:
        return extract_block(source, FORMATTER)
    block = extract_block(source, HANDLER)
    inline = re.search(r"snprintf\(remoteBuffer, sizeof\(remoteBuffer\),\s*\"Sent '[^;]*;", block)
    require(inline is not None, "cmd_espnow_remote: no text acknowledgement found")
    return ("static void espnowFormatRemoteAck(char* out, size_t outSize, const char* safeCommandText,\n"
            "    const char* targetText, const uint8_t targetMac[6], uint32_t msgId) {\n"
            "  struct Text { const char* text; const char* c_str() const { return text; } };\n"
            "  const Text safeCommand{safeCommandText}, target{targetText};\n"
            "  char remoteBuffer[256];\n  " + inline[0] + "\n"
            "  std::snprintf(out, outSize, \"%s\", remoteBuffer);\n}\n")


def render_text_acks(component: Path, cases, cxx: str, workdir: Path, size: int = 256,
                     flags=()) -> list[str]:
    """cases: iterable of (command, target, 6 mac bytes, request id), each
    optionally followed by queued (True: the command waits for the session to
    open; the formatter's seventh argument, 2026-11-18)."""
    component = Path(component)
    rows = []
    any_queued = False
    for case in cases:
        command, target, mac, request = case[:4]
        queued = bool(case[4]) if len(case) > 4 else False
        any_queued = any_queued or queued
        literal = lambda text: '"' + "".join("\\x%02x" % b for b in text.encode()) + '"'
        rows.append("  {%s, %s, {%s}, %dUL, %s}," % (literal(command), literal(target),
                                                     ", ".join("0x%02X" % b for b in mac), request,
                                                     "true" if queued else "false"))
    call_tail = ", c.queued" if any_queued else ""
    unit = workdir / "espnow_remote_ack.cpp"
    unit.write_text(
        "#include <cstdint>\n#include <cstdio>\n#include <cstring>\n"
        f'#include "{(component / "System_MacText.h").resolve()}"\n'
        + formatter_source(component) + "\n"
        "struct Case { const char* command; const char* target; uint8_t mac[6]; unsigned long request; bool queued; };\n"
        "static const Case kCases[] = {\n" + "\n".join(rows) + "\n};\n"
        "int main() {\n"
        f"  char out[{size}];\n"
        "  for (const Case& c : kCases) {\n"
        "    std::memset(out, '#', sizeof(out));\n"
        "    espnowFormatRemoteAck(out, sizeof(out), c.command, c.target, c.mac, (uint32_t)c.request"
        + call_tail + ");\n"
        "    std::printf(\"%zu:%s\\n\", std::strlen(out), out);\n"
        "  }\n  return 0;\n}\n")
    executable = workdir / "espnow_remote_ack"
    subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags, str(unit), "-o", str(executable)],
                   check=True, timeout=120)
    lines = subprocess.run([str(executable)], check=True, timeout=60, capture_output=True,
                           text=True).stdout.splitlines()
    acks = []
    for line in lines:
        length, _, text = line.partition(":")
        require(int(length) == len(text.encode()), "acknowledgement holds a line break")
        acks.append(text)
    return acks


def ok_stamp(component: Path) -> str:
    header = (Path(component) / "System_CommandReply.h").read_text(encoding="utf-8")
    match = re.search(r'constexpr char kCommandReplyOkStamp\[\] = ("(?:[^"\\]|\\.)*");', header)
    require(match is not None, "System_CommandReply.h: kCommandReplyOkStamp not found")
    return c_string(match[1])


def stamped(reply: str, stamp: str) -> str:
    """What executeCommand hands a caller for this handler reply (stampOkStatus
    and commandReplyIsFailure, System_Utils.cpp / System_CommandReply.h)."""
    failure = reply.startswith(("Error", "ERROR", "Usage:", "Unknown command:"))
    if failure or not reply or reply[0] in "{[" or reply.startswith(("OK", "SUCCESS")):
        return reply
    return stamp + reply
