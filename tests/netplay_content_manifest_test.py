import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'content_manifest', Path(__file__).resolve().parents[1] / 'tools/netplay_content_manifest.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ContentManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def file(self, tree, name, payload):
        path = self.root / tree / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)

    def manifest(self, tree, overlay=None):
        return module.build_manifest(self.root / tree, self.root / overlay if overlay else None)

    def test_same_effective_data_across_paths_and_case(self):
        self.file('a', 'DATA/CAR/CUP.CAR', b'cup')
        self.file('b', 'data/car/cup.car', b'cup')
        self.assertEqual(self.manifest('a'), self.manifest('b'))
        self.assertNotIn(str(self.root), str(self.manifest('a')))

    def test_overlay_replaces_and_adds_but_preserves_native(self):
        self.file('base', 'DATA/RES/COMMON.RES', b'native')
        self.file('base', 'DATA/CAR/NATIVE.CAR', b'native-car')
        self.file('overlay', 'data/res/common.res', b'combined')
        self.file('overlay', 'data/car/cup.car', b'cup')
        self.file('flat', 'DATA/RES/COMMON.RES', b'combined')
        self.file('flat', 'DATA/CAR/NATIVE.CAR', b'native-car')
        self.file('flat', 'DATA/CAR/CUP.CAR', b'cup')
        self.assertEqual(self.manifest('base', 'overlay'), self.manifest('flat'))
        self.assertNotEqual(self.manifest('base')['content_sha256'],
                            self.manifest('flat')['content_sha256'])

    def test_changed_byte_and_missing_file_change_identity(self):
        self.file('a', 'DATA/A', b'123')
        original = self.manifest('a')['content_sha256']
        self.file('a', 'DATA/A', b'124')
        self.assertNotEqual(original, self.manifest('a')['content_sha256'])
        self.file('a', 'DATA/B', b'other')
        with_extra = self.manifest('a')['content_sha256']
        (self.root / 'a/DATA/B').unlink()
        self.assertNotEqual(with_extra, self.manifest('a')['content_sha256'])

    def test_metadata_outside_data_does_not_change_identity(self):
        self.file('a', 'DATA/A', b'asset')
        before = self.manifest('a')
        self.file('a', 'composition-report.json', b'local machine paths')
        self.assertEqual(before, self.manifest('a'))

    def test_file_directory_conflict_rejected(self):
        self.file('a', 'DATA/CAR', b'file')
        self.file('b', 'DATA/CAR/CUP.CAR', b'car')
        with self.assertRaisesRegex(ValueError, 'file/directory collision'):
            self.manifest('a', 'b')

    def test_symlink_and_empty_tree_rejected(self):
        (self.root / 'a/DATA').mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, 'Empty'):
            self.manifest('a')
        (self.root / 'a/DATA/link').symlink_to(self.root)
        with self.assertRaisesRegex(ValueError, 'symlink'):
            self.manifest('a')

    def test_ambiguous_case_resolution_rejected(self):
        # Model a case-sensitive volume's ambiguous enumeration on any host.
        self.file('a', 'DATA/CUP.CAR', b'car')
        directory = self.root / 'a/DATA'
        child = directory / 'CUP.CAR'
        with patch.object(Path, 'iterdir', return_value=iter([child, child])):
            with self.assertRaisesRegex(ValueError, 'collision'):
                module._inventory(directory)


if __name__ == '__main__':
    unittest.main()
