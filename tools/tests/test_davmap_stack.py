"""Execute the recompiler's DAVMap scratch-frame layout against the overflow trigger."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
try:
    from recomp import Gen,FuncEmitter
except ImportError:
    Gen=FuncEmitter=None

@unittest.skipUnless(Gen and shutil.which('cc'),'rabbitizer and C compiler required')
class DavmapScratchFrame(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='davmap-frame-test-')
        cls.addClassCleanup(cls.tmp.cleanup)
        # Same frame/array/spill layout as native12A700. The loop reproduces its
        # dynamic used-vehicle clear; it must not erase the spilled output pointer.
        words=[0x27bdeff0,0xafa40f28,0xafbf1000,0x3c080044,0x8d089704,
               0x27a90a00,0x00085080,0x01495021,0xad200000,0x25290004,
               0x152afffd,0,0x8fa20f28,0x8fbf1000,0x03e00008,0x27bd1010]
        start=0x12a700
        class Elf:
            def read32(self,address):return words[(address-start)//4]
        gen=Gen(Elf(),{start},{},{})
        body='\n'.join(FuncEmitter(gen,start,list(range(start,start+len(words)*4,4)),'test').emit())
        # The source loop and outputs are test data, not a duplicate implementation
        # of the relocation: all frame/slot addressing comes from production emit.
        source='''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef union { uint64_t ud[2]; int64_t sd[2]; uint32_t uw[4]; int32_t sw[4]; } Reg;
Reg R[32]; uint8_t ram[8*1024*1024];
#define MEM_LW(a) (*(uint32_t*)(ram+(uint32_t)(a)))
#define MEM_SW(a,v) (MEM_LW(a)=(uint32_t)(v))
#define IRQ_CHECK() ((void)0)
static void run(void) {\n'''+body+'''\n}
int main(int argc,char **argv) {
 unsigned count=atoi(argv[1]); MEM_SW(0x439704,count);
 R[29].uw[0]=0x700000;R[31].uw[0]=0x123456;R[4].uw[0]=0x439578;
 run();
 if(R[2].uw[0]!=0x439578 || R[29].uw[0]!=0x700000 || R[31].uw[0]!=0x123456 || (count<=320 && R[9].uw[0]!=0x700000-0x1010+0xa00+count*4)) {
  fprintf(stderr,"count=%u output=%x sp=%x ra=%x\\n",count,R[2].uw[0],R[29].uw[0],R[31].uw[0]);return 1;
 }
 return 0;
}
'''
        path=Path(cls.tmp.name);(path/'test.c').write_text(source);cls.exe=path/'test'
        build=subprocess.run([shutil.which('cc'),'-std=c99','-O1',str(path/'test.c'),'-o',str(cls.exe)],capture_output=True,text=True)
        if build.returncode:raise RuntimeError(build.stderr)

    def test_unrelated_function_keeps_original_stack_layout(self):
        start=0x130000
        words={start:0x27bdeff0,start+4:0xafa40f28,start+8:0x03e00008,start+12:0x27bd1010}
        class Elf:
            def read32(self,address):return words[address]
        body='\n'.join(FuncEmitter(Gen(Elf(),{start},{},{}),start,list(words),'test').emit())
        self.assertNotIn('eod_davmap_stack_growth',body)
        self.assertIn('0xFFFFEFF0',body)
        self.assertIn('+ 3880',body)

    def test_native_and_extended_counts_preserve_spilled_arguments_and_returns(self):
        for count in (290,292,320,321,328,330,361,450,1000):
            with self.subTest(count=count):
                result=subprocess.run([str(self.exe),str(count)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)

if __name__=='__main__':unittest.main()
