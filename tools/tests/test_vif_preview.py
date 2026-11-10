"""Model preview regressions: native shadow stride, topology and real MODL census."""
import math
import os
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
import vif_mesh


def command(code, count=0, address=0, payload=b''):
    return struct.pack('<I',(code<<24)|(count<<16)|address)+payload+b'\0'*((-len(payload))%4)


def shadow_part():
    # Native shadow entry MSCAL4 reads normals at1+2*i and positions at2+2*i.
    # Body entry MSCAL0x5c instead reads stride3 and has UV/skin attributes.
    positions=((0,0,0),(1,0,0),(0,1,0),(0,0,1),(1,0,1),(0,1,1))
    stream=command(0x60,1,0xc000,struct.pack('<I',0x8006))
    stream+=command(1,address=0x0102)
    stream+=command(0x20,payload=struct.pack('<I',0x40404040))
    stream+=command(0x30,payload=struct.pack('<4f',0,0,0,1))
    stream+=command(0x78,6,0xc002,b''.join(struct.pack('<3f',*p) for p in positions))
    normals=b''.join(bytes((128,128,1 if i<3 else 255,128 if i in (0,1,3,4) else 0)) for i in range(6))
    stream+=command(0x6e,6,0xc001,normals)
    stream+=command(1,address=0x0101)
    stream+=command(0x6c,2,0xc0c6,struct.pack('<4I4f',0,0,0,0,10,20,30,0))
    stream+=command(0x14,address=4)
    data=bytearray(56)+stream
    data[:4]=b'4SPS'
    struct.pack_into('<I',data,4,len(data))
    struct.pack_into('<I',data,28,0x001f0007)
    return bytes(data),dict(offset=0,size=len(data))


def retail_models():
    root=Path(os.environ.get('EOD_DISC_DATA',str(Path.home()/'Library/Application Support/TDEODRecomp/disc/DATA')))
    for path in sorted(root.rglob('*')):
        if path.suffix.upper() not in ('.CAR','.RES','.TRK'):
            continue
        with path.open('rb') as f:
            header=f.read(40)
            directory,count=struct.unpack_from('<II',header,20)
            f.seek(directory)
            rows=f.read(count*68)
            for i in range(count):
                row=rows[i*68:(i+1)*68]
                if row[32:36]!=b'LDOM':
                    continue
                name=row[:32].split(b'\0')[0].decode('latin1')
                _,size,offset,_=struct.unpack_from('<4I',row,36)
                f.seek(directory+offset)
                yield path.name,name,f.read(size)


class VifPreviewTests(unittest.TestCase):
    def test_shadow_two_word_stride_keeps_positions_normals_and_adc(self):
        data,part=shadow_part()
        batch=vif_mesh.decode_part(data,part)[0]
        self.assertEqual(len(batch['verts']),6)
        self.assertEqual([v['p'] for v in batch['verts']],
            [(10,20,30),(11,20,30),(10,21,30),(10,20,31),(11,20,31),(10,21,31)])
        self.assertEqual([v['adc'] for v in batch['verts']],[True,True,False,True,True,False])
        self.assertEqual(batch['verts'][0]['n'],(0,0,-1))
        self.assertTrue(all(v['uv']==(0,0) and v['skin'] is None for v in batch['verts']))

    def test_shadow_winding_keeps_global_strip_parity_and_inward_stored_normals(self):
        data,part=shadow_part()
        batch=vif_mesh.decode_part(data,part)[0]
        self.assertEqual(vif_mesh.triangulate_batch(batch),[(0,1,2),(4,3,5)])

    def test_body_winding_still_matches_existing_contract(self):
        vertices=[dict(p=p,n=(0,0,1),adc=i<2) for i,p in enumerate(((0,0,0),(0,1,0),(1,0,0)))]
        batch=dict(mscal=0x5c,verts=vertices)
        self.assertEqual(vif_mesh.triangulate_batch(batch),[(0,2,1)])

    @unittest.skipUnless(os.environ.get('EOD_TEST_RETAIL_MODELS')=='1','Opt in to owner-supplied MODL resources')
    def test_desio_all_three_shadows_have_real_geometry(self):
        expected={'shadow1.mod':(249,3,64),'shadow2.mod':(112,2,28),'shadow3.mod':(19,1,4)}
        seen=set()
        for archive,name,data in retail_models():
            if archive!='DESIO.CAR' or name not in expected:
                continue
            mesh=vif_mesh.parse_modl(data)
            batches=[b for p in mesh['parts'] for b in vif_mesh.decode_part(data,p)]
            faces=[]
            for b in batches:
                for a,c,d in vif_mesh.triangulate_batch(b):
                    pa,pb,pc=[b['verts'][i]['p'] for i in (a,c,d)]
                    e=[pb[k]-pa[k] for k in range(3)]
                    f=[pc[k]-pa[k] for k in range(3)]
                    cross=(e[1]*f[2]-e[2]*f[1],e[2]*f[0]-e[0]*f[2],e[0]*f[1]-e[1]*f[0])
                    if sum(v*v for v in cross)>1e-20:
                        # Shadow records deliberately store inward normals;
                        # forcing their face winding to match corrupts parity.
                        self.assertLess(sum(cross[k]*b['verts'][d]['n'][k] for k in range(3)),0)
                        faces.append((a,c,d))
            self.assertEqual((sum(len(b['verts']) for b in batches),len(batches),len(faces)),expected[name])
            self.assertTrue(all(v['skin'] is None for b in batches for v in b['verts']))
            seen.add(name)
        self.assertEqual(seen,set(expected))

    @unittest.skipUnless(os.environ.get('EOD_TEST_RETAIL_MODELS')=='1','Opt in to owner-supplied MODL resources')
    def test_entire_490_model_census_decodes_finite_geometry(self):
        count=0
        for archive,name,data in retail_models():
            with self.subTest(archive=archive,name=name):
                mesh=vif_mesh.parse_modl(data)
                total=0
                for part in mesh['parts']:
                    requested=[]
                    vif_mesh.run_vif(data,part['offset']+56,part['offset']+part['size'],
                        lambda st:requested.append(st.mem.get(0,[0])[0]&0x7fff))
                    decoded=vif_mesh.decode_part(data,part)
                    self.assertEqual([len(batch['verts']) for batch in decoded],requested)
                    for batch in decoded:
                        total+=len(batch['verts'])
                        self.assertTrue(all(math.isfinite(x) for v in batch['verts'] for x in v['p']))
                        self.assertTrue(all(0<=i<len(batch['verts']) for tri in vif_mesh.triangulate_batch(batch) for i in tri))
                self.assertGreater(total,0)
            count+=1
        self.assertEqual(count,490)


if __name__=='__main__':
    unittest.main()
