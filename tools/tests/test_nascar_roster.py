"""Five-family batch contracts; owned-disc checks are opt-in via NASCAR_SOURCE_DATA."""
import copy
import os
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import import_nascar_cars as importer

class BatchContracts(unittest.TestCase):
    def test_pace_physics_accepts_its_own_wheel_profile(self):
        source={'sections':[{'name':n,'pairs':list(p.items()),'parent':''} for n,p in
                [('Chassis',{'mass':'3500'}),('Front',{'model':'ptire'}),('Rear',{'model':'ptire'})]]}
        result=importer.merge_physics(copy.deepcopy(source),source,wheel_class='npc')
        values={s['name']:dict(s['pairs']) for s in result['sections']}
        self.assertEqual(values['Front']['wheel_class'],'npc')
        self.assertEqual(values['Rear']['model'],'ptire')

    def test_batch_plan_has_five_families_and_stable_collision_free_ids(self):
        import nascar_roster as roster
        source=[{'family':'stock','source_number':'24','name':'Jeff Gordon','body_style':'chevy'},
                {'family':'truck','source_number':'4','name':'Source Truck','body_style':'chevy'},
                {'family':'hobby','source_number':'402','name':'Ted Finger','body_style':'chevy'}]
        before=copy.deepcopy(source)
        plan=roster.allocate_liveries(source,used_ids={'1024','2024'})
        self.assertEqual(len({p['id'] for p in plan}),3)
        self.assertTrue(all(p['id'] not in {'1024','2024'} for p in plan))
        self.assertEqual(plan,roster.allocate_liveries(list(reversed(source)),used_ids={'1024','2024'}))
        self.assertEqual(source,before)
        self.assertEqual(set(roster.FAMILIES),{'stock','truck','modified','hobby','pacecar'})

    def test_source_bones_keep_original_travel_and_weakness(self):
        import nascar_roster as roster
        source={'sections':[{'name':'bone_1','pairs':[('link_center','0, 20, 80'),('travel_min','-1,-2,-3'),('travel_max','1,2,3'),('weakness','7.0')]},
                            {'name':'bone_2','pairs':[('link_center','10, 20, -80'),('travel_min','-4,-5,-6'),('travel_max','4,5,6'),('weakness','9.0')]}]}
        template={'sections':[{'sec_vtable':4243672}]}
        mvl,local=roster.source_damage_model(template,source,{9:[(0,0,0),(1,1,1)]})
        for s in source['sections']:
            actual=next(s2 for s2 in mvl['sections'] if s2['name']==s['name'])
            self.assertEqual(dict(actual['pairs']),dict(s['pairs']))
        self.assertEqual(local[0][0],1)
        self.assertAlmostEqual(local[0][1][1],20*.0254)

    def test_family_group_mapping_does_not_apply_cup_filter_to_truck(self):
        import nascar_roster as roster
        import nascar_gen as gen
        import struct
        def part(g,tex):
            h=bytearray(0x38);struct.pack_into('<I',h,0x18,g);h[8:24]=tex.encode().ljust(16,b'\0')
            return {'hdr':h}
        m={'parts':[part(g,'Truck1.tex') for g in range(5)]}
        roster.remap_family_groups(m,'truck','chevy')
        self.assertEqual(len(m['parts']),5)
        self.assertEqual({gen.part_group(p)&0xffff for p in m['parts']},{9})
        m={'parts':[part(g,'Stock1.tex') for g in (5,6,7,8,9,10,11,12)]}
        roster.remap_family_groups(m,'stock','dodge')
        self.assertEqual(len(m['parts']),4)
        self.assertTrue(all(gen.part_group(p)&0xffff in (1,2,9,10) for p in m['parts']))


class SourceWheelProfiles(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('NASCAR_SOURCE_DATA'), 'owned source data not configured')
    def test_pace_wheels_preserve_all_generic_slow_and_fast_payload_positions(self):
        from rst0 import unpack_archive
        from valt import parse_binary_valt
        from nascar_wheels import build_source_wheels
        import nascar_gen as gen
        import tempfile
        source=Path(os.environ['NASCAR_SOURCE_DATA'])
        with tempfile.TemporaryDirectory() as tmp:
            race=Path(tmp)/'race';car=Path(tmp)/'car'
            unpack_archive(str(source/'RES'/'RACE.RES'),str(race))
            unpack_archive(str(source/'PACECAR.CAR'),str(car))
            phys=parse_binary_valt(importer.find_entry(car,'physics.val').read_bytes())
            resources,_=build_source_wheels(race,phys,prefix='npc')
            self.assertNotIn('npc0.mod',resources)
            for i in range(1,5):
                for suffix in ('','f'):
                    name=f'ptire{i}{suffix}.mod'
                    before=gen.read_modl(importer.find_entry(race,name).read_bytes())
                    after=gen.read_modl(resources[f'npc{i}{suffix}.mod'][0])
                    self.assertEqual(len(before['parts']),len(after['parts']))
                    for a,b in zip(before['parts'],after['parts']):
                        for x,y in zip(gen.walk_vif(a['stream']),gen.walk_vif(b['stream'])):
                            self.assertEqual(gen.batch_positions(a['stream'],x),gen.batch_positions(b['stream'],y))
            table=parse_binary_valt(resources['npc.val'][0]);dim=next(dict(s['pairs']) for s in table['sections'] if s['name']=='dimensions')
            self.assertAlmostEqual(float(dim['width']),305/25.4)

class PreviewContracts(unittest.TestCase):
    def test_wheel_placement_uses_source_axle_tracks_and_wheelbase(self):
        from nascar_preview import wheel_centers
        physics={'Chassis':{'wheelbase':'110','ftrack':'60','rtrack':'62'},'Front':{'tire_diameter':'28'},'Rear':{'tire_diameter':'30'}}
        points=wheel_centers(physics)
        self.assertEqual(len(points),4)
        self.assertAlmostEqual(points[0][0],-60*.0254/2)
        self.assertAlmostEqual(points[0][1],28*.0254/2)
        self.assertAlmostEqual(points[0][2],110*.0254/2)
        self.assertAlmostEqual(points[2][0],-62*.0254/2)
        self.assertAlmostEqual(points[2][1],30*.0254/2)

class DistantMeshContract(unittest.TestCase):
    def test_source_modl_without_helpers_transcodes(self):
        import nascar_gen as gen
        import struct
        # A source rigid mesh need not carry any TJRM helpers (actual Low5/6).
        h=bytearray(0x58);struct.pack_into('<6I',h,0,1,0x58,0,0,50,0)
        ph=bytearray(0x38);ph[:4]=b'4SPS';ph[8:24]=b'body.tex'.ljust(16,b'\0')
        stream=struct.pack('<4I',0x14000026,0,0,0)
        model=dict(head=h,parts=[dict(hdr=ph,stream=stream,trailer=b'\xe3'*8)],tjrm=None,nrec=50)
        result=gen.read_modl(importer.transcode_modl(gen.write_modl(model)))
        self.assertIsNone(result['tjrm'])
        self.assertEqual(struct.unpack_from('<I',result['parts'][0]['stream'])[0]&0xffff,0x55)

@unittest.skipUnless(os.environ.get('NASCAR_BATCH_BUILD'), 'batch candidate staging not configured')
class BatchArtifactContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import json
        cls.root=Path(os.environ['NASCAR_BATCH_BUILD'])
        cls.package=cls.root/'package'
        cls.manifest=json.loads((cls.package/'roster-provenance.json').read_text())

    def test_all_real_source_paints_have_unique_ct_and_both_preview_crops(self):
        import json
        ids=[r['id'] for r in self.manifest['liveries']]
        self.assertEqual(len(ids),125);self.assertEqual(len(ids),len(set(ids)))
        for ar in ('CT','UICAR'):
            names={e['name'].lower() for e in json.loads((self.root/f'native-{ar}'/'manifest.json').read_text())['entries']}
            for ident in ids:
                required=[f'{ident}.{suffix}' for suffix in ('tex','tm0')] if ar=='CT' else [f'{p}_{ident}.{s}' for p in ('uicar','uicars') for s in ('tex','tm0')]
                self.assertTrue(set(required)<=names,(ar,ident))

    def test_every_family_preserves_all_source_physics_and_bone_values(self):
        from valt import parse_binary_valt
        from nascar_roster import FAMILIES,CHASSIS
        def semantic(path):return [(s['name'],dict(s['pairs'])) for s in parse_binary_valt(path.read_bytes())['sections']]
        for family,choices in CHASSIS.items():
            source=self.root/('source-'+FAMILIES[family]['archive'])
            for name in choices.values():
                car=self.root/f'car-{name}'
                actual=dict(semantic(importer.find_entry(car,'physics.val')))
                for section,values in semantic(importer.find_entry(source,'physics.val')):
                    for key,value in values.items():self.assertEqual(actual[section][key],value,(name,section,key))
                self.assertEqual(semantic(importer.find_entry(source,'bone.val')),semantic(car/'Bone.val'))
                mvl=dict(semantic(car/'Bone.mvl'))
                bone=parse_binary_valt((car/'Bone.val').read_bytes())
                self.assertEqual(bone['unk1'],len(bone['sections']))
                for p in car.glob('*.val'):self.assertFalse(parse_binary_valt(p.read_bytes())['is_v6'],(name,p.name))
                for section,values in semantic(importer.find_entry(source,'bone.val')):
                    if section.startswith('bone_'):self.assertEqual(mvl[section],values)

    def test_selected_family_parts_survive_all_lods_and_skin_indices_fit(self):
        import nascar_gen as gen
        from nascar_roster import FAMILIES,CHASSIS,remap_family_groups
        for family,choices in CHASSIS.items():
            for style,name in choices.items():
                for lod in range(7):
                    source_name=f'Pacecar{lod}.mod' if family=='pacecar' else f'Low{lod}.mod'
                    source=gen.read_modl(importer.find_entry(self.root/('source-'+FAMILIES[family]['archive']),source_name).read_bytes())
                    if lod:remap_family_groups(source,family,style)
                    actual=gen.read_modl((self.root/f'car-{name}'/f'Low{lod}.mod').read_bytes())
                    self.assertEqual(len(actual['parts']),len(source['parts']),(name,lod))
                    for part in actual['parts']:
                        for _,skin in gen.read_part_skin(part):
                            self.assertLessEqual(skin[0],20);self.assertLessEqual(skin[1],20)

    def test_native_record_prefixes_and_payloads_are_preserved(self):
        from rst0 import unpack_archive
        from valt import parse_binary_valt
        import tempfile,hashlib
        disc=Path(os.environ['EOD_DISC_DATA'])
        with tempfile.TemporaryDirectory() as tmp:
            for ar in ('COMMON','CT','UICAR','RACE'):
                before=Path(tmp)/ar;unpack_archive(str(disc/'RES'/f'{ar}.RES'),str(before))
                for file in before.iterdir():
                    if file.name=='manifest.json':continue
                    actual=importer.find_entry(self.root/f'native-{ar}',file.name)
                    if ar=='COMMON' and file.name in ('dirt.val','buycar.val','car_stats.val','carclass.val'):
                        a=parse_binary_valt(file.read_bytes())['sections'];b=parse_binary_valt(actual.read_bytes())['sections']
                        for x,y in zip(a,b):
                            self.assertEqual(x['name'],y['name'])
                            self.assertEqual(x['pairs'],y['pairs'][:len(x['pairs'])])
                    else:self.assertEqual(file.read_bytes(),actual.read_bytes(),(ar,file.name))

    def test_native_prototype_registry_obeys_runtime_number_and_carousel_bounds(self):
        from valt import parse_binary_valt
        from collections import Counter
        rows=[]
        for name in ('dirt.val','buycar.val'):
            rows += [dict(s['pairs']) for s in parse_binary_valt((self.root/'native-COMMON'/name).read_bytes())['sections'] if 'number' in dict(s['pairs'])]
        numbers=[r['number'] for r in rows]
        self.assertEqual(len(numbers),len(set(numbers))) # native129b30 duplicate panic
        self.assertTrue(all(abs(int(n))<1000 for n in numbers)) # native1294b4
        counts=Counter(r['chassis'].lower() for r in rows)
        for family in self.manifest['families'].values():
            for chassis in family:self.assertLessEqual(counts[chassis.lower()],10) #2a7dac
        classes=parse_binary_valt((self.root/'native-COMMON/carclass.val').read_bytes())
        for section in classes['sections']:
            if section['name'].startswith('$'):self.assertLessEqual(len(section['pairs']),20)
        self.assertEqual(len(self.manifest['prototype_selector']['exposed_driver_ids']),36)
        self.assertEqual(len(self.manifest['dealer_aliases']),35)
        self.assertEqual(len(self.manifest['liveries']),125) #full pool remains intact

    def test_source_based_previews_are_distinct_and_nonempty(self):
        import json
        from PIL import Image
        images=self.manifest['previews']['images']
        self.assertEqual(len(images),125)
        self.assertEqual(len({i['sha256'] for i in images.values()}),125)
        for ident,info in images.items():
            with Image.open(info['png']) as image:
                self.assertEqual(image.size,(256,256))
            self.assertGreater(info['foreground_pixels'],1200)
            self.assertLess(info['foreground_pixels'],256*256*.8)

class ModlHeaderContracts(unittest.TestCase):
    def test_helper_lookup_uses_part_table_extent_not_runtime_record_count(self):
        import nascar_gen as gen
        import struct
        h=bytearray(0x58);struct.pack_into('<6I',h,0,1,0x58,1,0,1,0)
        ph=bytearray(0x38);ph[:4]=b'4SPS';ph[8:24]=b'body.tex'.ljust(16,b'\0')
        model=dict(head=h,parts=[dict(hdr=ph,stream=struct.pack('<4I',0x14000026,0,0,0),trailer=b'\xe3'*8)],tjrm=gen.build_tjrm([('test',gen.IDENTITY,(0,0,0))]),nrec=1)
        out=gen.read_modl(importer.transcode_modl(gen.write_modl(model)))
        self.assertEqual(len(out['parts']),1)
        self.assertEqual(gen.tjrm_records(out['tjrm'])[0][0],'test')

class MaterialTableContracts(unittest.TestCase):
    def test_retexture_reaches_parts_beyond_runtime_record_count(self):
        import nascar_gen as gen
        from modl import retexture_modl,parse_modl
        import struct
        h=bytearray(0x58);struct.pack_into('<6I',h,0,2,0x58,0,0,1,0)
        parts=[]
        for name in ('first.tex','late.tex'):
            ph=bytearray(0x38);ph[:4]=b'4SPS';ph[8:24]=name.encode().ljust(16,b'\0')
            parts.append(dict(hdr=ph,stream=struct.pack('<4I',0x14000026,0,0,0),trailer=b'\xe3'*8))
        blob=gen.write_modl(dict(head=h,parts=parts,tjrm=None,nrec=1))
        changed=retexture_modl(blob,'late.tex','new.tex')
        self.assertEqual(gen.part_texture(gen.read_modl(changed)['parts'][1]),'new.tex')
        self.assertEqual(len([p for p in parse_modl(blob)['parts'] if p.get('magic')=='4SPS']),2)

class SourceSetupContracts(unittest.TestCase):
    def test_gears_and_final_drive_use_proven_gar_offsets_without_multipliers(self):
        from nascar_roster import adapt_source_transmission
        import struct
        data=bytearray(196);struct.pack_into('<4f',data,0x78,3.2,2.2,1.65,1.1);struct.pack_into('<f',data,0x94,6)
        target={'sections':[{'name':'Transmission','pairs':[('num_gears','4'),('g1','9'),('g2','8'),('g3','7'),('g4','6'),('rear_end_ratio','7'),('ratio_factor','1.13'),('ratio','3.73')]}]}
        out=adapt_source_transmission(target,bytes(data))
        values=dict(out['sections'][0]['pairs'])
        for i,expected in enumerate((3.2,2.2,1.65,1.1),1):self.assertAlmostEqual(float(values[f'g{i}']),expected,places=6)
        self.assertEqual(float(values['rear_end_ratio']),6)
        self.assertNotIn('ratio_factor',values)
        self.assertEqual(values['ratio'],'3.73')

    def test_internal_id_collisions_fail_before_mutating_native_archives(self):
        from nascar_roster import allocate_liveries
        from nascar_roster import LIVERY_IDS
        ident=str(LIVERY_IDS['stock:24'])
        with self.assertRaisesRegex(ValueError,ident):
            allocate_liveries([{'family':'stock','source_number':'24'}],used_ids={ident})

class NativeRegistryBounds(unittest.TestCase):
    def test_every_pinned_driver_id_fits_native_number_and_menu_filename_bounds(self):
        from nascar_roster import LIVERY_IDS
        values=list(LIVERY_IDS.values())
        self.assertEqual(len(values),125)
        self.assertEqual(len(values),len(set(values)))
        for value in values:
            self.assertTrue(650<=value<800)
            self.assertLess(abs(value),1000) # native1294b4 and1297f8
            self.assertLess(len(f'uicars_{value}.tex'),16)



class NativeValtAccessibility(unittest.TestCase):
    def test_source_tables_expose_all_root_sections_after_native_conversion(self):
        from nascar_roster import native_source_valt
        from valt import compile_binary_valt,parse_binary_valt
        source={'is_v6':True,'unk1':0,'orig_sec_count':2,'sections':[{'name':n,'parent':'','pairs':[('weakness','7')],'sec_vtable':4235492,'tool_ptr':0} for n in ('bone_1','bone_2')]}
        native={'tool_vtable':4243992,'sections':[{'sec_vtable':4243672}]}
        actual=parse_binary_valt(compile_binary_valt(native_source_valt(source,native)))
        self.assertFalse(actual['is_v6'])
        self.assertEqual(actual['unk1'],2)
        self.assertEqual([s['name'] for s in actual['sections']],['bone_1','bone_2'])

if __name__=="__main__":unittest.main()
