"""Artwork cache lifecycle, identity, limits and safe fallbacks."""
from pathlib import Path
from dataclasses import replace
import re
import unittest

from tools.game import dev
from tools.webui.js_engine import JS_ENGINE, run_js


class ArtworkCacheTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_bounded_cache_and_original_fallback(self) -> None:
        result = run_js(Path(__file__).with_name("artwork_cache.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("ARTWORK_CACHE_RESULT PASS 29", result.stdout.splitlines())

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_gallery_embeds_shipping_recipes_and_parses(self) -> None:
        page = dev.artwork_preview_html().decode()
        self.assertNotIn("/* @PRODUCTION_ARTWORK@ */", page)
        self.assertIn("function paintFloorItem(", page)
        self.assertIn("function withSceneDepthBillboard(", page)
        scripts = re.findall(r"<script>(.*?)</script>", page, re.S)
        self.assertEqual(1, len(scripts))
        dev.syntax_check(replace(dev.assemble(), scripts=("", scripts[0])))


if __name__ == "__main__":
    unittest.main()
