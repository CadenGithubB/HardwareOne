"""Guard the source-to-firmware boundary without freezing future game content."""

from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path
import re
import tempfile
import unittest

from tools.game import dev


class SourceFixtureTests(unittest.TestCase):
    """Use a tiny complete source tree so invalid builds cannot touch firmware."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="hw1_game_source_test_")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "game"
        (self.root / "src").mkdir(parents=True)
        self.header = Path(self.tmp.name) / "generated.h"
        self.template = (
            '// fixture header\nR"CSS(@@CSS@@)CSS"\n'
            'R"HTML(@@HTML@@)HTML"\n'
            'R"JS(<script>@@BOOTSTRAP@@</script>\n'
            '<script>@@MAIN@@</script>)JS"\n'
        )
        self.manifest = {
            "format": 1,
            "template": "header.h.in",
            "parts": {
                "CSS": ["page.css"],
                "HTML": ["page.html"],
                "BOOTSTRAP": ["src/00-errors.js"],
                "MAIN": ["src/10-start.js", "src/20-end.js"],
            },
        }
        self.put("header.h.in", self.template)
        self.put("page.css", ".game { color: green; }")
        self.put("page.html", "<div class='game'>Game</div>")
        self.put("src/00-errors.js", "window.fixtureReady = true;\n")
        # A newline inserted between fragments would change this string value.
        self.put("src/10-start.js", 'var fixtureValue = "left')
        self.put("src/20-end.js", 'right";\n')
        self.save_manifest()

    def put(self, relative: str, text: str) -> None:
        (self.root / relative).write_text(text, encoding="utf-8")

    def save_manifest(self) -> None:
        self.put("manifest.json", json.dumps(self.manifest))

    def test_assembly_preserves_exact_bytes_and_manifest_order(self) -> None:
        result = dev.assemble(self.root)
        expected = self.template
        for name, paths in self.manifest["parts"].items():
            content = b"".join((self.root / path).read_bytes() for path in paths)
            expected = expected.replace("@@" + name + "@@", content.decode("utf-8"))
        self.assertEqual(expected.encode("utf-8"), result.header)
        self.assertEqual(
            ("window.fixtureReady = true;\n", 'var fixtureValue = "leftright";\n'),
            result.scripts,
        )

    def test_missing_fragment_is_rejected(self) -> None:
        (self.root / "src/20-end.js").unlink()
        with self.assertRaises(dev.SourceError):
            dev.assemble(self.root)

    def test_duplicate_fragment_is_rejected(self) -> None:
        self.manifest["parts"]["MAIN"].append("src/10-start.js")
        self.save_manifest()
        with self.assertRaises(dev.SourceError):
            dev.assemble(self.root)

    def test_fragment_cannot_be_shared_between_parts(self) -> None:
        self.manifest["parts"]["MAIN"].append("src/00-errors.js")
        self.save_manifest()
        with self.assertRaises(dev.SourceError):
            dev.assemble(self.root)

    def test_missing_unknown_and_duplicate_template_tokens_are_rejected(self) -> None:
        variants = {
            "missing": self.template.replace("@@MAIN@@", ""),
            "unknown": self.template + "@@SURPRISE@@",
            "duplicate": self.template + "@@MAIN@@",
        }
        for name, template in variants.items():
            with self.subTest(name=name):
                self.put("header.h.in", template)
                with self.assertRaises(dev.SourceError):
                    dev.assemble(self.root)

    def test_cpp_raw_string_and_html_script_terminators_are_rejected(self) -> None:
        collisions = [
            ("page.css", ')CSS"'),
            ("page.html", ')HTML"'),
            ("src/10-start.js", ')JS"'),
            ("src/00-errors.js", 'var text = "</script>";'),
            ("src/00-errors.js", 'var text = "</ScRiPt>";'),
        ]
        for relative, content in collisions:
            with self.subTest(relative=relative, content=content):
                original = (self.root / relative).read_text(encoding="utf-8")
                try:
                    self.put(relative, content)
                    with self.assertRaises(dev.SourceError):
                        dev.assemble(self.root)
                finally:
                    self.put(relative, original)

    def test_outside_source_paths_are_rejected(self) -> None:
        outside = Path(self.tmp.name) / "outside.css"
        outside.write_text("outside {}", encoding="utf-8")
        for path in ("../outside.css", str(outside)):
            with self.subTest(path=path):
                self.manifest["parts"]["CSS"] = [path]
                self.save_manifest()
                with self.assertRaises(dev.SourceError):
                    dev.assemble(self.root)

    def test_terminator_split_across_fragments_is_rejected(self) -> None:
        self.put("src/10-start.js", ")J")
        self.put("src/20-end.js", 'S"')
        with self.assertRaises(dev.SourceError):
            dev.assemble(self.root)

    def test_source_symlink_cannot_escape_root(self) -> None:
        outside = Path(self.tmp.name) / "outside.css"
        outside.write_text("outside {}", encoding="utf-8")
        (self.root / "escape.css").symlink_to(outside)
        self.manifest["parts"]["CSS"] = ["escape.css"]
        self.save_manifest()
        with self.assertRaises(dev.SourceError):
            dev.assemble(self.root)

    def test_symlink_alias_cannot_include_a_fragment_twice(self) -> None:
        (self.root / "src/alias.js").symlink_to(self.root / "src/10-start.js")
        self.manifest["parts"]["MAIN"].append("src/alias.js")
        self.save_manifest()
        with self.assertRaises(dev.SourceError):
            dev.assemble(self.root)

    def test_unlisted_javascript_is_rejected(self) -> None:
        self.put("src/30-forgotten.js", "var forgotten = true;\n")
        with self.assertRaises(dev.SourceError):
            dev.assemble(self.root)

    def test_stale_check_does_not_rewrite_the_header(self) -> None:
        self.header.write_bytes(b"existing firmware header\n")
        before = self.header.stat().st_mtime_ns
        with self.assertRaises(dev.SourceError):
            dev.check(self.root, self.header)
        self.assertEqual(b"existing firmware header\n", self.header.read_bytes())
        self.assertEqual(before, self.header.stat().st_mtime_ns)

    def test_build_is_idempotent_and_check_accepts_built_header(self) -> None:
        self.assertTrue(dev.build(self.root, self.header))
        expected = dev.assemble(self.root).header
        self.assertEqual(expected, self.header.read_bytes())
        before = self.header.stat().st_mtime_ns
        self.assertFalse(dev.build(self.root, self.header))
        self.assertEqual(before, self.header.stat().st_mtime_ns)
        self.assertEqual(expected, dev.check(self.root, self.header).header)

    def test_validation_failure_preserves_previous_header(self) -> None:
        self.assertTrue(dev.build(self.root, self.header))
        before = self.header.read_bytes()
        self.put("page.css", ')CSS"')
        with self.assertRaises(dev.SourceError):
            dev.build(self.root, self.header)
        self.assertEqual(before, self.header.read_bytes())

    def test_syntax_failure_preserves_previous_header(self) -> None:
        self.assertTrue(dev.build(self.root, self.header))
        before = self.header.read_bytes()
        self.put("src/10-start.js", "var fixtureValue = ;\n")
        self.put("src/20-end.js", "// end\n")
        with self.assertRaises(dev.SourceError):
            dev.build(self.root, self.header)
        self.assertEqual(before, self.header.read_bytes())

    def test_check_rejects_invalid_javascript_even_when_header_matches(self) -> None:
        self.put("src/10-start.js", "var fixtureValue = ;\n")
        self.put("src/20-end.js", "// end\n")
        self.header.write_bytes(dev.assemble(self.root).header)
        before = self.header.read_bytes()
        with self.assertRaises(dev.SourceError):
            dev.check(self.root, self.header)
        self.assertEqual(before, self.header.read_bytes())

    def test_source_map_rejects_out_of_range_lines(self) -> None:
        assembled = dev.assemble(self.root)
        for line in (0, -1, len(assembled.header.splitlines()) + 100):
            with self.subTest(line=line):
                with self.assertRaises(dev.SourceError):
                    dev.source_location(assembled, line)


class RepositorySourceTests(unittest.TestCase):
    def test_preview_rejects_duplicate_raw_string_blocks(self) -> None:
        assembled = dev.assemble()
        duplicated = replace(assembled, header=assembled.header + b'\nR"CSS(duplicate)CSS"\n')
        with self.assertRaises(dev.SourceError):
            dev.preview_html(duplicated)

    def test_checked_in_header_matches_editable_sources(self) -> None:
        self.assertEqual(dev.HEADER.read_bytes(), dev.assemble().header)

    def test_real_function_maps_to_its_editable_source_line(self) -> None:
        assembled = dev.assemble()
        marker = "function getWalkableLayerTopAt("
        header_lines = assembled.header.decode("utf-8").splitlines()
        matches = [i + 1 for i, line in enumerate(header_lines) if line.startswith(marker)]
        self.assertEqual(1, len(matches), "choose a unique real function for the source-map check")
        relative, source_line = dev.source_location(assembled, matches[0])
        source = dev.SOURCE_ROOT / relative
        self.assertTrue(source.is_relative_to(dev.SOURCE_ROOT))
        self.assertEqual(".js", source.suffix)
        source_lines = source.read_text(encoding="utf-8").splitlines()
        self.assertEqual(header_lines[matches[0] - 1], source_lines[source_line - 1])

    def test_preview_preserves_both_actual_script_bodies(self) -> None:
        assembled = dev.assemble()
        preview = dev.preview_html(assembled).decode("utf-8")
        scripts = re.findall(r"(?is)<script\b[^>]*>(.*?)</script\s*>", preview)
        self.assertEqual(3, len(scripts), "preview adds one hardware stub before the two game scripts")
        self.assertEqual(assembled.scripts, tuple(scripts[1:]))
        self.assertIn(assembled.css, preview)
        self.assertIn(assembled.html, preview)

    def test_preview_keeps_the_games_original_header_line_numbers(self) -> None:
        assembled = dev.assemble()
        header = assembled.header.decode("utf-8")
        preview = dev.preview_html(assembled).decode("utf-8")
        header_lines = header.splitlines()
        preview_lines = preview.splitlines()
        for script in assembled.scripts:
            self.assertEqual(header[:header.index(script)].count("\n"),
                             preview[:preview.index(script)].count("\n"))
        function_line = next(i for i, line in enumerate(header_lines)
                             if line.startswith("function getWalkableLayerTopAt("))
        self.assertEqual(header_lines[function_line], preview_lines[function_line])

    def test_preview_loads_no_external_scripts_or_assets(self) -> None:
        preview = dev.preview_html(dev.assemble()).decode("utf-8")
        self.assertIsNone(re.search(r"(?i)<script\b[^>]*\bsrc\s*=", preview))
        self.assertIsNone(re.search(r"(?i)<link\b", preview))
        self.assertIsNone(re.search(r"(?i)@import\b", preview))
        self.assertIsNone(re.search(r"(?i)\b(?:https?|wss?|ftp)://", preview))
        styles = re.findall(r"(?is)<style\b[^>]*>(.*?)</style>", preview)
        urls = re.findall(r"(?i)url\(\s*['\"]?([^'\")]*)", "\n".join(styles))
        self.assertEqual([], [url for url in urls if url and not url.startswith("data:")])


if __name__ == "__main__":
    unittest.main()
