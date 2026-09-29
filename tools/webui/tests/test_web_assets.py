"""Check the exact source-to-gzip boundary and actual shipping HTTP handler."""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tools.webui import extract_js

ROOT = Path(__file__).resolve().parents[3]
COMPONENT = ROOT / 'components/hardwareone'
ASSETS = COMPONENT / 'web_assets'
spec = importlib.util.spec_from_file_location('web_assets_generator', ASSETS / 'generate.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class ExactAssets(unittest.TestCase):
    def test_committed_payload_and_fixed_header(self):
        raw, packed, identity = generator.check()
        self.assertGreater(raw - packed, 45000)
        self.assertEqual(identity, hashlib.sha256(generator.SOURCE.read_bytes()).hexdigest())
        first = generator.gzip_exact(generator.SOURCE.read_bytes())
        self.assertEqual(first, generator.gzip_exact(generator.SOURCE.read_bytes()))
        self.assertEqual(first[:10], bytes.fromhex('1f8b08000000000002ff'))

    def test_changes_and_corruption_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix='hw1-web-assets-') as directory:
            path = Path(directory)
            source = path / 'source.js'; header = path / 'generated.h'
            source.write_bytes(generator.SOURCE.read_bytes())
            header.write_bytes(generator.HEADER.read_bytes())
            generator.check(header, source)
            source.write_bytes(source.read_bytes() + b'\n// changed\n')
            with self.assertRaises(ValueError): generator.check(header, source)
            source.write_bytes(generator.SOURCE.read_bytes())
            text = header.read_text()
            header.write_text(text.replace('0x1f', '0x1e', 1))
            with self.assertRaises((ValueError, generator.zlib.error)): generator.check(header, source)
            header.write_text(text.replace('// Source SHA256: ', '// Source SHA256: 0', 1))
            with self.assertRaises(ValueError): generator.check(header, source)
            header.write_text(text.replace('\n};', '\n  0x00,\n};', 1))
            with self.assertRaises(ValueError): generator.check(header, source)

    def test_script_runs_in_original_position_without_async(self):
        page = COMPONENT / 'WebPage_ESPNow.h'
        text = page.read_text()
        tag = '<script src="/assets/espnow-core.js"></script>'
        self.assertEqual(text.count(tag), 1)
        self.assertNotIn('Chunk 3A: listDevices function start', text)
        regions = extract_js.joined_regions(page)
        external = [i for i, r in enumerate(regions) if 'web_assets/espnow-core.js' in r.name]
        self.assertEqual(len(external), 1)
        index = external[0]
        self.assertEqual(regions[index].text.encode(), generator.SOURCE.read_bytes())
        self.assertIn('Chunk 2: Status functions ready', regions[index-1].text)
        self.assertIn('Chunk 4b: Mesh status functions start', regions[index+1].text)
        flattened, _ = extract_js.page_js(page)
        self.assertIn(generator.SOURCE.read_text(), flattened)

    def test_missing_or_unknown_script_fails_loudly(self):
        with tempfile.TemporaryDirectory(prefix='hw1-web-extract-') as directory:
            path = Path(directory) / 'page.h'
            path.write_text('R"JS(<script src="/assets/espnow-core.js"></script>)JS"')
            with self.assertRaises(extract_js.ExtractionError): extract_js.joined_regions(path)
            path.write_text('R"JS(<script src="https://example.invalid/app.js"></script>)JS"')
            with self.assertRaises(extract_js.ExtractionError): extract_js.joined_regions(path)

    @unittest.skipUnless(shutil.which('clang++'), 'clang++ required for actual-handler test')
    def test_shipping_http_handler(self):
        source = (COMPONENT / 'WebAssets.cpp').read_text()
        body = source.split('// WEB_ASSET_HANDLER_BEGIN:', 1)[1].split('\n', 1)[1].split('// WEB_ASSET_HANDLER_END', 1)[0]
        harness = (Path(__file__).parent.parent / 'harness/web_assets_http.cpp').read_text()
        with tempfile.TemporaryDirectory(prefix='hw1-web-handler-') as directory:
            path = Path(directory); cpp = path / 'test.cpp'; executable = path / 'test'
            cpp.write_text(harness.replace('// ACTUAL_HANDLER', body))
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-I', str(COMPONENT), str(cpp), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__': unittest.main()
