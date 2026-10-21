#!/usr/bin/env python3
"""G1 forward-only exact native branches: ISA byte oracle and fail-closed ABI checks.

An independent Clang assembler is optional. The compiler never invokes it.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

from native_manual_cross_isa import assemble


def cases(arch):
    if arch == 'aarch64':
        b = 'b {target:done};'
        instr = 'add {dst:x0,lhs:x0,rhs:x1};'
        safe2 = 'eor {dst:x2,lhs:x2,rhs:x1};'
        bad_saved = 'add {dst:x19,lhs:x0,rhs:x1};'
        allocate = 'sub {dst:sp,lhs:sp,rhs:16};'
        deallocate = 'add {dst:sp,lhs:sp,rhs:16};'
        save = 'str {src:x30,offset:0};'
        restore = 'ldr {dst:x30,offset:0};'
        asm = 'b done\nadd x0, x0, x1\ndone:\nret'
        asm2 = 'sub sp, sp, #16\nb done\nadd x0, x0, x1\ndone:\nadd sp, sp, #16\nret'
        other = 'b {target:missing};'
        backward = 'label {name:done}; b {target:done};'
        bad_rd = 'b {dst:x0,target:done};'
        saved_pre = 'str {src:x19,offset:0};'
    else:
        b = 'jal {dst:zero,target:done};'
        instr = 'add {dst:a0,lhs:a0,rhs:a1};'
        safe2 = 'xor {dst:t0,lhs:t0,rhs:a1};'
        bad_saved = 'add {dst:s0,lhs:a0,rhs:a1};'
        allocate = 'addi {dst:sp,lhs:sp,rhs:-16};'
        deallocate = 'addi {dst:sp,lhs:sp,rhs:16};'
        save = 'sd {src:ra,offset:0};'
        restore = 'ld {dst:ra,offset:0};'
        asm = 'jal zero, 8\nadd a0, a0, a1\nret'
        asm2 = 'addi sp, sp, -16\njal zero, 8\nadd a0, a0, a1\naddi sp, sp, 16\nret'
        other = 'jal {dst:x0,target:missing};'
        backward = 'label {name:done}; jal {dst:zero,target:done};'
        bad_rd = 'jal {dst:ra,target:done};'
        saved_pre = 'sd {src:s0,offset:0};'
    # tuple: success, body, independent reference assembly
    result = [
        (True, f'{b} {instr} label {{name:done}}; ret;', asm),
        (True, f'{allocate} {b} {instr} label {{name:done}}; {deallocate} ret;', asm2),
        (True, 'label {name:start}; ' + instr + ' ret;', None),
        (True, f'{b} label {{name:done}}; ret;', 'b done\ndone:\nret' if arch=='aarch64' else 'jal zero, 4\nret'),
        (True, f'{b} {instr} {safe2} label {{name:done}}; ret;', None),
        (False, other + ' label {name:done}; ret;', None),
        (False, backward + ' ret;', None),
        (False, f'{b} label {{name:done}}; label {{name:done}}; ret;', None),
        (False, f'{b} {allocate} label {{name:done}}; {deallocate} ret;', None),
        (False, f'{allocate} {save} {b} {restore} label {{name:done}}; {deallocate} ret;', None),
        (False, f'{allocate} {b} {save} label {{name:done}}; {restore} {deallocate} ret;', None),
        (False, f'{allocate} {b} {deallocate} label {{name:done}}; ret;', None),
        (False, f'{b} ret; label {{name:done}}; ret;', None),
        (False, f'{b} {bad_saved} label {{name:done}}; ret;', None),
        (False, f'{bad_rd} label {{name:done}}; ret;', None),
        (False, f'{b} {instr} label {{name:other}}; ret;', None),
        (False, 'label {name:done}; label {name:done}; ret;', None),
        (False, 'label {name:done};', None),
        (False, 'label {name:done}; ret; label {name:again};', None),
        (False, f'{b} {saved_pre} label {{name:done}}; ret;', None),
    ]
    # Deterministic offset and multiple-target stresses; no hidden instructions.
    for n in (1, 2, 3, 8, 31, 127, 511):
        start = f'{b} ' + ' '.join([instr] * n) + ' label {name:done}; ret;'
        if arch == 'aarch64':
            oracle = f'b {4 * (n + 1)}\n' + ('add x0, x0, x1\n' * n) + 'ret'
        else:
            oracle = f'jal zero, {4 * (n + 1)}\n' + ('add a0, a0, a1\n' * n) + 'ret'
        result.append((True, start, oracle))
    # Zero-size consecutive labels and two independent forward edges.
    jump_second = 'b {target:final};' if arch == 'aarch64' else 'jal {dst:x0,target:final};'
    result.append((True, f'{b} {instr} label {{name:done}}; {jump_second} {safe2} '
                         'label {name:almost}; label {name:final}; ret;', None))
    result.append((True, f'{b} {jump_second} label {{name:done}}; label {{name:final}}; ret;', None))
    if arch == 'riscv64':
        result.append((True, 'jal {dst:x0,target:done}; label {name:done}; ret;', 'jal x0, 4\nret'))
        result.append((False, 'jal {dst:a0,target:done}; label {name:done}; ret;', None))
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--target', choices=['aarch64', 'riscv64'], required=True)
    ap.add_argument('--clang')
    args = ap.parse_args()
    for j, (accept, native, asm) in enumerate(cases(args.target)):
        with tempfile.TemporaryDirectory(prefix='aster-native-branch-') as d:
            src, out = Path(d) / 'main.aa', Path(d) / 'main.bin'
            src.write_text('module test.manual_branch;\nexport fn raw(a:u64,b:u64)->u64 @naked {\n'
                           f'unsafe exact {args.target} {{ {native} }}\n}}\n', encoding='utf-8')
            run = subprocess.run([args.compiler, 'build', '--target', args.target, '--emit', 'bin',
                                  '-j', '1', '-o', str(out), str(src)], text=True,
                                 capture_output=True, timeout=35)
            if accept:
                if run.returncode != 0 or not out.is_file():
                    raise AssertionError(f'{args.target}[{j}] unexpectedly rejected: {run.stdout}{run.stderr}')
                code = out.read_bytes()
                if not code or len(code) % 4:
                    raise AssertionError(f'{args.target}[{j}] malformed byte count')
                if asm and args.clang:
                    reference = assemble(args.clang, args.target, asm, d)
                    if code != reference:
                        raise AssertionError(f'{args.target}[{j}] {code.hex()} != {reference.hex()}')
            elif run.returncode == 0 or out.exists():
                raise AssertionError(f'{args.target}[{j}] invalid branch accepted: {native}')
    print(f'{args.target}: {len(cases(args.target))} branch/label cases passed')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, subprocess.TimeoutExpired) as e:
        print(e, file=sys.stderr)
        sys.exit(1)
