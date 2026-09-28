#!/usr/bin/env python3
"""Meaningful transport checks: reject corruption before creating any image."""
import io
import tempfile
import unittest
import zlib
from pathlib import Path

from PIL import Image
from extract_camera_jpeg import decode_capture, extract


def fixture():
    stream = io.BytesIO()
    Image.new('RGB', (32, 24), (30, 100, 210)).save(stream, format='JPEG')
    data = stream.getvalue()
    lines = [f'CAM_JPEG_BEGIN bytes={len(data)} crc32={zlib.crc32(data):08x}']
    lines += [f'CAM_JPEG offset={offset} hex={data[offset:offset+128].hex()}'
              for offset in range(0, len(data), 128)]
    lines.append('CAM_JPEG_END')
    return data, '\n'.join(lines)


class CaptureIntegrity(unittest.TestCase):
    def test_complete_capture(self):
        expected, log = fixture()
        data, result = decode_capture(log)
        self.assertEqual(data, expected)
        self.assertEqual((result['width'], result['height']), (32, 24))

    def test_missing_end(self):
        with self.assertRaisesRegex(ValueError, 'end marker'):
            decode_capture(fixture()[1].replace('CAM_JPEG_END', ''))

    def test_out_of_order(self):
        with self.assertRaisesRegex(ValueError, 'offset'):
            decode_capture(fixture()[1].replace('offset=128 ', 'offset=256 '))

    def test_missing_final_chunk(self):
        lines = fixture()[1].splitlines()
        del lines[-2]
        with self.assertRaisesRegex(ValueError, 'length mismatch'):
            decode_capture('\n'.join(lines))

    def test_corrupt_payload(self):
        with self.assertRaisesRegex(ValueError, 'CRC mismatch'):
            decode_capture(fixture()[1].replace('hex=ffd8', 'hex=00d8'))

    def test_duplicate_capture(self):
        log = fixture()[1]
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            decode_capture(log + '\n' + log)

    def test_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log, out = root / 'capture.log', root / 'image.jpg'
            log.write_text(fixture()[1])
            out.with_suffix('.json').write_text('preserve')
            with self.assertRaisesRegex(ValueError, 'already exists'):
                extract(log, out)
            self.assertFalse(out.exists())
            self.assertEqual(out.with_suffix('.json').read_text(), 'preserve')


if __name__ == '__main__':
    unittest.main()
