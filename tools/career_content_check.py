#!/usr/bin/env python3
"""Career dealer content check (docs/CAREER_UNLOCKS.md).

The Career dealer (AutoShop, SLUS_209.10 0x2E2FC0) builds its stock from buycar.val: every [car] is a
chassis with a price, every [vehicle] is a paint job of a chassis. The game panics ("No matching chassis
to %s from buy list") when a [vehicle] names a chassis with no [car], and the Upgrades (catalog.val) are
per car class (carclass.val), so a dealer chassis in no class has no upgrades. Imported cars (Phantom, the
NASCAR Cup car, later NASCARs) must keep both invariants.

  python3 tools/career_content_check.py <COMMON.RES | folder holding DATA/RES/COMMON.RES>
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from valt import parse_binary_valt  # noqa: E402


def _pairs(section):
    return dict((k.lower(), v) for k, v in section['pairs'])


def check_dealer_content(buycar, carclass=None, catalog=None):
    """Problems found in decompiled buycar.val (and carclass.val / catalog.val when given)."""
    problems = []
    cars, vehicles = {}, []
    for s in buycar['sections']:
        v = _pairs(s)
        if s['name'] == 'car':
            name = v.get('chassis', '')
            if not name:
                problems.append('[car] without a chassis')
            elif name.lower() in cars:
                problems.append('chassis %s has two [car] sections' % name)
            else:
                cars[name.lower()] = dict(v, _name=name)
        elif s['name'] == 'vehicle':
            vehicles.append(v)
    numbers = set()
    for v in vehicles:
        chassis = v.get('chassis', '')
        if chassis.lower() not in cars:
            problems.append('[vehicle] %s names chassis "%s", which has no [car]' % (v.get('number', '?'), chassis))
        if v.get('number') in numbers:
            problems.append('vehicle number %s is listed twice' % v.get('number'))
        numbers.add(v.get('number'))
    for name, v in cars.items():
        if not any(x.get('chassis', '').lower() == name for x in vehicles):
            problems.append('chassis %s has a [car] but no [vehicle] (the dealer can never show it)' % v['_name'])
        if not v.get('price', '').isdigit():
            problems.append('chassis %s has no numeric price' % v['_name'])
    if carclass is not None:
        classed = {}
        for s in carclass['sections']:
            if s['name'].startswith('$') and s['name'].endswith('$chassis'):
                for k, _ in s['pairs']:
                    classed[k.lower()] = s['name']
        classes = [s for s in carclass['sections'] if not s['name'].startswith('$')]
        names = [s['name'].lower() for s in classes]
        for name, v in cars.items():
            if name not in classed:
                problems.append('chassis %s is in no car class (carclass.val): no upgrades, wrong Eve class' % v['_name'])
        if catalog is not None:
            have = set()
            for s in catalog['sections']:
                c = _pairs(s).get('car_class')
                if c:
                    have.add(c.lower())
            for name, v in cars.items():
                section = classed.get(name)
                if section is None:
                    continue
                idx = int(section[1:5]) if section[1:5].isdigit() else -1
                if 0 <= idx < len(names) and names[idx] in ('compact', 'midsize', 'muscle') and names[idx] not in have:
                    problems.append('chassis %s: class %s has no part in catalog.val' % (v['_name'], names[idx]))
    return problems


def _load_common_res(path):
    from rst0 import unpack_archive  # noqa: F401  (the 0TSR container)
    import rst0
    if os.path.isdir(path):
        path = os.path.join(path, 'DATA', 'RES', 'COMMON.RES')
    out = {}
    with tempfile.TemporaryDirectory() as tmp:
        rst0_main = getattr(rst0, 'cmd_unpack', None)
        if rst0_main is None:
            # the command line interface (rst0.py unpack <archive> <outdir>)
            import subprocess
            subprocess.check_call([sys.executable, os.path.join(os.path.dirname(__file__), 'rst0.py'), 'unpack', path, tmp],
                                  stdout=subprocess.DEVNULL)
        else:
            rst0_main(path, tmp)
        for name in ('buycar.val', 'carclass.val', 'catalog.val'):
            with open(os.path.join(tmp, name), 'rb') as f:
                out[name] = parse_binary_valt(f.read())
    return out


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    vals = _load_common_res(argv[1])
    problems = check_dealer_content(vals['buycar.val'], vals['carclass.val'], vals['catalog.val'])
    for p in problems:
        print('PROBLEM:', p)
    n_car = sum(1 for s in vals['buycar.val']['sections'] if s['name'] == 'car')
    n_veh = sum(1 for s in vals['buycar.val']['sections'] if s['name'] == 'vehicle')
    print('%d chassis, %d vehicles, %d problems' % (n_car, n_veh, len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
