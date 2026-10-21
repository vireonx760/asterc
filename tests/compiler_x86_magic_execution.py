#!/usr/bin/env python3
"""Slice 21: actual SysV x86-64 execution of magic u64 divide/remainder.

An independent Clang assembler validates the implicit RDX:RAX unsigned MUL
encoding. mmap/ctypes executes each full AsterASM BIN on an x86-64 Linux
host, so this is stronger than a decoder simulation. Never run cross-target.
"""
import argparse
import ctypes
import mmap
import platform
from pathlib import Path
import random
import subprocess
import tempfile

MASK = (1 << 64) - 1
DIVISORS = [3, 5, 6, 7, 9, 10, 11, 13, 17, 19, 31, 63, 127, 255,
            257, 65535, 65537, (1 << 32) - 1, (1 << 32) + 1,
            (1 << 63) - 1, (1 << 63) + 1, MASK - 2, MASK]


def compiler(asterc, source, output):
    proc = subprocess.run([asterc, 'build', '--target', 'x86_64', '-j', '1',
                           '-o', str(output), str(source)], capture_output=True,
                          text=True, timeout=45)
    if proc.returncode or not output.is_file():
        raise AssertionError('x86-64 source compilation: ' + proc.stdout + proc.stderr)
    return output.read_bytes()


def reference_bytes(clang, register, directory):
    # The implicit high half is RDX; assembler output must be F7 /4 and not
    # two-operand IMUL (0F AF). Reference checked against the complete bytes.
    asm = directory / 'mul.s'
    obj = directory / 'mul.o'
    asm.write_text(f'.text\nmulq %{register}\n', encoding='ascii')
    run = subprocess.run([clang, '-target', 'x86_64-unknown-linux-gnu', '-c',
                          str(asm), '-o', str(obj)], capture_output=True, text=True)
    if run.returncode:
        raise AssertionError('Clang MUL assembler: ' + run.stderr)
    out = directory / 'mul.bin'
    run = subprocess.run(['objcopy', '-O', 'binary', '-j', '.text', str(obj), str(out)],
                         capture_output=True, text=True)
    if run.returncode:
        raise AssertionError('objcopy: ' + run.stderr)
    return out.read_bytes()


def find_mul(code):
    matches = []
    for at in range(len(code) - 2):
        # REX.W + unsigned group-three F7 /4, ModRM register addressing.
        rex, opcode, modrm = code[at:at + 3]
        if 0x48 <= rex <= 0x4f and opcode == 0xf7 and modrm & 0xf8 == 0xe0:
            register = (modrm & 7) + (8 if rex & 1 else 0)
            matches.append((code[at:at + 3], register))
    return matches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--clang')
    ap.add_argument('--dumper')
    ap.add_argument('--execute', action='store_true')
    args = ap.parse_args()
    if args.execute and (platform.system() != 'Linux' or platform.machine().lower() not in
                         ('x86_64', 'amd64')):
        raise SystemExit('Native BIN execution was requested on a non-x86-64 Linux host')
    random_values = random.Random(0x51721)
    cases, executions, assembler_comparisons = 0, 0, 0
    with tempfile.TemporaryDirectory(prefix='aster-x86-magic-') as temp:
        folder = Path(temp)
        for divisor in DIVISORS:
            for op in ('/', '%'):
                source = folder / 'test.aa'
                binary = folder / 'test.bin'
                source.write_text('module test.x86_magic; export fn f(x:u64)->u64 @abi(c) '
                                  f'{{ return x {op} {divisor}; }}\n', encoding='utf8')
                code = compiler(args.compiler, source, binary)
                if args.dumper:
                    decoded = subprocess.run([args.dumper, '--target', 'x86_64',
                                              '--strict', str(binary)],
                                             capture_output=True, text=True, timeout=20)
                    if decoded.returncode or '.byte' in decoded.stdout:
                        raise AssertionError('strict x86 dumper failed: ' +
                                             decoded.stdout + decoded.stderr)
                muls = find_mul(code)
                if len(muls) != 1 or b'\x0f\xaf' not in code and op == '%':
                    raise AssertionError(f'x86-64 {op}{divisor}: missing expected MUL/IMUL')
                if args.clang:
                    raw, register = muls[0]
                    oracle = reference_bytes(args.clang, ('r' + str(register) if register >= 8 else
                        {0:'rax',1:'rcx',2:'rdx',3:'rbx',4:'rsp',5:'rbp',6:'rsi',7:'rdi'}[register]), folder)
                    if oracle != raw:
                        raise AssertionError(f'x86 MUL differs from assembler: {raw.hex()} vs {oracle.hex()}')
                    assembler_comparisons += 1
                cases += 1
                if not args.execute:
                    continue
                page = mmap.mmap(-1, max(4096, len(code)),
                                 prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
                try:
                    page.write(code)
                    fn = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_uint64)(
                        ctypes.addressof(ctypes.c_char.from_buffer(page)))
                    values = [0, 1, 2, 3, 7, 10, 31, 63, divisor - 1, divisor,
                              (divisor + 1) & MASK, MASK, MASK - 1, 1 << 32, 1 << 63,
                              (1 << 63) - 1] + [random_values.getrandbits(64) for _ in range(192)]
                    for value in values:
                        actual = fn(value)
                        expected = value // divisor if op == '/' else value % divisor
                        if actual != expected:
                            raise AssertionError((divisor, op, value, actual, expected))
                        executions += 1
                finally:
                    page.close()
        # Multiple MULs and live arguments stress the implicit RAX/RDX
        # constraints when register allocation assigns overlapping lifetimes.
        if args.execute:
            formulas = [
                ('x / 7 + y', lambda x, y: (x // 7 + y) & MASK),
                ('x / 7 + y / 19', lambda x, y: (x // 7 + y // 19) & MASK),
                ('x % 7 ^ y / 19', lambda x, y: (x % 7) ^ (y // 19)),
                ('x / 7 + x % 19', lambda x, y: (x // 7 + x % 19) & MASK),
                ('x % 7 + y % 19', lambda x, y: (x % 7 + y % 19) & MASK),
            ]
            for expr, oracle in formulas:
                source = folder / 'liveness.aa'
                binary = folder / 'liveness.bin'
                source.write_text('module test.x86_liveness; export fn f(x:u64,y:u64)->u64 @abi(c) '
                                  f'{{ return {expr}; }}\n', encoding='utf8')
                code = compiler(args.compiler, source, binary)
                page = mmap.mmap(-1, max(4096, len(code)),
                                 prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
                try:
                    page.write(code)
                    fn = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_uint64,
                                          ctypes.c_uint64)(ctypes.addressof(ctypes.c_char.from_buffer(page)))
                    for _ in range(256):
                        x, y = random_values.getrandbits(64), random_values.getrandbits(64)
                        actual, expected = fn(x, y), oracle(x, y)
                        if actual != expected:
                            raise AssertionError((expr, x, y, actual, expected))
                        executions += 1
                finally:
                    page.close()
        if args.dumper:
            for i, invalid in enumerate((b'\x48\xf7', b'\x48\xc1',
                                         b'\x48\xf7\xc0',  # unsupported TEST extension
                                         b'\x48\xc1\xf6\x02',  # reserved shift /6
                                         b'\x48\xc1\xea')):
                invalid_path = folder / f'invalid{i}.bin'
                invalid_path.write_bytes(invalid)
                proc = subprocess.run([args.dumper, '--target', 'x86_64',
                                       '--strict', str(invalid_path)],
                                      capture_output=True, text=True, timeout=20)
                if proc.returncode == 0:
                    raise AssertionError('strict x86 accepted malformed/reserved encoding')
        # This rewrite is **never** appropriate for signed operations or
        # 32-bit unsigned inputs. Preserve their preexisting paths.
        for ty, divisor in [('u32', 7), ('i32', -7), ('u64', 8), ('u64', 1)]:
            source = folder / 'fallback.aa'
            binary = folder / 'fallback.bin'
            source.write_text('module test.x86_fallback; export fn f(x:' + ty + ')->' + ty +
                              ' @abi(c) { return x / ' + str(divisor) + '; }\n')
            code = compiler(args.compiler, source, binary)
            if find_mul(code):
                raise AssertionError('unsound selection of wide unsigned MUL for ' + ty)
    print(f'x86-64 {cases} source/encoder cases, {assembler_comparisons} independent '
          f'assembler comparisons, {executions} native executions passed')


if __name__ == '__main__':
    main()
