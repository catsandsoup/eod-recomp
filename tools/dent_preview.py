#!/usr/bin/env python3
"""Offline damage preview: deform a car's LOD the way TDEOD's VU1 program does
(pos += wa/256*D[a] + wb/256*D[b], skin read in reverse vertex order per batch, D = bone
table of the part's bonesurf) for a synthetic impact, and render it with vif_mesh.

usage: dent_preview.py TOOLS_DIR CAR_DIR LOD OUT.png [--hit x,y,z] [--push dx,dy,dz] [--radius r]
       [--tex NAME=FILE] [--yaw Y] [--pitch P] [--bones-png OUT2.png]
"""
import argparse, glob, math, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vif_mesh  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('tools'); ap.add_argument('car'); ap.add_argument('lod'); ap.add_argument('out')
ap.add_argument('--hit', default='-0.9,0.5,1.6'); ap.add_argument('--push', default='0.25,0,-0.1')
ap.add_argument('--radius', type=float, default=0.6)
ap.add_argument('--tex', action='append', default=[]); ap.add_argument('--yaw', type=float, default=-35)
ap.add_argument('--pitch', type=float, default=20)
a = ap.parse_args()
sys.path.insert(0, a.tools)
from valt import parse_binary_valt  # noqa: E402

mv = [x for x in glob.glob(a.car + '/*') if x.lower().endswith('bone.mvl')][0]
S = {s['name']: dict(s['pairs']) for s in parse_binary_valt(open(mv, 'rb').read())['sections']}
surfs = sorted((int(v['first_bone']), int(v['last_bone']), int(v['group_mask'], 16)) for k, v in S.items() if k.startswith('bonesurf'))
B = {int(k[5:]): [float(x) / 39.37 for x in v['link_center'].split(',')] for k, v in S.items() if k.startswith('bone_')}
hit = [float(x) for x in a.hit.split(',')]; push = [float(x) for x in a.push.split(',')]
D = {}
for i, p in B.items():
    if p == [0.0, 0.0, 0.0]:
        continue
    d = math.dist(p, hit)
    f = max(0.0, 1 - d / a.radius)
    D[i] = [push[k] * f for k in range(3)]

path = [x for x in glob.glob(a.car + '/*') if os.path.basename(x).lower() == a.lod.lower() + '.mod'][0]
data = open(path, 'rb').read()
mesh = vif_mesh.parse_modl(data)
moved = 0
for part in mesh['parts']:
    g = part['group'] & 0xFFFF
    sf = [s for s in surfs if s[2] >> g & 1]
    first = sf[0][0] if sf else 0
    verts, tris = [], []
    for bt in vif_mesh.decode_part(data, part):
        vs = bt['verts']; n = len(vs)
        skins = [v['skin'] for v in vs]
        for k, v in enumerate(vs):
            s = skins[n - 1 - k]
            if s is None:
                continue
            ba, bb, wa, wb = s
            if wa > 255 or len(s) < 4:
                continue
            da = D.get(first + ba, [0, 0, 0]) if not (ba == 0 and first == 0) else [0, 0, 0]
            db = D.get(first + bb, [0, 0, 0]) if not (bb == 0 and first == 0) else [0, 0, 0]
            dp = [wa / 256 * da[c] + wb / 256 * db[c] for c in range(3)]
            if any(abs(x) > 1e-4 for x in dp):
                moved += 1
            v['p'] = tuple(v['p'][c] + dp[c] for c in range(3))
        base = len(verts)
        t = vif_mesh.fix_winding(vs, vif_mesh.strip_triangles(vs))
        tris += [(x + base, y + base, z + base) for x, y, z in t]
        verts += vs
    part['verts'], part['tris'] = verts, tris
textures = {}
for kv in a.tex:
    name, f = kv.split('=', 1)
    t = vif_mesh.load_texture(os.path.basename(f), [os.path.dirname(f)], a.tools)
    if t:
        textures[os.path.splitext(name)[0].lower()] = t
for part in mesh['parts']:
    key = os.path.splitext(part['texture'])[0].lower()
    if key not in textures:
        t = vif_mesh.load_texture(part['texture'], [a.car], a.tools)
        if t:
            textures[key] = t
vif_mesh.render(mesh, a.out, textures, yaw_deg=a.yaw, pitch_deg=a.pitch)
print('vertices moved', moved, '->', a.out)
