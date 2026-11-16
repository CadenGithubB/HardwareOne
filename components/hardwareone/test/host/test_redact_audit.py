#!/usr/bin/env python3
"""The command audit redaction gives the same text with fewer String blocks.

redactCmdForAudit (System_Utils.cpp) masks the secrets of a command line for
the audit log, the [CMD] echo and the feeds. For each of the 29 rows of its
rule table, redactRuleMatches used to parse the whole line with CommandArgs
again, copy the row's prefix into a String and parse that too; the verb test
for `login` worked on a lower-cased copy of the line. Now the line is parsed
once before the rule loop, each row's words are read in place from its
prefix, and the verb is found on the line itself, reading each byte as the
lower-cased copy held it.

The production redactor (both overloads, maskMacTokens, the rule table and
its helpers) and CommandArgs run on the real Arduino String with its heap
calls counted, built with the 32-bit device's inline capacity (13
characters). resolveCommand is the production name matcher over a name list.

Two corpora, each line run through the one-argument form, the two-argument
form with no entry and with an entry, and the one-argument form again on its
own result (not idempotent: `login` gives `login ***`, again `login *** ***`):
- fixed: every rule prefix in sixteen spellings (case changes, quoted words,
  the whole prefix quoted, tabs, vertical tabs, doubled spaces, leading
  blanks, a word cut short or extended, a NUL in a word, the words glued;
  a multi-word prefix also without its last word) with 33 tails (no argument, one to fifteen words, quoted words
  with spaces, an unterminated quote, a quote inside a word, a password-like
  word, MAC addresses, bytes 0x80-0xFF, an embedded NUL, a 100-character
  word, tabs and trailing blanks); the login forms; empty and blank lines;
  every byte 0x80-0xFF alone and around a verb; lines of 13, 14, 17, 18, 255,
  256, 257, 511, 512, 1023 and 1024 bytes; 4000 random lines from the same
  pieces. Names: the rule verbs and a few others, fixed here. The report's
  digest must equal REPORT_SHA256, printed by the tree before this change.
- registry: every command name the source registers (the scan of
  test_command_name_uniqueness) with none, one, ten, eleven and thirteen
  words, upper case, tabs, a quoted word and a password-like word, against
  that name list. In the same binary every rule row's match, for every line
  and its redacted text, must equal the match of the tree before (a copy of
  the old redactRuleMatches in the harness under another name).

--baseline <component dir> builds an older tree with the same harness and
compares both reports line by line. Blocks per call are checked against
EXPECTED_BLOCKS (device inline capacity). A source check keeps every rule
prefix to lowercase words separated by single spaces (no quote, no tab, at
most CommandArgs' ten words): the in-place match splits the prefix on
spaces, which is what CommandArgs gives for such a prefix.

Options: --cxx <compiler>, --sanitize, --component <dir>, --baseline <dir>,
--print-counts.
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
import sys
import tempfile

from arduino_string_host import arduino_string_counted
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
CORE = COMPONENT.parent / "arduino/cores/esp32"

# sha256 of the fixed corpus report printed by b3a8ff4f (before this change).
REPORT_SHA256 = "7ab8ae49246ab3565f184126ac93d17fec8824e9040582f8723c9a8f80680e34"

# String blocks one redactCmdForAudit(line) makes, device inline capacity
# (the tree before: 27, 58, 87, 59, 5, 9, 55, 23).
EXPECTED_BLOCKS = {
    "status": 0,
    "sensorlog start 5000": 2,
    "openfile /logging_captures/sensors/sensors_2026-11-15.csv": 3,
    "nosuchverb with some arguments": 3,
    "login alice example-password": 4,
    "wifiadd example-network example-secret": 8,
    "espnowremote peer alice example-password status": 7,
    "phone pair new": 2,
}

SPACES = [" ", "  ", "\t", " \t ", "\r", "\x0b", "\x0c"]


def device_string(directory: Path) -> None:
    """The Arduino String with the ESP32's inline capacity (15-byte buffer)."""
    header = (CORE / "WString.h").read_text()
    patched = header.replace("SSOSIZE = sizeof(struct _ptr) + 4 - 1",
                             "SSOSIZE = 15  // the 32-bit device's sizeof(struct _ptr) + 4 - 1")
    require(patched != header, "WString.h SSOSIZE line")
    (directory / "WString.h").write_text(patched)


def rule_rows(utils: str) -> list[str]:
    table = extract_block(utils, "static const RedactRule kRules[] =")
    rows = re.findall(r'\{\s*"([^"\\]*)",\s*(?:MASK_TOKEN_AT_POS|MASK_AFTER_TOKEN_POS|CALL_HANDLER)', table)
    require(len(rows) >= 25, f"the rule table ({len(rows)} rows)")
    return rows


def generate(component: Path) -> tuple[str, bool]:
    utils = (component / "System_Utils.cpp").read_text()
    command_header = (component / "System_Command.h").read_text()
    command_source = (component / "System_Command.cpp").read_text()
    arguments = extract_block(command_header, "class CommandArgs") + ";\nconst String CommandArgs::empty_;\n"
    arguments += "\n".join(extract_block(command_source, signature) for signature in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse(", "const String& CommandArgs::arg("))
    redactor_start = utils.index("namespace {", utils.index("// Audit / Redaction utilities"))
    redaction = extract_block(utils[redactor_start:], "namespace") + "\n"
    level = "static String redactAuditLevel(const String& argsInput,\n"
    for signature in ("static bool macHexDigit(char ch)", "static bool macGlued(char ch)",
                      "String maskMacTokens(const String& text)",
                      *((level,) if level in utils else ()),
                      "String redactCmdForAudit(const String& argsInput)",
                      "String redactCmdForAudit(const String& argsInput,\n"):
        redaction += extract_block(utils, signature) + "\n"
    in_place = "static bool redactRuleMatches(const CommandArgs& actual," in utils
    harness = (HERE / "redact_audit_harness.cpp").read_text()
    for marker, code in (("COMMAND_ARGS", arguments), ("REDACTION", redaction)):
        tag = "// INSERT_" + marker + "\n"
        require(harness.count(tag) == 1, "one marker " + marker)
        harness = harness.replace(tag, code + "\n")
    return f"#define REDACT_RULES_IN_PLACE {int(in_place)}\n" + harness, in_place


def build(component: Path, directory: Path, cxx: str, sanitize: list[str]) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    includes, sources = arduino_string_counted(directory, sanitize)
    device_string(directory)
    unit, binary = directory / "test.cpp", directory / "test"
    text, _ = generate(component)
    unit.write_text(text)
    subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-Wno-sign-compare", *sanitize, *includes, "-I", str(component), str(unit), *sources,
                    "-o", str(binary)], check=True, timeout=300)
    return binary


# ---------------------------------------------------------------------------
# Corpora
# ---------------------------------------------------------------------------
TAILS = [
    [], ["a"], ["a", "b"], ["a", "b", "c"], [f"w{i}" for i in range(1, 5)],
    [f"w{i}" for i in range(1, 10)], [f"w{i}" for i in range(1, 11)], [f"w{i}" for i in range(1, 12)],
    [f"w{i}" for i in range(1, 16)], ["S3cr3t!p@ss:w0rd"], ["alice", "S3cr3t!p@ss:w0rd"],
    ["alice", "S3cr3t!p@ss:w0rd", "status"], ['"My Net"', '"pa ss"', "1", "0"], ['"My Net pass123'],
    ['"My"Net"', "pass123"], ['""', "x"], ["aa:bb:cc:dd:ee:ff", "secret"], ["AA-BB-CC-DD-EE-01"],
    ["\xe9t\xe9", "\xff\x80"], ["pa\x00ss", "word"], ["y" * 100], ["a\tb\tc"], ["a", "b   "], ["a", "b\t"],
    ["0123456789abcdef", "spoken text"], ["0123456789ABCDEF", "reason"], ["mesh", '"pass phrase"'],
    ["label", "passphrase", "extra"], ["bob", "pw", "1", "superadmin"], ["bob", "pw", "dev", "adm", "apw", "mypw"],
    ["peer", "u", "p", "login", "x", "y"], ["peer", "u", "p", "wifiadd", "n", "k"], ["status"],
]


def heads(words: list[str]) -> list[str]:
    joined = " ".join(words)
    alternating = "".join(ch.upper() if i % 2 else ch for i, ch in enumerate(joined))
    out = [joined, joined.upper(), joined.title(), alternating, " ".join(f'"{w}"' for w in words),
           "\t".join(words), "  ".join(words), "  " + joined, "\t" + joined, "".join(words),
           " ".join(words[:-1] + [words[-1][:-1]]), " ".join(words[:-1] + [words[-1] + "x"]),
           # a NUL in a word: equalsIgnoreCase stops at it, so the first one matches
           " ".join(words[:-1] + [words[-1][:-1] + "\x00"]),
           " ".join([words[0][:2] + "\x00" + words[0][3:]] + words[1:]),
           '"' + joined + '"', joined.replace(" ", "\x0b")]
    if len(words) > 1:
        out.append(" ".join(words[:-1]))
    return out


def fixed_corpus(rows: list[str]) -> list[bytes]:
    lines: list[str] = ["", " ", "\t", "  \t ", "\r\n", "\x00", "\x00login a b", "a", "x" * 13, "x" * 14,
                        "x" * 17, "x" * 18, "\"", '"login" bob pw', "status", "STATUS", "status ",
                        " status", "help"]
    for row in rows:
        words = row.split()
        for head in heads(words):
            for tail in TAILS:
                lines.append(" ".join([head] + tail))
            lines.append(head + "\t" + "\t".join(["a", "b", "c", "d"]))
            lines.append(head + " " * 3)
    login = ["login", "login ", "LOGIN bob pw", "Login bob pw target", "login bob", "login bob pw",
             "login bob pw target", "login bob pw target extra", 'login "bob" "pw" target',
             'login bob "pa ss" target', 'login bob "pa"ss word', 'login bob "unterminated',
             "\tlogin\tbob\tpw", "  login  bob  pw  ", "login\rbob\rpw", "login\x0bbob pw", "logins bob pw",
             "xlogin bob pw", "logi bob pw", "login\x00x bob pw", "logi\x00n bob pw", "login\x00 bob pw",
             "LOGIN\x00X bob", "l\x00ogin bob", "login aa:bb:cc:dd:ee:ff pw", "login bob pw " + "z" * 300]
    lines += login
    for byte in range(0x80, 0x100):
        ch = chr(byte)
        lines += [ch, ch + " a", "wifiadd" + ch + " n k", ch + "login bob pw", "login " + ch + " pw",
                  "status " + ch, "login" + ch + " bob pw", "phone secret " + ch]
    for length in (13, 14, 17, 18, 255, 256, 257, 511, 512, 1023, 1024):
        for stem in ("status ", "wifiadd net ", "nosuchverb ", "login bob ", "llmhttptoken "):
            lines.append(stem + "f" * max(0, length - len(stem)))
            lines.append(stem + "g " * (max(0, length - len(stem)) // 2))
    pieces = (sorted({w for row in rows for w in row.split()}) +
              ["login", "status", "help", "LOGIN", "Wifiadd", '"', '"q q"', "aa:bb:cc:dd:ee:ff", "\x00", "\xe9",
               "\xff", "pw", "x", "secret", "1", "0"] + SPACES)
    rng = random.Random(20261115)
    for _ in range(4000):
        lines.append("".join(rng.choice(pieces) + rng.choice(["", " ", " ", "\t"])
                             for _ in range(rng.randint(1, 9))))
    return [line.encode("latin-1") for line in lines]


def fixed_names(rows: list[str]) -> list[str]:
    names = {row.strip() for row in rows} | {row.split()[0] for row in rows}
    names |= {"status", "help", "phone pair", "openring", "espnowunpair", "user list", "sensorlog",
              "openfile", "dictate"}
    return sorted(names)


def registry_names() -> list[str]:
    from test_command_name_uniqueness import scan
    rows, _ = scan()
    return sorted({found[4] for entries in rows.values() for found in entries})


def registry_corpus(names: list[str]) -> list[bytes]:
    lines = []
    for name in names:
        lines += [name, name.upper(), name + " a", name + " " + " ".join(f"w{i}" for i in range(10)),
                  name + " " + " ".join(f"w{i}" for i in range(11)),
                  name + " " + " ".join(f"w{i}" for i in range(13)), "\t" + name.replace(" ", "\t") + "\targ",
                  name + ' "quoted arg" x', name + " S3cr3t!p@ss:w0rd", name + "  " + "q" * 40]
    return [line.encode("latin-1") for line in lines]


def write_lines(path: Path, lines) -> None:
    path.write_text("".join((line.hex() or "-") + "\n" for line in lines))


def run_report(binary: Path, names: list[str], corpus: list[bytes], directory: Path, label: str) -> bytes:
    names_file, corpus_file, report_file = (directory / f"{label}.{suffix}" for suffix in ("names", "corpus",
                                                                                            "report"))
    names_file.write_text("".join(name + "\n" for name in names))
    write_lines(corpus_file, corpus)
    subprocess.run([str(binary), "report", str(names_file), str(corpus_file), str(report_file)],
                   check=True, timeout=600)
    return report_file.read_bytes()


def compare(new: bytes, old: bytes, label: str) -> None:
    new_lines, old_lines = new.splitlines(), old.splitlines()
    require(len(new_lines) == len(old_lines), f"{label}: {len(new_lines)} lines, before {len(old_lines)}")
    differences = [(a, b) for a, b in zip(new_lines, old_lines) if a != b]
    for a, b in differences[:5]:
        fields_new, fields_old = a.split(), b.split()
        print(f"{label}: input {bytes.fromhex(fields_new[0].decode()) if fields_new[0] != b'-' else b''!r}")
        print(f"  now:    {[bytes.fromhex(f.decode()) if f != b'-' else b'' for f in fields_new[1:]]}")
        print(f"  before: {[bytes.fromhex(f.decode()) if f != b'-' else b'' for f in fields_old[1:]]}")
    require(not differences, f"{label}: {len(differences)} of {len(new_lines)} lines differ from the baseline")
    print(f"{label}: {len(new_lines)} lines equal to the baseline")


def counts(binary: Path, names: list[str], directory: Path) -> dict[str, int]:
    names_file = directory / "counts.names"
    names_file.write_text("".join(name + "\n" for name in names))
    out = subprocess.run([str(binary), "counts", str(names_file)], check=True, capture_output=True,
                         text=True, timeout=60).stdout
    return {line.split(" ", 1)[1]: int(line.split(" ", 1)[0]) for line in out.splitlines()}


def source_check(component: Path) -> None:
    for row in rule_rows((component / "System_Utils.cpp").read_text()):
        require(re.fullmatch(r"[a-z0-9-]+( [a-z0-9-]+)* ?", row) is not None,
                f"rule prefix {row!r}: lowercase words separated by single spaces")
        require(len(row.split()) <= 10, f"rule prefix {row!r}: at most CommandArgs' ten words")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--print-counts", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    rows = rule_rows((args.component / "System_Utils.cpp").read_text())
    fixed, names = fixed_corpus(rows), fixed_names(rows)
    registry = registry_names()
    reg_corpus = registry_corpus(registry)
    with tempfile.TemporaryDirectory(prefix="hw1-redact-audit-") as tmp:
        d = Path(tmp)
        binary = build(args.component, d / "new", args.cxx, sanitize)
        fixed_report = run_report(binary, names, fixed, d, "fixed")
        registry_report = run_report(binary, registry, reg_corpus, d, "registry")
        blocks = counts(binary, names, d)
        if args.print_counts:
            for line, made in blocks.items():
                print(f"{made:4d} {line}")
        digest = hashlib.sha256(fixed_report).hexdigest()
        print(f"fixed corpus: {len(fixed)} lines, report sha256 {digest}")
        if args.baseline:
            old = build(args.baseline, d / "old", args.cxx, sanitize)
            compare(fixed_report, run_report(old, names, fixed, d, "old-fixed"), "fixed corpus")
            compare(registry_report, run_report(old, registry, reg_corpus, d, "old-registry"),
                    "registry corpus")
            if args.print_counts:
                for line, made in counts(old, names, d).items():
                    print(f"before {made:4d} {line}")
        require(digest == REPORT_SHA256, "the fixed corpus report differs from the tree before the change")
        for line, made in EXPECTED_BLOCKS.items():
            require(blocks.get(line) == made, f"{line!r}: {blocks.get(line)} String blocks, expected {made}")
    source_check(args.component)
    print(f"redact audit: {len(fixed)} + {len(reg_corpus)} lines equal, blocks per call as expected")


if __name__ == "__main__":
    main()
