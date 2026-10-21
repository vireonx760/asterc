#!/usr/bin/env python3
"""Liveness and ABI stress: four /% sites across two live arguments, all ISAs.

Validates actual emitted BIN via native x86 calling or separate target-word
interpreters. Regression for RAX/RDX implicit defs, clobbers and scratch aliases.
"""
import argparse
import ctypes
import os
import random
import subprocess
import tempfile
from pathlib import Path
from compiler_narrow_isa_execution import a64_exec, rv64_exec, sx, TYPES
from native_jit import executable_code, native_x86_supported

DTYPES={(8,False):ctypes.c_uint8,(8,True):ctypes.c_int8,
        (16,False):ctypes.c_uint16,(16,True):ctypes.c_int16,
        (32,False):ctypes.c_uint32,(32,True):ctypes.c_int32,
        (64,False):ctypes.c_uint64,(64,True):ctypes.c_int64}


def run_native(code,typ,x,y):
    with executable_code(code) as address:
        fun=ctypes.CFUNCTYPE(typ,typ,typ)(address)
        return fun(x,y)


def truncdiv(a,b):
    n=abs(a)//abs(b)
    return -n if (a<0)!=(b<0) else n


def main():
    pa=argparse.ArgumentParser()
    pa.add_argument('--compiler',required=True)
    pa.add_argument('--quick',action='store_true')
    a=pa.parse_args()
    rng=random.Random(0x242424)
    count=0
    with tempfile.TemporaryDirectory(prefix='aster-div-multisite-') as td:
        src=Path(td)/'t.aa'; out=Path(td)/'t.bin'
        for target,m in [('aarch64',False),('riscv64',True),('riscv64',False),('x86_64',False)]:
            if target=='x86_64' and not native_x86_supported():continue
            for typ,bits,sign in TYPES:
                lo=-(1<<(bits-1)) if sign else 0
                hi=(1<<(bits-1))-1 if sign else (1<<bits)-1
                # The numerator and divisor are deliberately used at multiple
                # places and in different orders, exercising SSA and scratch liveness.
                divs=(7, -3, 5, -7) if sign else (7, 3, 5, 11)
                if not all(lo<=d<=hi for d in divs):continue
                expr=f'(x / {divs[0]}) + (x % {divs[1]}) + (y / {divs[2]}) + (y % {divs[3]})'
                src.write_text(f'module division.multisite; export fn f(x:{typ},y:{typ})->{typ} @abi(c) '
                               '{ return '+expr+'; }\n',encoding='utf8')
                options=[a.compiler,'build','--target',target,'--emit','bin','-j','1','-o',str(out),str(src)]
                if target=='x86_64':options+=['--abi','win64' if os.name=='nt' else 'sysv']
                if m:options+=['--feature','m']
                for policy in ('hot','size'):
                    env=os.environ.copy();env['ASTERASM_DIV_POLICY']=policy
                    cp=subprocess.run(options,capture_output=True,text=True,env=env,timeout=35)
                    if cp.returncode or not out.is_file():
                        raise AssertionError(('multisite compiler error',target,typ,policy,cp.stderr))
                    code=out.read_bytes()
                    if target=='riscv64' and not m:
                        assert len(code)%4==0
                        if any((w&127) in (0x33,0x3b) and (w>>25)==1 for w in
                               (int.from_bytes(code[i:i+4],'little') for i in range(0,len(code),4))):
                            raise AssertionError('RV64I emitted M')
                    values=[lo,lo+1,lo+2,-7,-1,0,1,7,hi-1,hi]
                    values=[x for x in values if lo<=x<=hi]
                    samples=[(x,hi-x if not sign else -x if x!=-lo else x) for x in values]
                    samples=[(x,y) for x,y in samples if lo<=y<=hi]
                    samples += [(rng.randint(lo,hi),rng.randint(lo,hi)) for _ in range(3 if a.quick else 30)]
                    for x,y in samples:
                        def quo(a,d):return truncdiv(a,d) if sign else a//d
                        if sign:
                            r1=x - quo(x,divs[1])*divs[1]
                            r2=y - quo(y,divs[3])*divs[3]
                        else:r1=x % divs[1];r2=y % divs[3]
                        exp=quo(x,divs[0])+r1+quo(y,divs[2])+r2
                        exp &= (1<<bits)-1
                        if sign:exp=sx(exp,bits)
                        if target=='aarch64':got=a64_exec(code,x,y,check_abi=True)
                        elif target=='riscv64':got=rv64_exec(code,x,m,y,check_abi=True)
                        else:got=run_native(code,DTYPES[bits,sign],x,y)
                        got=sx(got,bits) if sign else got&((1<<bits)-1)
                        if got!=exp:
                            raise AssertionError(('multisite mismatch',target,m,typ,policy,x,y,got,exp,code.hex()))
                        count+=1
    print(f'PASS: {count} independently executed multi-site /% ABI tests across 4 ISA/feature profiles')

if __name__=='__main__':main()
