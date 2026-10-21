#!/usr/bin/env python3
"""Signed i64 literal division: explicit feature gating, fixed bytes, and
negative/wide-divisor fallback in final machine code.

The portable semantics oracle is test_signed_div_magic.c; this tests the
real code-generation pipeline. Clang assembler is an optional byte oracle.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
from native_manual_cross_isa import assemble

DIVISORS = [3, -3, 5, -5, 6, -6, 7, -7, 10, -10, 11, -11,
            13, -13, 19, -19, 31, -31, 63, -63, 127, -127,
            255, -255, 257, -257, 65535, -65535,
            65537, -65537, (1 << 32) - 1, -((1 << 32) - 1),
            (1 << 32) + 1, -((1 << 32) + 1),
            (1 << 63) - 1, -((1 << 63) - 1)]
FALLBACK_DIVISORS = [0, 1, -1, 2, -2, 4, -4, 8, -8, 1 << 62, -(1 << 62),
                     -(1 << 63)]


def inspect(data, arch, fast, oracle, tmp):
    if not data or len(data) % 4:
        raise AssertionError('not fixed-width executable machine words')
    words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
    if arch == 'aarch64':
        patterns = [w for w in words if w & 0xffe0fc00 == 0x9bc07c00]
        divs = [w for w in words if w & 0x7fe0fc00 == 0x1ac00c00]
        if fast and (len(patterns) != 1 or divs):
            raise AssertionError(('A64 fast path missing', len(patterns), divs))
        if not fast and patterns:
            raise AssertionError('unexpected A64 UMULH')
        if oracle and patterns:
            w = patterns[0]
            rd, rn, rm = w & 31, (w >> 5) & 31, (w >> 16) & 31
            ref = assemble(oracle, arch, f'umulh x{rd},x{rn},x{rm}', tmp)
            if w.to_bytes(4, 'little') != ref:
                raise AssertionError('UMULH differs from Clang reference')
    else:
        patterns = [w for w in words if w & 0xfe00707f == 0x02003033]
        if fast and len(patterns) != 1:
            raise AssertionError(('RV64 M fast path missing', len(patterns)))
        if not fast and patterns:
            raise AssertionError('RV64I illegally emitted M instruction')
        if oracle and patterns:
            w = patterns[0]
            rd, lhs, rhs = (w >> 7) & 31, (w >> 15) & 31, (w >> 20) & 31
            ref = assemble(oracle, arch, f'.option arch,+m\nmulhu x{rd},x{lhs},x{rhs}', tmp)
            if w.to_bytes(4, 'little') != ref:
                raise AssertionError('MULHU differs from Clang reference')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--clang')
    args = ap.parse_args()
    count = 0
    with tempfile.TemporaryDirectory(prefix='aster-signed-magic-') as tmp:
        root = Path(tmp)
        for arch in ('aarch64', 'riscv64'):
            for divisor in DIVISORS + FALLBACK_DIVISORS:
                if divisor == 0:
                    continue  # Literal division by zero is a frontend error.
                expect_magic = divisor in DIVISORS
                for op in ('/', '%'):
                    for extension in ([True, False] if arch == 'riscv64' else [True]):
                        src, out = root / 'test.aa', root / 'test.bin'
                        src.write_text('module test.signed_magic; '
                                       'export fn value(x:i64)->i64 @abi(c) '
                                       f'{{ return x {op} {divisor}; }}\n', encoding='utf8')
                        cmd = [args.compiler, 'build', '--target', arch, '-j', '1', '-o', str(out), str(src)]
                        if arch == 'riscv64' and extension:
                            cmd.extend(['--feature', 'm'])
                        res = subprocess.run(cmd, text=True, capture_output=True, timeout=45)
                        if res.returncode or not out.is_file():
                            raise AssertionError((arch, op, divisor, extension, res.stdout, res.stderr))
                        inspect(out.read_bytes(), arch, expect_magic and extension, args.clang, tmp)
                        count += 1
        # Nonzero signed literal outside supported fast path remains valid.
        # Never turn signed-divide overflow (INT_MIN/-1) into host UB in tests.
        src, out = root / 'zero.aa', root / 'zero.bin'
        src.write_text('module test.signed_magic; export fn value(x:i64)->i64 @abi(c) '
                       '{ return x / 0; }\n', encoding='utf8')
        result = subprocess.run([args.compiler, 'build', '--target', 'aarch64',
                                 '-o', str(out), str(src)], capture_output=True, text=True)
        if result.returncode == 0:
            raise AssertionError('division-by-zero constant accepted')
    print(f'{count} signed i64 magic ISA source/encoder cases passed')


if __name__ == '__main__':
    main()
