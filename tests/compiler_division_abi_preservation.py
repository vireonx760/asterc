#!/usr/bin/env python3
"""Division ABI probe: seeded nonvolatile registers + exact /% math on BIN.

A64/RV64: independent machine-word interpreter with poisoned ABI registers.
x86_64 SysV: native raw machine bytes called by independently assembled GAS
probe which checks RBX/RBP/R12-R15, not Python ctypes's wrapper.
"""
import argparse
import ctypes
import mmap
import os
import platform
import random
import subprocess
import tempfile
from pathlib import Path
from compiler_narrow_isa_execution import a64_exec, rv64_exec, sx, TYPES
from compiler_multisite_div_abi import truncdiv

ROOT=Path(__file__).resolve().parent

def expected(x, y, d1, d2, width, signed):
    if signed:
        result=truncdiv(x,d1) + (y-truncdiv(y,d2)*d2)
    else:
        result=x//d1 + (y%d2)
    result &= (1<<width)-1
    return sx(result,width) if signed else result

def run():
    ap=argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--cc', default='cc')
    ap.add_argument('--quick', action='store_true')
    args=ap.parse_args()
    random_gen=random.Random(0xD1A6A81)
    samples=0
    compiled=0
    profiles=[('x86_64',False),('aarch64',False),('riscv64',True),('riscv64',False)]
    if platform.system()!='Linux' or platform.machine() not in ('x86_64','amd64'):
        profiles=[(a,m) for a,m in profiles if a!='x86_64']
    with tempfile.TemporaryDirectory(prefix='aster-division-abi-proof-') as temp:
        root=Path(temp)
        lib=None
        if any(arch=='x86_64' for arch,_ in profiles):
            probe_path=root/'probe.so'
            subprocess.run([args.cc,'-fPIC','-shared','-o',str(probe_path),
                            str(ROOT/'division_x86_abi_probe.S')],check=True,
                           capture_output=True,text=True,timeout=25)
            lib=ctypes.CDLL(str(probe_path))
            lib.aster_div_probe.argtypes=[ctypes.c_void_p,ctypes.c_uint64,ctypes.c_uint64,
                                           ctypes.POINTER(ctypes.c_uint64)]
            lib.aster_div_probe.restype=ctypes.c_uint64
        src=root/'division.aa'; dest=root/'division.bin'
        for arch,with_m in profiles:
            for typ,width,signed in TYPES:
                lo=-(1<<(width-1)) if signed else 0
                hi=(1<<(width-1))-1 if signed else (1<<width)-1
                pairs=[(7,-3),(31,-7),(3,11),(127,-31)] if signed else [(7,3),(31,11),(3,251),(127,7)]
                if args.quick:pairs=pairs[:1]
                for d1,d2 in pairs:
                    if not lo<=d1<=hi or not lo<=d2<=hi:continue
                    src.write_text('module division.abi_preservation; '
                        f'export fn f(x:{typ},y:{typ})->{typ} @abi(c) '
                        '{ return (x / '+str(d1)+') + (y % '+str(d2)+'); }\n',encoding='utf8')
                    command=[args.compiler,'build','--target',arch,'--emit','bin',
                            '-j','1','-o',str(dest),str(src)]
                    if with_m:command+=['--feature','m']
                    for policy in ('hot', 'size'):
                        env=os.environ.copy();env['ASTERASM_DIV_POLICY']=policy
                        result=subprocess.run(command,capture_output=True,text=True,timeout=30,env=env)
                        if result.returncode or not dest.is_file():
                            raise AssertionError(('compiler',arch,typ,policy,result.stderr))
                        code=dest.read_bytes();compiled+=1
                        if arch=='riscv64' and not with_m:
                            for i in range(0,len(code),4):
                                inst=int.from_bytes(code[i:i+4],'little')
                                if (inst&127) in (0x33,0x3b) and (inst>>25)==1:
                                    raise AssertionError(('RV64I M extension',typ,policy))
                        values=[lo,lo+1,lo+2,-31,-7,-3,-1,0,1,2,3,7,31,hi-1,hi]
                        values=[x for x in values if lo<=x<=hi]
                        tests=[(x,y) for x in values for y in values[:(5 if args.quick else len(values))]]
                        tests.extend((random_gen.randint(lo,hi),random_gen.randint(lo,hi))
                                     for _ in range(8 if args.quick else 128))
                        memory=None
                        if arch=='x86_64':
                            memory=mmap.mmap(-1,max(4096, len(code)),prot=mmap.PROT_READ|mmap.PROT_WRITE|mmap.PROT_EXEC)
                            memory.write(code)
                            addr=ctypes.addressof(ctypes.c_char.from_buffer(memory))
                        try:
                            for x,y in tests:
                                want=expected(x,y,d1,d2,width,signed)
                                if arch=='aarch64':got=a64_exec(code,x,y,check_abi=True)
                                elif arch=='riscv64':got=rv64_exec(code,x,with_m,y,check_abi=True)
                                else:
                                    received=ctypes.c_uint64()
                                    rc=lib.aster_div_probe(ctypes.c_void_p(addr),
                                         ctypes.c_uint64(x & ((1<<64)-1)),
                                         ctypes.c_uint64(y & ((1<<64)-1)),ctypes.byref(received))
                                    if rc:raise AssertionError(('x86-64 callee-saved corruption',typ,policy,x,y))
                                    got=received.value
                                got=sx(got,width) if signed else got & ((1<<width)-1)
                                if got!=want:
                                    raise AssertionError(('ABI arithmetic',arch,with_m,typ,d1,d2,policy,x,y,got,want))
                                samples+=1
                        finally:
                            if memory is not None:memory.close()
    print(f'PASS: {compiled} compiled BIN variants, {samples} ABI preservation and arithmetic calls')

if __name__=='__main__':run()
