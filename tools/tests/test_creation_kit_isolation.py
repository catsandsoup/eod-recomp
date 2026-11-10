"""Development packaging must never replace the accepted application or game."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('builder', Path(__file__).resolve().parents[1] / 'build_modding_app.py')
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)

class IsolationTests(unittest.TestCase):
    def test_build_layout_has_its_own_app_and_swift_cache(self):
        self.assertTrue(hasattr(builder, 'build_layout'), 'Packaging still targets the accepted app')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            layout = builder.build_layout(root)
            self.assertEqual(layout['app'], root / 'build/creation-kit/EOD Creation Kit Development.app')
            self.assertEqual(layout['scratch'], root / 'build/creation-kit/swift')
            self.assertNotEqual(layout['app'], root / 'build/EOD Modding Suite.app')

    def test_symlink_cannot_redirect_development_output(self):
        self.assertTrue(hasattr(builder, 'build_layout'))
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve(); (root / 'build').mkdir(); (root / 'accepted').mkdir()
            (root / 'build/creation-kit').symlink_to(root / 'accepted', target_is_directory=True)
            with self.assertRaises(ValueError): builder.build_layout(root)

    def test_runtime_config_requires_an_existing_executable(self):
        self.assertTrue(hasattr(builder, 'runtime_config'))
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            with self.assertRaises(ValueError): builder.runtime_config(root, root / 'missing')
            game = root / 'game'; game.write_bytes(b'unchanged'); game.chmod(0o755)
            config = builder.runtime_config(root, game)
            self.assertEqual(config['gameExecutable'], str(game))
            self.assertEqual(game.read_bytes(), b'unchanged')
            self.assertEqual(builder.bundle_info()['CFBundleIdentifier'], 'org.eod-recomp.creationkit.development')

if __name__ == '__main__': unittest.main()
