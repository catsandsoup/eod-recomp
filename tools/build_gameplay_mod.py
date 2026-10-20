#!/usr/bin/env python3
"""Build opt-in Battle catalogs and selected native dynamic-barrier overlays.

Requires a legally installed SLUS_209.10 disc extraction. Generated archives
remain local; this script never writes the disc or claims live acceptance.
"""
import argparse
import contextlib
import copy
import hashlib
import io
import json
import math
import os
from pathlib import Path
import tempfile

from rst0 import unpack_archive, pack_archive
from trk import decompile_ild
from valt import parse_binary_valt, compile_binary_valt

TRACKS = ('Afton', 'Anoka', 'Baxter', 'Bemidji', 'Dennison', 'Dundas', 'Eagan',
          'Gilroy', 'Hastings', 'HomeBase', 'Kenyon', 'Mankato', 'Springfield', 'Welch')


def section(name, pairs):
    return {'name': name, 'parent': '', 'sec_vtable': 0x40C0D8,
            'tool_ptr': 0, 'pairs': pairs}


def battle_catalog(model, track):
    """Use race+Battle in Action, Battle in MP, inside native capacities.

    Keep Career/challenge Eve definitions and their dependencies. The optional
    mod intentionally reduces Action to race+Battle and MP to Battle;
    appending to the 312-contest Action catalog cannot fit all fourteen maps.
    """
    native_race = next((s for s in model['sections']
                        if dict(s['pairs']).get('event_type') == 'normal_race'), None)
    if native_race is None:
        raise ValueError('A native first-race contest is required by the solo Eve loader')
    native_battle = next((s for s in model['sections']
                          if dict(s['pairs']).get('event_type') == 'battle'), None)
    battle_pairs = dict(native_battle['pairs']) if native_battle else {}
    # A newly enabled arena must not silently lose the native course's ground
    # mask. Resolve only terrain fields through native parent tables, rather
    # than copying race-only behavior into a Battle descriptor.
    terrain_source = native_battle or native_race
    seen = set()
    while terrain_source:
        identity = id(terrain_source)
        if identity in seen:
            raise ValueError('Cyclic native contest inheritance')
        seen.add(identity)
        for key, value in terrain_source['pairs']:
            if key in ('terrain_mask', 'terrain_disable_mask', 'opt_terrain'):
                battle_pairs.setdefault(key, value)
        parent = terrain_source.get('parent', '')
        terrain_source = next((s for s in model['sections'] if parent and s['name'] == parent), None)
    battle_pairs.update(event_type='battle', external_table='c_battle', max_cars='4',
                        laps='1', rounds='1', purse='500')
    out = copy.deepcopy(model)
    original = out['sections']
    root_count = int(model.get('root_count', model.get('unk1', len(original))))
    if not 0 <= root_count <= len(original):
        raise ValueError('Invalid native VAL root count')
    def keep(s):
        return not (s['name'] == 'eve' and
                    dict(s['pairs']).get('mode', '') in ('', 'multi')) and s['name'] not in ('$9999$contest', '$9998$contest')
    roots = [s for s in original[:root_count] if keep(s)]
    children = [s for s in original[root_count:] if keep(s)]
    native_eve = next((s for s in original[:root_count]
                       if s['name'] == 'eve' and dict(s['pairs']).get('mode', '') == ''
                       and dict(s['pairs']).get('hidden', '').casefold() != 'yes'), None)
    metadata = [(k, v) for k, v in native_eve['pairs']
                if k not in ('@contest', 'mode', 'hidden')] if native_eve else []
    existing = {k.casefold() for k, _ in metadata}
    for key, value in [('track', track), ('name', track), ('date', '8/2'),
                       ('time', '12:00'), ('league', 'Dirt')]:
        if key.casefold() not in existing:
            metadata.append((key, value))
    pairs = [('@contest', '$9999$contest')] + metadata
    solo_pairs = [('@contest', '$9998$contest')] + pairs
    roots.extend([section('eve', solo_pairs), section('eve', pairs + [('mode', 'multi')])])
    race = copy.deepcopy(native_race)
    race['name'] = '$9998$contest'
    children.append(race)
    children.append(section('$9999$contest', list(battle_pairs.items())))
    out['sections'] = roots + children
    # The native root enumerator only sees the first header+8 sections. New
    # Eves must precede anonymous child tables, with an explicit root count.
    out['unk1'] = len(roots)
    out['root_count'] = len(roots)
    return out


def respawn_layout(model):
    """Derive eight candidate points from the native primary 2D ILIN course.

    Native 13AF20 / battle_navigation.cpp establish x,z at +0,+4 and tangent
    x,z at +8,+12, count in header +1C high halfword. Battle's 1D8F58 performs
    native ground placement; we supply no guessed world Y or terrain mesh.
    """
    count = int(model.get('flags', 0)) >> 16
    nodes = model.get('nodes', [])
    if model.get('node_size') != 60 or not 8 <= count <= len(nodes):
        raise ValueError('Expected at least eight primary native ILIN nodes')
    points = []
    for i in range(8):
        node = nodes[i * count // 8]
        x, z, dx, dz = node[:4]
        length = math.hypot(dx, dz)
        if not all(math.isfinite(n) for n in (x, z, dx, dz)) or length < 1e-6:
            raise ValueError('Non-finite respawn position or invalid native tangent')
        if any(math.hypot(x-p[0], z-p[1]) < 5 for p in points):
            raise ValueError('Respawn points lack five metres of planar separation')
        points.append((x, z, dx/length, dz/length))
    references = [(f'@eod{i}', f'${9900+i:04d}$eod{i}') for i in range(8)]
    sections = [section('respawn', references)]
    for i, (x, z, dx, dz) in enumerate(points):
        sections.append(section(f'${9900+i:04d}$eod{i}', [('pt', f'{x:.9g}, {z:.9g}'),
                                                   ('dir', f'{dx:.9g}, {dz:.9g}')]))
    return {'tool_vtable': 0x40C218, 'root_count': 1, 'sections': sections}


def remove_layout_objects(model, dynos):
    """Remove references to selected native dynos, preserving the other authored references.

    This handles authored dynamic obstacles, not static SOBL collision geometry.
    Unreferenced object sections stay intact so other references remain valid.
    """
    out = copy.deepcopy(model)
    selected = {d.casefold() for d in dynos}
    names = {s['name'] for s in out['sections']
             if dict(s['pairs']).get('dyno', '').casefold() in selected}
    removed = 0
    for s in out['sections']:
        kept = []
        for key, value in s['pairs']:
            if key == '@object' and value in names:
                removed += 1
            else:
                kept.append((key, value))
        s['pairs'] = kept
    return out, removed


def read_val(path):
    return parse_binary_valt(path.read_bytes())


def build(disc, output, battle=False, remove_dynos=()):
    disc, output = Path(disc).resolve(), Path(output).resolve()
    if output.exists():
        raise ValueError('Output already exists; select a fresh mod directory')
    if output == disc or disc in output.parents or output in disc.parents:
        raise ValueError('Mod output must be separate from the installed disc')
    if not battle and not remove_dynos:
        raise ValueError('Choose --battle-catalog and/or --remove-dennison-dyno')
    report = {'status': 'generated; live player acceptance pending', 'battle_catalog': battle,
              'tracks': [], 'sources': {}, 'removed_dynamic_objects': 0,
              'limits': ['Static SOL/BPP fences are unchanged.',
                         'Catalog reduces Action to race+Battle and MP to Battle; Career definitions remain.',
                         'Invincibility prevents Battle damage kills; disable it for ordinary scoring.']}
    output.parent.mkdir(parents=True, exist_ok=True)
    # Build completely in a temporary sibling. Failed builds cannot leave an
    # accidentally selectable partial mod root.
    with tempfile.TemporaryDirectory(prefix='.eod-gameplay-', dir=output.parent) as temporary:
        stage = Path(temporary) / 'mod'
        stage.mkdir()
        scratch = Path(temporary) / 'unpack'
        scratch.mkdir()

        def unpack(source, target):
            report['sources'][str(source.relative_to(disc))] = hashlib.sha256(source.read_bytes()).hexdigest()
            with contextlib.redirect_stdout(io.StringIO()):
                unpack_archive(str(source), str(target))

        def pack(source, target):
            target.parent.mkdir(parents=True, exist_ok=True)
            with contextlib.redirect_stdout(io.StringIO()):
                pack_archive(str(source), str(target))

        common = scratch / 'common'
        if battle:
            unpack(disc / 'DATA/RES/COMMON.RES', common)
            for track in TRACKS:
                table = common / f'{track.lower()}_eves.val'
                table.write_bytes(compile_binary_valt(battle_catalog(read_val(table), track)))
            pack(common, stage / 'DATA/RES/COMMON.RES')

        for track in TRACKS if battle else ('Dennison',):
            directory = scratch / track
            archive_name = 'SPRING_1.TRK' if track == 'Springfield' else f'{track.upper()}.TRK'
            unpack(disc / f'DATA/TRACKS/{archive_name}', directory)
            changed = False
            row = {'track': track, 'respawns': 'native', 'removed_dynamic_objects': 0}
            layout = directory / 'c_battle.va0'
            if battle and not layout.exists():
                layout.write_bytes(compile_binary_valt(respawn_layout(decompile_ild(str(directory / 'track.ild')))))
                manifest = directory / 'manifest.json'
                data = json.loads(manifest.read_text())
                data['entries'].append({'name': 'c_battle.va0', 'fourcc': 'VALT',
                                        'version': 11, 'is_extractable': True})
                manifest.write_text(json.dumps(data, indent=2))
                changed = True
                row['respawns'] = 'eight candidate primary course points; native grounding unverified live'
            if track == 'Dennison' and remove_dynos:
                for candidate in directory.glob('c_*.va0'):
                    updated, count = remove_layout_objects(read_val(candidate), remove_dynos)
                    if count:
                        candidate.write_bytes(compile_binary_valt(updated))
                        row['removed_dynamic_objects'] += count
                        changed = True
                report['removed_dynamic_objects'] += row['removed_dynamic_objects']
            if changed:
                pack(directory, stage / f'DATA/TRACKS/{archive_name}')
            report['tracks'].append(row)
        (stage / 'gameplay-mod-report.json').write_text(json.dumps(report, indent=2) + '\n')
        os.rename(stage, output)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', required=True)
    parser.add_argument('--output', required=True, help='New, separate EOD_MOD_DIR root')
    parser.add_argument('--battle-catalog', action='store_true', help='One native race plus Battle in Action, Battle-only MP on all 14 maps')
    parser.add_argument('--remove-dennison-dyno', action='append', default=[], metavar='NAME',
                        help='Explicit native dynamic obstacle name, repeatable (e.g. Semi_Trailer)')
    args = parser.parse_args()
    try:
        report = build(args.disc, args.output, args.battle_catalog, args.remove_dennison_dyno)
    except (ValueError, OSError) as error:
        parser.exit(1, f'Error: {error}\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
