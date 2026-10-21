#!/usr/bin/env python3
"""Two-argument narrow/wide division execution + target zero/overflow contracts.

AArch64/RV64 execute *real emitted bytes* in independent bounded decoders.
Native x86-64 executes under the host ABI, excluding explicit fault cases.
RV64I must remain M-free. Zero and MIN/-1 are checked against each target
instruction set, not silently treated as universal language semantics.
"""
import argparse
import ctypes
import os
import subprocess
import tempfile
from pathlib import Path
from compiler_narrow_isa_execution import a64_exec, rv64_exec, sx, TYPES
from native_jit import executable_code, native_x86_supported


def reference(x, y, bits, sign, op, target):
    mask = (1 << bits)-1
    x &= mask
    y &= mask
    if sign:
        x=sx(x,bits)
        y=sx(y,bits)
    if y==0:
        if target=='aarch64':return 0 if op=='/' else x
        if target=='x86_64':return None
        q=-1 if sign else mask
    elif sign and x==-(1<<(bits-1)) and y == -1:
        if target=='x86_64' and bits>=32:return None
        q=x
    elif sign:
        q=(abs(x)//abs(y))*(-1 if (x<0)!=(y<0) else 1)
    else:q=x//y
    result=q if op=='/' else (x-q*y)
    result &= mask
    return sx(result,bits) if sign else result


def run_native(code, bits, sign, x, y):
    types = {
        (8,False):ctypes.c_uint8,(8,True):ctypes.c_int8,
        (16,False):ctypes.c_uint16,(16,True):ctypes.c_int16,
        (32,False):ctypes.c_uint32,(32,True):ctypes.c_int32,
        (64,False):ctypes.c_uint64,(64,True):ctypes.c_int64,
    }
    typ=types[bits,sign]
    with executable_code(code) as address:
        callback=ctypes.CFUNCTYPE(typ,typ,typ)(address)
        return callback(x,y)


def main():
    pa=argparse.ArgumentParser()
    pa.add_argument('--compiler',required=True)
    pa.add_argument('--quick',action='store_true')
    a=pa.parse_args()
    n=0
    with tempfile.TemporaryDirectory(prefix='aster-dynamic-div-isa-') as td:
        root=Path(td)
        src=root/'d.aa'; out=root/'d.bin'
        for target,with_m in [('aarch64',False),('riscv64',True),('riscv64',False),('x86_64',False)]:
            if target=='x86_64' and not native_x86_supported():
                continue
            for typ,bits,sign in TYPES:
                minval=-(1<<(bits-1)) if sign else 0
                maxval=(1<<(bits-1))-1 if sign else (1<<bits)-1
                trials=[(minval,-1),(minval,0),(minval,1),(minval,3),
                        (minval+1,-1),(-128,7),(-7,3),(-1,0),(-1,-1),
                        (0,0),(0,-1),(0,3),(1,0),(1,-1),(1,3),
                        (3,0),(3,1),(3,2),(7,3),(127,7),
                        (maxval,0),(maxval,-1),(maxval,1),(maxval,3)]
                trials=[(x,y) for x,y in trials if minval<=x<=maxval and minval<=y<=maxval]
                if a.quick: trials=trials[:8]
                for op in ('/','%'):
                    src.write_text(f'module division.dynamic; export fn f(x:{typ},y:{typ})->{typ} @abi(c) '
                                   f'{{ return x {op} y; }}\n',encoding='utf8')
                    command=[a.compiler,'build','--target',target,'--emit','bin',
                             '-j','1','-o',str(out),str(src)]
                    if target=='x86_64':command+=['--abi','win64' if os.name=='nt' else 'sysv']
                    if with_m:command.extend(['--feature','m'])
                    cp=subprocess.run(command,capture_output=True,text=True,timeout=30)
                    if cp.returncode or not out.is_file():
                        raise AssertionError(('compile',target,typ,op,cp.stderr))
                    code=out.read_bytes()
                    if target=='riscv64' and not with_m:
                        assert len(code)%4==0
                        if any((w&127) in (0x33,0x3b) and (w>>25)==1 for w in
                                (int.from_bytes(code[i:i+4],'little') for i in range(0,len(code),4))):
                            raise AssertionError(('RV64I unexpectedly has M',typ,op))
                    for x,y in trials:
                        exp=reference(x,y,bits,sign,op,target)
                        if exp is None:continue
                        if target=='x86_64':got=run_native(code,bits,sign,x,y)
                        elif target=='aarch64':got=a64_exec(code,x,y,check_abi=True)
                        else:got=rv64_exec(code,x,with_m,y,check_abi=True)
                        got=sx(got,bits) if sign else got & ((1<<bits)-1)
                        if got!=exp:
                            raise AssertionError(('mismatch',target,with_m,typ,op,x,y,got,exp,code.hex()))
                        n+=1
    print('PASS:',n,'dynamic /% actual x86 calls or interpreted AArch64/RV64 calls; zero+overflow ISA policies checked')

if __name__=='__main__':main()
