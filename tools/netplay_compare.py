"""Strict ordered diagnostic comparison; no field masking or float tolerance."""
import json, sys

def first_difference(a,b,path='$'):
 if type(a) is not type(b): return f'{path}: types differ'
 if isinstance(a,dict):
  if a.keys()!=b.keys(): return f'{path}: keys differ'
  for k in a:
   d=first_difference(a[k],b[k],f'{path}.{k}')
   if d: return d
 elif isinstance(a,list):
  for i,(x,y) in enumerate(zip(a,b)):
   d=first_difference(x,y,f'{path}[{i}]')
   if d:return d
  if len(a)!=len(b):return f'{path}: length {len(a)} != {len(b)}'
 elif a!=b:return f'{path}: {a!r} != {b!r}'
 return None

if __name__=='__main__':
 traces=[[json.loads(line) for line in open(p)] for p in sys.argv[1:3]]
 d=first_difference(*traces)
 print(d or 'All recorded events match exactly (diagnostic coverage only).')
 sys.exit(bool(d))
