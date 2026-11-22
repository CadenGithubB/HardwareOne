#!/usr/bin/env python3
"""Source contract of the UART host link's framed command channel.

Link protocol 2 (System_UartLinkFraming.h, owner decision 2026-11-21): the
pure rules are tested on the production header by uart_link_framing_tests.
This checks the wiring in System_UartLink.cpp that the host suite cannot run
(the port, the lock, the session gates):

- the drain takes the "@<id> " prefix off a line before it is trimmed or read
  by anything else, and forgets the id after the line is answered;
- every reply uartProcessLine writes (login, logout, whoami, the control-plane
  intrinsics, refusals, the idle sign-out) carries that line's id;
- the command in flight keeps its id, and its reply's header carries the
  executor's ok flag (the one PendingCommand::poll reports), not a reading of
  the text, written with the body and newline under one TX-lock hold;
- an over-long line that opened with a valid prefix is answered with its id;
- a framed refusal of an unauthenticated line is always answered (the rate
  limit stays for unframed noise);
- every reply, refusals included (failed login, lockout, sign-in-first,
  auth-required, over-long line), is fenced to the transport incarnation that
  admitted its line: no unfenced text writer is left, so a framed reply can
  never land in a later session;
- the login reply and `uartlink status` carry link=<kUartLinkProtocolVersion>;
- unframed lines are written without an id (body and newline).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block, require  # noqa: E402


def calls(source: str, name: str) -> list[str]:
    """The argument text of every call of `name(` in source (not the
    definitions: a call is not preceded by a type)."""
    out = []
    for m in re.finditer(r"(?<![\w.])" + re.escape(name) + r"\(", source):
        before = source[max(0, m.start() - 40):m.start()]
        if re.search(r"(static\s+(bool|void)\s+)$", before):
            continue  # the definition
        depth, pos = 1, m.end()
        while depth:
            c = source[pos]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            pos += 1
        out.append(source[m.end():pos - 1])
    return out


def strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*", "", text)


def main() -> None:
    uart = (COMPONENT / "System_UartLink.cpp").read_text()
    header = (COMPONENT / "System_UartLinkFraming.h").read_text()
    require('#include "System_UartLinkFraming.h"' in uart, "the link includes the framing rules")
    require("constexpr int kUartLinkProtocolVersion = 2;" in header, "link protocol version is 2")

    process = extract_block(uart, "static void uartProcessLine(String& cmd, uint32_t admittedTransportEpoch)")
    code = strip_comments(process)

    # Every reply written while a line is processed carries the line's id.
    fenced = calls(code, "uartWriteLineForTransportSession")
    require(len(fenced) >= 12, f"uartProcessLine has its fenced replies ({len(fenced)})")
    for args in fenced:
        parts = [a.strip() for a in args.split(",")]
        require(len(parts) >= 4 and parts[2] == "sUartLineReqId",
                f"a fenced reply in uartProcessLine without the line's id: ({args.strip()})")
    # The refusals of the login gate are fenced to the admitted incarnation.
    flat = re.sub(r"\s+", " ", code)
    for refusal in ("msg, false, sUartLineReqId, admittedTransportEpoch",
                    '"Error: authentication failed", false, sUartLineReqId, admittedTransportEpoch',
                    '"Error: sign in first with bare: login <username> <password>", false, '
                    "sUartLineReqId, admittedTransportEpoch"):
        require(refusal in flat, f"a login-gate refusal is not fenced: {refusal}")
    require(not calls(strip_comments(uart), "uartWriteLine")
            and "uartTxLocked(" not in strip_comments(uart),
            "no unfenced text writer is left in the link")
    nag = calls(code, "uartWriteLineNagLimited")
    require(len(nag) == 1 and re.sub(r"\s+", " ", nag[0]).strip().endswith(
                "sUartLineReqId, admittedTransportEpoch"),
            "the auth-required nag carries the line's id and its incarnation")
    require("cmd.length() > 0 || sUartLineReqId[0]" in code,
            "a framed unauthenticated line is answered even when its command is empty")

    # The control-plane intrinsics report their own verdict in the header.
    control = [a for a in fenced if a.strip().startswith("sUartControlReply")]
    require(len(control) == 7, f"seven control-plane replies use their own verdict ({len(control)})")
    for args in control:
        require(args.split(",")[1].strip() == "uartControlReplyOk()",
                "a control-plane reply's header carries uartControlReplyOk()")
    # Login, logout and whoami: success replies are ok.
    for text in ('"OK: logged out", true, sUartLineReqId',
                 "msg, true, sUartLineReqId, loginTransportEpoch",
                 "msg, true, sUartLineReqId, whoamiTransportEpoch"):
        require(text in re.sub(r"\s+", " ", code), f"missing ok reply: {text}")
    # Idle sign-out and an empty framed command are refusals.
    require(re.search(r'"\[uart\] Signed out due to inactivity\. Please log in again\.",\s*false,\s*'
                      r"sUartLineReqId", code), "the idle sign-out is a framed refusal")
    require(re.search(r"kCommandReplyEmptyCommand,\s*false,\s*sUartLineReqId", code),
            "an empty framed command gets the executor's refusal")

    # The link version at login.
    require('"OK: logged in as %s%s link=%d"' in code and "kUartLinkProtocolVersion" in code,
            "the login reply carries link=<version>")
    status = extract_block(uart, "const char* uartLinkStatusLine()")
    require("link=%d" in status and "kUartLinkProtocolVersion" in status,
            "uartlink status reports the link version")

    # The command in flight keeps its id until its reply.
    require("memcpy(sUartPendingReqId, sUartLineReqId, sizeof(sUartPendingReqId));" in code,
            "the pending command keeps the line's id")
    require(code.index("memcpy(sUartPendingReqId") < code.index("sUartPending.start(uc, millis())"),
            "the id is kept before the command is queued")

    deliver = strip_comments(extract_block(uart, "static void uartDeliverReply(bool ok, String& out)"))
    require(re.search(r"uartFrameHeader\(header,\s*sizeof\(header\),\s*sUartPendingReqId,\s*ok,\s*bodyLen\)",
                      deliver), "the reply header carries the executor's ok flag and the body's length")
    require("commandReplyHasOkStamp" not in deliver and "commandReplyIsFailure" not in deliver,
            "the verdict is never read from the reply text")
    require(re.search(r"uartTxSegmentsForTransportSession\(\s*segments,\s*3,", deliver),
            "header, body and newline go out under one TX-lock hold")
    require(deliver.index("uartTxSegmentsForTransportSession") < deliver.index("sUartPendingReqId[0] = '\\0';"),
            "the id is forgotten only after the reply is written")
    pending = strip_comments(extract_block(uart, "static bool uartPendingTick() "))
    require("sUartPending.poll(millis(), out, ok)" in pending and "uartDeliverReply(ok, out)" in pending,
            "the flag uartDeliverReply frames is the one PendingCommand reports")

    segments = strip_comments(extract_block(
        uart, "static bool uartTxSegmentsForTransportSession(const UartTxSegment* segments,\n"
              "                                              size_t count,\n"
              "                                              uint32_t expectedEpoch,\n"
              "                                              uint32_t timeoutMs) "))
    take = segments.index("xSemaphoreTake(sTxMutex")
    write = segments.index("UART_LINK_PORT.write(segments[i].data")
    give = segments.index("xSemaphoreGive(sTxMutex)")
    require(take < write < give and segments.count("xSemaphoreGive(sTxMutex)") == 1,
            "every segment is written inside one TX-mutex hold")

    # The drain: prefix off before the trim and before uartProcessLine; id
    # forgotten after it.
    tick = strip_comments(extract_block(uart, "void uartLinkTick()"))
    parse = tick.index("uartFramedRequestPrefix(\n          cmd.c_str(), cmd.length(), sUartLineReqId)")
    remove = tick.index("if (framedPrefix) cmd.remove(0, framedPrefix);")
    trim = tick.index("cmd.trim();")
    process_call = tick.index("uartProcessLine(cmd, admittedTransportEpoch);")
    clear = tick.index("sUartLineReqId[0] = '\\0';")
    require(parse < remove < trim < process_call < clear, "the drain frames the line in order")
    # An over-long framed line is answered with its id.
    too_long = tick[tick.index("if (sUartCli.length() >= kUartLineCap)"):]
    too_long = too_long[:too_long.index("continue;")]
    require(too_long.index("uartFramedRequestPrefix(sUartCli.c_str()") < too_long.index('sUartCli = "";'),
            "the over-long line's id is read before the line is dropped")
    require(re.search(r'uartWriteLineNagLimited\("Error: line too long — discarded", tooLongId,\s*'
                      r"tooLongEpoch\)", too_long),
            "the over-long refusal carries the line's id and its incarnation")
    require(too_long.index("const uint32_t tooLongEpoch = sUartLineTransportEpoch;")
            < too_long.index("sUartLineTransportEpoch = 0;"),
            "the over-long line's incarnation is read before it is forgotten")

    nag_body = strip_comments(extract_block(
        uart, "static void uartWriteLineNagLimited(const char* s, const char* reqId,\n"
              "                                    uint32_t expectedEpoch) "))
    framed_branch = nag_body[:nag_body.index("sLastNagMs")]
    require("uartWriteLineForTransportSession(s, false, reqId, expectedEpoch)" in framed_branch,
            "a framed refusal is written, fenced, before the rate limit is consulted")
    require("uartWriteLineForTransportSession(s, false, nullptr, expectedEpoch)" in nag_body,
            "an unframed refusal stays unframed, fenced and rate-limited")

    # Every text write goes through the composer: framed with an id, the
    # unframed body + newline without one.
    for sig in ("static bool uartWriteLineForTransportSession(const char* s, bool replyOk,",):
        body = strip_comments(extract_block(uart, sig))
        require("uartFrameComposeLine(" in body, f"{sig.split('(')[0]} composes through the framing rules")
    writes = re.findall(r"UART_LINK_PORT\.write\(", strip_comments(uart))
    require(len(writes) == 2, f"the link port has its two locked writers only ({len(writes)})")

    print("uart_link_framing_source: ok")


if __name__ == "__main__":
    main()
