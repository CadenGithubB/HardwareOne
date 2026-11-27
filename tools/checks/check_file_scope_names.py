#!/usr/bin/env python3
"""File-scope names carry the prefix that says who can see them.

The rule (CONTRIBUTING.md, "Names"): a variable that is private to one source
file (`static` at file scope) is `sFoo`; a typed constant is `kFoo`. `gFoo` is
for state another file uses, and capitals only are for macros and build flags.

    python3 tools/checks/check_file_scope_names.py            # list what breaks the rule
    python3 tools/checks/check_file_scope_names.py --expect N # exit 1 unless exactly N break it
    python3 tools/checks/check_file_scope_names.py --rewrite  # rename what is safe to rename

What is read: every `static` variable defined at the start of a line in a
.cpp file of the firmware (the same files check_code_names.py reads). The C
sources of the updater and the OTA protocol keep ESP-IDF's `s_foo` style.
Functions, function pointers, statics inside functions and classes, and
statics in headers are not read.

What counts as a constant: `constexpr`, or `const` on the object itself
(`static const int kFoo`, `static const char* const kNames[]`). A pointer to
const that can itself be reassigned (`static const char* sName`) is a
variable, unless it is already named as a constant (`kFoo`, or capitals only):
then it is a constant and `kFoo`.

Kept names: `TAG`, the ESP-IDF log tag idiom, and `SD`, the Arduino library's
name for the card. Part numbers keep their capitals (`sAPDS9960`,
`kSSD1306BufSize`). Generated files (marked DO NOT EDIT) keep their
generator's names.

--rewrite renames a name everywhere in its file, comments included, and
leaves string literals alone. A comment in another file follows when no other
file uses the old name in code. It skips a name, and says why, when the rename
is not provably local: the new name is already used in the file or in any
header, the old name appears in a header's #define, the name is also used as a
member (`x.name`, `x->name`, `X::name`), or (for a name without a prefix) the
file declares something else with the same name. Skipped names stay in the
list this check prints; rename those by hand.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_code_names import ACRONYMS, COMPONENT, NAME, TOKEN, as_words, read, sources, write  # noqa: E402

UNIT_SUFFIXES = (".cpp",)
HEADER_SUFFIXES = (".h", ".hpp", ".inc")
KEPT_NAMES = {"TAG", "SD"}
NOT_A_TYPE = {"return", "case", "else", "sizeof", "delete", "new", "throw", "goto", "typeof", "alignof",
              "co_return", "not", "and", "or", "do", "in", "if", "while", "for", "switch", "typedef", "using"}
STATIC = re.compile(r"^static\b", re.M)
AGGREGATE = re.compile(r"^static\s+(?:const\s+|constexpr\s+)?(?:struct|union|enum|class)\b[^;=]*$")
ATTRIBUTE = re.compile(r"^[A-Z0-9_]+_ATTR$|^IRAM_ATTR$|^PROGMEM$")


def masked(text: str) -> str:
    """The text with comments, strings and include lines blanked; offsets are kept."""
    out, last = [], 0
    for match in TOKEN.finditer(text):
        if match.lastgroup in ("name", "number"):
            continue
        out.append(text[last:match.start()])
        out.append(re.sub(r"[^\n]", " ", match.group(0)))
        last = match.end()
    return "".join(out) + text[last:]


def statement(code: str, start: int):
    """(head, terminator, end of head) of the declaration that starts at `start`."""
    depth_round = depth_square = depth_angle = 0
    position = start
    while position < len(code):
        char = code[position]
        if char == "[":
            depth_square += 1
        elif char == "]":
            depth_square -= 1
        elif char == "<":
            depth_angle += 1
        elif char == ">":
            depth_angle = max(0, depth_angle - 1)
        elif depth_square == 0 and depth_round == 0 and (char in ";={(" or (char == "," and depth_angle == 0)):
            return code[start:position], char, position
        position += 1
    return code[start:], "", len(code)


def has_further_declarator(code: str, position: int) -> bool:
    """A top-level comma between `position` and the end of the statement."""
    depth = 0
    while position < len(code):
        char = code[position]
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == ";" and depth <= 0:
            return False
        elif char == "," and depth == 0:
            return True
        position += 1
    return False


def definitions(code: str):
    """File-scope static variables: (offset of the name, name, is constant)."""
    for match in STATIC.finditer(code):
        head, terminator, end = statement(code, match.start())
        if terminator in ("(", ",", "") or AGGREGATE.match(head.strip()):
            continue
        if has_further_declarator(code, end):
            continue
        cut = head.find("[")
        before_array = head if cut < 0 else head[:cut]
        names = list(NAME.finditer(before_array))
        while names and ATTRIBUTE.match(names[-1].group(0)):
            names.pop()
        if len(names) < 3:                       # static, a type and a name at least
            continue
        name = names[-1]
        words = [m.group(0) for m in names[:-1]]
        declarator = before_array[:name.start()]
        star = declarator.rfind("*")
        constant = "constexpr" in words or (
            "const" in words and (star < 0 or re.search(r"\bconst\b", declarator[star:]) is not None))
        yield match.start() + name.start(), name.group(0), constant, star >= 0 and "const" in words


def pascal(name: str) -> str:
    """The name's words run together, each starting with a capital, without its prefix.

    A name that is already in mixed case keeps its spelling after the prefix
    (`gAPDS9960` -> `APDS9960`); a name in capitals or with underscores is
    rebuilt word by word, and a part number in it stays in capitals.
    """
    name = name.strip("_")
    if re.match(r"^[gsk]_", name):
        name = name[2:]
    elif re.match(r"^[gskp][A-Z]", name) and re.search(r"[a-z]", name) and "_" not in name:
        return name[1:]                          # gFoo, sFoo, kFoo, and the pointer prefix of pFoo
    if "_" not in name and re.search(r"[a-z]", name):
        return name[:1].upper() + name[1:]

    def word(part: str) -> str:
        if re.search(r"[a-z]", part):
            return part[:1].upper() + part[1:]
        if part not in ACRONYMS and len(re.findall(r"[0-9]", part)) >= 3:
            return part                          # a part number: SSD1306, BNO055
        return part[:1] + part[1:].lower()

    return "".join(word(part) for part in name.split("_") if part)


def wanted(name: str, constant: bool, pointer_to_const: bool) -> str:
    if name in KEPT_NAMES:
        return name
    if not constant and pointer_to_const and (re.match(r"^k[A-Z]", name) or not re.search(r"[a-z]", name)):
        constant = True                          # named as a constant: a pointer to const text
    prefix = "k" if constant else "s"
    if re.match(rf"^{prefix}[A-Z]", name):
        return name
    return as_words(prefix + pascal(name))


def units(component: Path):
    """The source files this check reads; generated files keep their generator's names."""
    return [path for path in sources(component)
            if path.suffix in UNIT_SUFFIXES and "DO NOT EDIT" not in read(path)[:2000]]


def header_facts(component: Path):
    """(every name used in a header, every name used in a header's #define)."""
    used, in_defines = set(), set()
    for path in sources(component):
        if path.suffix not in HEADER_SUFFIXES:
            continue
        code = masked(read(path))
        used.update(NAME.findall(code))
        for line in re.findall(r"^[ \t]*#[ \t]*define[^\n]*(?:\\\n[^\n]*)*", code, re.M):
            in_defines.update(NAME.findall(line))
    return used, in_defines


def measure(component: Path):
    """{path: [(line, old, new, offset)]} for every file-scope static that breaks the rule."""
    wrong, total = {}, 0
    for path in units(component):
        text = read(path)
        code = masked(text)
        for offset, name, constant, pointer_to_const in definitions(code):
            total += 1
            new = wanted(name, constant, pointer_to_const)
            if new != name:
                wrong.setdefault(path, []).append((text.count("\n", 0, offset) + 1, name, new, offset))
    return wrong, total


def refusal(name: str, new: str, code: str, own_offsets: set, header_names: set, header_defines: set):
    """Why this rename is not provably local, or None."""
    if re.search(rf"(?<![A-Za-z0-9_]){re.escape(new)}(?![A-Za-z0-9_])", code):
        return f"`{new}` is already used in the file"
    if new in header_names:
        return f"`{new}` is already used in a header"
    if name in header_defines:
        return "a header's #define uses the name"
    prefixed = re.match(r"^[gsk][A-Z_]", name) or not re.search(r"[a-z]", name)
    for use in re.finditer(rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])", code):
        before = code[:use.start()].rstrip()
        if before.endswith((".", "->", "::")):
            return "the name is also used as a member"
        if prefixed or use.start() in own_offsets:
            continue
        after = code[use.end():].lstrip()[:1]
        previous = re.search(r"([A-Za-z_][A-Za-z0-9_]*|[*&>])\s*$", before)
        if previous and after in "=;,)[{:" and previous.group(1) not in NOT_A_TYPE:
            line_start = code.rfind("\n", 0, use.start()) + 1
            if not code.startswith("static", line_start):
                return "the file declares something else with this name"
    return None


def in_prose(renames: dict) -> dict:
    """The renames that apply inside comments: a plain word (`view`, `links`) is also English."""
    return {old: new for old, new in renames.items() if not re.fullmatch(r"[a-z]+", old)}


def rename_in(text: str, renames: dict) -> str:
    out, last = [], 0
    prose = in_prose(renames)
    for match in TOKEN.finditer(text):
        kind = match.lastgroup
        if kind == "name":
            new = renames.get(match.group(0))
        elif kind in ("line_comment", "block_comment"):
            new = NAME.sub(lambda m: prose.get(m.group(0), m.group(0)), match.group(0))
        else:
            continue
        if new and new != match.group(0):
            out.append(text[last:match.start()] + new)
            last = match.end()
    return "".join(out) + text[last:]


def follow_comments(component: Path, lines: list) -> list:
    """Rename in the comments of other files, for names only one file uses in code."""
    repo = component.parents[1]
    renamed = defaultdict(set)
    for line in lines:
        kind, where, old, new = line.split("\t")[:4]
        if kind == "DONE":
            renamed[old].add((where, new))
    texts = {path: read(path) for path in sources(component)}
    code_users = defaultdict(set)
    for path, text in texts.items():
        for match in TOKEN.finditer(text):
            if match.lastgroup == "name" and match.group(0) in renamed:
                code_users[match.group(0)].add(path)
    # After the rename no file uses the old name in code unless another file has its own.
    unique = in_prose({old: next(iter(rows))[1] for old, rows in renamed.items()
                       if len(rows) == 1 and not code_users[old]})
    notes = []
    for path, text in texts.items():
        out, last, hits = [], 0, set()
        for match in TOKEN.finditer(text):
            if match.lastgroup not in ("line_comment", "block_comment"):
                continue
            def swap(m):
                if m.group(0) in unique:
                    hits.add(m.group(0))
                    return unique[m.group(0)]
                return m.group(0)
            new = NAME.sub(swap, match.group(0))
            if new != match.group(0):
                out.append(text[last:match.start()] + new)
                last = match.end()
        if out:
            write(path, "".join(out) + text[last:])
            where = path.relative_to(repo).as_posix()
            notes.extend(f"COMMENT\t{where}\t{old}\t{unique[old]}" for old in sorted(hits))
    return notes


def rewrite(component: Path, record: Path | None) -> int:
    wrong, _ = measure(component)
    header_names, header_defines = header_facts(component)
    repo = component.parents[1]
    done, skipped, lines = 0, Counter(), []
    for path in sorted(wrong):
        text = read(path)
        code = masked(text)
        by_name = defaultdict(set)
        offsets = defaultdict(set)
        for _, name, new, offset in wrong[path]:
            by_name[name].add(new)
            offsets[name].add(offset)
        renames, targets = {}, Counter(next(iter(v)) for v in by_name.values() if len(v) == 1)
        for name, news in sorted(by_name.items()):
            where = path.relative_to(repo).as_posix()
            if len(news) > 1:
                reason = "defined both as a constant and as a variable"
            elif targets[next(iter(news))] > 1:
                reason = "two names in the file would get the same new name"
            else:
                reason = refusal(name, next(iter(news)), code, offsets[name], header_names, header_defines)
            if reason:
                skipped[reason] += 1
                lines.append(f"SKIP\t{where}\t{name}\t{'/'.join(sorted(news))}\t{reason}")
            else:
                renames[name] = next(iter(news))
                lines.append(f"DONE\t{where}\t{name}\t{renames[name]}")
        if renames:
            write(path, rename_in(text, renames))
            done += len(renames)
    lines.extend(follow_comments(component, lines))
    if record:
        record.write_text("\n".join(lines) + "\n")
    print(f"renamed {done} names; skipped {sum(skipped.values())}")
    for reason, count in skipped.most_common():
        print(f"  {count:5d}  {reason}")
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--expect", type=int, help="exit 1 unless exactly this many names break the rule")
    parser.add_argument("--rewrite", action="store_true")
    parser.add_argument("--record", type=Path, help="with --rewrite: write every rename and skip to this file")
    parser.add_argument("--quiet", action="store_true", help="print the count only")
    args = parser.parse_args()
    component = args.component.resolve()
    if args.rewrite:
        sys.exit(rewrite(component, args.record))

    wrong, total = measure(component)
    repo = component.parents[1]
    count = sum(len(rows) for rows in wrong.values())
    if not args.quiet:
        for path in sorted(wrong):
            for line, name, new, _ in wrong[path]:
                print(f"{path.relative_to(repo).as_posix()}:{line}: `{name}` should be `{new}`")
    print(f"file-scope names: {total} static variables in source files, {count} break the rule")
    if args.expect is None:
        sys.exit(1 if count else 0)
    if count != args.expect:
        print(f"expected exactly {args.expect}. A private static is `sFoo`, a typed constant `kFoo` "
              "(CONTRIBUTING.md, \"Names\"). If the count went down, lower the number where this check is run.")
        sys.exit(1)


if __name__ == "__main__":
    main()
