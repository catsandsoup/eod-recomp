import os, struct, sys
# Walks every model in the retail data for the vif_mesh reference dump (dump_ref.py).
# EOD_DISC = the extracted disc folder (the one containing DATA/).
TOOLS = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sys.path.insert(0, TOOLS)
from pathlib import Path
import modding_backend as mb
import world_graf as wg
import vif_mesh as vm
DISC = Path(os.environ.get('EOD_DISC', '.'))

def archives():
    out = []
    for root, dirs, files in os.walk(DISC / 'DATA'):
        dirs.sort()
        for f in sorted(files):
            if f.upper().endswith(('.CAR', '.RES', '.TRK')):
                out.append(Path(root) / f)
    return out

def members(path):
    """yield (name, type, bytes)"""
    ents = mb.directory(path)
    with open(path, 'rb') as h:
        for e in ents:
            if e['type'] in ('MODL', 'GRAF'):
                h.seek(e['offset']); yield e['name'], e['type'], h.read(e['size'])

def models(data, typ):
    """yield (model_label, parts list[dict offset,size,texture,group,slot])"""
    if typ == 'MODL':
        m = vm.parse_modl(data)
        yield 'modl', m['parts']
    else:
        g = wg.parse_graf(data)
        seen = set()
        for lf in g['leaves']:
            mo = lf.get('model_offset')
            if mo is None or mo in seen: continue
            seen.add(mo)
            m = wg.parse_model(data, mo)
            yield 'mr@%x' % mo, m['parts']
