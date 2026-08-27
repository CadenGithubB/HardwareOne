#!/usr/bin/env python3
"""Extract the shipping web UI's JavaScript out of the C++ that serves it.

The device has no bundler and no .js files. Every byte of browser JavaScript
lives inside a C++ raw string literal that gets handed to httpd_resp_send_chunk
at request time. That means the only way to test the web UI honestly is to read
the JS back out of the REAL source, the same rule updater/test/host/CMakeLists.txt:9-10
states for the C code: compile "the REAL source, not a host reimplementation of it".
A hand-copied .js fixture would drift the moment someone edits the header, and
would then pass forever while shipping broken JS.

WHAT IS COVERED
    - Finding R"<delim>( ... )<delim>" literals in a .h/.cpp and returning their
      bodies verbatim, with the source line each one opened on.
    - Stitching a file's blocks back into the byte stream the browser receives,
      then carving the <script> regions out of it.
    - page_js(), the single-page path: give me the JavaScript of one page.
    - A line map, so a parser's "SyntaxError on line 812" can be reported as
      components/hardwareone/WebPage_Settings.h:1153 instead of a temp-file line.

WHAT IS NOT COVERED
    - This is a lexical scanner, not a C++ parser. It does not know about //
      comments or ordinary "quoted strings", so an R"JS( sequence written inside
      one would be picked up. No such case exists in the tree today.
    - The C++ BETWEEN two chunks is dropped. Pages that compute a value at
      runtime and print it between blocks (WebPage_Maps.h calls
      streamMapsPageLodZoomConstants() between its two R"JS( chunks) therefore
      yield JS that PARSES but references identifiers that were never defined.
      This module is for parse-level and behavioral-unit testing, not for
      standing the whole page up.
    - No minification, no dead-code analysis, no HTML validation.

THE EXTRACTION RULE
    Derived from measuring the tree, not from taste. Each step earns its place:

    1 FIND      Scan for R"<delim>( with delim in [A-Za-z0-9_]{0,16}. The
                literal ends at the FIRST )<delim>" -- C++ [lex.string]
                guarantees it, because a body containing its own terminator
                would not compile. So block CONTENT can never fool the scanner;
                the hazards here are structural, not content-based.
    2 JOIN      Group by file, sort by start offset, concatenate the bodies with
                the EMPTY STRING. Empty is the only correct joiner and this was
                measured: any non-empty token breaks 8 regions, because some
                pages split a JS token across chunks to interpolate a runtime
                value. WebPage_LoginSuccess.h:73 is the clearest case -- the
                block ends mid-string-literal, at
                    document.cookie = 'session=)LOGINSUCCESS3"
                so a joiner lands INSIDE a JS string.
    3 REGIONS   Each <script ...> starts JS that runs to the next </script> (or
                EOF if unclosed). Everything outside is HTML and is discarded.
                <!-- --> is left alone; the parser accepts it.
    4 TAGLESS   A joined document with no <script> at all is NOT all JavaScript.
                Emit only maximal runs of adjacent blocks whose delimiter looks
                like JS. i2csensor_sths34pf80_web.h is the file that proves it:
                HTML block, JS block, HTML block, no script tags anywhere.
                Treating the whole document as JS gives
                "SyntaxError: Unexpected token '<'".
    5 NEVER     Never unescape a body. Raw strings are literal by definition;
                the backslashes in a JS regex are already the bytes the browser
                gets.
    6 NEVER     Never filter on delimiter NAME when building regions. R"HTML(
                carries 4765 JS bytes at WebPage_MQTT.cpp:332 and R"FBSCRIPT(
                carries 49KB at WebServer_Utils.h:187. Restricting to R"JS(
                would miss roughly 300KB of shipped JavaScript.

FAIL LOUD
    Every failure path raises ExtractionError rather than returning "". A
    silently-empty extraction is a vacuous green: the test suite reports success
    over zero bytes of JavaScript, which is strictly worse than no test at all.

Usage:
    python3 tools/webui/extract_js.py components/hardwareone/WebPage_LLM.h
    python3 tools/webui/extract_js.py --regions components/hardwareone/WebPage_CLI.h
    python3 tools/webui/extract_js.py --walk
    python3 tools/webui/extract_js.py --map-line 812 components/hardwareone/WebPage_CLI.h
"""

from __future__ import annotations

import argparse
import bisect
import re
import sys
from pathlib import Path
from typing import Iterable, NamedTuple, Sequence

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

COMPONENTS = REPO_ROOT / "components" / "hardwareone"

# Vendored, machine-generated, and 710KB of it. Nothing here is hand-written web
# UI, so it is noise in every report and slow in every walk.
WALK_EXCLUDE_NAMES = {"WebPage_DarkRoom.h"}

# Optional encoding prefixes (L, u, U, u8) are part of the raw-string token, so
# LR"JS( is a raw string too. The lookbehind is what stops `uint8R"` or any
# other identifier ending in R from being read as an opener.
_OPENER = re.compile(r'(?<![A-Za-z0-9_])(?:u8|[LuU])?R"([A-Za-z0-9_]{0,16})\(')

_SCRIPT_OPEN = re.compile(r"<script\b[^>]*>", re.IGNORECASE)
_SCRIPT_CLOSE = re.compile(r"</script\s*>", re.IGNORECASE)

# A directive inside a body means the scanner mis-paired an opener with a close
# that belongs to a different literal -- real block content is HTML/CSS/JS and
# never contains one. The trailing lookahead keeps a CSS id like `#include-me`
# from reading as an #include.
_DIRECTIVE = re.compile(
    r"^[ \t]*#[ \t]*(?:ifdef|ifndef|endif|elif|else|if|define|include)(?=[ \t]|$)",
    re.MULTILINE,
)

# Step 4 asks "does this delimiter look like it carries script?". Deliberately
# loose: it must catch EARLYJS, LEDRENDERJS and FBSCRIPT, not just JS.
_JS_DELIM = re.compile(r"JS|SCRIPT", re.IGNORECASE)


class ExtractionError(Exception):
    """Raised instead of returning an empty or partial extraction."""


class Block(NamedTuple):
    """One R"<delim>( ... )<delim>" literal.

    start_line is the 1-based source line the opener sits on, and start_offset
    is the offset of the first BODY byte. Because a raw string is copied to the
    binary verbatim, body-relative line r always maps to source line
    start_line + r -- that identity is the whole line map.
    """

    delim: str
    body: str
    start_line: int
    start_offset: int


class Region(NamedTuple):
    """A contiguous stretch of JavaScript carved out of one file's joined text."""

    name: str
    text: str
    first_line: int


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _relative(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def extract_blocks(path: str | Path, *, require: bool = True) -> list[Block]:
    """Return every raw-string literal in `path`, in file order.

    Scanning is sequential and resumes AFTER each close, never inside a body.
    Restarting the search mid-body would let a JS regex or string containing
    `R"(` register as a nested opener and desynchronise everything after it.

    require=False is for bulk walks over files that legitimately have no raw
    strings; the default is loud, because a caller naming one file expects it
    to contain something.
    """
    path = Path(path)
    try:
        text = path.read_text(errors="replace")
    except OSError as exc:
        raise ExtractionError(f"{_relative(path)}: cannot read: {exc}") from exc

    blocks: list[Block] = []
    pos = 0
    while True:
        opener = _OPENER.search(text, pos)
        if opener is None:
            break
        delim = opener.group(1)
        body_start = opener.end()
        terminator = ")" + delim + '"'
        close = text.find(terminator, body_start)
        if close < 0:
            raise ExtractionError(
                f'{_relative(path)}:{_line_of(text, opener.start())}: '
                f'unbalanced R"{delim}( -- no matching {terminator} in the file'
            )
        body = text[body_start:close]
        directive = _DIRECTIVE.search(body)
        if directive is not None:
            bad_line = _line_of(text, opener.start()) + body.count(
                "\n", 0, directive.start()
            )
            raise ExtractionError(
                f"{_relative(path)}:{bad_line}: preprocessor directive "
                f"{directive.group(0).strip()!r} inside a raw-string body -- the "
                f'R"{delim}( opened at line {_line_of(text, opener.start())} was '
                f"paired with the wrong close"
            )
        blocks.append(
            Block(
                delim=delim,
                body=body,
                start_line=_line_of(text, opener.start()),
                start_offset=body_start,
            )
        )
        pos = close + len(terminator)

    if require and not blocks:
        raise ExtractionError(
            f'{_relative(path)}: no R"<delim>( blocks found -- nothing to extract'
        )
    return blocks


class _Joined(NamedTuple):
    """Blocks concatenated, plus the index that maps back to source lines."""

    text: str
    starts: list[int]
    blocks: list[Block]

    def source_line(self, offset: int) -> int:
        """Source line for an offset into the joined text.

        An offset that lands in a joiner (page_js uses "\\n") belongs to no
        block; attribute it to the block that follows, which is where reading
        continues.
        """
        if not self.blocks:
            return 1
        index = bisect.bisect_right(self.starts, offset) - 1
        if index < 0:
            index = 0
        block = self.blocks[index]
        inside = offset - self.starts[index]
        if inside > len(block.body):
            if index + 1 < len(self.blocks):
                return self.blocks[index + 1].start_line
            inside = len(block.body)
        return block.start_line + block.body.count("\n", 0, max(inside, 0))


def _join(blocks: Sequence[Block], joiner: str) -> _Joined:
    ordered = sorted(blocks, key=lambda b: b.start_offset)
    parts: list[str] = []
    starts: list[int] = []
    cursor = 0
    for index, block in enumerate(ordered):
        if index:
            parts.append(joiner)
            cursor += len(joiner)
        starts.append(cursor)
        parts.append(block.body)
        cursor += len(block.body)
    return _Joined("".join(parts), starts, list(ordered))


def _script_spans(text: str) -> list[tuple[int, int]]:
    """Offsets of the JS inside each <script> element (step 3)."""
    spans: list[tuple[int, int]] = []
    pos = 0
    while True:
        opener = _SCRIPT_OPEN.search(text, pos)
        if opener is None:
            return spans
        close = _SCRIPT_CLOSE.search(text, opener.end())
        end = close.start() if close else len(text)
        spans.append((opener.end(), end))
        pos = close.end() if close else len(text)


def _tagless_runs(joined: _Joined) -> list[tuple[int, int]]:
    """Step 4: maximal runs of adjacent JS-looking blocks, as joined offsets."""
    runs: list[tuple[int, int]] = []
    start: int | None = None
    end = 0
    for index, block in enumerate(joined.blocks):
        if _JS_DELIM.search(block.delim):
            if start is None:
                start = joined.starts[index]
            end = joined.starts[index] + len(block.body)
        elif start is not None:
            runs.append((start, end))
            start = None
    if start is not None:
        runs.append((start, end))
    return runs


def joined_regions(path: str | Path) -> list[Region]:
    """Steps 2-4: every JavaScript region of one file, in file order.

    Uses ALL blocks regardless of delimiter (step 6) -- the biggest JS payloads
    in the tree ship under R"HTML( and R"FBSCRIPT(.
    """
    path = Path(path)
    joined = _join(extract_blocks(path), "")
    spans = _script_spans(joined.text)
    kind = "script"
    if not spans:
        spans = _tagless_runs(joined)
        kind = "run"
    name = Path(path).name
    return [
        Region(
            name=f"{name}#{kind}{number}",
            text=joined.text[start:end],
            first_line=joined.source_line(start),
        )
        for number, (start, end) in enumerate(spans, start=1)
        if joined.text[start:end].strip()
    ]


def page_js(
    path: str | Path, delim: str = "JS"
) -> tuple[str, list[tuple[int, int]]]:
    """The single-page path: the JavaScript of one page, plus its line map.

    `delim` is a regular expression matched against each block's delimiter, so
    the default "JS" also picks up EARLYJS and LEDRENDERJS. Pass r"^JS$" for an
    exact match.

    Returns (js_text, line_map). line_map is ascending (line_in_js_text,
    source_line) pairs; feed it to map_line().

    NOTE ON SCRIPT TAGS -- this deviates from the brief, and the deviation was
    measured. The brief said to strip only an ANCHORED leading <script...> and
    trailing </script>. That is correct for a page whose JS is one block wrapped
    in one script element, but 6 of the 13 R"JS(-bearing files are not that
    shape: a single R"JS( block on WebPage_ESPNow.h holds 14 separate <script>
    elements with HTML sentinels between them, WebPage_Logging.h holds 13, and
    WebPage_CLI.h's three blocks are each self-wrapped. Anchored-only stripping
    leaves 13, 12 and 2 surviving <script> tags respectively -- unparseable, and
    it would trip this function's own fail-loud check on the very files that
    most need testing. So the same region carving as step 3 is applied here,
    with the anchored case falling out as its degenerate one-region form.
    """
    path = Path(path)
    try:
        pattern = re.compile(delim, re.IGNORECASE)
    except re.error as exc:
        raise ExtractionError(f"bad delimiter pattern {delim!r}: {exc}") from exc

    selected = [b for b in extract_blocks(path) if pattern.search(b.delim)]
    if not selected:
        raise ExtractionError(
            f"{_relative(path)}: no blocks whose delimiter matches {delim!r} -- "
            "extracting nothing would be a vacuous pass"
        )

    joined = _join(selected, "\n")
    spans = _script_spans(joined.text)
    if not spans:
        # Step 4's degenerate case: every selected block already matched the JS
        # delimiter, so the whole joined text is one run.
        spans = [(0, len(joined.text))]

    chunks: list[str] = []
    line_map: list[tuple[int, int]] = []
    out_line = 1
    for start, end in spans:
        if chunks:
            chunks.append("\n")
            out_line += 1
        line_map.append((out_line, joined.source_line(start)))
        for index, block_start in enumerate(joined.starts):
            if start < block_start < end:
                crossed = out_line + joined.text.count("\n", start, block_start)
                line_map.append((crossed, joined.blocks[index].start_line))
        body = joined.text[start:end]
        chunks.append(body)
        out_line += body.count("\n")

    js_text = "".join(chunks)

    surviving = _SCRIPT_OPEN.search(js_text)
    if surviving is not None:
        line = _line_of(js_text, surviving.start())
        raise ExtractionError(
            f"{_relative(path)}: a {surviving.group(0)!r} survived into the "
            f"extracted JS at extracted line {line} "
            f"(source {_relative(path)}:{map_line(line_map, line)}) -- the "
            "region carving did not consume it, and the output is not JavaScript"
        )

    # Later entries win on a tie: when a block boundary lands mid-line the line
    # holds bytes from two sources, and every line AFTER it belongs to the newer
    # block, which is what linear extrapolation needs.
    collapsed: dict[int, int] = {}
    for out, src in line_map:
        collapsed[out] = src
    return js_text, sorted(collapsed.items())


def map_line(line_map: Sequence[tuple[int, int]], joined_line: int) -> int:
    """Translate a line in extracted JS back to a line in the C++ source."""
    if not line_map:
        return joined_line
    keys = [entry[0] for entry in line_map]
    index = bisect.bisect_right(keys, joined_line) - 1
    if index < 0:
        return line_map[0][1]
    out_line, source_line = line_map[index]
    return source_line + (joined_line - out_line)


def walk_sources(root: Path | None = None) -> Iterable[Path]:
    """Every candidate C++ source under components/hardwareone, in a stable order."""
    root = Path(root) if root is not None else COMPONENTS
    for path in sorted(root.rglob("*")):
        if path.suffix not in (".h", ".cpp") or not path.is_file():
            continue
        if path.name in WALK_EXCLUDE_NAMES:
            continue
        parts = path.relative_to(root).parts
        if any(p == ".claude" or p.startswith("build") for p in parts):
            continue
        yield path


def _cmd_walk(root: Path | None) -> int:
    scanned = blocks_total = regions_total = 0
    with_blocks = 0
    failures: list[tuple[Path, str]] = []
    for path in walk_sources(root):
        scanned += 1
        try:
            blocks = extract_blocks(path, require=False)
        except ExtractionError as exc:
            failures.append((path, str(exc)))
            continue
        if not blocks:
            continue
        with_blocks += 1
        blocks_total += len(blocks)
        try:
            regions_total += len(joined_regions(path))
        except ExtractionError as exc:
            failures.append((path, str(exc)))
    print(f"files scanned          {scanned}")
    print(f"files with blocks      {with_blocks}")
    print(f"blocks found           {blocks_total}")
    print(f"regions produced       {regions_total}")
    print(f"files that raised      {len(failures)}")
    for path, message in failures:
        print(f"  RAISED {_relative(path)}: {message}")
    return 1 if failures else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Extract shipping JavaScript from C++ raw string literals.",
        epilog="With no flags, dumps the extracted JS of each path to stdout.",
    )
    parser.add_argument("paths", nargs="*", type=Path, help="C++ source files")
    parser.add_argument(
        "--regions",
        action="store_true",
        help="list every JS region (all delimiters) with size and source line",
    )
    parser.add_argument(
        "--delim",
        default="JS",
        help='regex matched against block delimiters for the dump (default: "JS")',
    )
    parser.add_argument(
        "--map-line",
        type=int,
        metavar="N",
        help="report which source line extracted-JS line N came from",
    )
    parser.add_argument(
        "--walk",
        action="store_true",
        help="scan every .h/.cpp under components/hardwareone and report totals",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=None,
        help="directory for --walk (default: components/hardwareone)",
    )
    args = parser.parse_args(argv)

    if args.walk:
        return _cmd_walk(args.root)
    if not args.paths:
        parser.error("give at least one path, or --walk")

    status = 0
    for path in args.paths:
        try:
            if args.regions:
                regions = joined_regions(path)
                print(f"{_relative(path)}: {len(regions)} region(s)")
                for region in regions:
                    print(
                        f"  {region.name:<44} {len(region.text):>8} bytes  "
                        f"{_relative(path)}:{region.first_line}"
                    )
                continue
            js_text, line_map = page_js(path, args.delim)
            if args.map_line is not None:
                print(
                    f"{_relative(path)}: extracted line {args.map_line} -> "
                    f"{_relative(path)}:{map_line(line_map, args.map_line)}"
                )
                continue
            sys.stdout.write(js_text)
        except ExtractionError as exc:
            print(f"error: {exc}", file=sys.stderr)
            status = 1
    return status


if __name__ == "__main__":
    raise SystemExit(main())


# ===========================================================================
# Concatenated-literal path.
#
# Everything above reads R"<delim>( ... )" raw strings only, which is the
# right scope for the pages that use them -- but roughly 200 KB of shipped
# JavaScript is built from ORDINARY "..." literals (WebPage_Dashboard.h, the
# sensor web headers, System_*_Web.h, the hw runtime in WebServer_Utils.cpp),
# and that JS was parsed by nothing: the C++ compiler treats it as bytes and
# the walker above never sees it.
#
# This path lexes BOTH literal kinds in file order, unescapes the quoted ones
# (the browser receives the COMPILED bytes, so \n in source is a newline on
# the wire), joins with the empty string -- same measured rule as step 2
# above -- and carves <script> elements out of the joined stream. It is
# additive: nothing above changes behaviour, and the raw-string walker keeps
# running unmodified. Files are opted in via CONCAT_SOURCES rather than
# swept, because .cpp files are full of quoted strings that are NOT browser
# bytes (log lines, JSON keys); the <script> carving keeps those out of the
# parsed text, but the roster keeps the corpus reviewable.
# ===========================================================================

_SIMPLE_ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'",
    "0": "\0", "a": "\a", "b": "\b", "f": "\f", "v": "\v", "?": "?",
}


def _unescape(body: str) -> str:
    """Decode a quoted C++ literal body to the bytes the compiler emits."""
    out: list[str] = []
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c == "\\" and i + 1 < n:
            e = body[i + 1]
            if e in _SIMPLE_ESCAPES:
                out.append(_SIMPLE_ESCAPES[e])
                i += 2
                continue
            if e == "x":
                j = i + 2
                digits = ""
                while j < n and body[j] in "0123456789abcdefABCDEF" and len(digits) < 2:
                    digits += body[j]
                    j += 1
                if digits:
                    out.append(chr(int(digits, 16)))
                    i = j
                    continue
            if e == "u" and i + 5 < n:
                digits = body[i + 2:i + 6]
                try:
                    out.append(chr(int(digits, 16)))
                    i += 6
                    continue
                except ValueError:
                    pass
            out.append(e)
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def all_blocks(path: str | Path) -> list[Block]:
    """Every string literal in `path` -- raw AND quoted -- in file order.

    The scanner tracks comments and character literals so a quote inside
    either cannot open a phantom string. Char literals are lexed and DROPPED:
    they are never streamed to the browser, but skipping them keeps the state
    machine honest (an apostrophe in a char constant must not start a
    "string"). A digit separator (1'000) is not a char literal.
    """
    path = Path(path)
    try:
        text = path.read_text(errors="replace")
    except OSError as exc:
        raise ExtractionError(f"{_relative(path)}: cannot read: {exc}") from exc

    blocks: list[Block] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in "RLuU":
            m = _OPENER.match(text, i)
            if m is not None:
                delim = m.group(1)
                terminator = ")" + delim + '"'
                close = text.find(terminator, m.end())
                if close < 0:
                    raise ExtractionError(
                        f'{_relative(path)}:{_line_of(text, i)}: unbalanced R"{delim}('
                    )
                blocks.append(Block(delim, text[m.end():close],
                                    _line_of(text, i), m.end()))
                i = close + len(terminator)
                continue
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"' or text[j] == "\n":
                    break
                j += 1
            if j < n and text[j] == '"':
                blocks.append(Block("", _unescape(text[i + 1:j]),
                                    _line_of(text, i), i + 1))
                i = j + 1
                continue
            i += 1
            continue
        if c == "'":
            if 0 < i and text[i - 1].isdigit() and i + 1 < n and text[i + 1].isdigit():
                i += 1          # digit separator, not a char literal
                continue
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'" or text[j] == "\n":
                    break
                j += 1
            i = (j + 1) if (j < n and text[j] == "'") else i + 1
            continue
        i += 1
    return blocks


# The concatenated-literal corpus. Opt-in by name (see the module comment).
# WebPage_Maps.cpp is NOT here: its only script region belongs to
# handleWaypointsPage, which is unreachable dead code (never route-registered,
# calls a function defined nowhere) -- gate dead code and every future editor
# pays the parse bill for bytes no browser will ever run.
CONCAT_SOURCES = [
    "WebPage_Dashboard.h",
    "WebPage_Sensors.h",
    "WebPage_AviPlayer.h",
    "System_Camera_DVP_Web.h",
    "System_EdgeImpulse_Web.h",
    "System_Microphone_Web.h",
    "WebServer_Utils.cpp",
    "i2csensor_ano_encoder_web.h",
    "i2csensor_bno055_web.h",
    "i2csensor_ds3231_web.h",
    "i2csensor_mlx90640_web.h",
    "i2csensor_pa1010d_web.h",
    "i2csensor_rda5807_web.h",
    "i2csensor_seesaw_web.h",
    "i2csensor_sths34pf80_web.h",
    "i2csensor_vl53l4cx_web.h",
]


def concat_regions(path: str | Path) -> list[Region]:
    """<script> regions carved from the ALL-literals join of one file.

    Same carving rule as step 3 above; no tagless fallback -- in the
    concatenated corpus every byte of JS is inside an explicit <script>
    element, and a fallback here would sweep C++ log strings into the parser.
    """
    path = Path(path)
    joined = _join(all_blocks(path), "")
    spans = _script_spans(joined.text)
    name = Path(path).name
    return [
        Region(
            name=f"{name}#concat{number}",
            text=joined.text[start:end],
            first_line=joined.source_line(start),
        )
        for number, (start, end) in enumerate(spans, start=1)
        if joined.text[start:end].strip()
    ]


# ---------------------------------------------------------------------------
# Fragment files: JS streamed as bare chunks whose <script> wrapper lives in
# the COMPOSING page (WebPage_Sensors.h / WebPage_Files.h stream the tag, then
# call these files' stream functions for the body). Joining ALL quoted
# literals for such a file would sweep in #include filenames and JSON keys --
# text that is not browser bytes and happens to parse as JS expressions, which
# is exactly the vacuous-pass shape this suite exists to kill. So fragment
# extraction takes ONLY the literals that appear inside the argument list of a
# stream call: those are, by construction, the bytes the browser receives.
# ---------------------------------------------------------------------------

_STREAM_CALLS = ("httpd_resp_send_chunk", "streamChunkC")

FRAGMENT_SOURCES = [
    "WebPage_AviPlayer.h",
    "i2csensor_ds3231_web.h",
    "i2csensor_pa1010d_web.h",
    "i2csensor_sths34pf80_web.h",
]

# Regions the concat path can NEVER parse, each for a stated structural
# reason. These are asserted to STILL FAIL by the gate's tripwire test: an
# entry whose region starts parsing (the code changed) must be removed here,
# so this list cannot quietly rot into an exclusion of live coverage.
CONCAT_KNOWN_UNPARSEABLE = {
    "WebServer_Utils.cpp#concat2":
        "the __hwUser/__hwRoleRank snapshot is assembled at runtime from a C++ "
        "String with integer interpolations; the source literals join to "
        "'{guest:,user:,...}' which is not JavaScript",
    "i2csensor_mlx90640_web.h#concat1":
        "streams 'window.__thermalWebMaxFps=%d;' -- a printf template whose "
        "value exists only at request time",
}


def fragment_js(path: str | Path) -> str:
    """The quoted-literal bytes a fragment file hands to the stream calls.

    Scans code (comment- and string-aware), and whenever one of the
    _STREAM_CALLS opens, collects every quoted literal until its balanced
    closing parenthesis. Raw-string arguments are skipped: in the fragment
    corpus those are HTML card markup, and the raw-string walker above already
    covers raw blocks where they carry JS.
    """
    path = Path(path)
    text = path.read_text(errors="replace")
    out: list[str] = []
    i = 0
    n = len(text)
    depth = 0            # paren depth inside an open stream call, else 0
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i); i = n if j < 0 else j; continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2); i = n if j < 0 else j + 2; continue
        if c in "RLuU":
            m = _OPENER.match(text, i)
            if m is not None:
                terminator = ")" + m.group(1) + '"'
                close = text.find(terminator, m.end())
                i = n if close < 0 else close + len(terminator)
                continue
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\": j += 2; continue
                if text[j] == '"' or text[j] == "\n": break
                j += 1
            if depth > 0 and j < n and text[j] == '"':
                out.append(_unescape(text[i + 1:j]))
            i = (j + 1) if j < n else n
            continue
        if c == "'":
            if 0 < i and text[i - 1].isdigit() and i + 1 < n and text[i + 1].isdigit():
                i += 1; continue
            j = i + 1
            while j < n:
                if text[j] == "\\": j += 2; continue
                if text[j] == "'" or text[j] == "\n": break
                j += 1
            i = (j + 1) if j < n else n
            continue
        if depth == 0:
            for fn in _STREAM_CALLS:
                if text.startswith(fn, i) and not text[i - 1].isalnum() and text[i - 1] != "_":
                    k = i + len(fn)
                    while k < n and text[k] in " \t\n": k += 1
                    if k < n and text[k] == "(":
                        depth = 1
                        i = k + 1
                        break
            else:
                i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        i += 1
    return "".join(out)
