#!/usr/bin/env python3
"""Compile arbitrary unsigned u64 constants and independently check MULH bytes.

The C17 arithmetic reference is in test_div_magic.c; this tests the entire
frontend → legalization → regalloc → physical verifier → encoder pipeline.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
from native_manual_cross_isa import assemble

DIVISORS = [3, 5, 6, 7, 9, 10, 11, 13, 17, 19, 31, 63, 127, 255, 257,
            65535, 65537, (1 << 32) - 1, (1 << 32) + 1,
            (1 << 63) - 1, (1 << 63) + 1, (1 << 64) - 3,
            (1 << 64) - 1]


def inspect_machine(binary, arch, op, want_mulhi, assembler, work):
    raw = binary.read_bytes()
    if not raw or len(raw) % 4:
        raise AssertionError('invalid machine code size')
    words = [int.from_bytes(raw[i:i+4], 'little') for i in range(0, len(raw), 4)]
    if arch == 'aarch64':
        mulhi = [w for w in words if w & 0xffe0fc00 == 0x9bc07c00]
        divs = [w for w in words if w & 0x7fe0fc00 == 0x1ac00800]
        if want_mulhi and (len(mulhi) != 1 or divs):
            raise AssertionError(f'A64 magic not selected: {words}')
        if not want_mulhi and mulhi:
            raise AssertionError('incorrect A64 magic selected')
        if op == '%' and want_mulhi and not any(w & 0x7f008000 == 0x1b008000 for w in words):
            raise AssertionError('A64 remainder missing MSUB')
        if assembler and mulhi:
            w = mulhi[0]
            rd, rn, rm = w & 31, (w >> 5) & 31, (w >> 16) & 31
            oracle = assemble(assembler, arch, f'umulh x{rd},x{rn},x{rm}', work)
            if oracle != w.to_bytes(4, 'little'):
                raise AssertionError('A64 UMULH differs from Clang assembler')
    else:
        mulhi = [w for w in words if w & 0xfe00707f == 0x02003033]
        divs = [w for w in words if w & 0xfe00707f in (0x02005033, 0x02007033)]
        if want_mulhi and (len(mulhi) != 1 or divs):
            raise AssertionError(f'RV64 magic not selected: {words}')
        if not want_mulhi and mulhi:
            raise AssertionError('RV64 missing extension gate')
        if op == '%' and want_mulhi and not any(w & 0xfe00707f == 0x02000033 for w in words):
            raise AssertionError('RV64 remainder missing MUL')
        if assembler and mulhi:
            w=mulhi[0]
            rd, rs1, rs2=(w>>7)&31,(w>>15)&31,(w>>20)&31
            oracle=assemble(assembler,arch,f'.option arch, +m\nmulhu x{rd},x{rs1},x{rs2}',work)
            if oracle!=w.to_bytes(4,'little'):
                raise AssertionError('RV64 MULHU differs from Clang assembler')


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--compiler',required=True)
    ap.add_argument('--clang')
    ap.add_argument('--dumper')
    args=ap.parse_args()
    cases=0
    with tempfile.TemporaryDirectory(prefix='aster-div-magic-') as tmp:
        root=Path(tmp)
        for arch in ('aarch64','riscv64'):
            for divisor in DIVISORS:
                for operation in ('/','%'):
                    for extension in ([True,False] if arch=='riscv64' else [True]):
                        src=root/'test.aa'; binary=root/'test.bin'
                        src.write_text('module test.magic; export fn value(x:u64)->u64 @abi(c) '
                                       f'{{ return x {operation} {divisor}; }}\n',encoding='utf8')
                        cmd=[args.compiler,'build','--target',arch,'--dump-regalloc',
                             '-j','1','-o',str(binary),str(src)]
                        if arch=='riscv64' and extension:
                            cmd.extend(['--feature','m'])
                        res=subprocess.run(cmd,capture_output=True,text=True,timeout=45)
                        if res.returncode or not binary.is_file():
                            raise AssertionError((arch,operation,divisor,extension,res.stdout,res.stderr))
                        inspect_machine(binary,arch,operation,extension,args.clang,tmp)
                        if args.dumper and arch=='aarch64':
                            # Full A64 strength-reduced quotient/remainder is
                            # now recognized by the strict LLVM-free dumper.
                            whole=subprocess.run([args.dumper,'--target',arch,'--strict',str(binary)],
                                                 capture_output=True,text=True,timeout=20)
                            if whole.returncode or '.word' in whole.stdout:
                                raise AssertionError(('A64 full strict dump rejected',divisor,
                                                      operation,whole.stdout,whole.stderr))
                        if args.dumper and extension:
                            # Isolate MULH to prove opcode-level correctness
                            # independent of every other instruction in a body.
                            data=binary.read_bytes()
                            for i in range(0,len(data),4):
                                w=int.from_bytes(data[i:i+4],'little')
                                if (w & (0xffe0fc00 if arch=='aarch64' else 0xfe00707f)) == \
                                        (0x9bc07c00 if arch=='aarch64' else 0x02003033):
                                    alone=root/'mulhi_only.bin'
                                    alone.write_bytes(data[i:i+4])
                                    dumped=subprocess.run([args.dumper,'--target',arch,'--strict',str(alone)],
                                                          capture_output=True,text=True,timeout=20)
                                    expected='umulh' if arch=='aarch64' else 'mulhu'
                                    if dumped.returncode or expected not in dumped.stdout:
                                        raise AssertionError(('dumper rejects MULHI',arch,divisor,
                                                              dumped.stdout,dumped.stderr))
                                    break
                            else:
                                raise AssertionError('missing high-half multiply')
                        symbol='AST_MACHINE_A64_UMULH' if arch=='aarch64' else 'AST_MACHINE_RISCV_MULHU'
                        if (symbol in res.stdout)!=extension:
                            raise AssertionError((arch,divisor,operation,extension,res.stdout))
                        cases+=1
        # Existing typed signed semantics remain delegated to SDIV and RV soft or M DIV.
        for arch in ('aarch64','riscv64'):
            src=root/'test.aa';binary=root/'test.bin'
            src.write_text('module test.magic; export fn value(x:i64)->i64 @abi(c) '
                           '{ return x / -7; }\n',encoding='utf8')
            result=subprocess.run([args.compiler,'build','--target',arch,'-o',str(binary),str(src)],
                                  capture_output=True,text=True,timeout=45)
            if result.returncode or not binary.is_file():
                raise AssertionError(('signed regression',arch,result.stdout,result.stderr))
    print(f'u64 magic: {cases} cross-ISA codegen cases plus 2 signed-regression builds passed')

if __name__=='__main__':main()
