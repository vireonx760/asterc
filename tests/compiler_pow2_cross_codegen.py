#!/usr/bin/env python3
"""Cross-ISA u32/u64 unsigned power-of-two div/rem codegen regressions.

Checks the physical opcode selection and its *encoded bytes*, not merely IR.
Hardware execution is deliberately not claimed by this cross-compile test.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


TESTS = [
    ('aarch64', 'aapcs64', 'u32', '/', 8, 'A64_LSHR', 'A64_UDIV'),
    ('aarch64', 'aapcs64', 'u32', '%', 8, 'A64_UBFM', 'A64_UREM'),
    ('aarch64', 'aapcs64', 'u64', '/', 8, 'A64_LSHR', 'A64_UDIV'),
    ('aarch64', 'aapcs64', 'u64', '%', 8, 'A64_UBFM', 'A64_UREM'),
    ('aarch64', 'aapcs64', 'u64', '/', 1, None, 'A64_UDIV'),
    ('aarch64', 'aapcs64', 'u64', '%', 1, None, 'A64_UREM'),
    ('aarch64', 'aapcs64', 'u64', '/', 9223372036854775808, 'A64_LSHR', 'A64_UDIV'),
    ('aarch64', 'aapcs64', 'u64', '%', 9223372036854775808, 'A64_UBFM', 'A64_UREM'),
    ('aarch64', 'aapcs64', 'u64', '/', 7, 'A64_UMULH', 'A64_UDIV'),
    ('aarch64', 'aapcs64', 'i64', '/', 8, 'A64_SDIV', None),
    ('riscv64', 'lp64', 'u64', '/', 8, 'RISCV_SRLI', 'RISCV_DIVU'),
    ('riscv64', 'lp64', 'u64', '%', 8, 'RISCV_AND', 'RISCV_REMU'),
    ('riscv64', 'lp64', 'u64', '/', 1, None, 'RISCV_DIVU'),
    ('riscv64', 'lp64', 'u64', '%', 1, None, 'RISCV_REMU'),
    ('riscv64', 'lp64', 'u64', '/', 9223372036854775808, 'RISCV_SRLI', 'RISCV_DIVU'),
    ('riscv64', 'lp64', 'u64', '%', 9223372036854775808, 'RISCV_AND', 'RISCV_REMU'),
    ('riscv64', 'lp64', 'u64', '/', 7, 'RISCV_J', None),
    ('riscv64', 'lp64', 'i64', '/', 8, 'RISCV_J', None),
]


def execute(compiler: str) -> None:
    with tempfile.TemporaryDirectory(prefix='aster-cross-pow2-') as td:
        root = Path(td)
        for arch, abi, typ, op, divisor, required, excluded in TESTS:
            name = f'{arch}_{typ}_{"div" if op == "/" else "rem"}_{divisor}'
            src = root / (name + '.aa')
            dst = root / (name + '.bin')
            src.write_text('module test.pow2; export fn value(x:' + typ + ')->' + typ +
                           ' @abi(c) {return x ' + op + ' ' + str(divisor) + ';}\n', encoding='utf-8')
            completed = subprocess.run([compiler, 'build', '--target', arch, '--abi', abi,
                                        '--dump-regalloc', '-o', str(dst), str(src)],
                                       capture_output=True, text=True, timeout=45)
            if completed.returncode or not dst.exists() or not dst.stat().st_size:
                raise AssertionError((name, completed.stdout, completed.stderr))
            dump = completed.stdout
            if required is not None and ('AST_MACHINE_' + required) not in dump:
                raise AssertionError(('missing instruction', name, required, dump))
            if excluded is not None and ('AST_MACHINE_' + excluded) in dump:
                raise AssertionError(('unnecessary hardware div/rem', name, excluded, dump))
            if arch == 'aarch64' and typ == 'u64' and divisor == 8:
                # Independent A64 encodings: LSR x0,x0,#3 = D343FC00;
                # UBFM x0,x0,#0,#2 = D3400800; RET = D65F03C0.
                expected = bytes.fromhex('00fc43d3c0035fd6' if op == '/' else '000840d3c0035fd6')
                actual = dst.read_bytes()
                if actual != expected:
                    raise AssertionError(('incorrect A64 bytes', name, actual.hex(), expected.hex()))
            if arch == 'riscv64' and typ == 'u64' and divisor == 8:
                # Inspect base RV64I words rather than relying on a host-specific disassembler.
                words = [int.from_bytes(dst.read_bytes()[i:i + 4], 'little')
                         for i in range(0, dst.stat().st_size, 4)]
                if op == '/' and not any(w & 0x707f == 0x5013 and (w >> 20) & 0x3f == 3 for w in words):
                    raise AssertionError(('missing SRLI immediate 3', name, words))
                if op == '%' and not any(w & 0xfe00707f == 0x7033 for w in words):
                    raise AssertionError(('missing RV64 AND', name, words))
        print('cross-target power-of-two div/rem: ' + str(len(TESTS)) + ' codegen and encoding cases passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    execute(parser.parse_args().compiler)
