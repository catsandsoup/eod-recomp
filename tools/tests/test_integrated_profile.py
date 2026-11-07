"""Deterministic integrated profile tests use authored records/assets, not retail data."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from rst0 import pack_archive,unpack_archive
from valt import compile_binary_valt,parse_binary_valt
from build_integrated_profile import build_integrated_profile,merge_valt_records
from build_gameplay_mod import battle_catalog


def sec(section_name,**pairs):return dict(name=section_name,parent='',pairs=list(pairs.items()),sec_vtable=0x40c0d8,tool_ptr=0)
def table(sections,roots=None):return dict(sections=sections,tool_vtable=0x40c218,orig_sec_count=len(sections),unk1=len(sections)if roots is None else roots)
def pairs(model):return [(s['name'],s['parent'],s['pairs'])for s in model['sections']]


class IntegratedProfileTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name)
        self.disc=self.root/'disc';self.mods={n:self.root/n for n in ('phantom','battle','cupcar')}
        native=[sec('Info',car_len='5.2')]+[sec('Driver',number=n,chassis='Hurricane',name='native'+n,group_1='20')for n in ('199','296')]
        self.models={'dirt.val':table(native),'car_stats.val':table([sec('carstats',chassis='Hurricane',speed='7')]),
            'buycar.val':table([sec('car',chassis='Hurricane',price='100')]),
            'carclass.val':table([sec('Midsize',**{'@chassis':'$0001$chassis'}),sec('$0001$chassis',Hurricane='')],1)}
        common={k:compile_binary_valt(v)for k,v in self.models.items()};common['events.val']=b'native-events'
        events=table([sec('eve',**{'@contest':'race'}),sec('eve',mode='multi',**{'@contest':'derby'}),
            sec('eve',mode='career',**{'@contest':'challenge'}),sec('race',event_type='normal_race'),
            sec('derby',event_type='demolition_derby'),sec('challenge',event_type='jump')],3)
        self.native_events=compile_binary_valt(events);self.battle_events=compile_binary_valt(battle_catalog(events,'Afton'))
        common['afton_eves.val']=self.native_events;common['career_eves.val']=self.native_events
        self.archive(self.disc,'COMMON',common)
        self.archive(self.disc,'CT',{n+ext:('native-'+n+ext).encode()for n in ('199','296')for ext in ('.tex','.tm0')})
        self.archive(self.disc,'UICAR',{'uicar_'+n+ext:('native-ui-'+n+ext).encode()for n in ('199','296')for ext in ('.tex','.tm0')})
        self.archive(self.disc,'RACE',{'native-resource':b'native-race'})
        for label,chassis in [('phantom','Phantom'),('cupcar','Cupcar')]:
            models=copy.deepcopy(self.models)
            models['car_stats.val']['sections'].append(sec('carstats',chassis=chassis,speed='9'))
            models['carclass.val']['sections'][1]['pairs'].append((chassis,''))
            models['buycar.val']['sections'] += [sec('car',chassis=chassis,price='1200'),sec('vehicle',chassis=chassis,number='778',group_1='777')]
            for s in models['dirt.val']['sections'][1:]:s['pairs']=[(k,chassis if k=='chassis'else'777'if k=='group_1'else v)for k,v in s['pairs']]
            models['dirt.val']['sections'].append(sec('Driver',chassis=chassis,number='777',name=chassis+' Racer',group_1='777'))
            self.archive(self.mods[label],'COMMON',dict(common,**{k:compile_binary_valt(v)for k,v in models.items()}))
            ct={n+ext:('native-'+n+ext).encode()for n in ('199','296')for ext in ('.tex','.tm0')}
            for ext in ('.tex','.tm0'):
                ct['777'+ext]=(label+'-paint'+ext).encode()
                if label=='cupcar':
                    ct['778'+ext]=ct['777'+ext];ct['296'+ext]=('marathon'+ext).encode();ct['199'+ext]=ct['777'+ext]
            self.archive(self.mods[label],'CT',ct)
            self.archive(self.mods[label],'UICAR',{'uicar_'+n+ext:(label+'-ui-'+n+ext).encode()for n in ('199','296','777','778')for ext in ('.tex','.tm0')})
            car=self.mods[label]/'DATA/CAR'/(chassis.upper()+'.CAR');car.parent.mkdir(parents=True);car.write_bytes((label+'body').encode())
        self.archive(self.mods['battle'],'COMMON',dict(common,**{'afton_eves.val':self.battle_events}))
        self.archive(self.mods['cupcar'],'RACE',{'native-resource':b'native-race','ncu0.mod':b'original-source-wheel'})
        self.track(self.disc,{'track.val':b'native-course','ground.tex':b'native-ground'})
        self.track(self.mods['battle'],{'track.val':b'native-course','ground.tex':b'native-ground','c_battle.va0':b'battle-respawns'})

    def track(self,root,entries):
        src=self.root/(root.name+'-WELCH-src');src.mkdir(exist_ok=True)
        manifest=dict(version=6,build_time=123,unk=0,entries=[])
        for key,raw in entries.items():
            (src/key).write_bytes(raw);manifest['entries'].append(dict(name=key,fourcc='VALT',version=11,is_extractable=False))
        (src/'manifest.json').write_text(json.dumps(manifest));(root/'DATA/TRACKS').mkdir(parents=True,exist_ok=True)
        pack_archive(str(src),str(root/'DATA/TRACKS/WELCH.TRK'))

    def archive(self,root,name,entries):
        src=self.root/(root.name+'-'+name+'-src');src.mkdir(exist_ok=True)
        manifest=dict(version=6,build_time=123,unk=0,entries=[])
        for key,raw in entries.items():
            (src/key).write_bytes(raw);manifest['entries'].append(dict(name=key,fourcc='VALT'if key in self.models else'TEX0',version=11,is_extractable=False))
        (src/'manifest.json').write_text(json.dumps(manifest));pack_archive(str(src),str(root/'DATA/RES'/(name+'.RES')))

    def entries(self,output,name):
        dst=self.root/('inspect-'+name+'-'+output.name);unpack_archive(str(output/'DATA/RES'/(name+'.RES')),str(dst))
        m=json.loads((dst/'manifest.json').read_text());return {e['name'].lower():(dst/e['name']).read_bytes()for e in m['entries']}

    def test_owned_id_migration_restores_native_and_preserves_every_system(self):
        out=self.root/'integrated';report=build_integrated_profile(self.disc,self.mods,out)
        common=self.entries(out,'COMMON');drivers=parse_binary_valt(common['dirt.val'])
        keyed={dict(s['pairs'])['number']:dict(s['pairs'])for s in drivers['sections']if s['name']=='Driver'}
        self.assertEqual(keyed['199']['chassis'],'Hurricane');self.assertEqual(keyed['296']['chassis'],'Hurricane')
        self.assertEqual({n:keyed[n]['chassis']for n in ('775','777','779')},{'775':'Phantom','777':'Cupcar','779':'Cupcar'})
        self.assertTrue(all('group_1'not in keyed[n]for n in ('775','777','779')))
        self.assertEqual(common['events.val'],b'native-events')
        self.assertEqual((out/'DATA/CAR/PHANTOM.CAR').read_bytes(),b'phantombody')
        self.assertEqual((out/'DATA/CAR/CUPCAR.CAR').read_bytes(),b'cupcarbody')
        self.assertFalse((out/'DATA/TRACKS').exists(),'native track archives must not be replaced in the active overlay')
        self.assertEqual((out/'battle-alternates/TRACKS/WELCH/c_battle.va0').read_bytes(),b'battle-respawns')
        self.assertEqual(sorted(p.relative_to(out/'battle-alternates').as_posix()for p in (out/'battle-alternates').rglob('*')if p.is_file()),
                         ['TRACKS/WELCH/c_battle.va0'],'only the added Battle table is kept, never a whole track archive')
        ct=self.entries(out,'CT');self.assertEqual(ct['296.tex'],b'native-296.tex');self.assertEqual(ct['199.tex'],b'native-199.tex')
        self.assertEqual(ct['775.tex'],b'phantom-paint.tex');self.assertEqual(ct['776.tex'],ct['775.tex'])
        self.assertEqual(ct['777.tex'],b'cupcar-paint.tex');self.assertEqual(ct['779.tex'],b'marathon.tex')
        ui=self.entries(out,'UICAR');self.assertEqual(ui['uicar_296.tex'],b'native-ui-296.tex')
        self.assertEqual(ui['uicar_775.tex'],b'phantom-ui-777.tex');self.assertEqual(ui['uicar_779.tex'],b'cupcar-ui-296.tex')
        self.assertEqual(self.entries(out,'RACE')['ncu0.mod'],b'original-source-wheel')
        self.assertEqual(report['registry'],{'phantom_driver':775,'phantom_dealer':776,'cup24_driver':777,'cup24_dealer':778,'cup_marathon_driver':779})

    def test_native_action_multiplayer_career_catalogs_not_replaced_by_battle(self):
        out=self.root/'event-check';report=build_integrated_profile(self.disc,self.mods,out)
        common=self.entries(out,'COMMON')
        self.assertEqual(common['afton_eves.val'],self.native_events)
        self.assertEqual(common['b_afton_eves.val'],self.battle_events)
        self.assertEqual(common['career_eves.val'],self.native_events)
        self.assertFalse('b_career_eves.val'in common)
        native=parse_binary_valt(common['afton_eves.val'])
        self.assertEqual([dict(s['pairs']).get('mode','')for s in native['sections'][:native['unk1']]],['','multi','career'])
        self.assertFalse(report['runtime_activated'])
        self.assertNotIn('Battle session catalog lifecycle',report['acceptance_blockers'])

    def test_deterministic_bytes_and_no_input_mutation(self):
        files=[p for r in (self.disc,*self.mods.values())for p in r.rglob('*')if p.is_file()]
        before={p:hashlib.sha256(p.read_bytes()).hexdigest()for p in files}
        a,b=self.root/'a',self.root/'b'
        build_integrated_profile(self.disc,self.mods,a);build_integrated_profile(self.disc,dict(reversed(list(self.mods.items()))),b)
        hashes=lambda r:{str(p.relative_to(r)):hashlib.sha256(p.read_bytes()).hexdigest()for p in(r/'DATA').rglob('*')if p.is_file()}
        self.assertEqual(hashes(a),hashes(b));self.assertEqual(before,{p:hashlib.sha256(p.read_bytes()).hexdigest()for p in files})

    def test_missing_new_paint_cannot_publish_incomplete_profile(self):
        source=self.mods['cupcar']/'DATA/RES/CT.RES'
        entries=self.entries(self.mods['cupcar'],'CT');entries.pop('777.tm0')
        self.archive(self.mods['cupcar'],'CT',entries)
        out=self.root/'missing-art'
        with self.assertRaisesRegex(ValueError,'paint asset'):build_integrated_profile(self.disc,self.mods,out)
        self.assertFalse(out.exists())

    def test_capacity_counts_all_same_chassis_records_not_only_registry_ids(self):
        common=self.entries(self.mods['cupcar'],'COMMON')
        model=parse_binary_valt(common['dirt.val'])
        model['sections'] += [sec('Driver',chassis='Cupcar',number=str(n),name='extra')for n in range(780,790)]
        common['dirt.val']=compile_binary_valt(model);self.archive(self.mods['cupcar'],'COMMON',common)
        out=self.root/'overflow'
        with self.assertRaisesRegex(ValueError,'paint capacity'):build_integrated_profile(self.disc,self.mods,out)
        self.assertFalse(out.exists())

    def test_battle_track_archive_must_only_add_layout_tables(self):
        for entries,message in [({'track.val':b'changed','ground.tex':b'native-ground','c_battle.va0':b'x'},'modifies native'),
                                ({'track.val':b'native-course','c_battle.va0':b'x'},'drops native'),
                                ({'track.val':b'native-course','ground.tex':b'native-ground','extra.tex':b'x'},'non-layout')]:
            self.track(self.mods['battle'],entries);out=self.root/('bad-'+message.replace(' ','-'))
            with self.assertRaisesRegex(ValueError,message):build_integrated_profile(self.disc,self.mods,out)
            self.assertFalse(out.exists())

    def test_unknown_same_record_conflict_is_rejected(self):
        base=table([sec('carstats',chassis='Native',speed='7')]);a=copy.deepcopy(base);b=copy.deepcopy(base)
        a['sections'][0]['pairs'][-1]=('speed','8');b['sections'][0]['pairs'][-1]=('speed','9')
        with self.assertRaisesRegex(ValueError,'Conflicting'):merge_valt_records('car_stats.val',base,{'a':a,'b':b})

if __name__=='__main__':unittest.main()
