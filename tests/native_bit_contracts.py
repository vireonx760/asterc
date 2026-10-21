#!/usr/bin/env python3
"""RV64 Zba/Zbb/Zbc/Zbs: independent bytes, integer models and execution."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import re
import tempfile
from native_processor_qemu import command, other_kernel
from native_processor_contracts import assemble

ROOT=Path(__file__).resolve().parents[1]; MASK=(1 << 64)-1
UNARY='clz ctz cpop clzw ctzw cpopw sext.b sext.h zext.h orc.b rev8'.split()
REG='andn orn xnor min minu max maxu rol ror rolw rorw add.uw sh1add sh2add sh3add sh1add.uw sh2add.uw sh3add.uw clmul clmulr clmulh bset bclr binv bext'.split()
IMM='rori roriw slli.uw bseti bclri binvi bexti'.split()
NAMES=REG+UNARY+IMM
FEATURE={n:('zba' if n in 'add.uw sh1add sh2add sh3add sh1add.uw sh2add.uw sh3add.uw slli.uw'.split() else
            'zbc' if n in ('clmul','clmulr','clmulh') else 'zbs' if n.startswith(('bset','bclr','binv','bext')) else 'zbb') for n in NAMES}
MARCH='rv64im_zba_zbb_zbc_zbs'

def signed(v,bits=64):
    v &= (1 << bits)-1
    return v-(1 << bits) if v >> (bits-1) else v

def reference(n,a,b):
    a &= MASK; b &= MASK
    word=n in ('rolw','rorw','roriw'); bits=32 if word else 64; mask=(1 << bits)-1; x=a & mask
    if n=='andn': r=a & ~b
    elif n=='orn': r=a | ~b
    elif n=='xnor': r=~(a ^ b)
    elif n in ('min','max'): r=(min if n=='min' else max)(signed(a),signed(b))
    elif n in ('minu','maxu'): r=(min if n=='minu' else max)(a,b)
    elif n.startswith(('clz','ctz','cpop')):
        bits=32 if n.endswith('w') else 64; x=a & ((1 << bits)-1)
        r=bits-x.bit_length() if n.startswith('clz') else (x & -x).bit_length()-1 if n.startswith('ctz') and x else bits if n.startswith('ctz') else x.bit_count()
    elif n=='sext.b': r=signed(a,8)
    elif n=='sext.h': r=signed(a,16)
    elif n=='zext.h': r=a & 65535
    elif n=='orc.b': r=sum((255 if (a >> j) & 255 else 0) << j for j in range(0,64,8))
    elif n=='rev8': r=int.from_bytes(a.to_bytes(8,'little'),'big')
    elif n.startswith(('rol','ror')):
        shift=b & (bits-1)
        r=((x << shift) | (x >> ((bits-shift) % bits))) if n.startswith('rol') else ((x >> shift) | (x << ((bits-shift) % bits)))
    elif n=='add.uw': r=(a & 0xffffffff)+b
    elif n.startswith('sh') and 'add' in n: r=((a & 0xffffffff if n.endswith('.uw') else a) << int(n[2]))+b
    elif n=='slli.uw': r=(a & 0xffffffff) << b
    elif n.startswith('clmul'):
        product=0
        for j in range(64):
            if (b >> j) & 1: product ^= a << j
        r=product >> (64 if n=='clmulh' else 63 if n=='clmulr' else 0)
    elif n.startswith('bset'): r=a | (1 << (b & 63))
    elif n.startswith('bclr'): r=a & ~(1 << (b & 63))
    elif n.startswith('binv'): r=a ^ (1 << (b & 63))
    elif n.startswith('bext'): r=(a >> (b & 63)) & 1
    else: raise AssertionError(n)
    return (signed(r,32) if word else r) & MASK

def native(n,d='a0',a='a1',b='a2'):
    return n+' {'+f'dst:{d},'+(f'src:{a}' if n in UNARY else f'lhs:{a},rhs:{b}')+'};'

def asm(n,d,a,b): return f'{n} {d}, {a}'+('' if n in UNARY else f', {b}')

def cases(smoke):
    out=[]
    for n in NAMES:
        maximum=32 if n=='roriw' else 64
        for j in range(1 if smoke else 32): out.append((n,'a0',f'x{j}',str(j % maximum) if n in IMM else f'x{31-j}'))
        if smoke: continue
        for d in [0,5,6,7,*range(10,18),*range(28,32)]: out.append((n,f'x{d}','a0','63' if n in IMM and maximum==64 else '31' if n in IMM else 'a0'))
        if n in IMM:
            for v in range(maximum): out.append((n,'a0','a0',str(v)))
    return out

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('compiler','clang'): p.add_argument('--'+n,required=True)
    p.add_argument('--dumper'); p.add_argument('--qemu',action='store_true');p.add_argument('--smoke',action='store_true')
    p.add_argument('--out');p.add_argument('--python-path'); args=p.parse_args()
    inventory=re.findall(r'^AST_RV_SCALAR\(\w+,\s*"([^"]+)"',(ROOT/'include/asterasm/detail/riscv64_bit_instructions.inc').read_text(),re.M)
    assert len(inventory)==len(NAMES)==43 and set(inventory)==set(NAMES)
    modes=('native',) if args.smoke else ('native','exact','unsafe exact');levels=('0',) if args.smoke else ('0','2','z')
    rng=random.Random(0xb17c0de)
    values=[(0,0),(MASK,MASK),(1 << 63,1),(0x80000000,0xffffffff),(0x1122334455667788,65),
            (0xff00000000ff0000,63)]+[(rng.getrandbits(64),rng.getrandbits(64)) for _ in range(4)]
    records=[]; rejected=calls=compiled=0
    with tempfile.TemporaryDirectory(prefix='aster-bits-') as temp:
        work=Path(temp);source=work/'case.aa';raw=work/'case.bin'
        def build(text,level='0',pressure=0,promote=True,omit=None):
            nonlocal compiled
            source.write_text(text)
            argv=[args.compiler,'build','--target','riscv64','-O'+level,'--reg-limit',str(pressure),'-j','1','-o',raw,source,'--feature','m']
            for feature in ('zba','zbb','zbc','zbs'):
                if feature!=omit: argv+=['--feature',feature]
            if not promote: argv+=['--no-promote']
            command(argv); compiled+=1;return raw.read_bytes()
        if args.qemu:
            library=work/'functions.bin'
            for mode in modes:
                for level in levels:
                    binary=bytearray();entries=[]
                    for n in NAMES:
                        imm=31 if n in IMM else None
                        for sink in (False,True):
                            body=native(n,'zero' if sink else 'a0','a1',str(imm) if imm is not None else 'a2')
                            code=build('module bits.run;export fn run() @naked {'+mode+' riscv64 {'+body+'ret;}}',level)
                            assert len(code)==8
                            entries.append((n,imm,sink,len(binary)));binary.extend(code)
                        for pressure,promote in ([(2,False)] if args.smoke else [(0,True),(2,False)]):
                            code=build(managed(n,mode),level,pressure,promote)
                            entries.append((n,imm,'managed',len(binary)));binary.extend(code)
                    library.write_bytes(binary)
                    runner=['mv s11,sp']+[f'li s{j},{0x123450+j}' for j in range(11)]
                    for n,imm,sink,offset in entries:
                        for a,b in values:
                            a0,a1,a2=(a,b,0x31415926) if sink=='managed' else (0xabcdef0123456789,a,b)
                            expected=reference(n,a,b if imm is None else imm)
                            if sink=='managed': expected ^= (0x31415926*17+9) & MASK
                            elif sink: expected=a0
                            runner += [f'li a0,{a0}',f'li a1,{a1}',f'li a2,{a2}',f'la t0,aster_entry+{offset}','jalr ra,0(t0)',
                                       f'li t0,{expected}','bne a0,t0,failed','bne sp,s11,failed']
                            for j in range(11):runner += [f'li t0,{0x123450+j}',f'bne s{j},t0,failed']
                            calls+=1
                    elf,_=other_kernel('riscv64','\n'.join(runner)+'\n',library,work,args.clang,rv_cpu='rv64,zba=true,zbb=true,zbc=true,zbs=true')
                    records.append({'mode':mode,'level':level,'binaries':len(entries),'calls':len(entries)*len(values),
                                    'sha256':hashlib.sha256(binary).hexdigest(),'kernel_sha256':hashlib.sha256(elf.read_bytes()).hexdigest()})
                    print('PASS: RV64 bitmanip QEMU',mode,level,flush=True)
            report={'instruction_names':43,'qemu_runs':len(records),'guarded_calls':calls,'compiled_binaries':compiled,'records':records}
        else:
            corpus=cases(args.smoke); blob=bytearray()
            for start in range(0,len(corpus),256):
                batch=corpus[start:start+256];want=assemble(args.clang,'riscv64','\n'.join(asm(*x) for x in batch)+'\nret',work,rv_march=MARCH)
                for mode in modes:
                    for level in levels:
                        code=build('module bits.words;export fn run() @naked {'+mode+' riscv64 {'+''.join(native(*x) for x in batch)+'ret;}}',level)
                        assert code==want,('bit encoding batch',start,mode,level)
                blob.extend(code)
            raw.write_bytes(blob)
            dump=json.loads(command([args.dumper,'--target','riscv64','--strict','--format','json',raw]).stdout)
            assert assemble(args.clang,'riscv64','\n'.join(i['assembly'] for i in dump['instructions']),work,rv_march=MARCH)==blob
            for n in NAMES:
                for mode in modes:
                    for level in levels:
                        for pressure,promote in ([(2,False)] if args.smoke else [(0,True),(2,False)]):
                            code=build(managed(n,mode),level,pressure,promote)
                            records.append({'name':n,'mode':mode,'level':level,'pressure':pressure,'bytes':len(code),'sha256':hashlib.sha256(code).hexdigest()})
                print('PASS: RV64 bitmanip',n,flush=True)
            for mode in modes:
                for n in NAMES:
                    source.write_text('module bits.bad;export fn run() @naked {'+mode+' riscv64 {'+native(n,b='31' if n in IMM else 'a2')+'ret;}}')
                    argv=[args.compiler,'build','--target','riscv64','-j','1','-o',raw,source]
                    for feature in ('zba','zbb','zbc','zbs'):
                        if feature!=FEATURE[n]:argv+=['--feature',feature]
                    raw.unlink(missing_ok=True);r=command(argv,expected=1);assert r.stderr and not raw.exists();rejected+=1
                for n in IMM:
                    for imm in (-1,32 if n=='roriw' else 64):
                        source.write_text('module bits.bad;export fn run() @naked {'+mode+' riscv64 {'+native(n,b=str(imm))+'ret;}}')
                        raw.unlink(missing_ok=True)
                        argv=[args.compiler,'build','--target','riscv64','-j','1','-o',raw,source,'--feature',FEATURE[n]]
                        r=command(argv,expected=1);assert r.stderr and not raw.exists();rejected+=1
            report={'instruction_names':43,'oracle_cases':len(corpus),'llvm_dumper_roundtrips':len(corpus),
                    'compiled_binaries':compiled,'managed_binaries':len(records),'guarded_calls':calls,'rejections':rejected,'records':records}
        if args.out:Path(args.out).write_text(json.dumps(report,indent=2)+'\n')
    print('PASS: RV64 bitmanip',json.dumps({k:v for k,v in report.items() if k!='records'}))

def managed(n,mode):
    return ('module bits.managed;export fn run(a:u64,b:u64,c:u64)->u64 {let live:u64=c*17+9;let out:u64;'+mode+
            ' riscv64 inputs(a,b) outputs(out) {'+native(n,'out','a','31' if n in IMM else 'b')+'}return out^live;}')

if __name__=='__main__':main()
