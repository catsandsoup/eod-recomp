"""Keep NASCAR source physics intact while translating EOD-specific fields."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from import_nascar_cars import merge_physics


def model(sections):
    return {'sections':[{'name':name,'parent':'','pairs':list(pairs.items()),'sec_vtable':123,'tool_ptr':0}
                        for name,pairs in sections.items()]}


def sections(value):
    return {s['name']:dict(s['pairs']) for s in value['sections']}


class NascarPhysicsTests(unittest.TestCase):
    def setUp(self):
        self.target=model({'Chassis':{'grip':'0.95','damage_scale':'0.65','mass':'2800'},
                           'Front':{'stiff_c1':'100','wheel_class':'com'},
                           'Rear':{'stiff_c1':'101','wheel_class':'com'},
                           'FrontDirt':{'stiff_c1':'25','caster':'20'},
                           'RearDirt':{'stiff_c1':'26'}})
        self.source=model({'Chassis':{'mass':'3500','tachometer':'65,64'},
                           'Front':{'stiff_c1':'410','caster':'6'},
                           'Rear':{'stiff_c1':'418.2'},
                           'EngineRestrictor':{'power_max':'364'}})

    def test_preserves_all_source_sections_and_keys(self):
        actual=sections(merge_physics(self.target,self.source))
        for name,pairs in sections(self.source).items():
            self.assertIn(name,actual)
            for key,value in pairs.items():
                self.assertEqual(actual[name].get(key),value,(name,key))

    def test_neutral_eod_grip_and_damage_multipliers(self):
        actual=sections(merge_physics(self.target,self.source))
        self.assertEqual(float(actual['Chassis']['grip']),1.0)
        self.assertEqual(float(actual['Chassis']['damage_scale']),1.0)

    def test_stock_dirt_overrides_come_from_its_source_tyres(self):
        actual=sections(merge_physics(self.target,self.source))
        for axle in ('Front','Rear'):
            for key,value in sections(self.source)[axle].items():
                self.assertEqual(actual[axle+'Dirt'].get(key),value,(axle,key))

    def test_source_table_is_not_mutated(self):
        before=copy.deepcopy(self.source)
        merge_physics(self.target,self.source)
        self.assertEqual(before,self.source)

if __name__=='__main__':unittest.main()
