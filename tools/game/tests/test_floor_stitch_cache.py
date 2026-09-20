"""Bounded exact-output corner cache and geometry-finalization invalidation."""
from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class FloorStitchCacheTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_cached_stitches_and_invalidation(self) -> None:
        result = run_js(Path(__file__).with_name("floor_stitch_cache.js"), timeout=20)
        self.assertIn("FLOOR_STITCH_CACHE_RESULT PASS 17", result.stdout.splitlines(), result.stdout + result.stderr)
        self.assertEqual(17, sum(line.startswith("PASS ") for line in result.stdout.splitlines()))


if __name__ == "__main__":
    unittest.main()
