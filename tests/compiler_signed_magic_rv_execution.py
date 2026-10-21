#!/usr/bin/env python3
"""Independent instruction-level RV64M execution of *emitted* AsterASM BIN.

This explicitly models a constrained set of RV64I/M encoding forms, rather
than executing MIR or repeating the compiler's magic algorithm. It does not
replace whole-program RISC-V/QEMU/physical ABI qualification.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

U64 = (1 << 64) - 1
DIVISORS = (3, -3, 5, -5, 7, -7, 10, -10, 11, -11, 31, -31,
            127, -127, 255, -255, 65537, -65537, (1 << 32) - 1,
            -(1 << 32) + 1, (1 << 63) - 1, -((1 << 63) - 1))
VALUES = (-(1 << 63), -(1 << 63) + 1, -(1 << 62), -1234567890123456,
          -65537, -257, -128, -13, -7, -1, 0, 1, 2, 3, 7, 13,
          127, 128, 257, 65537, (1 << 32), (1 << 62), (1 << 63) - 1)


def sx(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


def rv_exec(code, value):
    if not code or len(code) % 4:
        raise AssertionError('RV64M binary is empty or misaligned')
    regs = [0] * 32
    regs[10] = value & U64
    pc = 0
    for _ in range(500):
        if pc % 4 or not 0 <= pc < len(code):
            raise AssertionError(f'RV64 invalid PC: {pc}')
        insn = int.from_bytes(code[pc:pc + 4], 'little')
        if insn == 0x00008067:  # ret = jalr x0, 0(ra)
            return sx(regs[10], 64)
        opcode = insn & 127
        rd = (insn >> 7) & 31
        funct3 = (insn >> 12) & 7
        a = (insn >> 15) & 31
        b = (insn >> 20) & 31
        funct7 = insn >> 25
        if opcode == 0x37:  # LUI sign-extends its 32-bit result
            result = sx(insn & 0xfffff000, 32)
        elif opcode == 0x13:
            imm = sx(insn >> 20, 12)
            if funct3 == 0:
                result = regs[a] + imm
            elif funct3 == 4:
                result = regs[a] ^ imm
            elif funct3 == 1 and (funct7 >> 1) == 0:
                result = regs[a] << ((insn >> 20) & 63)
            elif funct3 == 5 and (funct7 >> 1) == 0:
                result = regs[a] >> ((insn >> 20) & 63)
            elif funct3 == 5 and (funct7 >> 1) == 0x10:
                result = sx(regs[a], 64) >> ((insn >> 20) & 63)
            else:
                raise AssertionError(f'unsupported I form 0x{insn:08x}')
        elif opcode == 0x1b and funct3 == 0:  # ADDIW
            result = sx((regs[a] + sx(insn >> 20, 12)) & 0xffffffff, 32)
        elif opcode == 0x33:
            if funct3 == 0 and funct7 == 0:
                result = regs[a] + regs[b]
            elif funct3 == 0 and funct7 == 0x20:
                result = regs[a] - regs[b]
            elif funct3 == 4 and funct7 == 0:
                result = regs[a] ^ regs[b]
            elif funct3 == 0 and funct7 == 1:
                result = regs[a] * regs[b]
            elif funct3 == 3 and funct7 == 1:
                result = (regs[a] * regs[b]) >> 64
            else:
                raise AssertionError(f'unsupported R form 0x{insn:08x}')
        else:
            raise AssertionError(f'unsupported opcode 0x{insn:08x}')
        if rd:
            regs[rd] = result & U64
        regs[0] = 0
        pc += 4
    raise AssertionError('RV64 exceeded instruction budget')


def trunc_quotient(n, d):
    absolute = abs(n) // abs(d)
    return -absolute if (n < 0) != (d < 0) else absolute


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    args = ap.parse_args()
    checked = 0
    with tempfile.TemporaryDirectory(prefix='aster-rv64-signed-exec-') as tmp:
        src, out = Path(tmp) / 'fn.aa', Path(tmp) / 'fn.bin'
        for d in DIVISORS:
            for op in ('/', '%'):
                src.write_text('module test.rv_magic; export fn value(x:i64)->i64 @abi(c) '
                               f'{{ return x {op} {d}; }}\n', encoding='utf-8')
                proc = subprocess.run([args.compiler, 'build', '--target', 'riscv64',
                                       '--feature', 'm', '-j', '1', '-o', str(out), str(src)],
                                      text=True, capture_output=True, timeout=30)
                if proc.returncode or not out.exists():
                    raise AssertionError(('compiler failed', d, op, proc.stderr))
                binary = out.read_bytes()
                for n in VALUES:
                    q = trunc_quotient(n, d)
                    expected = q if op == '/' else n - q * d
                    actual = rv_exec(binary, n)
                    if actual != expected:
                        raise AssertionError((n, op, d, expected, actual))
                    checked += 1
    print(f'RV64M independently executed {checked} signed magic BIN cases')


if __name__ == '__main__':
    main()
