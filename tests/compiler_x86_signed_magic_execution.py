#!/usr/bin/env python3
"""Slice22: i64 magic divide/rem native x86-64, implicit-RAX/RDX safety.

The machine code is executed on a *real* Linux x86-64 CPU.  The oracle uses
pure Python signed integer operations with truncation towards zero.  Both
Clang-assembled MUL bytes and the compiler's own strict dumper are checked.
"""
import argparse
import ctypes
import mmap
import platform
import random
import subprocess
import tempfile
from pathlib import Path

MIN = -(1 << 63)
MAX = (1 << 63) - 1
MASK = (1 << 64) - 1
DIVISORS = [-MAX, -((1 << 32) + 1), -65537, -257, -127, -63, -31,
            -19, -13, -11, -9, -7, -6, -5, -3, 3, 5, 6, 7, 9, 11, 13,
            19, 31, 63, 127, 257, 65537, (1 << 32) + 1, MAX]


def truncdiv(x, d):
    return (abs(x) // abs(d)) * (-1 if (x < 0) != (d < 0) else 1)


def native(code, narg):
    page = mmap.mmap(-1, max(4096, len(code)),
                     prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
    page.write(code)
    func_type = ctypes.CFUNCTYPE(ctypes.c_int64, *([ctypes.c_int64] * narg))
    fn = func_type(ctypes.addressof(ctypes.c_char.from_buffer(page)))
    return page, fn


def build(cc, source, binary):
    p = subprocess.run([cc, 'build', '--target', 'x86_64', '--emit', 'bin',
                        '-j', '1', '-o', str(binary), str(source)],
                       capture_output=True, text=True, timeout=45)
    if p.returncode or not binary.exists():
        raise AssertionError(f'failed to compile: {p.stdout} {p.stderr}')
    return binary.read_bytes()


def mul_ops(data):
    return [(i, data[i:i + 3]) for i in range(len(data) - 2)
            if 0x48 <= data[i] <= 0x4f and data[i + 1] == 0xf7 and
            (data[i + 2] & 0xf8) == 0xe0]


def verify_clang(clang, folder, op):
    rex = op[1][0]
    r = (op[1][2] & 7) + (8 if rex & 1 else 0)
    asm = folder / 'oracle.s'
    binary = folder / 'oracle.o'
    raw = folder / 'oracle.bin'
    asm.write_text(f'.text\nmulq %r{r}\n' if r >= 8 else
                   '.text\nmulq %' + ('rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi')[r] + '\n')
    result = subprocess.run([clang, '--target=x86_64-unknown-linux-gnu',
                             '-c', str(asm), '-o', str(binary)],
                            capture_output=True, text=True)
    if result.returncode: raise AssertionError(result.stderr)
    result = subprocess.run(['objcopy', '-O', 'binary', '-j', '.text',
                             str(binary), str(raw)], capture_output=True, text=True)
    if result.returncode: raise AssertionError(result.stderr)
    if raw.read_bytes() != op[1]: raise AssertionError('MUL bytes differ from Clang')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--dumper')
    ap.add_argument('--clang')
    ap.add_argument('--execute', action='store_true')
    args = ap.parse_args()
    if args.execute and (platform.system() != 'Linux' or
                         platform.machine().lower() not in ('x86_64', 'amd64')):
        raise SystemExit('Native execution requested on unsupported CPU/OS')
    rng = random.Random(0xA57E220)
    inputs = [MIN, MIN + 1, MIN + 2, -((1 << 32) + 1), -65537, -255, -127,
              -19, -7, -3, -2, -1, 0, 1, 2, 3, 7, 19, 127, 255,
              (1 << 32) - 1, MAX - 2, MAX - 1, MAX]
    inputs += [rng.randrange(MIN, MAX + 1) for _ in range(192)]
    cases = runs = asm_comparisons = 0
    with tempfile.TemporaryDirectory(prefix='aster-x86-i64-magic-') as temp:
        d = Path(temp)
        for divisor in DIVISORS:
            for symbol in ('/', '%'):
                source, output = d / 's.aa', d / 's.bin'
                source.write_text('module test.x86_signed_magic; '
                                  'export fn f(x:i64)->i64 @abi(c) '
                                  f'{{ return x {symbol} {divisor}; }}\n')
                data = build(args.compiler, source, output)
                ops = mul_ops(data)
                if len(ops) != 1:
                    raise AssertionError((divisor, symbol, 'expected one unsigned high MUL', data.hex()))
                cases += 1
                if args.clang:
                    verify_clang(args.clang, d, ops[0])
                    asm_comparisons += 1
                if args.dumper:
                    p = subprocess.run([args.dumper, '--target', 'x86_64', '--strict',
                                        str(output)], capture_output=True, text=True, timeout=30)
                    if p.returncode or '.byte' in p.stdout:
                        raise AssertionError((divisor, symbol, p.stderr, p.stdout))
                if args.execute:
                    page, fn = native(data, 1)
                    try:
                        for x in inputs:
                            q = truncdiv(x, divisor)
                            expected = q if symbol == '/' else x - q * divisor
                            got = fn(x)
                            if got != expected:
                                raise AssertionError((divisor, symbol, x, got, expected))
                            runs += 1
                    finally:
                        page.close()
        # Probes multiple overlapped fixed-register definitions, signedness,
        # live arguments across independent magic operations and signed REM.
        formulas = [
            ('x / -7 + y / 19', lambda x, y: truncdiv(x, -7) + truncdiv(y, 19)),
            ('x % -7 + y % 19', lambda x, y: (x - truncdiv(x, -7) * -7) +
                                                  (y - truncdiv(y, 19) * 19)),
            ('x / 7 - x / -19', lambda x, y: truncdiv(x, 7) - truncdiv(x, -19)),
            ('x % 7 ^ y % -19', lambda x, y: (x - truncdiv(x, 7) * 7) ^
                                                   (y - truncdiv(y, -19) * -19)),
            ('x / -127 + y', lambda x, y: truncdiv(x, -127) + y),
        ]
        def signed64(n):
            n &= MASK
            return n if n <= MAX else n - (1 << 64)
        for expression, oracle in formulas:
            source, output = d / 'compound.aa', d / 'compound.bin'
            source.write_text('module test.x86_signed_compound; '
                              'export fn f(x:i64,y:i64)->i64 @abi(c) '
                              f'{{ return {expression}; }}\n')
            data = build(args.compiler, source, output)
            if args.execute:
                page, fn = native(data, 2)
                try:
                    for _ in range(256):
                        x, y = rng.randrange(MIN, MAX + 1), rng.randrange(MIN, MAX + 1)
                        expected = signed64(oracle(x, y))
                        actual = fn(x, y)
                        if actual != expected:
                            raise AssertionError((expression, x, y, actual, expected))
                        runs += 1
                finally:
                    page.close()
        # Guard against mistakenly selecting the *64-bit* MUL reciprocal
        # for narrow operands; Slice23 intentionally added MUL r/m32. Special
        # i64 divisors must keep the prior hardware/pow2 target contract.
        for ty, divisor in [('i8', -7), ('i16', 7), ('i32', -19),
                            ('i64', -1), ('i64', 8), ('i64', MIN)]:
            source, output = d / 'old.aa', d / 'old.bin'
            source.write_text('module test.x86_old; '
                              f'export fn f(x:{ty})->{ty} @abi(c) '
                              f'{{ return x / {divisor}; }}\n')
            p = subprocess.run([args.compiler, 'build', '--target', 'x86_64',
                                '-j', '1', '-o', str(output), str(source)],
                               capture_output=True, text=True, timeout=30)
            if p.returncode:
                # This stage does not claim support for all literal/overflow
                # spellings; a semantic diagnostic remains fail-closed.
                if output.exists(): raise AssertionError('failed build emitted output')
                continue
            if mul_ops(output.read_bytes()):
                raise AssertionError(f'unsupported signed narrow/special magic selection: {ty} {divisor}')
    print(f'{cases} i64 signed source/ISA cases, {asm_comparisons} Clang comparisons, '
          f'{runs} real x86-64 CPU executions passed')


if __name__ == '__main__':
    main()
