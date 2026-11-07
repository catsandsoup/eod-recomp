"""A repacked 0TSR v6 archive keeps streamed (extractable) entries on 2048-byte file boundaries.

The game reads those entries through its resource stream (whole sectors, indexed from the sector start), so
retail archives place every one on a sector. A tight 0x80 relayout once shipped them misaligned; the symptom was
a Career Battle ending in "MemFree: bad cookie" (a spectator animation decoded garbage and overran its block).
"""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from rst0 import pack_archive,unpack_archive


def entry(name,fourcc,size,extractable):
    return dict(name=name,fourcc=fourcc,version=1,size=size,offset=0,is_extractable=extractable,crc=0)


class Rst0AlignmentTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name)

    def pack(self,entries):
        src=self.root/'src';src.mkdir()
        payloads={}
        for i,e in enumerate(entries):
            payloads[e['name']]=bytes((i*37+k)&255 for k in range(e['size']))
            (src/e['name']).write_bytes(payloads[e['name']])
        (src/'manifest.json').write_text(json.dumps(dict(version=6,build_time=1,unk=0,extract_block=0,entries=entries)))
        out=self.root/'out.res';pack_archive(str(src),str(out))
        return out,payloads

    def directory(self,data):
        hdr_size,version,total,build,dir_offset,count=struct.unpack_from('<6I',data,4)
        rows={}
        for i in range(count):
            raw=data[dir_offset+i*0x44:dir_offset+(i+1)*0x44]
            name=raw[:32].split(b'\0')[0].decode()
            version_,size,offset,crc=struct.unpack_from('<IIII',raw,0x24)
            rows[name]=(dir_offset+offset,size)
        return rows

    def test_streamed_entries_start_on_a_sector_and_keep_their_bytes(self):
        entries=[entry('first.tex','TEX',301,False),entry('second.mod','MOD',1000,False),
                 entry('tail1.tm0','TM0',3001,True),entry('tail2.an0','AN0',5000,True),entry('tail3.tm0','TM0',17,True)]
        out,payloads=self.pack(entries)
        data=out.read_bytes();rows=self.directory(data)
        for name in ('tail1.tm0','tail2.an0','tail3.tm0'):
            offset,size=rows[name]
            self.assertEqual(offset%2048,0,name)
            self.assertEqual(data[offset:offset+size],payloads[name])
        for name in ('first.tex','second.mod'):  # resident entries keep the compact 0x80 layout
            offset,size=rows[name]
            self.assertEqual(offset%0x80,0,name);self.assertEqual(data[offset:offset+size],payloads[name])
        ends=sorted((o,o+s) for o,s in rows.values())
        for (a0,a1),(b0,b1) in zip(ends,ends[1:]):self.assertLessEqual(a1,b0,'entries overlap')

    def test_round_trip_through_unpack(self):
        entries=[entry('a.tex','TEX',64,False),entry('b.an0','AN0',4096,True),entry('c.an0','AN0',4097,True)]
        out,payloads=self.pack(entries)
        un=self.root/'un';unpack_archive(str(out),str(un))
        for name,payload in payloads.items():self.assertEqual((un/name).read_bytes(),payload)


if __name__=='__main__':
    unittest.main()
