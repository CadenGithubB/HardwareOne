#!/usr/bin/env python3
"""Assemble the game's editable sources without changing its script semantics."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit

REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOT = REPO_ROOT / "assets" / "games"
HEADER = REPO_ROOT / "components" / "hardwareone" / "WebPage_Games.h"
PARTS = ("CSS", "HTML", "BOOTSTRAP", "MAIN")
TOKEN = re.compile(r"@@([A-Za-z_][A-Za-z_0-9]*)@@")
SUITES = ("smoke", "browser", "cave_milestone", "caves_geometry", "caves_transition", "physics",
          "render", "enemies", "persistence", "loot", "perf", "tools")

if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))


class SourceError(Exception):
    """Invalid sources or an outdated generated header."""


@dataclass(frozen=True)
class SourceSpan:
    source: str
    text: str
    offset: int
    source_line: int = 1


@dataclass(frozen=True)
class Assembly:
    header: bytes
    scripts: tuple[str, ...]
    sources: tuple[SourceSpan, ...]
    css: str
    html: str


def read_source(root: Path, name: str) -> str:
    if not isinstance(name, str) or not name or "\\" in name:
        raise SourceError(f"invalid source path: {name!r}")
    path = Path(name)
    if path.is_absolute() or ".." in path.parts:
        raise SourceError(f"source path must stay inside {root}: {name}")
    resolved = (root / path).resolve()
    if not resolved.is_relative_to(root.resolve()):
        raise SourceError(f"source symlink escapes {root}: {name}")
    try:
        text = resolved.read_bytes().decode("utf-8")
    except (OSError, UnicodeError) as exc:
        raise SourceError(f"cannot read {name}: {exc}") from exc
    if not text or "\r" in text or "\x00" in text or text.startswith("\ufeff"):
        raise SourceError(f"{name}: require nonempty UTF-8 with LF newlines and no BOM/NUL")
    return text


def assemble(source_root: Path = SOURCE_ROOT) -> Assembly:
    source_root = Path(source_root)
    try:
        manifest = json.loads(read_source(source_root, "manifest.json"))
    except ValueError as exc:
        raise SourceError(f"invalid manifest JSON: {exc}") from exc
    if not isinstance(manifest, dict) or manifest.get("format") != 1:
        raise SourceError("manifest must be an object with format 1")
    if set(manifest) != {"format", "template", "parts"}:
        raise SourceError("manifest keys must be format, template, parts")
    parts = manifest["parts"]
    if not isinstance(parts, dict) or set(parts) != set(PARTS):
        raise SourceError(f"manifest parts must be {', '.join(PARTS)}")
    template_name = manifest["template"]
    template = read_source(source_root, template_name)
    matches = list(TOKEN.finditer(template))
    if sorted(m.group(1) for m in matches) != sorted(PARTS):
        raise SourceError("template must contain each of @@CSS@@, @@HTML@@, @@BOOTSTRAP@@, @@MAIN@@ exactly once")
    seen = {(source_root / template_name).resolve(), (source_root / "manifest.json").resolve()}
    fragments = {}
    for part in PARTS:
        names = parts[part]
        if not isinstance(names, list) or not names:
            raise SourceError(f"{part} must contain a nonempty ordered file list")
        if part != "MAIN" and len(names) != 1:
            raise SourceError(f"{part} must contain exactly one source file")
        fragments[part] = []
        for name in names:
            text = read_source(source_root, name)
            canonical = (source_root / name).resolve()
            if canonical in seen:
                raise SourceError(f"duplicate source file: {name}")
            seen.add(canonical)
            delimiter = part if part in ("CSS", "HTML") else "JS"
            if ")" + delimiter + '"' in text:
                raise SourceError(f"{name}: contains the C++ raw-string terminator for {delimiter}")
            if part in ("BOOTSTRAP", "MAIN") and re.search(r"</script", text, re.I):
                raise SourceError(f"{name}: contains a closing script tag")
            fragments[part].append((name, text))
    listed_js = {name for part in ("BOOTSTRAP", "MAIN") for name, _ in fragments[part]}
    actual_js = {str(p.relative_to(source_root)) for p in (source_root / "src").rglob("*.js")}
    if actual_js - listed_js:
        raise SourceError("JavaScript missing from manifest: " + ", ".join(sorted(actual_js - listed_js)))
    # Also check joins: a closing token may be split across two source files.
    joined = {part: "".join(text for _, text in fragments[part]) for part in PARTS}
    for part in ("BOOTSTRAP", "MAIN"):
        if ')JS"' in joined[part] or re.search(r"</script", joined[part], re.I):
            raise SourceError(f"{part}: unsafe closing token across fragment boundaries")

    output = []
    spans = []
    offset = 0

    def append(name: str, text: str, source_line: int = 1) -> None:
        nonlocal offset
        if text:
            output.append(text)
            spans.append(SourceSpan(name, text, offset, source_line))
            offset += len(text)

    cursor = 0
    for match in matches:
        append(template_name, template[cursor:match.start()], template[:cursor].count("\n") + 1)
        for name, text in fragments[match.group(1)]:
            append(name, text)
        cursor = match.end()
    append(template_name, template[cursor:], template[:cursor].count("\n") + 1)
    return Assembly("".join(output).encode("utf-8"),
                    (joined["BOOTSTRAP"], joined["MAIN"]), tuple(spans),
                    joined["CSS"], joined["HTML"])


def check(source_root: Path = SOURCE_ROOT, header: Path = HEADER) -> Assembly:
    result = assemble(source_root)
    try:
        current = Path(header).read_bytes()
    except OSError as exc:
        raise SourceError(f"cannot read generated header: {exc}; run the build command") from exc
    if current != result.header:
        raise SourceError("generated header is stale; review direct header edits, then run python3 tools/game/dev.py build")
    syntax_check(result)
    return result


def build(source_root: Path = SOURCE_ROOT, header: Path = HEADER) -> bool:
    result = assemble(source_root)
    syntax_check(result)
    header = Path(header)
    if header.exists() and header.read_bytes() == result.header:
        return False
    # Atomic replacement ensures a failed write does not leave a partial header.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=header.parent, prefix=".game-header-", delete=False) as f:
            temporary = Path(f.name)
            f.write(result.header)
            f.flush()
            os.fsync(f.fileno())
        temporary.chmod(header.stat().st_mode & 0o777 if header.exists() else 0o644)
        temporary.replace(header)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    return True


def source_location(assembly: Assembly, header_line: int) -> tuple[str, int]:
    lines = assembly.header.decode("utf-8").splitlines(keepends=True)
    if header_line < 1 or header_line > len(lines):
        raise SourceError(f"header line out of range: {header_line}")
    offset = sum(len(line) for line in lines[:header_line - 1])
    for span in assembly.sources:
        if span.offset <= offset < span.offset + len(span.text):
            return span.source, span.source_line + span.text[:offset - span.offset].count("\n")
    raise SourceError(f"no source mapping for header line {header_line}")


def syntax_check(assembly: Assembly) -> None:
    from tools.webui.js_engine import JS_ENGINE, run_js
    if JS_ENGINE is None:
        raise SourceError("syntax check needs a supported JavaScript engine (node, deno, bun, qjs, jsc, or osascript)")
    with tempfile.TemporaryDirectory(prefix="hw1-game-syntax-") as directory:
        root = Path(directory)
        entries = []
        for i, script in enumerate(assembly.scripts):
            path = root / f"script-{i}.js"
            path.write_bytes(script.encode("utf-8"))
            entries.append(json.dumps({"name": "game " + ("bootstrap" if i == 0 else "main"), "file": str(path)}))
        manifest = root / "scripts.jsonl"
        manifest.write_text("\n".join(entries) + "\n", encoding="utf-8")
        result = run_js(REPO_ROOT / "tools/webui/harness/syntax_check.js", [str(manifest)])
    lines = result.stdout.splitlines()
    if (result.returncode != 0 or lines.count("SYNTAX_RESULT PASS") != 1
            or sum(line.startswith("SYNTAX OK ") for line in lines) != 2
            or any(line.startswith("SYNTAX FAIL ") for line in lines)):
        raise SourceError("JavaScript syntax check failed:\n" + result.stdout + result.stderr)


def preview_html(assembly: Assembly) -> bytes:
    """Preview shipping bytes with a small local shell and no hardware backend."""
    from tools.webui.extract_js import extract_blocks
    # The extractor is tracked infrastructure, independent of the local audit lab.
    with tempfile.TemporaryDirectory(prefix="hw1-game-preview-") as directory:
        path = Path(directory) / "game.h"
        path.write_bytes(assembly.header)
        extracted = extract_blocks(path)
        blocks = {block.delim: block for block in extracted}
    if len(extracted) != 3 or set(blocks) != {"CSS", "HTML", "JS"}:
        raise SourceError("preview requires exactly the CSS, HTML, and JS raw-string blocks")
    prefix = ("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>HardwareOne game preview</title>\n"
              "<style>:root{--panel-bg:#15191e;--panel-fg:#d8dde3;--border:#3a4450}"
              "body{background:#0c0e11;color:#d8dde3;font:14px system-ui;margin:16px}"
              ".btn{background:#26303a;color:#d8dde3;border:1px solid #3a4450;padding:5px;cursor:pointer}"
              ".input-tall{padding:5px} .text-sm{font-size:12px}</style>\n"
              "<style>" + blocks["CSS"].body + "</style>\n"
              "<script>window.HW1_LOCAL_PREVIEW=true;window.hw=new Proxy({}, {get:function(t,k){"
              "if(k==='then'||typeof k==='symbol')return undefined;"
              "return function(){return new Promise(function(){});};}});</script>\n"
              "</head><body><p>Local preview · Hardware APIs disabled · Use Keyboard + Mouse. "
              "Run build, then refresh after edits.</p>\n" + blocks["HTML"].body)
    js = blocks["JS"]
    padding = js.start_line - prefix.count("\n") - 1
    if padding < 0:
        raise SourceError("preview shell exceeds header's script start; cannot preserve line mapping")
    return (prefix + "\n" * padding + js.body + "</div></body></html>\n").encode("utf-8")


def artwork_preview_html() -> bytes:
    """Small production-recipe gallery; never starts the game or hardware APIs."""
    page = (REPO_ROOT / "tools/game/artwork-preview.html").read_text(encoding="utf-8")
    names = ("01-materials.js", "07-decorations-lighting.js", "07-artwork-cache.js", "12-scene-depth.js")
    scripts = "\n".join(read_source(SOURCE_ROOT, "src/" + name) for name in names)
    return page.replace("/* @PRODUCTION_ARTWORK@ */", scripts).encode("utf-8")


def serve(port: int) -> None:
    check()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            route = urlsplit(self.path).path
            if route not in ("/", "/game.html", "/artwork.html"):
                self.send_error(404)
                return
            try:
                assembly = check()
                payload = artwork_preview_html() if route == "/artwork.html" else preview_html(assembly)
                status = 200
                content_type = "text/html; charset=utf-8"
            except (SourceError, OSError) as exc:
                payload = (str(exc) + "\n").encode("utf-8")
                status = 409
                content_type = "text/plain; charset=utf-8"
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)

    with ThreadingHTTPServer(("127.0.0.1", port), Handler) as server:
        print(f"Game preview: http://127.0.0.1:{server.server_port}/ (Ctrl-C to stop)", flush=True)
        server.serve_forever()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("build", help="generate the firmware header from editable sources")
    commands.add_parser("check", help="check header freshness and game JavaScript syntax")
    tests = commands.add_parser("test", help="run workflow tests and optionally one local audit suite")
    tests.add_argument("--suite", choices=SUITES)
    tests.add_argument("--probes", action="store_true", help="also run the selected suite's mutation probes")
    preview = commands.add_parser("serve", help="serve a local preview without device APIs")
    preview.add_argument("--port", type=int, default=8001)
    where = commands.add_parser("where", help="map a generated header line to editable source")
    where.add_argument("line", type=int)
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            print("Generated " + str(HEADER) if build() else "Generated header is already current")
        elif args.command == "where":
            name, line = source_location(check(), args.line)
            print(f"{SOURCE_ROOT / name}:{line}")
        elif args.command == "serve":
            serve(args.port)
        else:
            if args.command == "test" and args.probes and (not args.suite or args.suite == "smoke"):
                raise SourceError("--probes needs one probe-bearing --suite (smoke has no probe registry)")
            check()
            print("Generated header matches editable sources; both classic scripts parse", flush=True)
            if args.command == "test":
                modules = ["tools.game.tests.test_dev", "tools.game.tests.test_cave_queries",
                           "tools.game.tests.test_cave_geometry", "tools.game.tests.test_cave_lighting",
                           "tools.game.tests.test_cave_render", "tools.game.tests.test_cave_material",
                           "tools.game.tests.test_scene_depth", "tools.game.tests.test_cave_occlusion_integration",
                           "tools.game.tests.test_cave_wall_envelope", "tools.game.tests.test_cave_wall_cover",
                           "tools.game.tests.test_cave_surface_shading",
                           "tools.game.tests.test_cave_surface_material", "tools.game.tests.test_cave_surface_lighting",
                           "tools.game.tests.test_ruin_generation", "tools.game.tests.test_ruin_decor",
                           "tools.game.tests.test_wall_decor", "tools.game.tests.test_structure_floor_v1",
                           "tools.game.tests.test_entity_depth",
                           "tools.game.tests.test_actor_visibility", "tools.game.tests.test_loot_visibility",
                           "tools.game.tests.test_spell_visibility", "tools.game.tests.test_perf_cadence",
                           "tools.game.tests.test_browser_profile", "tools.game.tests.test_scene_depth_coalescing",
                           "tools.game.tests.test_floor_stitch_cache", "tools.game.tests.test_material_palette",
                           "tools.game.tests.test_sky_render",
                           "tools.game.tests.test_terrain_facing", "tools.game.tests.test_cave_exit_transition",
                           "tools.game.tests.test_artwork_cache", "tools.game.tests.test_casting_presentation",
                           "tools.game.tests.test_firstperson_art", "tools.game.tests.test_casting_studio",
                           "tools.game.tests.test_hand_rig", "tools.game.tests.test_casting_styles",
                           "tools.game.tests.test_frame_pause"]
                if args.suite:
                    module = f"tools/webui/tests/test_game_{args.suite}.py"
                    if not (REPO_ROOT / module).exists():
                        raise SourceError("optional local game audit lab is absent; core workflow does not require it")
                    modules.append(f"tools.webui.tests.test_game_{args.suite}")
                env = dict(os.environ, HW1_GAME_JOBS="1", HW1_GAME_PREFETCH="0",
                           HW1_GAME_PROBES="1" if args.probes else "0")
                return subprocess.run([sys.executable, "-m", "unittest", *modules], cwd=REPO_ROOT, env=env).returncode
        return 0
    except (SourceError, OSError, ValueError) as exc:
        print(f"game workflow: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
