#!/usr/bin/env python3
"""Slice 12: signed backedges and exact-state loop invariant proof, both ISAs.

No dependency on an external assembler for acceptance. When Clang/readelf is
available, compare complete native instruction bytes against that assembler.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
from native_manual_cross_isa import assemble


def generate(arch):
    a = arch == 'aarch64'
    cond = 'cbnz {reg:x0,target:start};' if a else 'bne {lhs:a0,rhs:a1,target:start};'
    cond_zero = 'cbz {reg:x0,target:start};' if a else 'beq {lhs:a0,rhs:a1,target:start};'
    jump = 'b {target:start};' if a else 'jal {dst:zero,target:start};'
    add = 'add {dst:x0,lhs:x0,rhs:x1};' if a else 'add {dst:a0,lhs:a0,rhs:a1};'
    alloc = 'sub {dst:sp,lhs:sp,rhs:16};' if a else 'addi {dst:sp,lhs:sp,rhs:-16};'
    release = 'add {dst:sp,lhs:sp,rhs:16};' if a else 'addi {dst:sp,lhs:sp,rhs:16};'
    store = 'str {src:x19,offset:0};' if a else 'sd {src:s1,offset:0};'
    mutate = 'eor {dst:x19,lhs:x0,rhs:x1};' if a else 'xor {dst:s1,lhs:a0,rhs:a1};'
    restore = 'ldr {dst:x19,offset:0};' if a else 'ld {dst:s1,offset:0};'
    # Numeric PC-relative offsets are used in the oracle so that RISC-V
    # assembler relocation relaxation cannot change an unresolved label.
    simple_asm = ('add x0,x0,x1\ncbnz x0,-4\nret' if a else
                  'add a0,a0,a1\nbne a0,a1,-4\nret')
    zero_asm = 'cbz x0,0\nret' if a else 'beq a0,a1,0\nret'
    cases = [
        (True, f'label {{name:start}}; {add} {cond} ret;', simple_asm),
        (True, f'label {{name:start}}; {cond_zero} ret;', zero_asm),
        (True, f'label {{name:start}}; {alloc} {release} {cond} ret;', None),
        (True, f'label {{name:start}}; {alloc} {store} {mutate} {restore} {release} {cond} ret;', None),
        (True, f'{alloc} {store} label {{name:start}}; {mutate} {cond} {restore} {release} ret;', None),
        (True, f'label {{name:start}}; label {{name:alias}}; {add} {cond_zero} ret;', None),
        (True, f'label {{name:start}}; {add} {cond} {add} ret;', None),
        (True, f'label {{name:start}}; {cond_zero} label {{name:other}}; {add} ret;', None),
        # State changes at a backedge are forbidden (including different
        # slot provenance and saved-register restoration status).
        (False, f'label {{name:start}}; {alloc} {cond} {release} ret;', None),
        (False, f'{alloc} label {{name:start}}; {release} {cond} ret;', None),
        (False, f'{alloc} label {{name:start}}; {store} {mutate} {cond} {restore} {release} ret;', None),
        (False, f'{alloc} label {{name:start}}; {store} {cond} {restore} {release} ret;', None),
        (False, f'label {{name:start}}; {jump} ret;', None),  # no reachable RET
        (False, f'label {{name:start}}; {cond} {alloc} ret;', None),
        (False, f'label {{name:start}}; {jump} {alloc} ret;', None),
        (False, f'label {{name:start}}; {cond} {release} ret;', None),
        (False, f'label {{name:start}}; {cond} {store} ret;', None),
        (False, f'{cond} ret;', None),  # unknown label
        (False, f'label {{name:start}}; label {{name:start}}; {cond} ret;', None),
    ]
    if not a:
        # B-type imm13: signed -4096..+4094 bytes. Each AsterASM native
        # instruction is exactly 4 bytes and labels have zero byte length.
        for n in (1023, 1024, 1025):
            body = 'label {name:start}; ' + (' '.join([add] * n)) + f' {cond} ret;'
            cases.append((n <= 1024, body, None))
        # J-type signed backedge range is wider; this is a modest oracle case.
        cases.append((True, f'label {{name:start}}; {add} {cond_zero} ret;', None))
    else:
        for n in (1, 31, 511, 2048):
            cases.append((True, 'label {name:start}; ' + (' '.join([add] * n)) + f' {cond} ret;', None))
    return cases


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--target', choices=('aarch64', 'riscv64'), required=True)
    parser.add_argument('--clang')
    args = parser.parse_args()
    cases = generate(args.target)
    for idx, (accepted, body, oracle) in enumerate(cases):
        with tempfile.TemporaryDirectory(prefix='aster-native-loop-') as tmp:
            src = Path(tmp)/'test.aa'
            dst = Path(tmp)/'test.bin'
            src.write_text('module test.cycles;\nexport fn raw(a:u64,b:u64)->u64 @naked {\n'
                           f'unsafe exact {args.target} {{ {body} }}\n}}\n', encoding='utf8')
            process = subprocess.run([args.compiler, 'build', '--target', args.target,
                                      '--emit', 'bin', '-j', '1', '-o', str(dst), str(src)],
                                     capture_output=True, text=True, timeout=60)
            if accepted:
                if process.returncode or not dst.is_file():
                    raise AssertionError(f'{args.target} case {idx} rejected: {process.stderr}')
                binary = dst.read_bytes()
                if not binary or len(binary) % 4:
                    raise AssertionError(f'{args.target} case {idx} missing or invalid code')
                if oracle and args.clang:
                    expected = assemble(args.clang, args.target, oracle, tmp)
                    if binary != expected:
                        raise AssertionError(f'{args.target} case {idx}: '
                                             f'{binary.hex()} != {expected.hex()}')
            elif process.returncode == 0 or dst.exists():
                raise AssertionError(f'{args.target} case {idx}: unsound accepted CFG')
    print(f'{args.target}: {len(cases)} signed-backedge/loop cases passed')


if __name__ == '__main__':
    main()
