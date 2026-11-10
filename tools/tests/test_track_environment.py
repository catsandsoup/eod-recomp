"""Native environmental editing and track selection contracts."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_modding_backend import ModdingBackendTest
import unittest,shutil,json,contextlib,io,hashlib,struct
from test_texture_preview import fixture
from rst0 import unpack_archive, pack_archive
from tex import write_png, read_png

class TrackEnvironmentTest(unittest.TestCase):
    def setUp(self):
        ModdingBackendTest.setUp(self)
        self.track=self.disc/'DATA/TRACKS/ANOKA.TRK';self.track.parent.mkdir(parents=True)
        shutil.copyfile(self.archive,self.track)
    def track_request(self,action,**kwargs):
        return self.backend.dispatch(dict(action=action,disc=str(self.disc),project=str(self.project),archive='DATA/TRACKS/ANOKA.TRK',asset='physics.val',**kwargs))
    def test_saved_track_selects_that_track_in_native_launch(self):
        self.track_request('save',edits=[dict(section='Chassis',key='mass',value='4200')])
        exe=self.root/'eod';exe.write_bytes(b'game');exe.chmod(0o755)
        plan=self.track_request('launch_plan',executable=str(exe),headless=True,frames=300)
        self.assertEqual(plan['track'],'Anoka')
        self.assertEqual(plan['environment']['EOD_EDITOR_TEST'],'1')
        self.assertIn('track=Anoka;',plan['environment']['EOD_DIRECT_START'])
    def test_environment_inventory_is_native_not_sample_data(self):
        result=self.track_request('environment')
        self.assertEqual(result['track'],'Anoka')
        self.assertEqual(result['textures'],[])
        self.assertFalse(result['worldGeometryEditable'])

    def add_sky(self):
        stage=self.root/'sky-stage'
        with contextlib.redirect_stdout(io.StringIO()):unpack_archive(str(self.track),str(stage))
        manifest=json.loads((stage/'manifest.json').read_text())
        for name in ['sky01A.tex','sky01B.tex']:
            (stage/name).write_bytes(fixture(128,128,0x201))
            manifest['entries'].append(dict(name=name,fourcc='TEX ',version=10,is_extractable=False))
        (stage/'manifest.json').write_text(json.dumps(manifest))
        with contextlib.redirect_stdout(io.StringIO()):pack_archive(str(stage),str(self.track))
    def test_texture_preview_save_reopen_and_png_replacement_preserve_other_assets(self):
        self.add_sky()
        before=self.track.read_bytes()
        req=dict(disc=str(self.disc),project=str(self.project),archive='DATA/TRACKS/ANOKA.TRK',asset='sky01A.tex')
        call=lambda action,**kw:self.backend.dispatch(dict(req,action=action,**kw))
        prepared=call('prepare_texture',allSky=True,tint=[2,0.5,3])
        self.assertEqual(prepared['count'],2)
        self.assertEqual(self.track.read_bytes(),before)
        saved=call('save',textureEdits=prepared['edits'])
        call('validate')
        original,_=self.backend.payload(self.track,'physics.val')
        built,_=self.backend.payload(Path(saved['archivePath']),'physics.val')
        self.assertEqual(original,built)
        inspected=call('inspect')
        self.assertEqual(read_png(inspected['image'])[2],read_png(prepared['image'])[2])
        png=self.root/'export.png';call('export_texture',destination=str(png))
        write_png(str(png),128,128,bytes((30,70,120,255))*(128*128))
        replacement=call('prepare_texture',png=str(png))
        call('save',textureEdits=replacement['edits'])
        png.unlink()
        call('load');call('validate')
        pixels=read_png(call('inspect')['image'])[2]
        self.assertEqual(pixels,bytes((30,70,120,255))*(128*128))
        self.assertEqual(self.track.read_bytes(),before)
        document=json.loads(self.project.read_text())
        document['archives'][req['archive']]['assets']['sky01A.tex']['sha256']='bad'
        self.project.write_text(json.dumps(document))
        with self.assertRaises(ValueError):call('validate')

    def test_texture_edit_preserves_preexisting_unedited_crc_mismatch(self):
        self.add_sky()
        entries=self.backend.directory(self.track)
        index=next(i for i,e in enumerate(entries) if e['name']=='untouched.bin')
        source=bytearray(self.track.read_bytes());directory=struct.unpack_from('<I',source,20)[0]
        struct.pack_into('<I',source,directory+index*68+48,0x12345678)
        self.track.write_bytes(source)
        req=dict(disc=str(self.disc),project=str(self.project),archive='DATA/TRACKS/ANOKA.TRK',asset='sky01A.tex')
        prepared=self.backend.dispatch(dict(req,action='prepare_texture',tint=[2,1,1]))
        built=self.backend.dispatch(dict(req,action='save',textureEdits=prepared['edits'],asset='untouched.bin'))
        document=self.backend.dispatch(dict(req,action='load'))
        self.assertEqual(document['selectedAsset'],'sky01A.tex')
        result=Path(built['archivePath']).read_bytes()
        self.assertEqual(result[directory+index*68:directory+(index+1)*68],source[directory+index*68:directory+(index+1)*68])
        self.assertEqual(built['sourceCrcWarnings'],['untouched.bin'])
        self.backend.dispatch(dict(req,action='validate'))

if __name__=='__main__':unittest.main()
