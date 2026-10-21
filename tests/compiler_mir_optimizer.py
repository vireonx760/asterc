#!/usr/bin/env python3
"""Differential O0/O2 execution against integer oracles on real ISA emulators.

The shared CPU harness checks nonvolatile registers, return/SP, guarded NX stack
and bounded execution. The corpus covers executable edges, live/conflicting
PHIs, unused PHI cycles, nested loops, typed folds, memory, calls and traps.
Instruction counts describe the emulator trace, not hardware cycle estimates.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile

from compiler_division_unicorn import PROFILES, TYPES, execute, normalize


def corpus():
    cases = []
    def add(name, body, expected, typ='u64', bits=64, signed=False, helpers=''):
        text = f'module qual.optimizer; export fn run(x:{typ},y:{typ})->{typ} @abi(c) {{ {body} }}\n'+helpers
        cases.append((name, text, expected, bits, signed))
    add('computed_branch', 'let c:u64=3; let dead:u64=(x+7)*9; '
        'if c+4==7 { return x+5; } else { return x+99; }', lambda x,y: x+5)
    add('same_operand_width', 'return x&x;', lambda x,y:x)
    add('large_u32_constant', 'let a:u32=2000000000; let b:u32=2000000000; '
        'let sum:u32=a+b; let diff:u32=sum-1234; return diff^x;', lambda x,y:(4000000000-1234)^x,
        typ='u32',bits=32)
    add('common_phi', 'let r:u64=x; if y==0 { r=x; } else { r=x; } return r;', lambda x,y:x)
    add('conflict_phi', 'let r:u64=0; if y==0 { r=x+7; } else { r=x+9; } return r;', lambda x,y:x+(9 if y else 7))
    add('dead_phi_cycle', 'let n:u64=x&15; let i:u64=0; let sum:u64=0; let dead:u64=y; '
        'while i<n { sum=sum+i; dead=dead+i; i=i+1; } return sum;', lambda x,y:(x&15)*((x&15)-1)//2)
    add('nested_loop', 'let i:u64=0; let sum:u64=0; while i<(x&7) { '
        'let j:u64=0; while j<(y&7) { sum=sum+i+j; j=j+1; } i=i+1; } return sum;',
        lambda x,y:(y&7)*(x&7)*((x&7)-1)//2+(x&7)*(y&7)*((y&7)-1)//2)
    add('continue', 'let i:u64=0; let sum:u64=0; while i<(x&15) { i=i+1; '
        'if (i&1)==0 { continue; } sum=sum+i; } return sum;', lambda x,y:sum(i for i in range(1,(x&15)+1) if i&1))
    add('false_loop', 'let c:u64=3; let r:u64=x; while c==9 { r=r+1; } return r+y;', lambda x,y:x+y)
    add('call', 'let c:u64=3; let dead:u64=x*9; if c==3 { return helper(x,y)+x; } '
        'else { return y; }', lambda x,y:(x^y)+x,
        helpers='fn helper(x:u64,y:u64)->u64 { return x^y; }')
    add('memory', 'let cell:u64=x; store.u64 [&cell]=y; '
        'if x==0 { store.u64 [&cell]=7; } return load.u64 [&cell];', lambda x,y:7 if x==0 else y)
    add('unused_call', 'let unused:u64=helper(x,y); return x+1;', lambda x,y:x+1,
        helpers='fn helper(x:u64,y:u64)->u64 { return x^y; }')
    add('unused_division', 'let unused:u64=x/y; return x+1;', lambda x,y:x+1)
    add('unused_remainder', 'let unused:u64=x%y; return y+1;', lambda x,y:y+1)
    add('zero_extend', 'let c:u8=255; return x+zext<u64>(c);', lambda x,y:x+255)
    add('sign_extend', 'let c:i8=-1; return x+sext<i64>(c);', lambda x,y:x-1,typ='i64',signed=True)
    for typ,bits,signed in TYPES:
        a,b=(-7,3) if signed else (7,3)
        add('typed_'+typ, f'let a:{typ}={a}; let b:{typ}={b}; '
            f'if (a+b)<b {{ return x+y; }} else {{ return x-y; }}',
            (lambda x,y:x+y) if signed else (lambda x,y:x-y), typ,bits,signed)
        # Exercise width-specific wraparound, signed arithmetic and comparisons.
        add('fold_'+typ, f'let a:{typ}={a}; let b:{typ}={b}; '
            f'let c:{typ}=(a*b)+(a-b); return x^c;', lambda x,y,c=a*b+a-b:x^c,typ,bits,signed)
    # Deterministic generated expressions have an independent Python evaluator.
    rng=random.Random(0x1a57e)
    for i in range(12):
        a,b,c=[rng.randrange(1,60) for _ in range(3)]
        value=((a+b)*c)^(a<<2)
        add(f'generated_{i}', f'let a:u64={a}; let b:u64={b}; let c:u64={c}; '
            f'let v:u64=((a+b)*c)^(a<<2); if v=={value} {{ return x+v; }} else {{ return y; }}',
            lambda x,y,v=value:x+v)
    return cases


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',required=True)
    parser.add_argument('--profile',type=int,choices=range(len(PROFILES)))
    parser.add_argument('--python-path',default='')
    parser.add_argument('--out')
    args=parser.parse_args()
    if args.python_path:sys.path.insert(0,args.python_path)
    records=[]
    with tempfile.TemporaryDirectory(prefix='aster-mir-optimizer-') as temp:
        root=Path(temp);source=root/'unit.aa';output=root/'unit.bin'
        for profile in (range(len(PROFILES)) if args.profile is None else [args.profile]):
            target,abi,m=PROFILES[profile]
            for name,text,reference,bits,signed in corpus():
                source.write_text(text)
                lo=-(1<<(bits-1)) if signed else 0
                hi=(1<<(bits-int(signed)))-1
                rng=random.Random(0x5571+bits)
                samples=[(lo,0),(hi,1),(0,hi),(1,3),(7,0),(15,7)]+[(rng.randint(lo,hi),rng.randint(lo,hi)) for _ in range(8)]
                for promote,limit,policy in ((True,0,'hot'),(True,2,'size'),(False,2,'hot')):
                    pair=[]
                    for level in ('0','2'):
                        command=[args.compiler,'build','-O'+level,'--target',target,'--abi',abi,
                            '--no-red-zone','--reg-limit',str(limit),'-j','1','-o',str(output),str(source)]
                        if m:command+=['--feature','m']
                        if not promote:command+=['--no-promote']
                        env=os.environ.copy();env['ASTERASM_DIV_POLICY']=policy
                        result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=45)
                        if result.returncode:raise AssertionError((name,profile,promote,limit,level,result.stderr))
                        code=output.read_bytes();traces=[];faults=0
                        for x,y in samples:
                            fault=name.startswith('unused_') and name in ('unused_division','unused_remainder') and y==0 and target=='x86_64'
                            want=None if fault else normalize(reference(x,y),bits,signed)
                            got,count=execute(code,profile,x,y,bits,signed,fault)
                            if got!=want:raise AssertionError((name,profile,promote,limit,level,x,y,got,want))
                            traces.append(count);faults+=fault
                        pair.append({'level':level,'bytes':len(code),'sha256':hashlib.sha256(code).hexdigest(),
                            'instructions':sum(traces),'calls':len(samples),'faults':faults})
                    records.append({'name':name,'profile':profile,'promote':promote,'reg_limit':limit,
                        'policy':policy,'variants':pair})
    if args.out:Path(args.out).write_text(json.dumps({'records':records},indent=2)+'\n')
    variants=[v for record in records for v in record['variants']]
    print(f'PASS: {len(variants)} optimized/baseline binaries, {sum(v["calls"] for v in variants)} CPU calls, '
          f'{sum(v["faults"] for v in variants)} expected exceptions')


if __name__=='__main__':main()
