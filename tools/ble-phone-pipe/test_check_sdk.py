"""The production checker must reject unknown source without modifying it."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('phone_sdk_check',Path(__file__).with_name('check_sdk.py'))
checker=importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

class CheckSdk(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='hw1-phone-sdk-')
        self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        self.source=self.root/'source'
        self.assets=self.root/'assets';self.assets.mkdir()
        patch=b'synthetic patch\n';(self.assets/'arduino.patch').write_bytes(patch)
        self.manifest={'schema':1,'patch_sha256':hashlib.sha256(patch).hexdigest(),'files':{}}
        for name in sorted(checker.EXPECTED_FILES):
            path=self.source/name;path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(b'after\n')
            self.manifest['files'][name]={'before':hashlib.sha256(b'before\n').hexdigest(),'after':hashlib.sha256(b'after\n').hexdigest()}
        self.save()
    def save(self):
        (self.assets/'manifest.json').write_text(json.dumps(self.manifest))
    def test_matching_output(self):
        checker.check(self.source,self.assets)
    def test_baseline_is_not_silently_changed(self):
        target=self.source/sorted(checker.EXPECTED_FILES)[0];target.write_bytes(b'before\n')
        with self.assertRaisesRegex(ValueError,'apply the reviewed patch'):checker.check(self.source,self.assets)
        self.assertEqual(target.read_bytes(),b'before\n')
    def test_unknown_edits_are_preserved(self):
        target=self.source/sorted(checker.EXPECTED_FILES)[0];target.write_bytes(b'local work\n')
        with self.assertRaisesRegex(ValueError,'unrecognized changes'):checker.check(self.source,self.assets)
        self.assertEqual(target.read_bytes(),b'local work\n')
    def test_missing_file(self):
        (self.source/sorted(checker.EXPECTED_FILES)[0]).unlink()
        with self.assertRaisesRegex(ValueError,'missing'):checker.check(self.source,self.assets)
    def test_tampered_patch(self):
        (self.assets/'arduino.patch').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'checksum'):checker.check(self.source,self.assets)
    def test_incomplete_manifest(self):
        self.manifest['files'].pop(sorted(checker.EXPECTED_FILES)[0]);self.save()
        with self.assertRaises(ValueError):checker.check(self.source,self.assets)
    def test_symlink(self):
        target=self.source/sorted(checker.EXPECTED_FILES)[0];target.unlink()
        real=self.root/'real';real.write_bytes(b'after\n');target.symlink_to(real)
        with self.assertRaisesRegex(ValueError,'symlink'):checker.check(self.source,self.assets)

if __name__=='__main__':unittest.main()
