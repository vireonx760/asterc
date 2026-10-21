"""Full-build fixed-vector correctness matrix across all 64-bit backends."""
from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser(); p.add_argument('--compiler',required=True); p.add_argument('--case',type=int,required=True); a=p.parse_args()
TARGETS=(('x86_64','sysv'),('aarch64','aapcs64'),('riscv64','lp64'))
SHAPES=('arith','compare_select','const_index','dynamic_index','odd_abi','wide_return')
TYPES=('u8','u16','u32','u64')
LANES=(3,4,8)
TOTAL=len(TARGETS)*len(SHAPES)*len(TYPES)*len(LANES)
if not 0<=a.case<TOTAL: raise SystemExit(f'case must be [0,{TOTAL-1}]')
slot=a.case; lanes=LANES[slot%len(LANES)]; slot//=len(LANES); ty=TYPES[slot%len(TYPES)]; slot//=len(TYPES); shape=SHAPES[slot%len(SHAPES)]; slot//=len(SHAPES); target,abi=TARGETS[slot]
bits=int(ty[1:]); count=lanes
# Keep total fixed-vector size within the language/backend 512-bit contract.
if bits*count>512: raise SystemExit('internal matrix overflow')
vt=f'vec<{count},{ty}>'
if shape=='arith':
    body=f'fn run(a:{vt},b:{vt})->{ty}{{ let c=(a+b)^b; let d=c* b; return d[{count-1}]; }}'
elif shape=='compare_select':
    body=f'fn run(a:{vt},b:{vt})->{ty}{{ let m:mask<{count}> = a < b; let c:{vt} = select(m,a,b); return c[{count-1}]; }}'
elif shape=='const_index':
    body=f'fn run(a:{vt})->{ty}{{ return a[{count-1}]; }}'
elif shape=='dynamic_index':
    body=f'fn run(a:{vt},i:u64)->{ty}{{ return a[i]; }}'
elif shape=='odd_abi':
    body=f'fn id(x:{vt})->{vt}{{return x;}} fn run(a:{vt})->{ty}{{let b=id(a);return b[{count-1}];}}'
else:
    body=f'fn run(a:{vt},n:u32)->{vt}{{ return (a<<n)+a; }}'
code='module vector.codegen_matrix;\n'+body+'\n'
with tempfile.TemporaryDirectory(prefix=f'aster-vector-{a.case}-') as td:
    root=Path(td); src=root/'case.aa'; out=root/'case.bin'; src.write_text(code,encoding='utf-8')
    cmd=[a.compiler,'build','--target',target,'--abi',abi,'--emit','bin','-j','1','-o',str(out),str(src)]
    pr=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=30)
    if pr.returncode: raise SystemExit(f'{a.case}: {target}/{shape}/{vt} failed\n{code}\n{pr.stdout}{pr.stderr}')
    if not out.exists() or not out.read_bytes(): raise SystemExit(f'{a.case}: empty output')
