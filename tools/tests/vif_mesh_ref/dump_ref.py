"""Dump tools/vif_mesh.py output for every retail model part to a binary reference.
Record: 'PREF' u32 keylen key | u32 status | 16s texture | u32 group | u32 nbatch | nbatch x (u32 mscal,u32 nverts)
        | u32 nv | nv x (3f p, 2f uv, 3f n, u8 adc, u8 skinmask, 4I skin) | u32 nt | nt x 3I
Python doubles are rounded to f32 (exact for f32+f32 sums and int/127 quotients: double rounding is innocuous)."""
import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import struct, time
from walk import *
out = open(sys.argv[1], 'wb')
t0 = time.time(); nparts = nverts = nfail = 0
VERT = struct.Struct('<8fBB4I')
for a in archives():
    rel = os.path.relpath(a, DISC / 'DATA').replace(chr(92), '/')
    for name, typ, data in members(a):
        for lab, parts in models(data, typ):
            for idx, p in enumerate(parts):
                key = '%s|%s|%s|%d' % (rel, name, lab, p['slot'] if typ == 'MODL' else idx)
                kb = key.encode()
                status = 0; batches = []; verts = []; tris = []
                try:
                    batches = vm.decode_part(data, p)
                    for bt in batches:
                        base = len(verts)
                        for (x, y, z) in vm.triangulate_batch(bt):
                            pa, pb, pc = bt['verts'][x]['p'], bt['verts'][y]['p'], bt['verts'][z]['p']
                            if pa == pb or pb == pc or pa == pc:
                                continue
                            tris.append((x + base, y + base, z + base))
                        verts.extend(bt['verts'])
                except Exception as e:
                    status = 1; nfail += 1; print('FAIL', key, repr(e), file=sys.stderr)
                    batches, verts, tris = [], [], []
                tex = p['texture'].encode('latin1')[:16].ljust(16, b'\0')
                buf = [b'PREF', struct.pack('<I', len(kb)), kb, struct.pack('<I', status), tex,
                       struct.pack('<II', p['group'], len(batches))]
                buf += [struct.pack('<II', b['mscal'], len(b['verts'])) for b in batches]
                buf.append(struct.pack('<I', len(verts)))
                for v in verts:
                    sk = v['skin']; mask = 0; sv = [0, 0, 0, 0]
                    if sk is not None:
                        for c in range(4):
                            if sk[c] is not None: mask |= 1 << c; sv[c] = sk[c] & 0xFFFFFFFF
                    buf.append(VERT.pack(*v['p'], *v['uv'], *v['n'], 1 if v['adc'] else 0, mask, *sv))
                buf.append(struct.pack('<I', len(tris)))
                buf.append(struct.pack('<%dI' % (3 * len(tris)), *[i for t in tris for i in t]))
                out.write(b''.join(buf))
                nparts += 1; nverts += len(verts)
print('parts', nparts, 'verts', nverts, 'python failures', nfail, 'seconds %.1f' % (time.time() - t0))
