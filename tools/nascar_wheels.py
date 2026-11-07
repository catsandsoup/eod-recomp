"""Source-owned Cup wheels for the native data-driven wheelmod loader.

SLUS_209.10: 21f780 loads <class>.val; 21f240 reads up to eight lodN
entries; 21f3a0 loads <base>N.mod and derives the fast texture variant.
Dimensions are inches (21f830/850 halve diameter, 21f888 converts width).
"""
import hashlib
import struct
from pathlib import Path
import nascar_gen as gen
from valt import compile_binary_valt


def build_cup_wheels(source, physics):
    return build_source_wheels(source, physics, prefix="ncu")


def build_source_wheels(source, physics, prefix="ncu"):
    """Translate the exact source axle model prefix, including native missing-LOD0 fallback."""
    # Delayed import avoids a cycle with the importer which owns the proven VIF converter.
    from import_nascar_cars import transcode_vif_stream,convert_tex_v9_to_v10
    files={p.name.lower():p for p in Path(source).iterdir()}
    resources,provenance={},{}
    axles=[dict(s['pairs']) for s in physics['sections'] if s['name'] in ('Front','Rear')]
    if len(axles)!=2 or any(a['model'].lower()!=axles[0]['model'].lower() for a in axles):
        raise ValueError('Separate source axle wheel profiles required')
    model=axles[0]['model'].lower()
    if model not in ('rtire','ptire') or not prefix.isalnum() or len(prefix)>3:
        raise ValueError('Unverified source wheel model or invalid namespace')
    def add(name,blob,version,fourcc,origin,method):
        resources[name]=(blob,version,fourcc)
        provenance[name]={'source':origin,'conversion':method,'sha256':hashlib.sha256(blob).hexdigest()}
    textures=set()
    for stem in [f'{i}{suffix}' for i in range(5) for suffix in ('', 'f')]:
        p=files.get(f'{model}{stem}.mod')
        if p is None:
            if stem in ('0','0f') and model=='ptire':continue
            raise ValueError(f'Missing source wheel {model}{stem}.mod')
        m=gen.read_modl(p.read_bytes())
        for part in m['parts']:
            batches=gen.walk_vif(part['stream'])
            if any(b['mscal']!=0x26 for b in batches):
                raise ValueError('Cup wheel is not the verified rigid MSCAL26 format')
            tex=gen.part_texture(part);textures.add(tex.lower())
            renamed=prefix+tex
            if len(renamed.encode('latin1'))>15:raise ValueError('Wheel texture name too long')
            part['hdr'][8:24]=renamed.encode('latin1').ljust(16,b'\0')
            part['stream']=transcode_vif_stream(part['stream'])
        add(f'{prefix}{stem}.mod',gen.write_modl(m),9,'MODL',f'RACE.RES/{p.name}',
            'Rigid VIF MSCAL26→55, offset normals/UV row; namespaced texture references; unchanged source vertices')
    remap=({'tire.tex':'blurtire.tex','tread.tex':'blrtread.tex'} if model=='rtire'
           else {'ptire.tex':'ptireblr.tex'})
    for name in textures|set(remap.values()):
        p=files[name]
        add(prefix+p.name,convert_tex_v9_to_v10(p.read_bytes()),10,'TEX',f'RACE.RES/{p.name}',
            'TEX v9→v10 header; unchanged source pixels')
    # NASCAR TABL: u32 rows,cols; 0x44 process-state bytes; fixed 33-byte cells.
    tab=files[model+'.tab'].read_bytes();rows,cols=struct.unpack_from('<II',tab)
    if (rows,cols)!=(5,2) or len(tab)!=0x4c+rows*cols*33:
        raise ValueError('Unexpected Cup tyre LOD table layout')
    lod=[]
    for i in range(rows):
        at=0x4c+i*66
        dist=tab[at:at+33].split(b'\0')[0].decode().strip()
        rotates=tab[at+33:at+66].split(b'\0')[0].decode().strip()
        float(dist)
        if rotates not in ('rot',''):raise ValueError('Unknown tyre LOD flag')
        lod.append((f'lod{i}',dist+'\t'+('y' if rotates else 'n')))
    axle=next(dict(s['pairs']) for s in physics['sections'] if s['name']=='Front')
    def section(name,pairs):return {'name':name,'parent':'','pairs':list(pairs)}
    table={'sections':[
        section('dimensions',[('tire_diameter',axle['tire_diameter']),('rim_diameter',axle['diameter']),
                              ('width',str(float(axle['width'])/25.4))]),
        section('model',[('base',prefix),('@fast_remap','$0000$fast_remap')]),
        section('lod',lod),
        section('$0000$fast_remap',[(prefix+files[k].name,prefix+files[v].name) for k,v in remap.items()])],
        'root_count':3}
    add(prefix+'.val',compile_binary_valt(table),11,'VALT',
        f'Source physics.val Front tire_diameter/diameter/width; RACE.RES/{model}.tab; source wheel texture names',
        'Native wheelmod schema; source mm width→inches; source LOD distance/rotation flags; source blur texture remap')
    return resources,provenance
