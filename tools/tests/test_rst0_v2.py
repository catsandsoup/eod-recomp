"""NASCAR v2 sizes exclude the leading !IGM container marker."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from rst0 import unpack_archive


def archive(entries):
    directory=bytearray();body=bytearray()
    start=24+56*len(entries)
    for name,payload in entries:
        nb=name.encode();directory+=nb+b'\0'*(36-len(nb))+b' T X'+struct.pack('<II',9,len(payload))+b'\0'*8
        body+=b'!IGM'+payload+b'\0'*12
    total=start+len(body)
    return b'0TSR'+struct.pack('<5I',2,0,total,123,len(entries))+directory+body


class Rst0V2Tests(unittest.TestCase):
    def extract(self,data):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        root=Path(self.tmp.name);src=root/'source.res';src.write_bytes(data)
        unpack_archive(str(src),str(root/'out'))
        return root/'out'

    def test_payload_keeps_its_final_four_bytes(self):
        payload=b'header-and-pixels-TAIL'
        out=self.extract(archive([('sample.tex',payload)]))
        self.assertEqual((out/'sample.tex').read_bytes(),payload)
        self.assertEqual(json.loads((out/'manifest.json').read_text())['entries'][0]['size'],len(payload))

    def test_payload_marker_at_end_is_not_next_resource(self):
        out=self.extract(archive([('first.tex',b'first-payload!IGM'),('second.tex',b'second-payload-TAIL')]))
        self.assertEqual((out/'second.tex').read_bytes(),b'second-payload-TAIL')

    def test_truncated_payload_is_rejected(self):
        raw=archive([('sample.tex',b'payload-TAIL')])
        with self.assertRaisesRegex(ValueError,'Truncated'):
            self.extract(raw[:-16])

if __name__=='__main__':unittest.main()
