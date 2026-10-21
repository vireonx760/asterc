#!/usr/bin/env python3
"""Execute inlining, stack ownership and compact layout against integer oracles.

Every binary runs with the independent guarded CPU/ABI harness. Size comparisons
use final linked bytes, including retained callees, rather than MIR estimates.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile

from compiler_division_unicorn import PROFILES, execute, normalize, oracle


def corpus():
    cases = []
    def add(name, body, expected, helpers='', typ='u64', bits=64, signed=False, imports=''):
        text = f'module qual.inlining; {imports} export fn run(x:{typ},y:{typ})->{typ} @abi(c) {{ {body} }}\n'+helpers
        cases.append((name, text, expected, bits, signed))
    add('nested_chain', 'return outer(x)+y;', lambda x,y: x+12+y,
        'fn outer(x:u64)->u64 { return middle(x)+5; } '
        'fn middle(x:u64)->u64 { return leaf(x)+3; } fn leaf(x:u64)->u64 { return x+4; }')
    add('early_returns', 'return choose(x,y)+choose(y,x);',
        lambda x,y: (x+7 if y==0 else x^y)+(y+7 if x==0 else y^x),
        'fn choose(x:u64,y:u64)->u64 { if y==0 { return x+7; } return x^y; }')
    add('callee_loop', 'return sum(x&15)+y;', lambda x,y: (x&15)*((x&15)-1)//2+y,
        'fn sum(n:u64)->u64 { let i:u64=0; let s:u64=0; while i<n { s=s+i; i=i+1; } return s; }')
    add('caller_loop', 'let i:u64=0; let s:u64=y; while i<(x&15) { s=step(s,i); i=i+1; } return s;',
        lambda x,y: y+(x&15)*((x&15)-1)//2+(x&15)*7,
        'fn step(x:u64,y:u64)->u64 { if y==0 { return x+7; } return x+y+7; }')
    add('nested_loops', 'return sum(x&7,y&7);',
        lambda x,y: sum((i^j)+1 for i in range(x&7) for j in range(y&7)),
        'fn sum(n:u64,m:u64)->u64 { let i:u64=0; let s:u64=0; while i<n { let j:u64=0; '
        'while j<m { s=s+step(i,j); j=j+1; } i=i+1; } return s; } '
        'fn step(x:u64,y:u64)->u64 { return (x^y)+1; }')
    add('argument_once', 'let cell:u64=x; let value:u64=twice(bump(&cell)); '
        'return value+(load.u64 [&cell]);', lambda x,y: (x+1)*3,
        'fn bump(p:ptr<u64>)->u64 { let v:u64=(load.u64 [p])+1; store.u64 [p]=v; return v; } '
        'fn twice(x:u64)->u64 { return x+x; }')
    add('argument_order', 'let cell:u64=x; return combine(bump(&cell),bump(&cell));',
        lambda x,y: (x+1)*17+(x+2),
        'fn bump(p:ptr<u64>)->u64 { let v:u64=(load.u64 [p])+1; store.u64 [p]=v; return v; } '
        'fn combine(a:u64,b:u64)->u64 { return a*17+b; }')
    add('void_store', 'let cell:u64=x; update(&cell,y); return load.u64 [&cell];', lambda x,y: y+7,
        'fn update(p:ptr<u64>,x:u64) { if x==0 { store.u64 [p]=7; return; } store.u64 [p]=x+7; }')
    add('local_frame', 'return cell(x)+cell(y);', lambda x,y: (x^17)+(y^17),
        'fn cell(x:u64)->u64 { let a:u64=x; store.u64 [&a]=x^17; return load.u64 [&a]; }')
    add('frame_escape_barrier', 'return outer(x)+y;', lambda x,y: x+9+y,
        'fn outer(x:u64)->u64 { let a:u64=x; return mutate(&a); } '
        'fn mutate(p:ptr<u64>)->u64 { store.u64 [p]=(load.u64 [p])+9; return load.u64 [p]; }')
    add('recursion', 'return factorial(x&7)+y;', lambda x,y: math.factorial(x&7)+y,
        'fn factorial(n:u64)->u64 { if n==0 { return 1; } return n*factorial(n-1); }')
    add('mutual_recursion', 'return even(x&15)+y;', lambda x,y: int((x&1)==0)+y,
        'fn even(n:u64)->u64 { if n==0 { return 1; } return odd(n-1); } '
        'fn odd(n:u64)->u64 { if n==0 { return 0; } return even(n-1); }')
    add('exported_definition', 'return public_helper(x)+y;', lambda x,y: (x^31)+y,
        'export fn public_helper(x:u64)->u64 @abi(c) { return x^31; }')
    add('indirect_address_root', 'let p=&increment; return (*p)(x)+y;', lambda x,y:x+1+y,
        'fn increment(x:u64)->u64 @abi(c) { return x+1; }')
    add('cross_module', 'return lib.inline_leaf.apply(x,y);', lambda x,y:(x^y)+7,
        imports='import lib.inline_leaf;')
    add('nested_generics', 'return outer(x)+outer(y)+width(x);', lambda x,y:x+y+8,
        'fn identity<T:type>(x:T)->T { return x; } '
        'fn outer<T:type>(x:T)->T { return identity(x); } '
        'fn width<T:type>(x:T)->u64 { return sizeof(T); }')
    add('explicit_noinline', 'return helper(x)+y;', lambda x,y:(x^7)+y,
        'fn helper(x:u64)->u64 @noinline { return x^7; }')
    add('floating_arguments', 'return cast<u64>(blend(cast<f64>(x&1023),cast<f64>(y&1023)));',
        lambda x,y:(x&1023)*2+(y&1023)+3,
        'fn blend(x:f64,y:f64)->f64 { return x*2.0+y+3.0; }')
    add('stack_arguments', 'return nine(x,y,x,y,x,y,x,y,x);',lambda x,y:5*x+4*y,
        'fn nine(a:u64,b:u64,c:u64,d:u64,e:u64,f:u64,g:u64,h:u64,i:u64)->u64 { return a+b+c+d+e+f+g+h+i; }')
    large_body=' '.join(f'v=(v^{i+1})+y;' for i in range(70))
    def huge(x,y):
        for i in range(70):x=normalize((x^(i+1))+y,64,False)
        return x
    add('huge_callee', 'return huge(x,y);', huge,
        'fn huge(x:u64,y:u64)->u64 { let v:u64=x; '+large_body+' return v; }')
    add('dead_private_body', 'return x+y;', lambda x,y: x+y,
        'fn unused(x:u64)->u64 { let v:u64=x; let i:u64=0; while i<100 { v=v*17+9; i=i+1; } return v; }')
    for bits in (8,16,32,64):
        typ='u'+str(bits); a=(1<<bits)-3
        add('offset_wrap_'+typ, f'return helper(x)+y;', lambda x,y,a=a: x+a+7+y,
            f'fn helper(x:{typ})->{typ} {{ return (x+{a})+7; }}',typ,bits)
        add('offset_cancel_'+typ, 'return helper(x)-y;', lambda x,y: x-y,
            f'fn helper(x:{typ})->{typ} {{ return (x-7)+7; }}',typ,bits)
        typ='i'+str(bits)
        add('signed_return_'+typ, 'return choose(x,y);', lambda x,y: x if x<y else y,
            f'fn choose(x:{typ},y:{typ})->{typ} {{ if x<y {{ return x; }} return y; }}',typ,bits,True)
    for op,name in (('/','division'),('%','remainder')):
        # The target's exceptional division contract must survive call removal.
        add(name, 'return helper(x,y);', None,
            f'fn helper(x:i64,y:i64)->i64 {{ return x{op}y; }}','i64',64,True)
    return cases


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',required=True)
    parser.add_argument('--profile',type=int,choices=range(len(PROFILES)))
    parser.add_argument('--python-path',default='')
    parser.add_argument('--out')
    args=parser.parse_args()
    if args.python_path:
        import sys
        sys.path.insert(0,args.python_path)
    records=[]; reproducible=0
    with tempfile.TemporaryDirectory(prefix='aster-mir-inline-') as temp:
        root=Path(temp); source=root/'unit.aa'; output=root/'unit.bin'
        (root/'lib').mkdir()
        (root/'lib/inline_leaf.aa').write_text('module lib.inline_leaf; '
            'export fn apply(x:u64,y:u64)->u64 @abi(c) { return leaf(x^y); } '
            'fn leaf(x:u64)->u64 { return x+7; }')
        for profile in (range(len(PROFILES)) if args.profile is None else [args.profile]):
            target,abi,with_m=PROFILES[profile]
            for name,text,reference,bits,signed in corpus():
                source.write_text(text)
                lo=-(1<<(bits-1)) if signed else 0; hi=(1<<(bits-int(signed)))-1
                rng=random.Random(0x1a11+bits)
                samples=[(lo,0),(hi,1),(0,hi),(1,3),(7,0),(15,7)]
                if signed:samples += [(lo,-1),(-7,3),(7,-3)]
                samples += [(rng.randint(lo,hi),rng.randint(lo,hi)) for _ in range(7)]
                for promote,limit in ((True,0),(True,2),(False,2)):
                    variants=[]
                    for level,no_inline in (('0',False),('2',True),('2',False),('z',True),('z',False)):
                        command=[args.compiler,'build','-O'+level,'--target',target,'--abi',abi,
                            '--no-red-zone','--reg-limit',str(limit),'-j','1','--stats','-I',str(root),'-o',str(output),str(source)]
                        if with_m:command+=['--feature','m']
                        if name=='floating_arguments' and target=='riscv64':command+=['--feature','f','--feature','d']
                        if not promote:command+=['--no-promote']
                        if no_inline:command+=['--no-inline']
                        env=os.environ.copy();env.pop('ASTERASM_DIV_POLICY',None)
                        result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=45)
                        if result.returncode:raise AssertionError((name,profile,promote,limit,level,no_inline,result.stderr))
                        code=output.read_bytes(); steps=0; faults=0
                        if name=='dead_private_body' and not re.search(r'functions-removed=[1-9][0-9]*',result.stderr):
                            raise AssertionError(('dead private function retained',profile,level,result.stderr))
                        if profile==4:
                            for offset in range(0,len(code),4):
                                word=int.from_bytes(code[offset:offset+4],'little')
                                if word&0x7f in (0x33,0x3b) and (word>>25)&0x7f==1:
                                    raise AssertionError(('M instruction in RV64I',name,offset,hex(word)))
                        for x,y in samples:
                            want=oracle(x,y,bits,signed,'/' if name=='division' else '%',target) if reference is None else normalize(reference(x,y),bits,signed)
                            got,count=execute(code,profile,x,y,bits,signed,want is None,enable_fp=name=='floating_arguments')
                            if got!=want:raise AssertionError((name,profile,promote,limit,level,no_inline,x,y,got,want))
                            steps+=count;faults+=want is None
                        variants.append({'level':level,'no_inline':no_inline,'bytes':len(code),
                            'sha256':hashlib.sha256(code).hexdigest(),'instructions':steps,'calls':len(samples),'faults':faults,
                            'inline_stats':re.findall(r'MIR inline: [^\n]+',result.stderr)})
                        if promote and not limit and name=='nested_chain' and level=='z' and not no_inline:
                            # Source paths, thread counts and measuring passes must not perturb bytes.
                            repeat=command.copy();repeat[repeat.index('-j')+1]='4'
                            subprocess.run(repeat,env=env,capture_output=True,check=True,timeout=45)
                            if output.read_bytes()!=code:raise AssertionError('non-deterministic inlining/layout')
                            reproducible+=1
                    for baseline,candidate in ((variants[1],variants[2]),(variants[3],variants[4])):
                        if candidate['bytes']>baseline['bytes']:
                            raise AssertionError(('inlining grew final linked code',name,profile,baseline,candidate))
                    for speed,size in ((variants[1],variants[3]),(variants[2],variants[4])):
                        if size['bytes']>speed['bytes']:
                            raise AssertionError(('size mode grew the final artifact over O2',name,profile,speed,size))
                    records.append({'name':name,'profile':profile,'promote':promote,'reg_limit':limit,'variants':variants})
    if args.out:Path(args.out).write_text(json.dumps({'records':records,'reproducible':reproducible},indent=2)+'\n')
    variants=[v for r in records for v in r['variants']]
    print(f'PASS: {len(variants)} inlining/layout binaries, {sum(v["calls"] for v in variants)} CPU calls, '
          f'{sum(v["faults"] for v in variants)} expected exceptions, {reproducible} deterministic rebuilds')


if __name__=='__main__':main()
