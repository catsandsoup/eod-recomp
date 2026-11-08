#!/usr/bin/env python3
"""Career dealer stock for the special vehicles (docs/CAREER_UNLOCKS.md, "Special vehicles").

Retail has six special vehicles (Postal, Taxi, Ambulance, Copcar, Hearse, Bus) that only exist as AI drivers
and in a few Special-class Eves; the Career dealer never sells them and, being in the Special class, they
could not enter a Compact / Midsize / Muscle Eve. This adds, to an existing integrated content profile, one
Career car for each:

* a new chassis (a byte-for-byte copy of the retail DATA/CAR file under a new name, so the original chassis
  stays in the Special class and its Action and Kenyon events are untouched) with the SAME physics, damage and
  model; nothing is retuned;
* a [car] (price inside the price band of the native cars of similar performance) and a [vehicle] in
  buycar.val, a [carstats] row (the ratings the dealer shows, copied from the original) in car_stats.val, and
  the chassis in an EXISTING car class of carclass.val (the class of its closest native peers);
* the original vehicle's paint and dealer art (CT.RES, UICAR.RES) under the new vehicle number.

  python3 tools/build_career_specials.py --disc <retail disc folder> --base <integrated profile> --output <new profile>

Offline only: nothing installed, no save touched. Retail data never enters the repository.
"""
import argparse
import copy
import json
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_mod_profiles import _archive, _data_root, _find_case, _publish_exclusively  # noqa: E402
from create_custom_car import car_archive_filename  # noqa: E402
from rst0 import pack_archive  # noqa: E402
from valt import compile_binary_valt, parse_binary_valt  # noqa: E402

# base chassis -> the Career car. Peers and prices: docs/CAREER_UNLOCKS.md (performance from the retail
# physics.val: mass, engine power; ratings from car_stats.val). art_from: the retail vehicle whose paint and
# dealer art is copied. Vehicle numbers 780-785 are unused in retail and in the integrated profile.
SPECIALS = [
    dict(base='Bus', name='Schoolbus', cls='Midsize', price=280, number=785, art_from=790, color='240, 170, 0',
         peers='Ocelot 230, Desio 325'),
    dict(base='Hearse', name='Funeral', cls='Midsize', price=800, number=784, art_from=502, color='20, 20, 20',
         peers='Hunter 630, Arrow 1000'),
    dict(base='Ambulance', name='Medic', cls='Midsize', price=1100, number=782, art_from=601, color='230, 230, 230',
         peers='Digit 885, Fillmore 1250'),
    dict(base='Taxi', name='Cabbie', cls='Midsize', price=1600, number=781, art_from=508, color='230, 190, 20',
         peers='Fillmore 1250, Empire 1700'),
    dict(base='Postal', name='Mailvan', cls='Compact', price=1800, number=780, art_from=503, color='200, 200, 215',
         peers='Digit 885, Bulldog 2399, Hurricane 3000'),
    dict(base='Copcar', name='Patrol', cls='Muscle', price=3500, number=783, art_from=504, color='20, 20, 60',
         peers='Hurricane 3000, Mongoose 3900'),
]
CLASS_SECTION = {'Compact': '$0000$chassis', 'Midsize': '$0001$chassis', 'Muscle': '$0002$chassis'}


def _pairs(section):
    return dict(section['pairs'])


def add_specials(buycar, carclass, car_stats, specials=SPECIALS):
    """The three tables with the Career cars added (models are copied; the inputs are untouched)."""
    buycar, carclass, car_stats = copy.deepcopy(buycar), copy.deepcopy(carclass), copy.deepcopy(car_stats)
    template = next(s for s in buycar['sections'] if s['name'] == 'vehicle')
    cars = {_pairs(s).get('chassis', '').casefold() for s in buycar['sections'] if s['name'] == 'car'}
    numbers = {_pairs(s).get('number') for s in buycar['sections'] if s['name'] == 'vehicle'}
    stats = {_pairs(s).get('chassis', '').casefold(): s for s in car_stats['sections']}
    classes = {s['name']: s for s in carclass['sections']}
    for sp in specials:
        if sp['name'].casefold() in cars or str(sp['number']) in numbers:
            raise ValueError('%s / %d already exist in buycar.val' % (sp['name'], sp['number']))
        if sp['base'].casefold() not in stats:
            raise ValueError('no car_stats row for %s' % sp['base'])
        buycar['sections'].append(dict(template, name='car', pairs=[('chassis', sp['name']), ('price', str(sp['price']))]))
        buycar['sections'].append(dict(template, name='vehicle', pairs=[
            ('body_color', sp['color']), ('chassis', sp['name']), ('number', str(sp['number']))]))
        row = copy.deepcopy(stats[sp['base'].casefold()])
        row['pairs'] = [('chassis', sp['name'])] + [(k, v) for k, v in row['pairs'] if k != 'chassis']
        car_stats['sections'].append(row)
        classes[CLASS_SECTION[sp['cls']]]['pairs'].append((sp['name'], ''))
    buycar['root_count'] = len(buycar['sections'])
    car_stats['root_count'] = len(car_stats['sections'])
    carclass['root_count'] = carclass['unk1']  # the four native class roots
    return buycar, carclass, car_stats


def _add_art(entries, source_number, number, prefixes):
    """Copy the paint / dealer art of vehicle source_number to vehicle number (entries: name -> (meta, raw))."""
    added = []
    for prefix in prefixes:
        for suffix in ('.tex', '.tm0'):
            old, new = prefix + str(source_number) + suffix, prefix + str(number) + suffix
            if old.casefold() not in entries:
                raise ValueError('missing art %s' % old)
            if new.casefold() in entries:
                raise ValueError('art %s already exists' % new)
            meta, raw = entries[old.casefold()]
            meta = dict(meta, name=new)
            entries[new.casefold()] = (meta, raw)
            added.append(new)
    return added


def _write_archive(manifest, entries, original_order, target, scratch):
    staged = Path(scratch)
    staged.mkdir(parents=True)
    out = {k: manifest[k] for k in ('version', 'build_time', 'unk') if k in manifest}
    out['entries'] = []
    for key in original_order + [k for k in entries if k not in original_order]:
        meta, raw = entries[key]
        out['entries'].append(meta)
        (staged / meta['name']).write_bytes(raw)
    (staged / 'manifest.json').write_text(json.dumps(out, indent=2))
    pack_archive(str(staged), str(target))


def build(disc, base, output, specials=SPECIALS):
    disc, base, output = Path(disc).resolve(), Path(base).resolve(), Path(output).resolve()
    native, data = _data_root(disc), _data_root(base)
    if output.exists():
        raise FileExistsError('output already exists: %s' % output)
    report = dict(specials=[dict(sp) for sp in specials], art={})
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.specials-', dir=output.parent) as tmp:
        tmp = Path(tmp)
        staged = tmp / 'profile'
        shutil.copytree(base, staged, symlinks=False, ignore=shutil.ignore_patterns('composition-report.json'))
        sdata = _data_root(staged)
        # COMMON.RES: the three tables
        common = _find_case(sdata, Path('RES/COMMON.RES'))
        manifest, entries = _archive(common, tmp / 'common_in')
        order = list(entries)
        models = {k: parse_binary_valt(entries[k][1]) for k in ('buycar.val', 'carclass.val', 'car_stats.val')}
        buycar, carclass, car_stats = add_specials(models['buycar.val'], models['carclass.val'], models['car_stats.val'], specials)
        for name, model in (('buycar.val', buycar), ('carclass.val', carclass), ('car_stats.val', car_stats)):
            entries[name] = (entries[name][0], compile_binary_valt(model))
        _write_archive(manifest, entries, order, common, tmp / 'common_out')
        # paint and dealer art
        for archive, prefixes in (('RES/CT.RES', ['']), ('RES/UICAR.RES', ['uicar_', 'uicars_'])):
            path = _find_case(sdata, Path(archive))
            manifest, entries = _archive(path, tmp / (archive + '_in').replace('/', '_'))
            order = list(entries)
            report['art'][archive] = [a for sp in specials for a in _add_art(entries, sp['art_from'], sp['number'], prefixes)]
            _write_archive(manifest, entries, order, path, tmp / (archive + '_out').replace('/', '_'))
        # the chassis archives: the retail file under the new name
        for sp in specials:
            source = _find_case(native, Path('CAR') / car_archive_filename(sp['base']))
            target = sdata / 'CAR' / car_archive_filename(sp['name'])
            if target.exists():
                raise ValueError('%s already exists' % target)
            shutil.copyfile(source, target)
        (staged / 'career-specials-report.json').write_text(json.dumps(report, indent=2) + '\n')
        _publish_exclusively(staged, output)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('disc', 'base', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    report = build(a.disc, a.base, a.output)
    print('Added %d Career special vehicles to %s' % (len(report['specials']), a.output))


if __name__ == '__main__':
    main()
