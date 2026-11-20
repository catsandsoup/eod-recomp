"""Lifecycle parser coverage, including the mlist-specific VU0 counter."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class AuditParserTest(unittest.TestCase):
    TOOL = Path(__file__).resolve().parents[1] / "tools" / "r7_audit.py"
    ZERO = ("[r7-audit] vu1=0 gif_qwords=0 gs_raster=0 gs_suppressed=0 "
            "sync_vram=0 deferred_wait=0 clut_wait=0 vu0_mlist=0")

    def test_shut_down_log(self):
        cases = [
            (self.ZERO, 0),
            ("boot\nshutdown", 1),
            (self.ZERO + "\n" + self.ZERO, 1),
            (self.ZERO + " unexpected", 1),
            (self.ZERO.replace(" vu0_mlist=0", ""), 1),
            (self.ZERO.replace("vu0_mlist=0", "vu0_mlist=7"), 1),
            (self.ZERO.replace("gs_suppressed=0", "gs_suppressed=2"), 1),
            (self.ZERO.replace("deferred_wait=0", "deferred_wait=1"), 1),
        ]
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.log"
            for contents, expected in cases:
                with self.subTest(contents=contents):
                    log.write_text(contents + "\n")
                    result = subprocess.run([sys.executable, str(self.TOOL), str(log)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
