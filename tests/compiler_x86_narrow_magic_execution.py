#!/usr/bin/env python3
"""Slice23 native x86-64 narrow / and %: independent Python oracle and Clang MUL encoding."""
import argparse
import ctypes
import mmap
import os
import platform
import random
import subprocess
import tempfile
from pathlib import Path

WIDTHS = (("u8", 8, False), ("i8", 8, True),
          ("u16", 16, False), ("i16", 16, True),
          ("u32", 32, False), ("i32", 32, True))

def expected(x, d, signed, width, op):
    if signed:
        q = (abs(x) // abs(d)) * (-1 if (x < 0) != (d < 0) else 1)
    else:
        q = x // d
    val = q if op == '/' else x - q * d
    mask = (1 << width) - 1
    val &= mask
    if signed and val >= (1 << (width - 1)):
        val -= 1 << width
    return val

def run_native(blob, signed, width):
    p = mmap.mmap(-1, max(4096, len(blob)), prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
    p.write(blob)
    ty = {(True,8):ctypes.c_int8,(True,16):ctypes.c_int16,(True,32):ctypes.c_int32,
          (False,8):ctypes.c_uint8,(False,16):ctypes.c_uint16,(False,32):ctypes.c_uint32}[(signed,width)]
    fn = ctypes.CFUNCTYPE(ty, ty)(ctypes.addressof(ctypes.c_char.from_buffer(p)))
    return p, fn

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--clang')
    ap.add_argument('--execute', action='store_true')
    ap.add_argument('--policy', choices=('hot','size'), default='hot')
    a = ap.parse_args()
    if a.execute and (platform.system() != 'Linux' or platform.machine().lower() not in ('x86_64', 'amd64')):
        raise SystemExit('unsupported native host')
    rng = random.Random(0x23A57)
    cases = executes = oracle_ops = 0
    with tempfile.TemporaryDirectory(prefix='aster-s23-narrow-') as td:
        folder = Path(td)
        for typ,width,signed in WIDTHS:
            low = -(1 << (width-1)) if signed else 0
            high = (1 << (width-1))-1 if signed else (1 << width)-1
            divisors = [-127,-31,-19,-7,-5,-3,3,5,7,11,19,31,63,127,257,65537,2147483647]
            for d in divisors:
                if d < low or d > high: continue
                for symbol in ('/', '%'):
                    src=folder/'f.aa'; dest=folder/'f.bin'
                    src.write_text('module t.narrow; '+f'export fn f(x:{typ})->{typ} @abi(c) '+
                                   '{ return x '+symbol+' '+str(d)+'; }\n')
                    environment=os.environ.copy()
                    environment['ASTERASM_DIV_POLICY']=a.policy
                    cp=subprocess.run([a.compiler,'build','--target','x86_64','--emit','bin','-j','1',
                                       '-o',str(dest),str(src)],capture_output=True,text=True,timeout=30,env=environment)
                    if cp.returncode or not dest.is_file():
                        raise AssertionError((typ,d,symbol,cp.stdout,cp.stderr))
                    blob=dest.read_bytes()
                    # MUL r/m32 must preserve explicit EAX:EDX effects and use F7 /4.
                    patterns=[blob[i:i+3] for i in range(len(blob)-2)
                              if (0x40<=blob[i]<=0x4f and blob[i+1]==0xf7 and (blob[i+2]&0xf8)==0xe0)]
                    if len(patterns)!=(0 if a.policy=='size' else 1):
                        raise AssertionError((typ,d,symbol,'wrong MUL32 lowering',blob.hex()))
                    if a.clang and patterns:
                        pattern=patterns[0]
                        r=(pattern[2]&7)+(8 if pattern[0]&1 else 0)
                        name=(['eax','ecx','edx','ebx','esp','ebp','esi','edi'][r] if r<8 else 'r%dd'%r)
                        asm=folder/'oracle.s'; obj=folder/'oracle.o'; raw=folder/'oracle.bin'
                        asm.write_text('.text\nmull %'+name+'\n')
                        for command in ([a.clang,'--target=x86_64-unknown-linux-gnu','-c',str(asm),'-o',str(obj)],
                                        ['objcopy','-O','binary','-j','.text',str(obj),str(raw)]):
                            chk=subprocess.run(command, capture_output=True,text=True)
                            if chk.returncode: raise AssertionError(chk.stderr)
                        # Clang may omit the optional 0x40 REX for r0-r7.
                        if raw.read_bytes()!=pattern and not (r<8 and raw.read_bytes()==pattern[1:]):
                            raise AssertionError(('Clang MUL32 mismatch',pattern.hex(),raw.read_bytes().hex()))
                        oracle_ops+=1
                    cases+=1
                    if a.execute:
                        page,fn=run_native(blob,signed,width)
                        try:
                            values=[low,low+1,low+2,-127,-19,-7,-1,0,1,2,3,5,7,19,127,high-2,high-1,high]
                            values=[x for x in values if low<=x<=high]
                            if width==8: values=list(range(low,high+1))
                            else: values += [rng.randint(low,high) for _ in range(256)]
                            for x in values:
                                if signed and x==low and d==-1: continue
                                got=fn(x)
                                ref=expected(x,d,signed,width,symbol)
                                if got != ref: raise AssertionError((typ,symbol,x,d,got,ref,blob.hex()))
                                executes+=1
                        finally: page.close()
    print(f'narrow magic: {cases} source+ISA, {oracle_ops} Clang encodings, {executes} native calls PASS')

if __name__=='__main__': main()
