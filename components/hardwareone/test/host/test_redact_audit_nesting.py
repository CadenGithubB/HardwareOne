#!/usr/bin/env python3
"""The audit redactor walks nested forwarding commands in a loop, not by recursion.

The forwarding verbs espnowremote / espnowroomcmd / espnowtagcmd keep
"<verb> <target> <user> ***" and, when the line tokenizes unambiguously,
the redaction of the command they forward. That used to be a recursive call
from the forwarding handler (redactPeerCredCmd) back into redactCmdForAudit:
a crafted line such as "espnowremote a b c " repeated to the console's 2047
bytes nests about a hundred levels and needed tens of KB of stack on
whichever task audited it (web, serial, BLE, ESP-NOW, MQTT, OLED, G2,
automations), with every level's Strings alive at once. Now the handler
hands the forwarded command back and redactCmdForAudit iterates: each
forwarded command is resolved on its own and redacted as a fresh line, its
text after its forwarder's head and one space. The text must be the same,
byte for byte, for every input at every depth.

The production redactor (both overloads, maskMacTokens, the rule table and
its helpers from System_Utils.cpp) and CommandArgs run on the real Arduino
String with its heap calls counted and the live String bytes tracked;
resolveCommand is the production name matcher over a fixed name list.

- Corpus (seed CORPUS_SEED): every nesting depth 0 to MAX_DEPTH with the
  three forwarding verbs mixed (case changes, quoted, tabs and the other
  blanks as separators), under 33 bottom commands (login forms, wifiadd,
  usersync, automation add, dictate result / fail, phone pair new, the
  non-forwarding espnowbrowse / espnowfetch, unknown verbs, MAC addresses,
  short forwarding lines, an empty tail); lines cut or built to exactly 2047
  bytes; RANDOM_LINES random lines mixing quoted, unterminated and butted
  quotes, MAC addresses, NULs, bytes 0xE9 / 0xFF and leading / trailing
  blanks. Each line goes through the one-argument form, the two-argument
  form with no entry and with an entry (the caller's resolution is for the
  outermost level only), and the one-argument form again on its result.
  The report's digest must equal REPORT_SHA256, printed by the recursive
  redactor (18534fa6).
- --baseline <component dir>: that tree's redactor is compiled into the same
  binary and every output is compared with it directly.
- Stack: on a painted thread stack, one call on a 2047-byte line nested
  about a hundred levels uses no more than STACK_SLACK bytes beyond a
  line nested once (the recursive code: about 1 KB more per level), and the
  peak of live String bytes stays under HEAP_FACTOR times the line length.
  The deep lines also redact on a SMALL_STACK-byte thread stack (the
  recursive code overflows it: about 100 KB, 220 KB under --sanitize).
- Source: nothing in the rule table's handlers or in one level of the
  redaction calls redactCmdForAudit; only the one-argument overload does.

Options: --cxx <compiler>, --sanitize, --component <dir>, --baseline <dir>,
--print-stack.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import tempfile

from arduino_string_host import arduino_string_counted
from test_redact_audit import device_string
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

CORPUS_SEED = 20261115
RANDOM_LINES = 6000
MAX_DEPTH = 130
LINE_LIMIT = 2047
# sha256 of the report the recursive redactor (18534fa6) prints for this corpus.
REPORT_SHA256 = "88c2a620d9d556d3ba0c9319fd4a2c35d355905fb7b15800804a57b9ba3ddf61"
STACK_SLACK = 2048
HEAP_FACTOR = 8
SMALL_STACK = 64 * 1024

FORWARDERS = ["espnowremote", "espnowroomcmd", "espnowtagcmd"]
NAMES = sorted({*FORWARDERS, "espnowbrowse", "espnowfetch", "login", "wifiadd", "usersync",
                "automation add", "automationadd", "dictate", "dictate result", "dictate fail",
                "phone pair", "phone pair new", "phone secret", "status", "help", "mqttpassword",
                "otapin", "llmhttptoken", "useradd", "openfile", "sensorlog"})
SEPARATORS = [" ", "  ", "\t", " \t ", "\r", "\x0b", "\x0c", "\n"]
BOTTOMS = [
    "", "status", "status aa:bb:cc:dd:ee:ff", "help me", "login bob pw", "login bob pw target",
    "LOGIN\tbob\tpw", 'login bob "pa ss" target', 'login bob "unterminated', "wifiadd net secret",
    'wifiadd "My Net" "pa ss" 1 0', 'wifiadd "My"Net" pass123', "usersync bob pw dev adm apw mypw",
    "automation add name trigger cmd secret", "automationadd x y z", "dictate result 0123456789abcdef text",
    "dictate fail 0123456789ABCDEz reason", "phone pair new", "phone secret pass phrase",
    "espnowbrowse peer u pw /path", "espnowfetch aa:bb:cc:dd:ee:ff u pw /file", "espnowbrowse peer u",
    "nosuchverb secret words", "logni bob pw", "mqttpassword hunter2", "otapin 1234 5678",
    "espnowremote", "espnowremote peer", "espnowroomcmd room u", "espnowtagcmd tag u pw",
    "\x00login a b", "llmhttptoken tok", "useradd bob pw 0 superadmin",
]
WORDS = ["a", "b", "c", "peer", "alice", "bob", "S3cr3t!pw", "kitchen", "room-2", "tag7", "0", "x" * 20]
ODD_TOKENS = ["aa:bb:cc:dd:ee:ff", "AA-BB-CC-DD-EE-01", "aa:bb:cc:dd:ee:ff:00", '"my peer"', '"pa ss"',
              '""', '"x"', "pa\x00ss", "\xe9t\xe9", "\xff", "<mac>"]
AMBIGUOUS = ['"unterminated', '"pw"x', 'p"w', 'x"', '"a"b"']


def verb_spelling(rng: random.Random, verb: str) -> str:
    return rng.choice([verb, verb, verb, verb.upper(), verb.title(), '"' + verb + '"'])


def level(rng: random.Random, verb: str, odd: float, ambiguous: float, separators: list[str]) -> str:
    words = []
    for _ in range(3):
        roll = rng.random()
        if roll < ambiguous:
            words.append(rng.choice(AMBIGUOUS))
        elif roll < ambiguous + odd:
            words.append(rng.choice(ODD_TOKENS))
        else:
            words.append(rng.choice(WORDS))
    parts = [verb_spelling(rng, verb)] + words
    return "".join(part + rng.choice(separators) for part in parts)


def nested(rng: random.Random, depth: int, bottom: str, odd: float, ambiguous: float,
           separators: list[str], first: int = 0) -> str:
    return "".join(level(rng, FORWARDERS[(first + d) % 3], odd, ambiguous, separators)
                   for d in range(depth)) + bottom


def corpus() -> list[bytes]:
    rng = random.Random(CORPUS_SEED)
    lines: list[str] = []
    # Every depth with the verbs mixed, plain and with other blanks. Past about
    # 107 levels a line is longer than the console's 2047 bytes (a level is at
    # least 19); the redactor itself has no limit, so these are kept whole.
    for depth in range(MAX_DEPTH + 1):
        for k in range(4):
            bottom = BOTTOMS[(depth * 4 + k) % len(BOTTOMS)]
            separators = [" "] if k % 2 == 0 else SEPARATORS
            lines.append(nested(rng, depth, bottom, 0.0 if k < 2 else 0.1, 0.0, separators, first=k))
        lines.append(("espnowremote a b c " * depth) + "login bob pw")
    # Each bottom under a few depths, with one ambiguous token at one level.
    for bottom in BOTTOMS:
        for depth in (1, 2, 3, 7, 25):
            lines.append(nested(rng, depth, bottom, 0.1, 0.0, SEPARATORS))
            text = nested(rng, depth, bottom, 0.0, 0.0, [" "])
            cut = text.split(" ")
            where = rng.randrange(len(cut))
            cut[where] = rng.choice(AMBIGUOUS)
            lines.append(" ".join(cut))
    # Exactly the console's limit: the crafted line, mixed verbs, and cut at
    # every offset of one level.
    crafted = ("espnowremote a b c " * 200)[:LINE_LIMIT]
    lines.append(crafted)
    for cut in range(19):
        lines.append(("espnowremote a b c " * 200)[cut:cut + LINE_LIMIT])
    for k in range(60):
        text = nested(rng, 200, rng.choice(BOTTOMS), 0.05 * (k % 3), 0.0, [" "] if k % 2 else SEPARATORS,
                      first=k)
        lines.append(text[:LINE_LIMIT])
        lines.append(text[-LINE_LIMIT:])
    # Random lines: depth, blanks, odd and ambiguous tokens, leading and
    # trailing blanks, a cut anywhere.
    for _ in range(RANDOM_LINES):
        depth = rng.choice([rng.randint(0, 3), rng.randint(0, 20), rng.randint(0, MAX_DEPTH)])
        ambiguous = rng.choice([0.0, 0.0, 0.01, 0.05])
        separators = rng.choice([[" "], SEPARATORS, [" ", "\t"]])
        text = (rng.choice(["", "", " ", "\t", "  "]) +
                nested(rng, depth, rng.choice(BOTTOMS), rng.choice([0.0, 0.1, 0.3]), ambiguous, separators,
                       first=rng.randrange(3)) +
                rng.choice(["", "", " ", "\t", " \r\n"]))
        if rng.random() < 0.2:
            text = text[:rng.randint(0, len(text))]
        lines.append(text[:LINE_LIMIT])
    return [line.encode("latin-1") for line in lines]


def stack_corpus() -> list[bytes]:
    unnested = "login bob pw"
    deep = [("espnowremote a b c " * 200)[:LINE_LIMIT],
            nested(random.Random(1), 200, "", 0.0, 0.0, [" "])[:LINE_LIMIT]]
    shallow = [("espnowremote a b c " * depth) + unnested for depth in (1, 10, 50)]
    return [line.encode("latin-1") for line in [unnested, *shallow, *deep]]


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
def redactor_code(component: Path) -> str:
    utils = (component / "System_Utils.cpp").read_text()
    start = utils.index("namespace {", utils.index("// Audit / Redaction utilities"))
    code = extract_block(utils[start:], "namespace") + "\n"
    level = "static String redactAuditLevel(const String& argsInput,\n"
    for signature in ("static bool macHexDigit(char ch)", "static bool macGlued(char ch)",
                      "String maskMacTokens(const String& text)", *((level,) if level in utils else ()),
                      "String redactCmdForAudit(const String& argsInput)",
                      "String redactCmdForAudit(const String& argsInput,\n"):
        code += extract_block(utils, signature) + "\n"
    return code


HEAP_TRACKER = r'''#include "string_heap_count.h"
#include <cstdlib>
#include <cstring>
StringHeapCount gStringHeap;
size_t gStringLive = 0, gStringPeak = 0;
static const size_t kHead = 16;
static void grow(size_t n) { gStringLive += n; if (gStringLive > gStringPeak) gStringPeak = gStringLive; }
static size_t sizeOf(void* user) { size_t n; std::memcpy(&n, static_cast<char*>(user) - kHead, sizeof n); return n; }
void* hostStringMalloc(size_t n) {
  ++gStringHeap.made;
  char* base = static_cast<char*>(std::malloc(n + kHead));
  if (!base) return nullptr;
  std::memcpy(base, &n, sizeof n);
  grow(n);
  return base + kHead;
}
void* hostStringRealloc(void* user, size_t n) {
  if (!user) return hostStringMalloc(n);
  ++gStringHeap.regrown;
  const size_t old = sizeOf(user);
  char* base = static_cast<char*>(std::realloc(static_cast<char*>(user) - kHead, n + kHead));
  if (!base) return nullptr;
  std::memcpy(base, &n, sizeof n);
  gStringLive -= old;
  grow(n);
  return base + kHead;
}
void hostStringFree(void* user) {
  if (!user) return;
  ++gStringHeap.freed;
  gStringLive -= sizeOf(user);
  std::free(static_cast<char*>(user) - kHead);
}
'''


def build(component: Path, baseline: Path | None, directory: Path, cxx: str, sanitize: list[str]) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    includes, sources = arduino_string_counted(directory, sanitize)
    device_string(directory)
    (directory / "string_heap_count.cpp").write_text(HEAP_TRACKER)
    command_header = (COMPONENT / "System_Command.h").read_text()
    command_source = (COMPONENT / "System_Command.cpp").read_text()
    arguments = extract_block(command_header, "class CommandArgs") + ";\nconst String CommandArgs::empty_;\n"
    arguments += "\n".join(extract_block(command_source, signature) for signature in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse(", "const String& CommandArgs::arg("))
    harness = (HERE / "redact_audit_nesting_harness.cpp").read_text()
    for marker, code in (("COMMAND_ARGS", arguments), ("NEW", redactor_code(component)),
                         ("OLD", redactor_code(baseline) if baseline else "")):
        tag = "// INSERT_" + marker + "\n"
        require(harness.count(tag) == 1, "one marker " + marker)
        harness = harness.replace(tag, code + "\n")
    unit, binary = directory / "test.cpp", directory / "test"
    unit.write_text(f"#define REDACT_HAVE_OLD {int(baseline is not None)}\n" + harness)
    subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-Wno-sign-compare", *sanitize, *includes, "-I", str(component), str(unit), *sources,
                    "-o", str(binary)], check=True, timeout=300)
    return binary


def write_lines(path: Path, lines) -> None:
    path.write_text("".join((line.hex() or "-") + "\n" for line in lines))


# ---------------------------------------------------------------------------
# Source check
# ---------------------------------------------------------------------------
def code_only(text: str) -> str:
    """The text with comments, string and character literals blanked."""
    return re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', " ", text)


def source_check(component: Path) -> None:
    utils = (component / "System_Utils.cpp").read_text()
    start = utils.index("namespace {", utils.index("// Audit / Redaction utilities"))
    handlers = code_only(extract_block(utils[start:], "namespace"))
    require("redactCmdForAudit(" not in handlers,
            "a rule handler (redactPeerCredCmd) calls redactCmdForAudit: the nesting recurses")
    level_signature = "static String redactAuditLevel(const String& argsInput,\n"
    require(level_signature in utils, "one level of the redaction (redactAuditLevel) is its own function")
    level = code_only(extract_block(utils, level_signature))
    require("redactCmdForAudit(" not in level, "redactAuditLevel calls redactCmdForAudit")
    resolved = code_only(extract_block(utils, "String redactCmdForAudit(const String& argsInput,\n"))
    body = resolved[resolved.index("{"):]
    require("redactCmdForAudit(" not in body, "the two-argument redactCmdForAudit calls itself")
    require(body.count("redactAuditLevel(") == 2 and "while" in body,
            "the two-argument redactCmdForAudit runs the levels in a loop")


# ---------------------------------------------------------------------------
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--print-stack", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    lines = corpus()
    require(sum(len(line) == LINE_LIMIT for line in lines) >= 100, "the corpus reaches the console's line limit")
    with tempfile.TemporaryDirectory(prefix="hw1-redact-nesting-") as tmp:
        d = Path(tmp)
        binary = build(args.component, args.baseline, d / "build", args.cxx, sanitize)
        names_file, corpus_file, report_file = d / "names", d / "corpus", d / "report"
        names_file.write_text("".join(name + "\n" for name in NAMES))
        write_lines(corpus_file, lines)
        subprocess.run([str(binary), "report", str(names_file), str(corpus_file), str(report_file)],
                       check=True, timeout=900)
        report = report_file.read_bytes()
        digest = hashlib.sha256(report).hexdigest()
        # The corpus must reach deep nesting in the redacted text itself: each
        # kept forwarding level is "<verb> <target> <user> ***".
        kept = [bytes.fromhex(row.split()[1].decode()) for row in report.splitlines()
                if row.split()[1] != b"-"]
        deepest_kept = max(text.count(b" ***") for text in kept)
        verbs_deep = {verb for text in kept if text.count(b" ***") >= 100 for verb in FORWARDERS
                      if verb.encode() in text.lower()}
        print(f"corpus: {len(lines)} lines (seed {CORPUS_SEED}), report sha256 {digest}, "
              f"up to {deepest_kept} levels kept")
        require(deepest_kept >= 120 and verbs_deep == set(FORWARDERS),
                f"the corpus nests {deepest_kept} kept levels deep, verbs {sorted(verbs_deep)}")
        require(digest == REPORT_SHA256, "the report differs from the recursive redactor's")

        stack_lines = stack_corpus()
        stack_file = d / "stack.corpus"
        write_lines(stack_file, stack_lines)
        out = subprocess.run([str(binary), "stack", str(names_file), str(stack_file)], check=True,
                             capture_output=True, text=True, timeout=300).stdout.split("\n")
        rows = [list(map(int, row.split())) for row in out if row.strip()]
        require(len(rows) == len(stack_lines), "one stack row per line")
        if args.print_stack:
            for row, line in zip(rows, stack_lines):
                print(f"stack {row[0]:7d} heap {row[1]:6d}  before: stack {row[2]:7d} heap {row[3]:6d}"
                      f"  ({row[4]} bytes, {line[:40]!r})")
        once_stack = rows[1][0]
        for row, line in zip(rows[1:], stack_lines[1:]):
            require(row[0] <= once_stack + STACK_SLACK,
                    f"{len(line)}-byte line {line[:40]!r}: {row[0]} bytes of stack, a line nested once "
                    f"{once_stack} (the redaction recurses per nesting level)")
        for row, line in zip(rows, stack_lines):
            require(row[1] <= HEAP_FACTOR * len(line) + 512,
                    f"{len(line)}-byte line {line[:40]!r}: {row[1]} live String bytes at the peak")
            if args.baseline:
                require(row[1] <= row[3], f"{line[:40]!r}: peak String bytes {row[1]}, before {row[3]}")
        deepest = max(rows[-2:], key=lambda row: row[0])
        print(f"stack: {deepest[0]} bytes for a 2047-byte nested line, {once_stack} nested once; "
              f"peak String bytes {deepest[1]}")
        subprocess.run([str(binary), "small", str(names_file), str(stack_file), str(SMALL_STACK)],
                       check=True, timeout=300)
    source_check(args.component)
    print(f"redact audit nesting: {len(lines)} lines equal to the recursive redactor, stack flat in depth")


if __name__ == "__main__":
    main()
