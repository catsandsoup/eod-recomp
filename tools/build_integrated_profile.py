#!/usr/bin/env python3
"""Compose accepted Phantom/Battle/Cup content against retail, with explicit ID migration.

Offline preparation only. No installed profile, executable, save or config is
changed. Unknown record/asset conflicts fail instead of selecting a last writer.
"""
import argparse
import copy
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile

from build_mod_profiles import (_archive,_data_root,_find_case,_inventory,
    _publish_exclusively,HEADER_FIELDS)
from rst0 import pack_archive,unpack_archive
from valt import parse_binary_valt,compile_binary_valt

REGISTRY={'phantom_driver':775,'phantom_dealer':776,'cup24_driver':777,
          'cup24_dealer':778,'cup_marathon_driver':779}
TABLES={'car_stats.val','carclass.val','buycar.val','dirt.val'}


def _key(filename,section):
    values=dict(section['pairs']);name=section['name']
    if name in ('Driver','vehicle'):
        if 'number'not in values:raise ValueError('Missing vehicle record number')
        return name.casefold(),str(int(values['number']))
    if name in ('carstats','car'):
        if 'chassis'not in values:raise ValueError('Missing chassis record identity')
        return name.casefold(),values['chassis'].casefold()
    return 'section',name.casefold()


def _sections(filename,model):
    out={}
    for section in model['sections']:
        key=_key(filename,section)
        if key in out:raise ValueError(f'Ambiguous {filename} section: {key}')
        out[key]=section
    return out


def _choose(original,variants,label):
    changed=[]
    for value in variants:
        if value!=original and value not in changed:changed.append(value)
    if len(changed)>1:raise ValueError(f'Conflicting {label}')
    return changed[0]if changed else original


def _pair_map(section):
    if len(dict(section['pairs']))!=len(section['pairs']):raise ValueError('Duplicate VALT keys')
    return dict(section['pairs'])


def merge_valt_records(filename,base,overlays):
    """Known native tables: preserve record order/identity, merge disjoint key changes."""
    if filename not in TABLES:raise ValueError(f'No semantic adapter for {filename}')
    original=_sections(filename,base);sources={n:_sections(filename,m)for n,m in overlays.items()}
    keys=list(original)+sorted(set().union(*(m.keys()for m in sources.values()))-original.keys())
    result=copy.deepcopy(base);result['sections']=[]
    for key in keys:
        old=original.get(key);variants=[m.get(key)for _,m in sorted(sources.items())]
        if old is None:
            nonempty=[s for s in variants if s is not None]
            chosen=_choose(None,nonempty,f'new {filename} record {key}')
        elif any(s is None for s in variants):
            chosen=_choose(old,variants,f'{filename} deleted record {key}')
        else:
            chosen=copy.deepcopy(old)
            for field in ('name','parent','sec_vtable','tool_ptr'):
                chosen[field]=_choose(old.get(field),[s.get(field)for s in variants],f'{filename} {key} {field}')
            before=_pair_map(old);changed=[_pair_map(s)for s in variants]
            pairkeys=list(before)+sorted(set().union(*(m.keys()for m in changed))-before.keys())
            chosen['pairs']=[]
            for pairkey in pairkeys:
                value=_choose(before.get(pairkey),[m.get(pairkey)for m in changed],f'{filename} {key} key {pairkey}')
                if value is not None:chosen['pairs'].append((pairkey,value))
        if chosen is not None:result['sections'].append(copy.deepcopy(chosen))
    # Class root count stays four in retail; never append a data-only fifth class.
    if filename=='carclass.val':result['root_count']=base['unk1']
    else:
        if base['unk1']!=len(base['sections']):raise ValueError('Unexpected nested record table')
        result['root_count']=len(result['sections'])
    return result


def _set(section,key,value):
    pairs=[(k,value if k==key else v)for k,v in section['pairs']]
    if key not in dict(pairs):pairs.append((key,value))
    section['pairs']=pairs


def _normalize_table(filename,base,model,label,actions):
    model=copy.deepcopy(model);native=_sections(filename,base)
    own='Phantom'if label=='phantom'else'Cupcar'
    for section in model['sections']:
        values=dict(section['pairs'])
        if values.get('chassis','').casefold()!=own.casefold():continue
        number=values.get('number')
        if filename=='dirt.val'and number in ('199','296'):
            key=_key(filename,section)
            if key not in native:raise ValueError('Legacy native record absent from baseline')
            section.clear();section.update(copy.deepcopy(native[key]));actions.append(dict(action='restore-native-driver',profile=label,number=number));continue
        if filename=='dirt.val'and number=='777':
            _set(section,'number','775'if label=='phantom'else'777')
        if filename=='buycar.val'and section['name']=='vehicle'and number=='778':
            _set(section,'number','776'if label=='phantom'else'778')
        # Historical prototype incorrectly used a paint number as a geometry mask.
        if dict(section['pairs']).get('group_1')=='777':
            section['pairs']=[(k,v)for k,v in section['pairs']if k!='group_1']
    if filename=='dirt.val'and label=='cupcar':
        prototypes=[s for s in model['sections']if s['name']=='Driver'and dict(s['pairs']).get('number')=='777'and dict(s['pairs']).get('chassis','').casefold()=='cupcar']
        if len(prototypes)!=1:raise ValueError('Expected one accepted Cup24 driver')
        marathon=copy.deepcopy(prototypes[0]);_set(marathon,'number','779');_set(marathon,'name','Cupcar Marathon')
        model['sections'].append(marathon)
    return model


def _renamed(entry,target):
    meta,raw=entry;meta=copy.deepcopy(meta);meta['name']=target
    return meta,raw


def _normalize_entries(archive,base,entries,label,actions):
    entries=dict(entries)
    if archive=='res/common.res'and label in ('phantom','cupcar'):
        for filename in TABLES:
            if filename not in entries or filename not in base:raise ValueError('Missing legacy COMMON table')
            model=_normalize_table(filename,parse_binary_valt(base[filename][1]),parse_binary_valt(entries[filename][1]),label,actions)
            meta,raw=entries[filename];entries[filename]=(meta,compile_binary_valt(model))
    if archive=='res/common.res'and label=='battle':
        for key in list(entries):
            if not re.fullmatch(r'[a-z]+_eves\.val',key):continue
            if key not in base:raise ValueError('Battle table has no native catalog baseline')
            if entries[key]==base[key]:continue
            native_model=parse_binary_valt(base[key][1]);battle_model=parse_binary_valt(entries[key][1])
            for section in native_model['sections']:
                if section['name']=='eve'and dict(section['pairs']).get('mode','')in('','multi'):continue
                if section not in battle_model['sections']:
                    raise ValueError(f'Battle modified protected native Career/challenge data: {key}')
            target='b_'+key
            if target in entries or target in base or len(target.encode())>=32:raise ValueError('Battle catalog namespace collision')
            entries[target]=_renamed(entries[key],target)
            entries[key]=base[key] # EXACT native catalog payload; no contest append past320.
            actions.append(dict(action='separate-bounded-battle-catalog',native=key,battle=target,
                native_sha256=hashlib.sha256(base[key][1]).hexdigest(),
                battle_sha256=hashlib.sha256(entries[target][1]).hexdigest()))
    if archive in ('res/ct.res','res/uicar.res')and label in ('phantom','cupcar'):
        source=dict(entries);prefix=''if archive=='res/ct.res'else'uicar_'
        for number in ('199','296'):
            for suffix in ('.tex','.tm0'):
                key=prefix+number+suffix
                if key not in base:raise ValueError(f'Missing native paint baseline {key}')
                entries[key]=base[key]
        if label=='phantom':
            for old,new in [('777','775'),('778','776')]:
                for suffix in ('.tex','.tm0'):
                    sourcekey=prefix+old+suffix
                    # Legacy Phantom dealer omitted CT778: use its actual777 paint.
                    if sourcekey not in source and archive=='res/ct.res'and old=='778':sourcekey='777'+suffix
                    if sourcekey not in source:raise ValueError(f'Missing Phantom art {sourcekey}')
                    target=prefix+new+suffix
                    if target in base:raise ValueError(f'Integrated paint ID collides with retail {target}')
                    entries[target]=_renamed(source[sourcekey],target);entries.pop(prefix+old+suffix,None)
                    actions.append(dict(action='remap-art',profile=label,archive=archive,source=sourcekey,target=target,sha256=hashlib.sha256(source[sourcekey][1]).hexdigest()))
        else:
            for suffix in ('.tex','.tm0'):
                sourcekey=prefix+'296'+suffix;target=prefix+'779'+suffix
                if target in base:raise ValueError('Marathon paint ID collides with retail')
                entries[target]=_renamed(source[sourcekey],target)
                actions.append(dict(action='retain-marathon-art',archive=archive,source=sourcekey,target=target,sha256=hashlib.sha256(source[sourcekey][1]).hexdigest()))
    return entries


def _merge_archive(relative,baseline,mods,target,scratch,report):
    header,base=_archive(baseline,scratch/'base')
    sources={}
    for label,path in sorted(mods.items()):
        metadata,entries=_archive(path,scratch/label)
        if any(metadata.get(k)!=header.get(k)for k in HEADER_FIELDS):raise ValueError(f'Incompatible {relative} header')
        sources[label]=_normalize_entries(relative,base,entries,label,report['actions'])
    keys=list(base)+sorted(set().union(*(m.keys()for m in sources.values()))-base.keys())
    staged=scratch/'merged';staged.mkdir();manifest={k:header.get(k)for k in HEADER_FIELDS};manifest['entries']=[]
    final={}
    for key in keys:
        original=base.get(key);variants=[m.get(key)for m in sources.values()]
        changes=[]
        for entry in variants:
            if entry!=original and entry not in changes:changes.append(entry)
        if len(changes)>1:
            if relative=='res/common.res'and key in TABLES and original and all(v is not None for v in variants):
                metadata=_choose(original[0],[v[0]for v in variants],f'{relative}/{key} metadata')
                model=merge_valt_records(key,parse_binary_valt(original[1]),{n:parse_binary_valt(m[key][1])for n,m in sources.items()})
                chosen=metadata,compile_binary_valt(model)
            else:raise ValueError(f'Conflicting archive resource: {relative}/{key}')
        else:chosen=changes[0]if changes else original
        if original is not None and chosen!=original and not(relative=='res/common.res'and key in TABLES):
            raise ValueError(f'Native payload modification has no semantic adapter: {relative}/{key}')
        if chosen is not None:
            final[key]=chosen
            metadata,raw=chosen;manifest['entries'].append(metadata);(staged/metadata['name']).write_bytes(raw)
    (staged/'manifest.json').write_text(json.dumps(manifest,indent=2));pack_archive(str(staged),str(target))
    report['archives'][relative]={'native_entry_count':len(base),'output_entry_count':len(manifest['entries']),
        'native_payloads_byte_preserved':sum(final.get(k)==v for k,v in base.items())}
    return base,final


def _validate(common_path,scratch,maps):
    _,entries=_archive(common_path,scratch)
    models={k:parse_binary_valt(entries[k][1])for k in TABLES}
    baseline={k:parse_binary_valt(maps['res/common.res'][0][k][1])for k in TABLES}
    rows={};native_rows={};counts=Counter();native_counts=Counter()
    for filename in ('dirt.val','buycar.val'):
        for source,destination,counter in [(baseline,native_rows,native_counts),(models,rows,counts)]:
            for key,section in _sections(filename,source[filename]).items():
                if section['name']not in('Driver','vehicle'):continue
                values=dict(section['pairs']);number=str(int(values['number']));chassis=values['chassis'].casefold()
                if not 0<=int(number)<1000:raise ValueError('Vehicle number exceeds native limit')
                if number in destination:raise ValueError('Duplicate final numeric vehicle ID')
                destination[number]=(chassis,section);counter[chassis]+=1
    for number,record in native_rows.items():
        if rows.get(number)!=record:raise ValueError(f'Native driver/dealer record changed: {number}')
    for chassis,count in counts.items():
        if count>10 and (count!=native_counts[chassis] or
            {n for n,(c,_)in rows.items()if c==chassis}!={n for n,(c,_)in native_rows.items()if c==chassis}):
            raise ValueError(f'Native per-chassis paint capacity exceeded: {chassis} {count}')
    for section in models['carclass.val']['sections']:
        if section['name'].startswith('$')and len(section['pairs'])>20:raise ValueError('Native class bucket capacity exceeded')
    classes={k.casefold()for s in models['carclass.val']['sections']if s['name'].startswith('$')for k,_ in s['pairs']}
    stats={dict(s['pairs']).get('chassis','').casefold()for s in models['car_stats.val']['sections']}
    for number,(chassis,section)in rows.items():
        if number not in native_rows and(chassis not in classes or chassis not in stats):raise ValueError('New vehicle has no class/stats')
        for archive in ('res/ct.res','res/uicar.res'):
            before,after=maps[archive]
            for suffix in ('.tex','.tm0'):
                candidates=[number+suffix]if archive=='res/ct.res'else['uicar_'+number+suffix,'uicars'+number+suffix]
                present=any(n in after for n in candidates)
                old_present=any(n in before for n in candidates)
                if not present and(number not in native_rows or old_present):
                    raise ValueError(f'Missing paint asset: {archive} vehicle {number}{suffix}')
    from create_custom_car import car_archive_filename
    data_root=common_path.parent.parent
    for chassis in {c for n,(c,_)in rows.items()if n not in native_rows}:
        try:_find_case(data_root,Path('CAR')/car_archive_filename(chassis))
        except ValueError as error:raise ValueError(f'New chassis archive is missing: {chassis}')from error
    classes_model=models['carclass.val'];base_classes=baseline['carclass.val']
    if classes_model['unk1']!=base_classes['unk1']or classes_model['sections'][:classes_model['unk1']]!=base_classes['sections'][:base_classes['unk1']]:
        raise ValueError('Native four-class roots changed without a runtime adapter')
    own={n:rows[str(n)][0]for n in REGISTRY.values()}
    if own!={775:'phantom',776:'phantom',777:'cupcar',778:'cupcar',779:'cupcar'}:raise ValueError('Incomplete integrated paint registry')
    return {'all_chassis_counts':dict(sorted(counts.items())),
        'unchanged_native_capacity_exceptions':{c:n for c,n in native_counts.items()if n>10},
        'new_record_count':len(rows)-len(native_rows),'native_record_count':len(native_rows)}


BATTLE_LAYOUTS=frozenset({'c_battle.va0'})


def battle_layout_entries(native_archive,battle_archive,work):
    """Battle track archives may only add Battle layout tables to retail ones.

    The retail archive stays the one the game streams; the runtime serves the
    added table only to a Battle contest (src/battle_events.cpp, 0x1515B8).
    Any dropped or modified native entry is refused rather than shipped.
    """
    native_dir,battle_dir=Path(work)/'native',Path(work)/'battle'
    unpack_archive(str(native_archive),str(native_dir));unpack_archive(str(battle_archive),str(battle_dir))
    names=lambda d:{e['name']for e in json.loads((d/'manifest.json').read_text())['entries']}
    native,battle=names(native_dir),names(battle_dir)
    if not native<=battle:raise ValueError('Battle track archive drops native entries: '+', '.join(sorted(native-battle)))
    for name in sorted(native):
        if (native_dir/name).read_bytes()!=(battle_dir/name).read_bytes():
            raise ValueError('Battle track archive modifies native entry: '+name)
    added=battle-native
    if not added or not added<=BATTLE_LAYOUTS:raise ValueError('Battle track archive adds non-layout entries: '+', '.join(sorted(added)))
    return {name:(battle_dir/name).read_bytes()for name in sorted(added)}


def build_integrated_profile(disc,profiles,output):
    disc,output=Path(disc).resolve(),Path(output).resolve();profiles={n:Path(p).resolve()for n,p in profiles.items()}
    if set(profiles)!={'phantom','battle','cupcar'}:raise ValueError('Expected accepted Phantom, Battle and Cup profiles')
    roots=[disc,*profiles.values(),output]
    for i,path in enumerate(roots):
        for other in roots[i+1:]:
            if path==other or path in other.parents or other in path.parents:raise ValueError('Source/output trees must be distinct and not nested')
    if output.exists():raise FileExistsError('Integrated output already exists')
    inventories={n:_inventory(p)for n,p in profiles.items()};native=_data_root(disc)
    report={'profile':'integrated','runtime_activated':False,'registry':REGISTRY.copy(),'actions':[],'archives':{},'files':[],
        'save_migration':'Legacy IDs are ambiguous without originating profile/chassis; no save file modified',
        'acceptance_blockers':['controller frontend and default activation','prototype-save compatibility audit','integrated runtime regression']}
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.integrated-',dir=output.parent)as temporary:
        scratch=Path(temporary);staged=scratch/'profile';staged.mkdir();maps={}
        for index,key in enumerate(sorted(set().union(*(m.keys()for m in inventories.values())))):
            variants={n:m[key][1]for n,m in sorted(inventories.items())if key in m}
            relative=next(m[key][0]for _,m in sorted(inventories.items())if key in m)
            # Battle must be strictly additive: a track archive never replaces the native one.
            # Only the Battle layout tables it adds are kept, beside the overlay, for the
            # runtime to serve to Battle contests (battle-alternates/TRACKS/<TRACK>/<table>).
            if relative.parts[0].upper()=='TRACKS':
                if set(variants)!={'battle'}:raise ValueError(f'Unexpected track archive owner: {relative}')
                work=scratch/str(index);work.mkdir()
                for name,raw in battle_layout_entries(_find_case(native,relative),variants['battle'],work).items():
                    target=staged/'battle-alternates'/'TRACKS'/relative.stem.upper()/name
                    target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
                    report['files'].append(dict(path=target.relative_to(staged).as_posix(),sha256=hashlib.sha256(raw).hexdigest()))
                continue
            target=staged/'DATA'/relative;target.parent.mkdir(parents=True,exist_ok=True)
            if relative.suffix.lower()=='.res':
                baseline=_find_case(native,relative);work=scratch/str(index);work.mkdir()
                maps[key]=_merge_archive(key,baseline,variants,target,work,report)
            else:
                values={p.read_bytes()for p in variants.values()}
                if len(values)!=1:raise ValueError(f'Conflicting runtime file: {relative}')
                shutil.copy2(next(iter(variants.values())),target)
            report['files'].append(dict(path='DATA/'+relative.as_posix(),sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
        report['registry_validation']=_validate(staged/'DATA/RES/COMMON.RES',scratch/'validated',maps)
        (staged/'composition-report.json').write_text(json.dumps(report,indent=2)+'\n')
        _publish_exclusively(staged,output)
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('disc','phantom','battle','cupcar','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();report=build_integrated_profile(a.disc,{n:getattr(a,n)for n in ('phantom','battle','cupcar')},a.output)
    print(f'Prepared integrated resources with {len(report["files"])} files; no runtime activation or save migration')

if __name__=='__main__':main()
