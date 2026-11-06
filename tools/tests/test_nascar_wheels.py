"""NASCAR wheel resource contract and source-backed conversion checks."""
import inspect
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from import_nascar_cars import merge_physics
from test_nascar_car_physics import model, sections

class WheelPhysicsTests(unittest.TestCase):
    def test_new_class_replaces_donor_on_both_axles(self):
        target=model({'Chassis':{}, 'Front':{'wheel_class':'mus'},'Rear':{'wheel_class':'mus'}})
        source=model({'Chassis':{}, 'Front':{},'Rear':{}})
        actual=sections(merge_physics(target,source))
        for axle in ('Front','Rear'):
            self.assertEqual(actual[axle]['wheel_class'],'ncu')

class WheelSourceTests(unittest.TestCase):
    @unittest.skipUnless(Path('/Volumes/SLUS_20441/DATA/RES/RACE.RES').exists(),'owner source disc unavailable')
    def test_source_geometry_textures_and_lods_survive_conversion(self):
        from nascar_wheels import build_cup_wheels
        from rst0 import unpack_archive
        from nascar_gen import read_modl,walk_vif,batch_positions,part_texture
        from valt import parse_binary_valt
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'source';unpack_archive('/Volumes/SLUS_20441/DATA/RES/RACE.RES',str(src))
            stock=Path(tmp)/'stock';unpack_archive('/Volumes/SLUS_20441/DATA/STOCK.CAR',str(stock))
            resources,provenance=build_cup_wheels(src,parse_binary_valt((stock/'physics.val').read_bytes()))
            table=sections(parse_binary_valt(resources['ncu.val'][0]))
            self.assertEqual(float(table['dimensions']['tire_diameter']),28)
            self.assertAlmostEqual(float(table['dimensions']['width']),305/25.4)
            self.assertEqual([float(v.split()[0]) for v in table['lod'].values()],[5,10,20,40,240])
            for i in range(5):
                source=next(p for p in src.iterdir() if p.name.lower()==f'rtire{i}.mod')
                before=read_modl(source.read_bytes());after=read_modl(resources[f'ncu{i}.mod'][0])
                for a,b in zip(before['parts'],after['parts'],strict=True):
                    ab=walk_vif(a['stream']);bb=walk_vif(b['stream'])
                    for x,y in zip(ab,bb,strict=True):
                        self.assertEqual(batch_positions(a['stream'],x),batch_positions(b['stream'],y))
                        self.assertEqual(y['mscal'],0x55)
                    self.assertIn(part_texture(b),resources)
            for i in range(5):
                source=next(p for p in src.iterdir() if p.name.lower()==f'rtire{i}f.mod')
                before=read_modl(source.read_bytes())
                self.assertTrue(f'ncu{i}f.mod' in resources, 'source fast geometry must be imported')
                after=read_modl(resources[f'ncu{i}f.mod'][0])
                self.assertEqual(len(after['parts']),len(before['parts']))
                for a,b in zip(before['parts'],after['parts'],strict=True):
                    for x,y in zip(walk_vif(a['stream']),walk_vif(b['stream']),strict=True):
                        self.assertEqual(batch_positions(a['stream'],x),batch_positions(b['stream'],y))
                        self.assertEqual(y['mscal'],0x55)
                    self.assertIn(part_texture(b),resources)
            self.assertEqual(set(resources),set(provenance))
            self.assertTrue(all(p['source'] for p in provenance.values()))

if __name__=='__main__':unittest.main()
