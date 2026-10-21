#!/usr/bin/env python3
"""End-to-end AsterASM -> ELF64 ET_REL -> ld.lld linked native call.

Tests relocation records from the AsterASM object writer, *not* merely a
synthetic assembler object. No target CPU execution is claimed.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def run(args, success=True):
    p=subprocess.run(args,capture_output=True,text=True,timeout=30)
    if (p.returncode==0) != success:
        raise AssertionError(f'{args}\nreturn={p.returncode}\n{p.stdout}\n{p.stderr}')
    return p


def main():
    a=argparse.ArgumentParser()
    a.add_argument('--compiler',required=True)
    a.add_argument('--target',choices=('aarch64','riscv64'),required=True)
    a.add_argument('--clang',required=True)
    a.add_argument('--readelf',required=True)
    a.add_argument('--lld',required=True)
    a.add_argument('--objdump',required=True)
    a.add_argument('--dumper')
    args=a.parse_args()
    arch=args.target
    arm=arch=='aarch64'
    save='sub {dst:sp,lhs:sp,rhs:16}; str {src:x30,offset:0};' if arm else 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0};'
    restore='ldr {dst:x30,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret;' if arm else 'ld {dst:ra,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;'
    call='bl {symbol:external};' if arm else 'jal {dst:ra,symbol:external};'
    with tempfile.TemporaryDirectory(prefix='aster-link-') as tmp:
        folder=Path(tmp)
        source=folder/'client.aa'
        source.write_text('module test.elf_symbol;\nextern fn external(a:u64,b:u64)->u64;\n'
                          'export fn f(a:u64,b:u64)->u64 @naked {\n'
                          f'unsafe exact {arch} {{ {save} {call} {restore} }}\n}}\n',encoding='utf8')
        obj=folder/'client.o'
        run([args.compiler,'build','--target',arch,'--emit','obj','--format','elf',
             '-j','1','-o',str(obj),str(source)])
        elf=run([args.readelf,'-h','-SW','-r','-sW',str(obj)]).stdout
        name='R_AARCH64_CALL26' if arm else 'R_RISCV_CALL_PLT'
        if args.dumper:
            own = run([args.dumper,'--target',arch,'--format','text','--strict',str(obj)]).stdout
            if name not in own or 'external' not in own or 'relocation pending' not in own:
                raise AssertionError(f'asterdump did not show native call relocation: {own}')
        if name not in elf or 'UND external' not in elf or '.rela.text' not in elf or 'ELF64' not in elf:
            raise AssertionError(f'bad {arch} ELF object: {elf}')
        # COFF is explicitly unsupported by the small C fallback; do not
        # emit a misleading ELF object under the wrong object-format flag.
        run([args.compiler,'build','--target',arch,'--emit','obj','--format','coff',
             '-j','1','-o',str(folder/'no-coff.obj'),str(source)],success=False)
        if (folder/'no-coff.obj').exists():
            raise AssertionError('unsupported COFF emitted partial object')
        asm=folder/'external.s'; external=folder/'external.o'
        asm.write_text('.text\n.global external\n.type external,@function\nexternal:\n'
                       + ('ret\n' if arm else 'ret\n'),encoding='utf8')
        cmd=[args.clang,'-target','aarch64-none-elf' if arm else 'riscv64-none-elf']
        if not arm: cmd+=['-march=rv64i','-mabi=lp64']
        run(cmd+['-c',str(asm),'-o',str(external)])
        exe=folder/'linked.elf'
        run([args.lld,'-m','aarch64elf' if arm else 'elf64lriscv','-e','f',
             str(obj),str(external),'-o',str(exe)])
        reloc_linked=run([args.readelf,'-r',str(exe)]).stdout
        if 'R_AARCH64_CALL26' in reloc_linked or 'R_RISCV_CALL_PLT' in reloc_linked:
            raise AssertionError('unresolved call relocation in linked executable')
        asmout=run([args.objdump,'-d',str(exe)]).stdout
        if not re.search(r'<external>:',asmout) or not re.search(r'\bbl\b[^\n]*<external>' if arm else r'\bjalr\b[^\n]*<external>',asmout):
            raise AssertionError(f'{arch}: linked call does not resolve to external: {asmout}')
        # Distinguish symbol resolution from ELF correctness: a missing
        # provider must be rejected by the linker.
        run([args.lld,'-m','aarch64elf' if arm else 'elf64lriscv','-e','f',
             str(obj),'-o',str(folder/'undefined.elf')],success=False)
        print(f'{arch}: ELF64 RELA + symbol + ld.lld link + resolved machine call PASS')


if __name__ == '__main__':
    main()
