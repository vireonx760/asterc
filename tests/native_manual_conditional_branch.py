#!/usr/bin/env python3
"""Native G1 conditional forward edges: independent assembler oracle + ABI negatives."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from native_manual_cross_isa import assemble


def cases(arch):
    if arch == 'aarch64':
        yes = 'cbz {reg:x0,target:done};'
        no = 'cbnz {reg:w0,target:done};'
        asm_yes, asm_no = 'cbz x0, done', 'cbnz w0, done'
        arith = 'add {dst:x0,lhs:x0,rhs:x1};'
        arith_asm = 'add x0, x0, x1'
        alloc, dealloc = 'sub {dst:sp,lhs:sp,rhs:16};', 'add {dst:sp,lhs:sp,rhs:16};'
        save, restore = 'str {src:x19,offset:0};', 'ldr {dst:x19,offset:0};'
        reg_bad = 'cbz {reg:sp,target:done};'
        link_bad = 'cbz {reg:x30,target:done};'
        args_bad = 'cbz {dst:x0,target:done};'
        unreachable = 'cbz {reg:x0,target:done};'
    else:
        yes = 'beq {lhs:a0,rhs:a1,target:done};'
        no = 'bne {lhs:a0,rhs:a1,target:done};'
        asm_yes, asm_no = 'beq a0, a1, 8', 'bne a0, a1, 8'
        arith = 'add {dst:a0,lhs:a0,rhs:a1};'
        arith_asm = 'add a0, a0, a1'
        alloc, dealloc = 'addi {dst:sp,lhs:sp,rhs:-16};', 'addi {dst:sp,lhs:sp,rhs:16};'
        save, restore = 'sd {src:s0,offset:0};', 'ld {dst:s0,offset:0};'
        reg_bad = 'beq {lhs:sp,rhs:a1,target:done};'
        link_bad = 'beq {lhs:ra,rhs:a1,target:done};'
        args_bad = 'beq {reg:a0,target:done};'
        unreachable = 'beq {lhs:a0,rhs:a1,target:done};'
    return [
        (True, f'{yes} {arith} label {{name:done}}; ret;',
         f'{asm_yes}\n{arith_asm}\ndone:\nret'),
        (True, f'{no} {arith} label {{name:done}}; ret;',
         f'{asm_no}\n{arith_asm}\ndone:\nret'),
        (True, f'{alloc} {yes} {arith} label {{name:done}}; {dealloc} ret;', None),
        (True, f'{alloc} {save} {no} {arith} label {{name:done}}; {restore} {dealloc} ret;', None),
        (True, f'{yes} label {{name:done}}; ret;', None),
        (True, f'{no} {arith} {arith} label {{name:done}}; ret;', None),
        (False, f'{unreachable} {alloc} label {{name:done}}; {dealloc} ret;', None),
        (False, f'{alloc} {save} {yes} {restore} label {{name:done}}; {dealloc} ret;', None),
        (False, f'{alloc} {yes} {dealloc} label {{name:done}}; ret;', None),
        (False, f'{yes} ret; label {{name:done}}; ret;', None),
        (True, f'{yes} {yes} label {{name:done}}; ret;', None),
        (False, f'{yes} label {{name:done}}; label {{name:done}}; ret;', None),
        (False, f'{yes} label {{name:else}}; ret;', None),
        (True, f'label {{name:done}}; {yes} ret;', None),
        (False, f'{reg_bad} label {{name:done}}; ret;', None),
        (False, f'{link_bad} label {{name:done}}; ret;', None),
        (False, f'{args_bad} label {{name:done}}; ret;', None),
        (False, f'{yes} {save} label {{name:done}}; ret;', None),
        # A real two-way diamond: the condition jumps across another branch.
        (True,
         (f'{yes} {arith} b {{target:final}}; label {{name:done}}; '
          f'eor {{dst:x0,lhs:x0,rhs:x1}}; label {{name:final}}; ret;')
         if arch == 'aarch64' else
         (f'{yes} {arith} jal {{dst:zero,target:final}}; label {{name:done}}; '
          f'xor {{dst:a0,lhs:a0,rhs:a1}}; label {{name:final}}; ret;'),
         ('cbz x0, else\nadd x0,x0,x1\nb final\nelse:\neor x0,x0,x1\nfinal:\nret'
          if arch == 'aarch64' else
          'beq a0,a1,12\nadd a0,a0,a1\njal zero,8\nxor a0,a0,a1\nret')),
        # Multiple conditionals can cross each other if all bypassed instructions are ABI-inert.
        (True,
         (f'{yes} {no.replace("done", "final")} {arith} label {{name:done}}; '
          f'{arith} label {{name:final}}; ret;'), None),
    ]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--compiler', required=True)
    p.add_argument('--target', choices=('aarch64', 'riscv64'), required=True)
    p.add_argument('--clang')
    args = p.parse_args()
    result = cases(args.target)
    arith = 'add {dst:x0,lhs:x0,rhs:x1};' if args.target == 'aarch64' else 'add {dst:a0,lhs:a0,rhs:a1};'
    chosen = result[0][1].split(';')[0] + ';'
    for n in (1, 2, 4, 15, 63, 127, 511, 1023):
        # RV conditional B-type has a maximum positive displacement of 4094.
        result.append((n <= 1022 if args.target == 'riscv64' else True,
                       f'{chosen} ' + ' '.join([arith] * n) + ' label {name:done}; ret;', None))
    for j, (accepted, body, asm) in enumerate(result):
        with tempfile.TemporaryDirectory(prefix='aster-cond-') as d:
            src, out = Path(d)/'main.aa', Path(d)/'out.bin'
            src.write_text('module test.cond;\nexport fn raw(a:u64,b:u64)->u64 @naked {\n'
                           f'unsafe exact {args.target} {{ {body} }}\n}}\n', encoding='utf8')
            run = subprocess.run([args.compiler, 'build', '--target', args.target, '--emit', 'bin',
                                  '-j', '1', '-o', str(out), str(src)],
                                 capture_output=True, text=True, timeout=35)
            if accepted:
                if run.returncode != 0 or not out.exists():
                    raise AssertionError(f'{args.target} case {j} rejected: {run.stdout} {run.stderr}')
                raw = out.read_bytes()
                if not raw or len(raw)%4:
                    raise AssertionError(f'{args.target} case {j} malformed code')
                if asm and args.clang:
                    ref = assemble(args.clang, args.target, asm, d)
                    if raw != ref:
                        raise AssertionError(f'{args.target} case {j}: {raw.hex()} != {ref.hex()}')
            elif run.returncode == 0 or out.exists():
                raise AssertionError(f'{args.target} case {j} invalid code accepted: {body[:180]}')
    print(f'{args.target}: {len(result)} conditional branch cases passed')

if __name__ == '__main__':
    main()
