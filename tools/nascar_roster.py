#!/usr/bin/env python3
"""Build one source-backed five-family NASCAR overlay (offline candidate only).

Native class registration is explicitly a temporary Midsize prototype. Dedicated
Stock Car, faithful gearing/audio, source damage threshold and runtime acceptance
are still required. Never writes to installed profiles or edits original discs.
"""
import argparse
import copy
import hashlib
import json
import re
import shutil
import struct
import tempfile
from pathlib import Path

import nascar_gen as gen
from import_nascar_cars import (find_entry, transcode_modl, convert_tex_v9_to_v10,
    drop_top_mip, merge_physics, group_points, make_shadows, fix_cockpit_roof,
    hood_helpers, add_helpers, set_entry, STOCK_INTERIOR_TEXTURES)
from create_custom_car import car_archive_filename
from modl import retexture_modl
from nascar_wheels import build_source_wheels
from rst0 import unpack_archive, pack_archive
from valt import parse_binary_valt, compile_binary_valt

FAMILIES={
    'stock':dict(archive='STOCK',body='Stock1.tex',paint='PAINT256',roster='cup.val',price=18000,stats=(10,8,6)),
    'truck':dict(archive='TRUCK',body='Truck1.tex',paint='TRUCKSET',roster='truck.val',price=14000,stats=(9,6,8)),
    'modified':dict(archive='MODIFIED',body='Modfied1.tex',paint='MODFYSET',roster='modified.val',price=8500,stats=(8,9,4)),
    'hobby':dict(archive='HOBBY',body='Hobby.tex',paint='HOBBYSET',roster='dirt.val',price=3500,stats=(6,6,7)),
    'pacecar':dict(archive='PACECAR',body='Pacecar.tex',paint=None,roster=None,price=0,stats=(7,7,7)),
}
LIVERY_IDS=json.loads(Path(__file__).with_name('nascar_livery_ids.json').read_text())['ids']
CHASSIS={'stock':{'chevy':'Cupcar','ford':'CupFord','pontiac':'CupPont','dodge':'CupDodge'},
         'truck':{'all':'NTruck'},'modified':{'all':'NMod'},'hobby':{'all':'NHobby'},'pacecar':{'all':'NPace'}}
# Source SLUS_204.41:1461f8..2c0 maps Pontiac/Ford/Chevy/Dodge to0/1/2/3;
# 16f440 maps these to42/44/43/45;16ed78..ee18 constructs these exact masks.
# Preserve source selection, including Ford's shared group8 rear/group9 roof.
STOCK_MASKS={'pontiac':0x1e07,'ford':0x1b07,'chevy':0x1867,'dodge':0x1999}
# Dodge's baseline1819 includes3/4 (absent here), plus180 selects7/8.

def allocate_liveries(records, used_ids=()):
    """Stable IDs independent of source traversal; fail rather than renumber an existing save."""
    used={str(v) for v in used_ids};out=[]
    order={k:i for i,k in enumerate(FAMILIES)}
    for r in sorted(records,key=lambda r:(order[r['family']],int(r['source_number']))):
        key=f'{r["family"]}:{r["source_number"]}'
        if key not in LIVERY_IDS:raise ValueError(f'Extend pinned livery registry without renumbering: {key}')
        ident=str(LIVERY_IDS[key])
        if not 0<=int(ident)<800:raise ValueError('Driver ID violates native number/AI range')
        if ident in used:raise ValueError(f'NASCAR internal livery ID collision: {ident}')
        used.add(ident);out.append(dict(r,id=ident))
    return out


def remap_family_groups(model,family,style='chevy',body=None):
    """Preserve every selected source part; only Cup has manufacturer visibility filtering."""
    body=(body or FAMILIES[family]['body']).lower()
    parts=[]
    for part in model['parts']:
        raw=gen.part_group(part);g=raw&0xffff;tex=gen.part_texture(part).lower()
        if family=='stock':
            if not STOCK_MASKS[style]&(1<<g):continue
            if g in (5,7,9):g=9
            elif g in (6,8,10):g=10
            elif g==11:g=2
            elif g==12:g=1
            elif g==0:g=0 if tex in STOCK_INTERIOR_TEXTURES | {"nsuit.tex"} else 9
        else:
            # Native detachable group numbers mean different things. Keep all source
            # panels in the body surface rather than mislabel a lightbar as a door.
            rigid={'template01c.tex','nsuit.tex','rollblk.tex','rollslv.tex','rollbar.tex',
                   'padding.tex','airclean.tex','filter.tex','pipe.tex','windcage.tex','flagger.tex',
                   'spoke.tex','steer2.tex','steer3.tex'}
            g=0 if tex in rigid or (family=='pacecar' and tex=='lights.tex') else 9
        struct.pack_into('<I',part['hdr'],0x18,(raw&0xffff0000)|g)
        parts.append(part)
    model['parts']=parts


def source_damage_model(template,source,points):
    """Keep source damage centers, travel and weakness; adapt only native bounds/surface.

    Bone.val supplies links; every source key/value survives native VALT serialization. Source has no
    dmg_dist key: omit it, exposing the native reader's documented0.2032m default
    until the source threshold is traced. No retail weakness/travel is invented.
    """
    bones=[copy.deepcopy(s) for s in source['sections'] if re.fullmatch(r'bone_\d+',s['name'])]
    bones.sort(key=lambda s:int(s['name'][5:]))
    if not bones or [int(s['name'][5:]) for s in bones]!=list(range(1,len(bones)+1)) or len(bones)>79:
        raise ValueError('Source skeleton must be contiguous and fit the native80-entry VU surface')
    vt=template['sections'][0]['sec_vtable']
    bounds=[]
    for g in range(32):
        qs=points.get(g,[(0,0,0)])
        for label,fn in [('max',max),('min',min)]:
            bounds.append((f'{label}_{g}',', '.join(f'{fn(q[k] for q in qs):.5f}' for k in range(3))))
    def sec(name,pairs):return dict(name=name,parent='',sec_vtable=vt,tool_ptr=0,pairs=pairs)
    mask=sum(1<<g for g in points)
    mvl=copy.deepcopy(template)
    mvl['sections']=[sec('bounds',sorted(bounds)),sec('bonesurf_1',[('first_bone','0'),('last_bone',str(len(bones))),('group_mask',f'{mask:X}')])]
    local=[]
    for b in bones:
        vals=dict(b['pairs']);xyz=tuple(float(v)*.0254 for v in vals['link_center'].split(','))
        local.append((int(b['name'][5:]),xyz));b.update(sec_vtable=vt,tool_ptr=0,parent='')
        mvl['sections'].append(b)
    return mvl,local


def adapt_source_transmission(physics,setup):
    """Source1abf00 ratios and1ac898/1ac760 final-drive algebra → target native keys.

    The caller explicitly chooses the source-authored setup. This does not infer
    which source track or mode should drive an unrelated native EOD circuit.
    """
    if len(setup)!=196:raise ValueError('Expected complete196-byte NASCAR setup')
    section=next(s for s in physics['sections'] if s['name']=='Transmission')
    pairs=dict(section['pairs']);count=int(pairs['num_gears'])
    if not 1<=count<=4:raise ValueError('Unverified source gear count')
    import math
    ratios=struct.unpack_from('<'+str(count)+'f',setup,0x78)
    final=struct.unpack_from('<f',setup,0x94)[0]
    if any(not math.isfinite(v) or v<=0 for v in (*ratios,final)):
        raise ValueError('Invalid source transmission ratios')
    for i,value in enumerate(ratios,1):pairs[f'g{i}']=format(value,'.9g')
    pairs['rear_end_ratio']=format(final,'.9g')
    pairs.pop('ratio_factor',None) # no target reader; historical prototype artifact
    section['pairs']=list(pairs.items())
    return physics


def preserve_source_setups(nascar_data,sources,package):
    """Keep every complete track/mode setup for the later per-event adapter."""
    root=package/'source-setups';root.mkdir();inventory={}
    def save(relative,blob):
        path=root/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(blob)
        inventory[str(relative)]=dict(size=len(blob),sha256=sha(blob),
            gears=list(struct.unpack_from('<4f',blob,0x78)) if len(blob)>=0x88 else None,
            final_drive=struct.unpack_from('<f',blob,0x94)[0] if len(blob)>=0x98 else None)
    for p in sources['RACE'].iterdir():
        if p.suffix.lower()=='.gar':save(Path('RACE')/p.name,p.read_bytes())
    for path in sorted(nascar_data.glob('TRACK*/*.TRK')):
        with tempfile.TemporaryDirectory(prefix='source-setups-') as temp:
            directory=Path(temp);unpack_archive(str(path),str(directory))
            for p in directory.iterdir():
                if p.suffix.lower()=='.gar':save(path.relative_to(nascar_data).with_suffix('')/p.name,p.read_bytes())
    (root/'inventory.json').write_text(json.dumps(inventory,indent=2))
    return inventory


def native_source_valt(source,native):
    """Convert source all-root VALT semantics without retaining v6 index-count0."""
    result=copy.deepcopy(source);result['is_v6']=False
    result['tool_vtable']=native.get('tool_vtable',0x40c218)
    result['root_count']=len(result['sections'])
    for section in result['sections']:
        section['sec_vtable']=native['sections'][0]['sec_vtable'];section['tool_ptr']=0
    return result


def section_like(table,name,pairs):
    meta={k:v for k,v in table['sections'][0].items() if k not in ('name','parent','pairs')}
    return dict(meta,name=name,parent='',pairs=list(pairs.items()))


def sha(blob):return hashlib.sha256(blob).hexdigest()


class Archive:
    def __init__(self,path,directory):
        self.path=Path(directory);unpack_archive(str(path),str(self.path))
        self.manifest=json.loads((self.path/'manifest.json').read_text())
        self.original={e['name'].lower():sha((self.path/e['name']).read_bytes()) for e in self.manifest['entries']}
    def read(self,name):return find_entry(self.path,name).read_bytes()
    def table(self,name):return parse_binary_valt(self.read(name))
    def add(self,name,blob,version=11,fourcc='VALT',extractable=False,replace=False):
        existing=[e for e in self.manifest['entries'] if e['name'].lower()==name.lower()]
        if existing and not replace:raise ValueError(f'Archive entry collision: {name}')
        if existing and existing[0]['name']!=name:
            (self.path/existing[0]['name']).unlink()
        (self.path/name).write_bytes(blob);set_entry(self.manifest,name,len(blob),version,fourcc,extractable)
    def write_table(self,name,model):self.add(name,compile_binary_valt(model),replace=True)
    def pack(self,out):
        for e in self.manifest['entries']:
            e.pop('orig_offset',None);e.pop('offset',None)
            e['size']=(self.path/e['name']).stat().st_size
        self.manifest.pop('original_layout',None)
        (self.path/'manifest.json').write_text(json.dumps(self.manifest,indent=2))
        Path(out).parent.mkdir(parents=True,exist_ok=True);pack_archive(str(self.path),str(out))
    def assert_native_preserved(self,allowed=()):
        allow={a.lower() for a in allowed}
        for n,digest in self.original.items():
            if n not in allow and sha(self.read(n))!=digest:raise AssertionError(f'Native payload changed: {n}')


def collect_roster(sources):
    records=[]
    for family,cfg in FAMILIES.items():
        if family=='pacecar':
            records.append(dict(family=family,source_number='0',name='NASCAR Pace Car',body_style='all',paint='Pacecar.tex',driver=None,chassis='NPace'));continue
        drivers=[s for s in parse_binary_valt(find_entry(sources['COMMON'],cfg['roster']).read_bytes())['sections'] if s['name']=='Driver']
        seen=set()
        for driver in drivers:
            d=dict(driver['pairs'])
            if d.get('custom')=='yes':continue
            number=d['number'];seen.add(number)
            style=d.get('body_style','chevy').lower()
            chassis=CHASSIS[family][style if family=='stock' else 'all']
            paint=find_entry(sources[cfg['paint']],f'Car{number}.tex').name
            records.append(dict(family=family,source_number=number,name=d['name'],body_style=style,paint=paint,driver=d,chassis=chassis))
        # Preserve real alternate paints too; never fabricate a driver for them.
        for p in sorted(sources[cfg['paint']].iterdir()):
            match=re.fullmatch(r'car(\d+)\.tex',p.name,re.I)
            if match and match[1] not in seen:
                records.append(dict(family=family,source_number=match[1],name=f'{family.title()} source alternate {match[1]}',body_style='chevy',paint=p.name,driver=None,chassis=CHASSIS[family].get('chevy',CHASSIS[family].get('all'))))
    return records


def build_chassis(family,style,name,records,sources,native_car,destination):
    """Construct from source resources only, with explicit remaining native-format adapters."""
    cfg=FAMILIES[family];src=sources[cfg['archive']];car=destination;car.mkdir()
    entries=[];provenance={};adapters=[]
    def put(fname,blob,version=11,fourcc='VALT',origin='',method='',extractable=False):
        (car/fname).write_bytes(blob)
        entries.append(dict(name=fname,size=len(blob),version=version,fourcc=fourcc,is_extractable=extractable))
        provenance[fname]=dict(source=origin,conversion=method,sha256=sha(blob))
    def lookup_texture(tex):
        for ar in (cfg['archive'],'RACE','COMMON'):
            try:return find_entry(sources[ar],tex),ar
            except FileNotFoundError:pass
        raise ValueError(f'{family} missing source texture {tex}')
    body=f'{name}.tex';models={};used=set()
    source_models=[f'Pacecar{i}.mod' if family=='pacecar' else f'Low{i}.mod' for i in range(7)]
    for i,original in enumerate(source_models):
        path=find_entry(src,original);blob=transcode_modl(path.read_bytes())
        blob=retexture_modl(blob,cfg['body'],body)
        blob=retexture_modl(blob,'template01c.tex','nsuit.tex')
        if family=='pacecar':
            # These two steering names and Hood.tex are unresolved in source shared
            # archives. Preserve the original model separately; source-backed
            # material substitutions are explicit candidate adapters, not fidelity.
            for old,new in [('steer2.tex','Wheel2.tex'),('Steer3.tex','Wheel2.tex'),('Hood.tex',body)]:blob=retexture_modl(blob,old,new)
        model=gen.read_modl(blob)
        if i>0:remap_family_groups(model,family,style,body=body)
        models[i]=model
    points=group_points(models[1])
    src_bones=parse_binary_valt(find_entry(src,'bone.val').read_bytes())
    mvl,local=source_damage_model(native_car.table('Bone.mvl'),src_bones,points)
    for i,model in models.items():
        for part in model['parts']:
            g=gen.part_group(part)&0xffff
            gen.write_part_skin(part,lambda p,rigid=(i==0 or g==0): (0,0,255,0) if rigid else gen.compute_skin(p,local))
        if family=='stock' and i>0:add_helpers(model,hood_helpers(points))
    if family=='stock':fix_cockpit_roof(models[0],points)
    for i,model in models.items():
        blob=gen.write_modl(model)
        put(f'Low{i}.mod',blob,9,'MODL',f'{cfg["archive"]}.CAR/{source_models[i]}','VIF target conversion; source geometry; family visibility/native group adapter; source bone skin')
        used.update(gen.part_texture(p).lower() for p in model['parts'] if gen.part_texture(p).lower().endswith('.tex'))
    put('Bone.mvl',compile_binary_valt(mvl),origin=f'{cfg["archive"]}.CAR/bone.val + Low1.mod',method='Exact source bone center/travel/weakness; source geometry bounds; native bonesurf adapter; native default dmg_dist remains unverified')
    bone_model=native_source_valt(src_bones,native_car.table('Bone.val'))
    put('Bone.val',compile_binary_valt(bone_model),origin=f'{cfg["archive"]}.CAR/bone.val',method='All source skeleton/links and exact values; native VALT vtable adapter')
    shadows={f'shadow{i}.mod':native_car.read(f'shadow{i}.mod') for i in (1,2,3)}
    for fname,blob in make_shadows(points,shadows).items():
        put(fname,blob,9,'MODL',f'{cfg["archive"]}.CAR/Low1.mod','Generated source body convex hull; native packet schema')
    default=next((r for r in records if family=='stock' and style=='chevy' and r['source_number']=='24'),
                 next((r for r in records if r['driver']),records[0]));paint_source=sources[cfg['paint']] if cfg['paint'] else src
    livery=convert_tex_v9_to_v10(find_entry(paint_source,default['paint']).read_bytes(),aux=0x100)
    put(body,drop_top_mip(livery),10,'TEX',f'{cfg.get("paint") or cfg["archive"]}/{default["paint"]}','Source next mip; native header')
    put(f'{name}.tm0',livery,10,'TEX',f'{cfg.get("paint") or cfg["archive"]}/{default["paint"]}','Native header; source pixels',True)
    for tex in sorted(used-{body.lower()}):
        if tex=='nsuit.tex':
            # Original source generates template01c from the team texture. Import
            # its actual generic team skin instead of the prior padding substitute.
            path=find_entry(sources[cfg['paint']],'team000.tex') if cfg['paint'] in ('PAINT256','TRUCKSET') else find_entry(sources['PAINT256'],'team000.tex')
            ar=cfg['paint'] if cfg['paint'] in ('PAINT256','TRUCKSET') else 'PAINT256'
        else:path,ar=lookup_texture(tex)
        put(tex,convert_tex_v9_to_v10(path.read_bytes()),10,'TEX',f'{ar}/{path.name}','TEX v9→v10 header; exact source pixels')
    phys_source=parse_binary_valt(find_entry(src,'physics.val').read_bytes())
    physics=merge_physics(native_car.table('physics.val'),phys_source,wheel_class='npc' if family=='pacecar' else 'ncu')
    physics=adapt_source_transmission(physics,find_entry(sources['RACE'],'default.gar').read_bytes())
    put('physics.val',compile_binary_valt(physics),origin=f'{cfg["archive"]}.CAR/physics.val + native physics schema',method='Every source section/key; neutral grip/damage_scale; exact source axle dirt copies; source wheel class adapter; source-authored RACE/default.gar gear/final-drive baseline for native EOD tracks; unresolved spring/audio/gauge adapters')
    lod=native_car.table('lod.val')
    for s in lod['sections']:
        if s['name']=='low':s['pairs']=[(k,body.lower() if k=='base_tex' else v) for k,v in s['pairs']]
    put('lod.val',compile_binary_valt(lod),origin='Native lod.val schema',method='TEMPORARY native LOD distances; source base texture')
    put('srclod.val',compile_binary_valt(native_source_valt(parse_binary_valt(find_entry(src,'lod.val').read_bytes()),native_car.table('lod.val'))),origin=f'{cfg["archive"]}.CAR/lod.val',method='Preserved source LOD keys/values with native VALT schema; not yet active')
    put('efx.val',native_car.read('efx.val'),origin='Native ALGAR.CAR/efx.val',method='UNRESOLVED donor effects adapter')
    # Other target runtime helper geometry is intentionally listed, rather than
    # pulling an entire donor archive and hiding its unrelated unused assets.
    for e in native_car.manifest['entries']:
        fname=e['name']
        if fname.lower().endswith('.mod') and not re.fullmatch(r'(low\d+|shadow\d)\.mod',fname,re.I):
            put(fname,native_car.read(fname),e['version'],e['fourcc'],f'Native ALGAR.CAR/{fname}','UNRESOLVED native cockpit/helper adapter')
    if family=='pacecar':
        for n in ('pacelite.val',):put(n,compile_binary_valt(native_source_valt(parse_binary_valt(find_entry(src,n).read_bytes()),native_car.table('efx.val'))),origin=f'PACECAR.CAR/{n}',method='Preserved source keys/values with native VALT schema; animation integration pending')
        adapters.append('Pacecar cockpit Hood material uses source Pacecar paint; steering uses source Wheel2 texture; original source references unresolved. Light groups retained rigid; animation not integrated.')
    (car/'manifest.json').write_text(json.dumps(dict(version=6,entries=entries),indent=2))
    return dict(family=family,chassis=name,style=style,resources=provenance,adapters=adapters,
                performance={s['name']:dict(s['pairs']) for s in phys_source['sections']},
                native_adapters=['physics spring_factor, engine_sound and speedometer; unsupported source aero/suspension consumers need behavior adapters'
                    ,'source RACE/default.gar gearing baseline is not source per-track setup selection','lod.val distances','efx.val','external cockpit helpers','native dmg_dist default; source collision/deformation equivalence pending'])


def build_batch(nascar_data,eod_data,output,*,work_dir,prototype_native_class=False,previews=True):
    if not prototype_native_class:
        raise ValueError('Dedicated Stock Car hooks are pending; explicitly opt into temporary native-class registration')
    nascar_data,eod_data,output,work_dir=map(lambda p:Path(p).resolve(),(nascar_data,eod_data,output,work_dir))
    for source in (nascar_data,eod_data):
        if output==source or source in output.parents or output in source.parents:
            raise ValueError('Output must be separate from source discs')
    if output.exists():raise FileExistsError(f'Refusing to overwrite candidate {output}')
    if work_dir.exists() and any(work_dir.iterdir()):raise FileExistsError(f'Staging directory is not empty: {work_dir}')
    work_dir.mkdir(parents=True,exist_ok=True)
    sources={}
    for ar in ('COMMON','RACE','PAINT256','TRUCKSET','MODFYSET','HOBBYSET','UIBIG','PAINTKIT'):
        path=nascar_data/'RES'/f'{ar}.RES'
        if path.exists():
            dest=work_dir/f'source-{ar}';unpack_archive(str(path),str(dest));sources[ar]=dest
    for ar in [c['archive'] for c in FAMILIES.values()]:
        dest=work_dir/f'source-{ar}';unpack_archive(str(nascar_data/f'{ar}.CAR'),str(dest));sources[ar]=dest
    native={ar:Archive(eod_data/'RES'/f'{ar}.RES',work_dir/f'native-{ar}') for ar in ('COMMON','CT','UICAR','RACE')}
    base=Archive(eod_data/'CAR'/'ALGAR.CAR',work_dir/'native-base')
    tables={n:native['COMMON'].table(n) for n in ('carclass.val','car_stats.val','dirt.val','buycar.val')}
    original_tables=copy.deepcopy(tables)
    dealer_aliases=[];dealer_id=930
    used={str(dict(s['pairs'])['number']) for name in ('dirt.val','buycar.val') for s in tables[name]['sections'] if 'number' in dict(s['pairs'])}
    # Include archive-only IDs, not just roster rows, to protect every native paint.
    used.update(m[1] for e in native['CT'].manifest['entries'] if (m:=re.fullmatch(r'(\d+)\.(?:tex|tm0)',e['name'],re.I)))
    records=allocate_liveries(collect_roster(sources),used)
    # CarChoose2a7dac..dcc has ten paint slots per chassis. Keep5 real
    # driver paints +5 unique dealer aliases in the temporary native prototype.
    # Every source driver remains in separate pools for the required UI expansion.
    exposed=set()
    for choices in CHASSIS.values():
        for chassis in choices.values():
            choices_for_car=[r for r in records if r['chassis']==chassis and (r['driver'] or r['family']=='pacecar')]
            exposed.update(r['id'] for r in choices_for_car[:5])
    known={dict(s['pairs']).get('chassis','').lower() for s in tables['car_stats.val']['sections']}
    for choices in CHASSIS.values():
        for chassis in choices.values():
            if chassis.lower() in known or (eod_data/'CAR'/car_archive_filename(chassis)).exists():raise ValueError(f'Chassis collision: {chassis}')
    package=work_dir/'package';(package/'DATA'/'CAR').mkdir(parents=True);(package/'DATA'/'RES').mkdir()
    manifest=dict(format=1,acceptance='offline candidate; runtime, fifth class and full behavior pending',
                  native_class_prototype='Midsize (1); no fifth-class arrays changed',families={},liveries=records,
                  prototype_selector={'exposed_driver_ids':sorted(exposed,key=int),'cap':'5 source drivers +5 distinct dealer numbers per chassis; full125 selection requires native UI storage extension'},
                  native_resources={},unresolved_behavior=['source audio conversion; native-track setup-selection policy','source damage threshold and runtime dent behavior','spring_factor and remaining native-only physics keys','source LOD distance mapping','effects and Pace light animation','per-driver team suit selection','Career unlocks and dirt tyre shop'])
    for family,choices in CHASSIS.items():
        family_prov={}
        for style,chassis in choices.items():
            paints=[r for r in records if r['chassis']==chassis]
            car_dir=work_dir/f'car-{chassis}'
            prov=build_chassis(family,style,chassis,paints,sources,base,car_dir)
            pack_archive(str(car_dir),str(package/'DATA'/'CAR'/car_archive_filename(chassis)))
            family_prov[chassis]=prov
            # Only append to an existing safe native list. Never append class4.
            target=next(s for s in tables['carclass.val']['sections'] if s['name']=='$0001$chassis')
            target['pairs'].append((chassis,''))
            cfg=FAMILIES[family];speed,handling,durability=cfg['stats']
            tables['car_stats.val']['sections'].append(section_like(tables['car_stats.val'],'carstats',dict(chassis=chassis,speed=str(speed),handling=str(handling),durability=str(durability))))
            if family!='pacecar':
                desc=(f'NASCAR {family.title()} from Dirt to Daytona. '+
                      f"{prov['performance']['EngineNormal']['power_max']} HP; {prov['performance']['Chassis']['mass']} lb. Prototype behavior adapters pending.")
                tables['buycar.val']['sections'].append(section_like(tables['buycar.val'],'car',dict(chassis=chassis,desc=desc,price=str(cfg['price']))))
                for paint in [r for r in paints if r['id'] in exposed][:5]:
                    if str(dealer_id) in used or dealer_id>=1000:raise ValueError('Dealer ID collision/range failure')
                    alias=dict(paint,id=str(dealer_id),source_id=paint['id']);dealer_aliases.append(alias);used.add(str(dealer_id));dealer_id+=1
                    tables['buycar.val']['sections'].append(section_like(tables['buycar.val'],'vehicle',dict(chassis=chassis,number=alias['id'],display_number=paint['source_number'],body_color='255, 255, 255')))
        manifest['families'][family]=family_prov
        # Retain a distinct pool for eventual Stock Car event/class hooks.
        pool=copy.deepcopy(original_tables['dirt.val']);pool['sections']=[]
        for paint in [r for r in records if r['family']==family]:
            d=copy.deepcopy(paint['driver']) if paint['driver'] else dict(name=paint['name'],rank='101',body_color='0xFFFFFFFF')
            d.update(chassis=paint['chassis'],number=paint['id'],display_number=paint['source_number'])
            d.pop('ui_image',None) # source portrait format integration is separate; no dangling native IMG lookup
            row=section_like(original_tables['dirt.val'],'Driver',d)
            pool['sections'].append(row)
            if paint['id'] in exposed:tables['dirt.val']['sections'].append(copy.deepcopy(row))
        native['COMMON'].add(f'ns{family[:6]}.val',compile_binary_valt(pool))
    loaded=[dict(s['pairs'])['number'] for n in ('dirt.val','buycar.val') for s in tables[n]['sections'] if 'number' in dict(s['pairs'])]
    if len(loaded)!=len(set(loaded)) or any(not 0<=int(n)<1000 for n in loaded):raise ValueError('Duplicate or out-of-range native driver/dealer number')
    for name,table in tables.items():
        before=original_tables[name]['sections'];after=table['sections']
        # The class-list pair append is the sole modification of native records.
        if name=='carclass.val':
            for a,b in zip(before,after):
                expected=copy.deepcopy(a)
                if a['name']=='$0001$chassis':expected['pairs']=b['pairs'][:len(a['pairs'])]
                if expected!=a or b['pairs'][:len(a['pairs'])]!=a['pairs']:raise AssertionError('Native class data changed')
        elif after[:len(before)]!=before:raise AssertionError(f'Native records changed: {name}')
        native['COMMON'].write_table(name,table)
    for paint in records:
        cfg=FAMILIES[paint['family']]
        directory=sources[cfg['paint']] if cfg['paint'] else sources[cfg['archive']]
        blob=convert_tex_v9_to_v10(find_entry(directory,paint['paint']).read_bytes(),aux=0x100)
        for ext,data,extract in [('tm0',blob,True),('tex',drop_top_mip(blob),False)]:
            native['CT'].add(f'{paint["id"]}.{ext}',data,10,'TEX',extract)
    # Dealer records must have unique numbers globally (native129b30 verifier).
    for alias in dealer_aliases:
        for ext in ('tex','tm0'):
            native['CT'].add(f'{alias["id"]}.{ext}',native['CT'].read(f'{alias["source_id"]}.{ext}'),10,'TEX',ext=='tm0')
    manifest['dealer_aliases']=dealer_aliases
    # Historical Cup prototype used777. Keep save-resource lookup as a documented
    # alias of real source Car24, without inventing or reassigning a native driver.
    legacy=next(r for r in records if r['family']=='stock' and r['source_number']=='24')
    if '777' in used:raise ValueError('Historical Cup777 alias collides with native livery')
    for ext in ('tex','tm0'):
        native['CT'].add('777.'+ext,native['CT'].read(legacy['id']+'.'+ext),10,'TEX',ext=='tm0')
    manifest['legacy_alias']={'id':'777','source_id':legacy['id'],'chassis':'Cupcar','source':'PAINT256.RES/Car24.tex','note':'Resource alias for prototype saves; no duplicate driver'}
    manifest['setups']=preserve_source_setups(nascar_data,sources,package)
    manifest['setup_baseline']={'source':'RACE.RES/default.gar','policy':'Explicit source-authored global baseline on native EOD tracks; source live retail loader normally selects track/mode setups. Per-track mapping remains pending.','gear_offsets':'0x78+4*i','final_drive_offset':'0x94','target':'Transmission.g1..gN / rear_end_ratio, direct ratios; ratio_factor removed'}
    wheel_prov={}
    for family,prefix in [('stock','ncu'),('pacecar','npc')]:
        cfg=FAMILIES[family];physics=parse_binary_valt(find_entry(sources[cfg['archive']],'physics.val').read_bytes())
        resources,provenance=build_source_wheels(sources['RACE'],physics,prefix=prefix)
        for name,(blob,ver,fourcc) in resources.items():native['RACE'].add(name,blob,ver,fourcc)
        wheel_prov.update(provenance)
    manifest['wheels']=wheel_prov
    if previews:
        from nascar_preview import build_previews
        manifest['previews']=build_previews(records,work_dir,sources,native['UICAR'])
        for alias in dealer_aliases:
            for prefix in ('uicar','uicars'):
                for ext in ('tex','tm0'):
                    native['UICAR'].add(f'{prefix}_{alias["id"]}.{ext}',native['UICAR'].read(f'{prefix}_{alias["source_id"]}.{ext}'),10,'TEX',ext=='tm0')
        for prefix in ('uicar','uicars'):
            for ext in ('tex','tm0'):
                native['UICAR'].add(f'{prefix}_777.{ext}',native['UICAR'].read(f'{prefix}_{legacy["id"]}.{ext}'),10,'TEX',ext=='tm0')
    else:manifest['previews']={'status':'omitted; candidate has no selection art'}
    # UIBIG canvases are UV paint-shop templates. Preserve source canvases under
    # namespaced names only after lookup/regions are traced; never use these as promos.
    manifest['paint_shop']={'status':'source canvases/regions not yet mapped; native UIBIG untouched'}
    for ar,archive in native.items():
        allowed=tables.keys() if ar=='COMMON' else ()
        archive.assert_native_preserved(allowed)
        manifest['native_resources'][ar]=dict(preserved=len(archive.original),adapted=sorted(allowed))
        archive.pack(package/'DATA'/'RES'/f'{ar}.RES')
    # Keep per-family/resource provenance independently readable.
    for family,data in manifest['families'].items():
        (package/f'{family}-provenance.json').write_text(json.dumps(data,indent=2))
    (package/'roster-provenance.json').write_text(json.dumps(manifest,indent=2))
    (package/'wheel-provenance.json').write_text(json.dumps(wheel_prov,indent=2))
    shutil.copytree(package,output)
    return manifest


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--nascar-data',required=True);ap.add_argument('--eod-data',required=True)
    ap.add_argument('--output',required=True);ap.add_argument('--work-dir',required=True)
    ap.add_argument('--prototype-native-class',action='store_true',help='Required explicit acknowledgement: temporary Midsize registration; no new Stock Car class')
    ap.add_argument('--no-previews',action='store_true')
    a=ap.parse_args();m=build_batch(a.nascar_data,a.eod_data,a.output,work_dir=a.work_dir,prototype_native_class=a.prototype_native_class,previews=not a.no_previews)
    print(f'Built offline candidate: {len(m["families"])} families, {len(m["liveries"])} real source paints at {a.output}')

if __name__=='__main__':main()
