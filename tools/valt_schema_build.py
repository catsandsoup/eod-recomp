"""Rebuilds tools/formats/valt_schema_data.json from the retail disc and SLUS_209.10.

usage: python3 tools/valt_schema_build.py DISC_DIR [OUT.json]

Records, for every (family, section pattern, key pattern): how many times it occurs, the retail
numeric value range, and which executable functions reference the key text (lui/addiu
string references, data-table pointers and printf-style variants such as g1 -> "g%d").
"""
import json,re,sys,struct,bisect,collections
from pathlib import Path
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE)); sys.path.insert(0,str(HERE/'formats'))
import modding_backend as mb
from valt import parse_binary_valt
from valt_schema import family_of


def iter_valts(disc):
    """Yield (archive, member, parsed table) for every VALT member of every archive under DATA/."""
    for path in sorted((Path(disc) / "DATA").rglob("*")):
        if path.suffix.upper() not in (".CAR", ".RES", ".TRK") or not path.is_file():
            continue
        try:
            entries = mb.directory(path)
        except Exception:
            continue
        for e in entries:
            if e["type"] != "VALT":
                continue
            try:
                data, _ = mb.payload(path, e["name"], entries, verify_crc=False)
                yield str(path.relative_to(disc)), e["name"], parse_binary_valt(data)
            except Exception:
                continue


def main():
    disc=Path(sys.argv[1]); out=sys.argv[2] if len(sys.argv)>2 else str(HERE/'formats'/'valt_schema_data.json')
    exe=(disc/'SLUS_209.10').read_bytes()
    BASE=0x100000-0x1000
    words=struct.unpack('<%dI'%(len(exe)//4),exe)
    TEXT_LO,TEXT_HI=0x100000,0x3d8000
    idx=lambda va:(va-BASE)//4
    refs=collections.defaultdict(set); lui={}
    for va in range(TEXT_LO,TEXT_HI,4):
        w=words[idx(va)];op=w>>26;rs=(w>>21)&31;rt=(w>>16)&31;imm=w&0xffff
        if op==0x0f: lui[rt]=(imm<<16,va)
        elif op in (0x09,0x0d) and rs in lui and va-lui[rs][1]<4096:
            si=imm-0x10000 if (op==9 and imm>=0x8000) else imm
            refs[((lui[rs][0]+si)&0xffffffff) if op==9 else lui[rs][0]|imm].add(va)
        elif op in (0x23,0x21,0x31,0x25,0x24) and rs in lui and va-lui[rs][1]<4096:
            si=imm-0x10000 if imm>=0x8000 else imm
            refs[(lui[rs][0]+si)&0xffffffff].add(va)
    starts=[va for va in range(TEXT_LO,TEXT_HI,4) if (words[idx(va)]>>16)==0x27bd and (words[idx(va)]&0x8000)]
    func=lambda va:starts[bisect.bisect_right(starts,va)-1]
    dataptr=set(words[i] for i in range(0,len(words)) if 0x3d0000<=words[i]<0x4f0000 and (i*4+BASE)>=0x3d0000)
    # all strings
    strings=[]
    for m in re.finditer(rb'[\x20-\x7e]{2,}\x00',exe[0x3d0000-BASE:0x4f0000-BASE]):
        strings.append((0x3d0000+m.start(),m.group()[:-1].decode('latin1')))
    def strrefs(va):
        fs={func(r) for r in refs.get(va,())}
        return fs,(va in dataptr)
    fmt=[(va,s) for va,s in strings if '%' in s and len(s)<40]
    def trace(key):
        """-> (used, funcs) ; used True/False/None"""
        k=key.strip()
        fs=set();data=False;present=False
        # The game's key lookup (0x135C80) compares with a case-insensitive strcmp (0x3A72D8).
        kl=k.lower()
        cands=[(va,s) for va,s in strings if s.lower()==kl or (len(k)>=4 and s.lower().endswith(kl))]
        # format variants:  g1 -> g%d
        gen=re.sub(r'\d+','%d',k)
        for va,s in fmt:
            if gen!=k and (s.lower()==gen.lower() or s.replace('%0.2d','%d').replace('%2d','%d').lower()==gen.lower()): cands.append((va,s))
        for va,s in cands:
            present=True
            f,dp=strrefs(va); fs|=f; data|=dp
            # tail address of a suffix match is va + len(s)-len(k): refs to the exact tail
            if s.lower()!=kl and s.lower().endswith(kl):
                f,dp=strrefs(va+len(s)-len(k)); fs|=f; data|=dp
        # "%s%d" readers: speedometer0 is looked up as ("speedometer", 0)
        tail=re.match(r'^(.*?[A-Za-z_])(\d+)$',k)
        if not (fs or data) and tail and any(s=='%s%d' for _,s in fmt):
            for va,s in strings:
                if s.lower()==tail.group(1).lower():
                    present=True; f,dp=strrefs(va); fs|=f; data|=dp
        if fs or data: return True,sorted(fs)
        return (None if present else False),[]
    rows=[];cache={}
    fam_keys=collections.defaultdict(lambda:collections.defaultdict(list))
    concrete=collections.defaultdict(lambda:collections.defaultdict(set))
    for arch,member,t in iter_valts(disc):
        fam=family_of(member)
        for s in t['sections']:
            sp=re.sub(r'\d+','#',s['name'])
            for k,v in s['pairs']:
                fam_keys[fam][(sp,re.sub(r'\d+','#',k))].append(v)
                concrete[fam][(sp,re.sub(r'\d+','#',k))].add(k.strip().lstrip('@'))
    res={'version':1,'families':{}}
    for fam,ks in fam_keys.items():
        d={}
        for (sp,kp),vs in ks.items():
            ent={'n':len(vs)}
            nums=[]
            for v in vs:
                try: nums.append(float(v.strip()))
                except: nums=None;break
            if nums: ent['lo']=min(nums);ent['hi']=max(nums)
            else:
                # numeric tuple?
                try:
                    cols=list(zip(*[[float(x) for x in re.split(r'[,\s]+',v.strip()) if x!=''] for v in vs]))
                    if cols and all(len(c)==len(vs) for c in cols) and len(cols)<=4: ent['cols']=[[min(c),max(c)] for c in cols]
                except Exception: pass
            # No sample values: the data file must not carry text copied from the disc.
            if (fam,sp,kp) not in cache:
                # trace every concrete spelling of the key (house_car_frame64, g1..g6), not a stand-in
                found=[trace(k2) for k2 in sorted(concrete[fam][(sp,kp)])[:8]]
                us=[u for u,_ in found]
                u=True if True in us else (None if None in us else False)
                cache[(fam,sp,kp)]=(u,sorted({f for _,fl in found for f in fl}))
            u,fs=cache[(fam,sp,kp)]
            ent['used']=u
            if fs: ent['fns']=['%06X'%f for f in fs[:6]]
            d[sp+'\t'+kp]=ent
        res['families'][fam]=d
    json.dump(res,open(out,'w'),separators=(',',':'),sort_keys=True)
    print({f:len(d) for f,d in res['families'].items()})


if __name__ == '__main__':
    main()
