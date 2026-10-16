# tools/contact_sheet.py <dir> <first_frame> [n] [cols] -- half-size contact sheet of frame_*.ppm (macOS: sips makes the PNG)
import glob, sys, os
d=sys.argv[1]; first=int(sys.argv[2]); n=int(sys.argv[3]) if len(sys.argv)>3 else 9; cols=int(sys.argv[4]) if len(sys.argv)>4 else 3
fs=[f for f in sorted(glob.glob(d+'/frame_*.ppm')) if int(os.path.basename(f)[6:11])>=first][:n]
def rd(p):
    b=open(p,'rb').read(); parts=b.split(b'\n',3); w,h=map(int,parts[1].split()); return w,h,parts[3]
imgs=[rd(f) for f in fs]
W,H,_=imgs[0]; sw,sh=W//2,H//2; rows=(len(imgs)+cols-1)//cols
out=bytearray(sw*cols*sh*rows*3)
for i,(w,h,px) in enumerate(imgs):
    ox=(i%cols)*sw; oy=(i//cols)*sh
    for y in range(min(sh,h//2)):
        row=px[(y*2)*w*3:(y*2+1)*w*3]
        half=b''.join(row[x*6:x*6+3] for x in range(min(sw,w//2)))
        d0=((oy+y)*sw*cols+ox)*3; out[d0:d0+len(half)]=half
open(d+'/sheet.ppm','wb').write(b'P6\n%d %d\n255\n'%(sw*cols,sh*rows)+bytes(out))
os.system('sips -s format png %s/sheet.ppm --out %s/sheet.png >/dev/null'%(d,d))
print([os.path.basename(f) for f in fs])
