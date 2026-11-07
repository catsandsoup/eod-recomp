"""CPU-only source mesh/livery selection previews; never uses a game or GPU.

Reuses the inspected MODL/VIF software renderer. Source wheel centers are an
explicit presentation adapter using axle tracks, wheelbase and physical radius;
not a claim of faithful suspension-pose simulation. UV paint canvases are separate.
"""
import copy
import hashlib
import json
from pathlib import Path

from import_nascar_cars import find_entry
from tex import decompile_tex, compile_tex
from valt import parse_binary_valt
from vif_mesh import load_mesh, render


def wheel_centers(physics):
    c=physics['Chassis'];out=[]
    for axle,sign,key in [('Front',1,'ftrack'),('Rear',-1,'rtrack')]:
        x=float(c[key])*.0254/2;y=float(physics[axle]['tire_diameter'])*.0254/2
        z=sign*float(c['wheelbase'])*.0254/2
        out.extend([(-x,y,z),(x,y,z)])
    return out


def preview_mesh(body_path,physics,wheel_path):
    mesh=load_mesh(body_path);wheel=load_mesh(wheel_path)
    for pos in wheel_centers(physics):
        for part in wheel['parts']:
            p=copy.deepcopy(part)
            # Native opposite side transform rotates around Y; preserve handedness.
            side=-1 if pos[0]>0 else 1
            for v in p['verts']:
                x,y,z=v['p'];v['p']=(side*x+pos[0],y+pos[1],side*z+pos[2])
            mesh['parts'].append(p)
    points=[v['p'] for p in mesh['parts'] for v in p['verts']]
    mesh['bbox']={label:tuple(fn(p[k] for p in points) for k in range(3)) for label,fn in [('min',min),('max',max)]}
    return mesh


def build_previews(records,work,sources,archive):
    from PIL import Image,ImageOps
    work=Path(work);out=work/'previews';out.mkdir()
    geometry={};texture_cache={};metadata={};unresolved=set()
    for r in records:
        name=r['chassis'];car=work/f'car-{name}'
        if name not in geometry:
            phys={s['name']:dict(s['pairs']) for s in parse_binary_valt(find_entry(car,'physics.val').read_bytes())['sections']}
            wheel_model=phys['Front']['model'];wheel=find_entry(sources['RACE'],wheel_model+'1.mod')
            geometry[name]=preview_mesh(find_entry(car,'Low1.mod'),phys,wheel)
        mesh=geometry[name];textures={}
        # Body paints are always actual source livery data, not a color swatch.
        from nascar_roster import FAMILIES
        cfg=FAMILIES[r['family']];paint_root=sources[cfg['paint']] if cfg['paint'] else sources[cfg['archive']]
        for p in mesh['parts']:
            tex=p['texture'].lower();cachekey=(name,tex)
            if tex==name.lower()+'.tex':
                path=find_entry(paint_root,r['paint']);cachekey=str(path)
            else:
                path=None
                for root in (car,sources['RACE'],sources['COMMON']):
                    try:path=find_entry(root,tex);break
                    except FileNotFoundError:pass
            if cachekey not in texture_cache:
                try:
                    if path is None:raise ValueError('No source texture')
                    w,h,rgba,_=decompile_tex(path.read_bytes());texture_cache[cachekey]=(w,h,rgba)
                except ValueError as error:
                    unresolved.add(f'{name}/{tex}: {error}');texture_cache[cachekey]=None
            image=texture_cache[cachekey]
            if image:textures[Path(tex).stem]=image
        big=out/f'{r["id"]}.png'
        render(mesh,str(big),textures,width=384,height=256,yaw_deg=35,pitch_deg=18,bg=(235,235,240))
        with Image.open(big) as raster:im=raster.convert('RGBA')
        # The debug rasterizer uses the opposite screen handedness to the guest.
        # Verified against source Car101 Marathon rear and existing runtime capture.
        # Correct presentation only: never change guest source geometry or UVs.
        im=ImageOps.mirror(im)
        # Native art uses transparent background. Remove only the exact clear color.
        pixels=list(im.getdata());pixels=[(0,0,0,0) if q[:3]==(235,235,240) else q for q in pixels]
        im.putdata(pixels)
        im=im.resize((256,171),Image.Resampling.LANCZOS)
        canvas=Image.new('RGBA',(256,256));canvas.alpha_composite(im,(0,42));im=canvas;im.save(big)
        alpha=im.getchannel('A');bbox=alpha.getbbox()
        occupied=sum(a!=0 for a in alpha.getdata())
        if bbox is None or occupied<1200 or len(set(im.getdata()))<24:raise ValueError(f'Degenerate preview {r["id"]}')
        if any(v<1 for v in bbox[:2]) or bbox[2]>=255 or bbox[3]>=255:raise ValueError(f'Clipped preview {r["id"]}: {bbox}')
        # The native small and large prefixes use different crops of the same model.
        for prefix,crop in [('uicar',im),('uicars',im.crop(bbox).resize((224,160),Image.Resampling.LANCZOS))]:
            if prefix=='uicars':
                canvas=Image.new('RGBA',(256,256));canvas.alpha_composite(crop,((256-crop.width)//2,(256-crop.height)//2));crop=canvas
            for ext,size in [('tm0',256),('tex',16)]:
                image=crop.resize((size,size),Image.Resampling.LANCZOS)
                # Quantize before the existing native TEX encoder to avoid O(N*256)
                # nearest-color searches. Pillow quantization uses actual render pixels.
                image=image.quantize(colors=256,method=Image.Quantize.FASTOCTREE).convert('RGBA')
                blob=compile_tex(size,size,image.tobytes(),fmt=0x201,flags=0x320600)
                archive.add(f'{prefix}_{r["id"]}.{ext}',blob,10,'TEX',ext=='tm0')
        metadata[r['id']]=dict(chassis=name,family=r['family'],paint=r['paint'],png=str(big),width=256,height=256,
                              foreground_pixels=occupied,bounds=bbox,body_texture_decoded=Path(name.lower()).stem in textures,sha256=hashlib.sha256(big.read_bytes()).hexdigest(),
                              wheel_source=f'RACE.RES/{"ptire" if r["family"]=="pacecar" else "rtire"}1.mod')
        print(f'CPU preview {r["id"]}: {name} {r["paint"]}, {occupied} visible pixels',flush=True)
    (out/'previews.json').write_text(json.dumps(metadata,indent=2))
    return dict(status='Source geometry rendered with native screen handedness; unsupported4-bit textures use gray fallback. Preview fidelity remains incomplete.',images=metadata,decoded_body_paints=sum(i['body_texture_decoded'] for i in metadata.values()),unresolved_textures=sorted(unresolved))
