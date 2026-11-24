"""Lifecycle parser and VU census coverage."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class AuditParserTest(unittest.TestCase):
    TOOL = Path(__file__).resolve().parents[1] / "tools" / "r7_audit.py"
    ZERO = ("[r7-audit] v=2 vu1=0 gif_qwords=0 gs_raster=0 gs_suppressed=0 "
            "sync_vram=0 deferred_wait=0 clut_wait=0 vu0_mlist=0 vu0_micro=0 vu0_macro=0 vu0_control=0")

    def test_shut_down_log(self):
        cases = [
            (self.ZERO, 0),
            ("boot\nshutdown", 1),
            ("[r7-audit-note] " + self.ZERO, 1),
            (self.ZERO.replace("v=2", "v=1"), 1),
            (self.ZERO + "\n" + self.ZERO, 1),
            (self.ZERO + " unexpected", 1),
            (self.ZERO.replace(" v=2", "").split(" vu0_micro=")[0], 1),
            (self.ZERO.replace(" vu0_mlist=0", ""), 1),
            (self.ZERO.replace("vu0_mlist=0", "vu0_mlist=7"), 1),
            (self.ZERO.replace("vu0_micro=0", "vu0_micro=1"), 1),
            (self.ZERO.replace("vu0_macro=0", "vu0_macro=1"), 1),
            (self.ZERO.replace("vu0_control=0", "vu0_control=1"), 1),
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

    def test_census_reconciliation(self):
        empty_vu0 = "[r7-vu] vu=0 rows=0 dropped=0"
        empty_vu1 = "[r7-vu] vu=1 rows=0 dropped=0"
        launch = "[r7-vu] vu=0 kind=vcallms address=00000048 image=0123456789012345 context=00358808 count=17"
        launch_end = "[r7-vu] vu=0 rows=1 dropped=0"
        launch_audit = self.ZERO.replace("vu0_micro=0", "vu0_micro=17")
        retirement = "[r7-vu] vu=1 kind=retired_world address=00000270 image=0123456789012345 context=00000080 count=13"
        clipped_retirement = ("[r7-vu] vu=1 kind=retired_world_clipped address=00000618 "
                              "image=fd0ce30ab0ec263c context=00000280 count=13")
        executed = "[r7-vu] vu=1 kind=mscnt address=00000618 image=0123456789012345 context=00000280 count=9"
        vu1_end = "[r7-vu] vu=1 rows=1 dropped=0"
        cases = [
            ([empty_vu0, empty_vu1, self.ZERO], 0, "reconciles"),
            ([empty_vu0, self.ZERO, empty_vu1], 1, "after the R7 shutdown summary"),
            ([self.ZERO, empty_vu0, empty_vu1], 1, "after the R7 shutdown summary"),
            ([empty_vu0, retirement, vu1_end, self.ZERO], 0, "reconciles"),
            ([empty_vu0, clipped_retirement, vu1_end, self.ZERO], 0, "reconciles"),
            ([launch, launch_end, empty_vu1, launch_audit], 1, "vu0_micro=17"),
            ([empty_vu0, executed, vu1_end, self.ZERO], 1, "vu1 differs"),
            ([launch, launch_end, empty_vu1, self.ZERO], 1, "vu0_micro differs"),
            ([launch, launch_end, self.ZERO], 1, "missing VU census"),
            ([launch, launch, launch_end, empty_vu1, launch_audit], 1, "duplicate"),
            ([empty_vu0, empty_vu1.replace("dropped=0", "dropped=1"), self.ZERO], 1, "truncated"),
        ]
        for kind, address in (("retired_0000", "00000000"), ("retired_2538", "00002538"),
                              ("retired_25f0", "000025f0"), ("retired_queue_dent", "00000d48")):
            row = retirement.replace("retired_world", kind).replace("00000270", address)
            cases.append(([empty_vu0, row, vu1_end, self.ZERO], 0, "reconciles"))
            cases.append(([empty_vu0, row.replace("address=" + address, "address=00000008"),
                           vu1_end, self.ZERO], 1, "invalid address"))
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.log"
            for lines, status, message in cases:
                with self.subTest(lines=lines):
                    log.write_text("\n".join(lines) + "\n")
                    result = subprocess.run([sys.executable, str(self.TOOL), "--check-vu-census", str(log)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, status, result.stdout + result.stderr)
                    self.assertIn(message, result.stdout)


if __name__ == "__main__":
    unittest.main()
