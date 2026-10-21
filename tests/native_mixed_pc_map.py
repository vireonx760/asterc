#!/usr/bin/env python3
"""Slice16: RV64 mixed 4/8-byte PC maps, exact branches, symbols and ELF64.

The independent assembler oracle is only used in tests; the compiler and its
ELF object writer remain free of LLVM runtime dependencies.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from native_manual_cross_isa import assemble


def signed(v, bits):
    return (v ^ (1 << (bits - 1))) - (1 << (bits - 1))


def rv_b_disp(word):
    if word & 0x7f != 0x63 or ((word >> 12) & 7) not in (0, 1):
        raise ValueError(f'not RV B-type BEQ/BNE: {word:08x}')
    immediate = ((word >> 31) & 1) << 12
    immediate |= ((word >> 7) & 1) << 11
    immediate |= ((word >> 25) & 63) << 5
    immediate |= ((word >> 8) & 15) << 1
    return signed(immediate, 13)


def rv_j_disp(word):
    if word & 0x7f != 0x6f or (word >> 7) & 31 != 0:
        raise ValueError(f'not RV JAL x0: {word:08x}')
    immediate = ((word >> 31) & 1) << 20
    immediate |= ((word >> 21) & 1023) << 1
    immediate |= ((word >> 20) & 1) << 11
    immediate |= ((word >> 12) & 255) << 12
    return signed(immediate, 21)


def generate(variant):
    setup = 'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0};'
    call = 'jal {dst:ra,symbol:callee};'
    load = 'ld {dst:ra,offset:0};'
    close = 'addi {dst:sp,lhs:sp,rhs:16}; ret;'
    add = 'add {dst:a0,lhs:a0,rhs:a1};'
    if variant == 'forward':
        return (f'{setup} beq {{lhs:a0,rhs:a1,target:skip}}; {add} '
                f'label {{name:skip}}; {call} '
                f'beq {{lhs:a0,rhs:a0,target:done}}; '
                f'add {{dst:a0,lhs:a0,rhs:a0}}; '
                f'label {{name:done}}; {load} {close}')
    if variant == 'backedge':
        # Exactly one word and one two-word pair between label and BEQ:
        # sd (4) + call (8) + ld (4) = 16 bytes to jump backward.
        return (f'{setup} {call} {load} '
                f'label {{name:again}}; sd {{src:ra,offset:0}}; {call} {load} '
                f'beq {{lhs:a0,rhs:a0,target:again}}; {close}')
    if variant == 'double':
        return (f'{setup} {call} beq {{lhs:a0,rhs:a0,target:skip}}; '
                f'add {{dst:a0,lhs:a0,rhs:a0}}; label {{name:skip}}; '
                f'{call} {load} {close}')
    if variant == 'diamond':
        # Both arms perform the symbol call and restore/re-pin RA before
        # the shared exit. The zero-link JAL skips an eight-byte call pair.
        return (f'{setup} beq {{lhs:a0,rhs:a1,target:elsepart}}; '
                f'{call} {load} sd {{src:ra,offset:0}}; '
                f'jal {{dst:zero,target:joined}}; '
                f'label {{name:elsepart}}; {call} {load} '
                f'sd {{src:ra,offset:0}}; label {{name:joined}}; {load} {close}')
    if variant == 'missing_label':
        return f'{setup} {call} beq {{lhs:a0,rhs:a0,target:no_label}}; {load} {close}'
    if variant == 'volatile_read':
        return f'{setup} {call} beq {{lhs:a0,rhs:a1,target:done}}; label {{name:done}}; {load} {close}'
    if variant == 'unsaved_link':
        return f'beq {{lhs:a0,rhs:a1,target:go}}; label {{name:go}}; {call} ret;'
    if variant == 'unbalanced':
        return (f'{setup} beq {{lhs:a0,rhs:a1,target:done}}; '
                f'addi {{dst:sp,lhs:sp,rhs:-16}}; {call} '
                f'label {{name:done}}; {load} {close}')
    if variant == 'skip_symbol':
        # The call would be bypassed on a path; cannot merge incompatible
        # link/volatile provenance at the label without an explicit proof.
        return (f'{setup} beq {{lhs:a0,rhs:a1,target:after}}; '
                f'{call} label {{name:after}}; {load} {close}')
    if variant == 'bad_dest':
        return f'{setup} jal {{dst:a0,symbol:callee}}; {load} {close}'
    raise AssertionError(variant)


def run(cmd, good=True):
    p = subprocess.run(cmd, text=True, capture_output=True, timeout=45)
    if (p.returncode == 0) != good:
        raise AssertionError(f'{cmd}: rc={p.returncode}\n{p.stdout}\n{p.stderr}')
    return p


def main():
    a = argparse.ArgumentParser()
    a.add_argument('--compiler', required=True)
    a.add_argument('--clang')
    a.add_argument('--lld')
    a.add_argument('--readelf')
    a.add_argument('--dumper')
    opts = a.parse_args()
    positive = {'forward': [8, 8], 'backedge': [-16], 'double': [8], 'diamond': [24]}
    negative = ('missing_label', 'volatile_read', 'unsaved_link',
                'unbalanced', 'skip_symbol', 'bad_dest')
    with tempfile.TemporaryDirectory(prefix='aster-mixed-pc-') as directory:
        root = Path(directory)
        for variant in (*positive, *negative):
            source = root / (variant + '.aa')
            output = root / (variant + '.bin')
            source.write_text('module test.mixed_pc;\n'
                              'export fn callee(a:u64,b:u64)->u64 { return a+b; }\n'
                              'export fn wrapper(a:u64,b:u64)->u64 @naked {\n'
                              'unsafe exact riscv64 { ' + generate(variant) + ' }\n}\n',
                              encoding='utf-8')
            accepted = variant in positive
            run([opts.compiler, 'build', '--target', 'riscv64', '--emit', 'bin',
                 '-j', '1', '-o', str(output), str(source)], good=accepted)
            if not accepted:
                if output.exists():
                    raise AssertionError(f'{variant}: invalid BIN output exists')
                continue
            raw = output.read_bytes()
            if not raw or len(raw) % 4:
                raise AssertionError('malformed output binary')
            words = struct.unpack('<' + 'I' * (len(raw) // 4), raw)
            bwords = [w for w in words if w & 0x7f == 0x63 and ((w >> 12) & 7) in (0, 1)]
            offsets = [rv_b_disp(word) for word in bwords]
            if offsets != positive[variant]:
                raise AssertionError(f'{variant}: observed branch offsets {offsets}, expected {positive[variant]}')
            pairs = sum((w & 0x7f == 0x17 and (w >> 7) & 31 == 1)
                        for w in words)
            if pairs != (2 if variant in ('double', 'backedge', 'diamond') else 1):
                raise AssertionError(f'{variant}: incorrect AUIPC/JALR pair count: {pairs}')
            jwords = [w for w in words if w & 0x7f == 0x6f and (w >> 7) & 31 == 0]
            if [rv_j_disp(w) for w in jwords] != ([20] if variant == 'diamond' else []):
                raise AssertionError(f'{variant}: local JAL offset ignores AUIPC/JALR byte widths')
            if opts.clang:
                for word in jwords:
                    offset = rv_j_disp(word)
                    with tempfile.TemporaryDirectory(prefix='rv-jal-oracle-') as oracle_dir:
                        ref = assemble(opts.clang, 'riscv64', f'jal zero, {offset}', oracle_dir)
                        if ref != struct.pack('<I', word):
                            raise AssertionError(f'Clang ISA oracle mismatch for JAL x0 {offset}')
                for word in bwords:
                    offset = rv_b_disp(word)
                    asm = ('bne' if word & (1 << 12) else 'beq') + f' a0, a0, {offset}'
                    # The first forward BEQ compares a0/a1, not a0/a0.
                    if word >> 20 & 31 == 11:
                        asm = f'beq a0, a1, {offset}'
                    with tempfile.TemporaryDirectory(prefix='rv-oracle-') as oracle_dir:
                        ref = assemble(opts.clang, 'riscv64', asm, oracle_dir)
                        if ref != struct.pack('<I', word):
                            raise AssertionError(f'Clang ISA oracle mismatch for {asm}')
            if opts.dumper:
                # asterdump currently displays the RV64 call pair and B-type
                # without depending on LLVM. Noninstruction padding is OK.
                dump = run([opts.dumper, '--target', 'riscv64', '--format', 'text',
                            str(output)]).stdout
                if 'beq' not in dump or 'auipc' not in dump or 'jalr' not in dump:
                    raise AssertionError('asterdump did not decode mixed-width code')

        if opts.clang and opts.lld and opts.readelf:
            # Real external relocations, one- and two-call mixed regions.
            asm = root / 'callee.s'
            asm.write_text('.text\n.global callee\n.type callee,@function\ncallee:\nret\n', encoding='ascii')
            provider = root / 'callee.o'
            run([opts.clang, '-target', 'riscv64-none-elf', '-march=rv64i',
                 '-mabi=lp64', '-c', str(asm), '-o', str(provider)])
            for variant, expected in (('forward', 1), ('diamond', 2)):
                source = root / (variant + '_extern.aa')
                source.write_text('module test.ext_mix;\n'
                                  'extern fn callee(a:u64,b:u64)->u64;\n'
                                  'export fn wrapper(a:u64,b:u64)->u64 @naked {\n'
                                  'unsafe exact riscv64 { ' + generate(variant) + ' }\n}\n',
                                  encoding='utf-8')
                obj = root / (variant + '_client.o')
                run([opts.compiler, 'build', '--target', 'riscv64', '--emit', 'obj',
                     '--format', 'elf', '-j', '1', '-o', str(obj), str(source)])
                read = run([opts.readelf, '-r', '-sW', str(obj)]).stdout
                if read.count('R_RISCV_CALL_PLT') != expected or 'UND callee' not in read:
                    raise AssertionError(f'{variant}: bad relocation or symbol: {read}')
                linked = root / (variant + '_linked.elf')
                run([opts.lld, '-m', 'elf64lriscv', '-e', 'wrapper',
                     str(obj), str(provider), '-o', str(linked)])
                if 'R_RISCV_CALL_PLT' in run([opts.readelf, '-r', str(linked)]).stdout:
                    raise AssertionError('unresolved call relocation after link')
                run([opts.lld, '-m', 'elf64lriscv', '-e', 'wrapper', str(obj),
                     '-o', str(root / 'missing.elf')], good=False)
                print(f'RV64 mixed-width {variant}: {expected} ELF64 symbol relocation(s) linked')
    print('RV64: 4 accepted mixed PC patterns, 6 rejected patterns, and independent branch byte oracles PASS')


if __name__ == '__main__':
    main()
