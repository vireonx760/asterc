#!/usr/bin/env python3
"""Slice 13: bounded intra-region leaf calls; independent assembler oracle.

These are exact local calls, not external object relocations.  All accepted
samples must encode as fixed-width instructions; failure cases must fail shut.
"""
import argparse
import pathlib
import subprocess
import tempfile
from native_manual_cross_isa import assemble


def suite(arch):
    a = arch == 'aarch64'
    frame = 'sub {dst:sp,lhs:sp,rhs:16};' if a else 'addi {dst:sp,lhs:sp,rhs:-16};'
    unframe = 'add {dst:sp,lhs:sp,rhs:16};' if a else 'addi {dst:sp,lhs:sp,rhs:16};'
    store = 'str {src:x30,offset:0};' if a else 'sd {src:ra,offset:0};'
    load = 'ldr {dst:x30,offset:0};' if a else 'ld {dst:ra,offset:0};'
    call = 'bl {target:leaf};' if a else 'jal {dst:ra,target:leaf};'
    add = 'add {dst:x0,lhs:x0,rhs:x1};' if a else 'add {dst:a0,lhs:a0,rhs:a1};'
    modify = 'eor {dst:x19,lhs:x0,rhs:x1};' if a else 'xor {dst:s1,lhs:a0,rhs:a1};'
    leaf_save = 'str {src:x19,offset:8};' if a else 'sd {src:s1,offset:8};'
    sp_adjust = frame
    # ARM/RV assembler needs a local named leaf; the PC-relative encoding is
    # checked byte-for-byte to catch offset and destination register bugs.
    asm = ('sub sp,sp,#16\nstr x30,[sp]\nbl leaf\nldr x30,[sp]\nadd sp,sp,#16\nret\nleaf:\nadd x0,x0,x1\nret' if a else
           'addi sp,sp,-16\nsd ra,0(sp)\njal ra,16\nld ra,0(sp)\naddi sp,sp,16\nret\nadd a0,a0,a1\nret')
    prefix = f'{frame} {store} '
    postfix = f'{load} {unframe} ret;'
    base = f'{prefix} {call} {postfix} label {{name:leaf}}; {add} ret;'
    return [
        (True, base, asm),
        (True, f'{prefix}{call}{postfix} label {{name:leaf}}; ret;', None),
        (False, f'{prefix}{call}{add}{postfix} label {{name:leaf}}; {add} ret;', None),
        (True, f'{prefix}{add}{call}{postfix} label {{name:leaf}}; {add} ret;', None),
        (False, f'{call} ret; label {{name:leaf}}; ret;', None),
        (False, f'{frame} {call} {unframe} ret; label {{name:leaf}}; ret;', None),
        (False, f'{prefix}{load} {call} {unframe} ret; label {{name:leaf}}; ret;', None),
        (False, f'{prefix}{call} {unframe} ret; label {{name:leaf}}; ret;', None),
        (False, f'{prefix}{call}{postfix} label {{name:leaf}}; {modify} ret;', None),
        (False, f'{prefix}{call}{postfix} label {{name:leaf}}; {sp_adjust} ret;', None),
        (False, f'{prefix}{call}{postfix} label {{name:leaf}}; {leaf_save} ret;', None),
        (False, f'{prefix}{call}{postfix} label {{name:leaf}}; {call} ret;', None),
        (False, f'{prefix}{call}{postfix} label {{name:leaf}}; {call} {add} ret;', None),
        (True, f'{prefix}{call}{postfix} label {{name:leaf}}; {frame} {unframe} ret;', None),
        (False, f'{prefix}{call}{postfix} label {{name:leaf}}; {load} ret;', None),
        (False, f'{prefix}{call}{postfix} label {{name:leaf}}; {add}', None),
        (False, f'{prefix}{call}{postfix} label {{name:leaf}}; {add} {call} ret;', None),
        # A leaf must return through LR/RA; it may not jump into the caller's
        # epilogue and bypass the abstract call continuation.
        (False, f'{prefix}{call} label {{name:after}}; {postfix} '
                f'label {{name:leaf}}; '
                f'{"b {target:after};" if a else "jal {dst:zero,target:after};"} ret;', None),
        # ABI correctness: a1/X1 is caller-clobbered, so reading it after
        # either call without reloading a saved copy must be rejected.
        (False, f'{prefix}{call}{call}{postfix} label {{name:leaf}}; {add} ret;', None),
    ]


def run():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--target', choices=('aarch64', 'riscv64'), required=True)
    ap.add_argument('--clang')
    args = ap.parse_args()
    cases = suite(args.target)
    for i, (should_accept, body, oracle) in enumerate(cases):
        with tempfile.TemporaryDirectory(prefix='aster-local-call-') as temp:
            root = pathlib.Path(temp)
            source = root / 'sample.aa'
            output = root / 'sample.bin'
            source.write_text('module test.calls;\nexport fn f(a:u64,b:u64)->u64 @naked {\n'
                              f'unsafe exact {args.target} {{ {body} }}\n}}\n', encoding='utf-8')
            process = subprocess.run([args.compiler, 'build', '--target', args.target,
                                      '--emit', 'bin', '-j', '1', '-o', str(output), str(source)],
                                     capture_output=True, text=True, timeout=60)
            if should_accept:
                assert process.returncode == 0 and output.is_file(), (i, body, process.stderr)
                binary = output.read_bytes()
                assert binary and len(binary) % 4 == 0, (i, binary)
                if oracle and args.clang:
                    expected = assemble(args.clang, args.target, oracle, temp)
                    assert binary == expected, (i, binary.hex(), expected.hex())
            else:
                assert process.returncode != 0 and not output.is_file(), (i, body, process.stderr)
    print(f'{args.target}: {len(cases)} local-call scenarios passed')


if __name__ == '__main__':
    run()
