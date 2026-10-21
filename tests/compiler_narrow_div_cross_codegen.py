#!/usr/bin/env python3
"""Narrow division backend smoke: A64/RV64M/RV64I fixed words, feature gates.
This is an ISA/compile oracle, NOT native target execution."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from native_manual_cross_isa import assemble

TYPES = ('u8','i8','u16','i16','u32','i32')

def main():
    pa=argparse.ArgumentParser()
    pa.add_argument('--compiler',required=True)
    pa.add_argument('--clang')
    args=pa.parse_args()
    cases = oracle = fallback = 0
    with tempfile.TemporaryDirectory(prefix='aster-s23-cross-narrow-') as td:
        path=Path(td); src=path/'s.aa'; binary=path/'s.bin'
        for arch, m in [('aarch64',False),('riscv64',True),('riscv64',False)]:
            for typ in TYPES:
                for divisor in ([3,7,8,-7] if typ.startswith('i') else [3,7,8,31]):
                    for operation in ('/','%'):
                        for policy in ('hot','size'):
                            src.write_text('module division.narrow; '+f'export fn f(x:{typ})->{typ} @abi(c) '+
                                           '{ return x '+operation+' '+str(divisor)+'; }\n')
                            env=os.environ.copy(); env['ASTERASM_DIV_POLICY']=policy
                            cmd=[args.compiler,'build','--target',arch,'--emit','bin',
                                 '-o',str(binary),'-j','1',str(src)]
                            if arch=='riscv64' and m: cmd.extend(['--feature','m'])
                            proc=subprocess.run(cmd,capture_output=True,text=True,timeout=30,env=env)
                            if proc.returncode or not binary.is_file():
                                raise AssertionError((arch,m,typ,divisor,operation,policy,proc.stderr))
                            data=binary.read_bytes()
                            if not data or len(data)%4: raise AssertionError('unaligned target binary')
                            words=[int.from_bytes(data[i:i+4],'little') for i in range(0,len(data),4)]
                            if arch=='riscv64':
                                op_m=[w for w in words if (w&0x7f) in (0x33,0x3b) and (w>>25)==1]
                                if not m and op_m:
                                    raise AssertionError(('illegal RV64 M instruction in RV64I',typ,policy,op_m))
                                if not m: fallback+=1
                            else:
                                divwords=[w for w in words if (w&0x7fe0fc00) in (0x1ac00800,0x1ac00c00)]
                                if not divwords and not (typ.startswith('u') and divisor == 8):
                                    raise AssertionError(('missing A64 hardware division',typ,divisor,operation,policy))
                                if args.clang and divwords:
                                    w=divwords[0]
                                    kind='sdiv' if (w&0x7fe0fc00)==0x1ac00c00 else 'udiv'
                                    width='x' if w & (1<<31) else 'w'
                                    dst, lhs, rhs = w&31,(w>>5)&31,(w>>16)&31
                                    reference=assemble(args.clang,'aarch64',
                                              f'{kind} {width}{dst},{width}{lhs},{width}{rhs}', td)
                                    if reference!=w.to_bytes(4,'little'):
                                        raise AssertionError(('A64 Clang byte mismatch',w,reference.hex()))
                                    oracle+=1
                            cases+=1
        # Constant divisor zero is an error, not silent reciprocal fallback.
        src.write_text('module division.zero; export fn f(x:u32)->u32 @abi(c) { return x / 0; }\n')
        for target in ('x86_64','aarch64','riscv64'):
            proc=subprocess.run([args.compiler,'build','--target',target,'-o',str(binary),str(src)],
                                capture_output=True,text=True,timeout=30)
            if proc.returncode==0: raise AssertionError(('accepted constant-zero division',target))
    print(f'narrow cross: {cases} target/width/profile cases, {oracle} Clang DIV encodings, '
          f'{fallback} RV64I non-M cases, zero-divisor rejection PASS')

if __name__=='__main__': main()
