#!/usr/bin/env python3
"""Copy the linked non-system dylib closure and sign a personal .app locally."""
import pathlib, shutil, subprocess, sys, plistlib, re, json, os
app=pathlib.Path(sys.argv[1]).resolve()
if len(sys.argv)>2:
    (app/'Contents/MacOS').mkdir(parents=True,exist_ok=True)
    shutil.copyfile(sys.argv[2],app/'Contents/MacOS/eod')
    (app/'Contents/MacOS/eod').chmod(0o755)
    shutil.copyfile(pathlib.Path(__file__).with_name('Info.plist.in'),app/'Contents/Info.plist')
exe=app/'Contents/MacOS/eod'
frameworks=app/'Contents/Frameworks'
# Rebuilding with static SDL or another backend must not retain stale dylibs.
if frameworks.exists(): shutil.rmtree(frameworks)
frameworks.mkdir()
def run(*args): return subprocess.check_output(args,text=True)
def rpaths(path):
    lines=run('otool','-l',str(path)).splitlines(); result=[]
    for i,line in enumerate(lines):
        if 'cmd LC_RPATH' in line:
            result.append(lines[i+2].strip().split(' (offset')[0].removeprefix('path '))
    return result
seen={}; pending=[(exe,exe)]
resources=app/'Contents/Resources'; resources.mkdir(exist_ok=True)
vulkan_resources=resources/'vulkan'
if vulkan_resources.exists(): shutil.rmtree(vulkan_resources)
# MoltenVK is dlopened by the Vulkan loader: it does not appear in its linked
# dependency closure, so a Vulkan build must explicitly package the ICD too.
if 'libvulkan' in run('otool','-L',str(exe)).lower():
    molten=pathlib.Path(os.environ.get('EOD_MOLTENVK_LIBRARY','/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib')).resolve()
    if not molten.is_file(): raise RuntimeError('Vulkan app requires MoltenVK; set EOD_MOLTENVK_LIBRARY to its dylib')
    driver=frameworks/'libMoltenVK.dylib'
    shutil.copyfile(molten,driver); driver.chmod(0o755)
    seen[driver.name]=molten; pending.append((molten,driver))
    subprocess.run(['install_name_tool','-id','@rpath/'+driver.name,str(driver)],check=True)
    icd_dir=vulkan_resources/'icd.d'; icd_dir.mkdir(parents=True)
    manifest={'file_format_version':'1.0.0','ICD': {
        'library_path':'../../../Frameworks/libMoltenVK.dylib',
        'api_version':'1.3.0','is_portability_driver':True}}
    (icd_dir/'MoltenVK_icd.json').write_text(json.dumps(manifest,indent=2)+'\n')
while pending:
    src,dst=pending.pop()
    paths=rpaths(src)+rpaths(exe)
    for line in run('otool','-L',str(src)).splitlines()[1:]:
        dep=line.strip().split(' (compatibility')[0]
        if dep.startswith(('/System/','/usr/lib/')): continue
        if dep.startswith('@rpath/'):
            suffix=dep[len('@rpath/'):]
            candidates=[pathlib.Path(p.replace('@loader_path',str(src.parent)).replace('@executable_path',str(exe.parent)))/suffix for p in paths]
            candidates += [pathlib.Path('/opt/homebrew/lib')/suffix]
        else:
            candidates=[pathlib.Path(dep.replace('@loader_path',str(src.parent)).replace('@executable_path',str(exe.parent)))]
        actual=next((p.resolve() for p in candidates if p.exists()),None)
        if actual is None: raise RuntimeError(f'Cannot resolve {dep} from {src}')
        target=frameworks/actual.name
        # A dylib's own install name appears in otool -L too.
        if actual==src.resolve(): continue
        if target.name in seen and seen[target.name]!=actual: raise RuntimeError(f'Dylib name collision: {actual}')
        if target.name not in seen:
            seen[target.name]=actual; shutil.copyfile(actual,target); target.chmod(0o755)
            subprocess.run(['install_name_tool','-id','@rpath/'+target.name,str(target)],check=True)
            pending.append((actual,target))
        subprocess.run(['install_name_tool','-change',dep,'@rpath/'+target.name,str(dst)],check=True)
resources=app/'Contents/Resources'; resources.mkdir(exist_ok=True)
icon=pathlib.Path('/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericApplicationIcon.icns')
if icon.exists(): shutil.copyfile(icon,resources/'AppIcon.icns')
licenses=resources/'Licenses'; licenses.mkdir(exist_ok=True)
for name,license_path in [('SDL3','/opt/homebrew/opt/sdl3/LICENSE.txt'),('libusb','/opt/homebrew/opt/libusb/COPYING'),('MoltenVK','/opt/homebrew/opt/molten-vk/LICENSE')]:
    source=pathlib.Path(license_path)
    if source.exists(): shutil.copyfile(source,licenses/(name+'.txt'))
# The local SDK/Homebrew binaries can impose a newer minimum than our source.
minimum=(13,0)
for binary in [*frameworks.iterdir(),exe]:
    versions=re.findall(r'^\s+minos (\d+(?:\.\d+)+)',run('otool','-l',str(binary)),re.MULTILINE)
    for version in versions: minimum=max(minimum,tuple(map(int,version.split('.'))))
plist_path=app/'Contents/Info.plist'
with plist_path.open('rb') as f: info=plistlib.load(f)
info['LSMinimumSystemVersion']='.'.join(map(str,minimum))
with plist_path.open('wb') as f: plistlib.dump(info,f)
# macOS may propagate Finder metadata from Documents into linker outputs.
subprocess.run(['xattr','-cr',str(app)],check=True)
for binary in [*frameworks.iterdir(),exe]:
    # All dependencies live in the bundle, including indirect dylib loads.
    rel='@executable_path/../Frameworks' if binary==exe else '@loader_path'
    for old in rpaths(binary):
        if old.startswith('/'):
            subprocess.run(['install_name_tool','-delete_rpath',old,str(binary)],check=True)
    if rel not in rpaths(binary): subprocess.run(['install_name_tool','-add_rpath',rel,str(binary)],check=True)
    subprocess.run(['xattr','-cr',str(app)],check=True)
    subprocess.run(['codesign','--force','--sign','-',str(binary)],check=True)
subprocess.run(['xattr','-cr',str(app)],check=True)
subprocess.run(['xattr','-d','com.apple.FinderInfo',str(app)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
subprocess.run(['codesign','--force','--sign','-',str(app)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
