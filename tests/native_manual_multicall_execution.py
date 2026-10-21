#!/usr/bin/env python3
"""Cross-ISA local multicall execution plus independently assembled bytes.

A deliberately limited step-bounded ISA interpreter is *not* QEMU/hardware.
It proves selected emitted call sequences return through the expected PC.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from native_manual_call_execution import execute, MASK
from native_manual_cross_isa import assemble
from native_manual_multicall import scenarios


def oracle(arch, which):
    arm = arch == 'aarch64'
    if arm:
        frame, save, restore, release, add = ('sub sp,sp,#16', 'str x30,[sp]',
                                               'ldr x30,[sp]', 'add sp,sp,#16',
                                               'add x0,x0,x1')
        save_arg, load_arg, cal = 'str x1,[sp,#8]', 'ldr x1,[sp,#8]', 'bl'
    else:
        frame, save, restore, release, add = ('addi sp,sp,-16', 'sd ra,0(sp)',
                                               'ld ra,0(sp)', 'addi sp,sp,16',
                                               'add a0,a0,a1')
        save_arg, load_arg, cal = 'sd a1,8(sp)', 'ld a1,8(sp)', 'jal ra,'
    epilogue = [restore, release, 'ret']
    if which == 0:  # same leaf, two distinct call continuations
        if arm:
            return '\n'.join([frame,save,save_arg, 'bl shared',load_arg,
                              'bl shared',*epilogue,'shared:',add,'ret'])
        return '\n'.join([frame,save,save_arg,'jal ra,24',load_arg,
                          'jal ra,16',*epilogue,add,'ret'])
    if which in (2, 3):  # 2- or 3-deep exact returns
        depth = 2 if which == 2 else 3
        out = []
        for level in range(depth):
            out += [frame,save]
            if arm:
                out.append('bl leaf'+str(level+1))
            else:
                out.append('jal ra,16')
            out += epilogue
            if arm: out.append('leaf'+str(level+1)+':')
        out += [add, 'ret']
        return '\n'.join(out)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--target', choices=('aarch64','riscv64'), required=True)
    ap.add_argument('--clang')
    args = ap.parse_args()
    cases = scenarios(args.target)
    inputs = (0, 1, 7, 19, 255, 2**32, 2**63, MASK)
    passes = 0
    for idx, (accepted, code, title) in enumerate(cases):
        if not accepted: continue
        with tempfile.TemporaryDirectory(prefix='aster14-exec-') as td:
            src, out = Path(td) / 'test.aa', Path(td) / 'test.bin'
            src.write_text('module tests.multicall_exec;\n'
                           'export fn raw(a:u64,b:u64)->u64 @naked {\n'
                           f'unsafe exact {args.target} {{ {code} }}\n}}\n',encoding='utf-8')
            process = subprocess.run([args.compiler,'build','--target',args.target,
                                      '--emit','bin','-j','1','-o',str(out),str(src)],
                                     capture_output=True,text=True,timeout=30)
            if process.returncode or not out.is_file():
                raise AssertionError(f'{args.target} {title}: {process.stderr}')
            binary = out.read_bytes()
            if args.clang:
                reference = oracle(args.target,idx)
                if reference:
                    external = assemble(args.clang,args.target,reference,td)
                    if binary != external:
                        raise AssertionError(f'{args.target} {title}: bytes diverge '
                                             f'{binary.hex()} != {external.hex()}')
            for x in inputs:
                for y in inputs:
                    additions = (int(title.split('-')[0]) if title.endswith('-call-sites')
                                 else (2 if idx in (0, 1) else (1 if idx in (2, 3) else 0)))
                    wanted = (x + additions * y) & MASK
                    actual = execute(args.target,binary,x,y)
                    if actual != wanted:
                        raise AssertionError(f'{args.target} {title}: {x=} {y=} {actual=} {wanted=}')
                    passes += 1
    print(f'{args.target}: {passes} executed call-context cases, independent byte oracle')


if __name__ == '__main__':
    main()
