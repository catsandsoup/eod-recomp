"""Roof lining must follow the local ceiling, above windows and driver eyes."""
from pathlib import Path
import sys
import os
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import import_nascar_cars as importer

class RoofHeightTests(unittest.TestCase):
    def test_window_vertices_do_not_pull_ceiling_to_eye_level(self):
        roof=[(-.3,1.05,0),(.3,1.05,0),(-.3,1.22,0),(.3,1.22,0)]
        self.assertAlmostEqual(importer.roof_liner_height(roof,0),1.20)

    def test_follows_local_roof_rather_than_distant_peak(self):
        roof=[(0,1.21,0),(0,1.05,0),(0,1.4,2)]
        self.assertAlmostEqual(importer.roof_liner_height(roof,0),1.19)

class SourceRoofTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('NASCAR_STOCK_LOW0'), 'Owner-supplied cockpit fixture unavailable')
    def test_lining_keeps_source_width_and_longitudinal_coverage(self):
        import nascar_gen as gen
        data=Path(os.environ['NASCAR_STOCK_LOW0']).read_bytes()
        mesh=gen.read_modl(importer.transcode_modl(data))
        before=[pt for part in mesh['parts'] for pt in gen.part_points(part)]
        moved=importer.fix_cockpit_roof(mesh,{9:[(-.5,1.05,0),(.5,1.05,0),(-.5,1.22,0),(.5,1.22,0)]})
        after=[pt for part in mesh['parts'] for pt in gen.part_points(part)]
        self.assertGreater(moved,0)
        self.assertEqual([(p[0],p[2]) for p in before],[(p[0],p[2]) for p in after])

if __name__=='__main__':unittest.main()
