#!/usr/bin/env python3
"""Quick disassembler: dis.py <hexaddr> [count]"""
import rabbitizer as r, struct, sys
data=open(__file__.rsplit('/',2)[0]+'/SLUS_209.10','rb').read()
def word(a): return struct.unpack_from('<I',data,a-0x100000+0x1000)[0]
va=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 40
for k in range(n):
    a=va+4*k; w=word(a)
    i=r.Instruction(w,a,r.InstrCategory.R5900)
    print(f'{a:08x} {w:08x} {i.disassemble()}')
