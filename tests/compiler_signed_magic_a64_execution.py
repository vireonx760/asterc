#!/usr/bin/env python3
"""Minimal independent A64 instruction execution for generated signed division.
This verifies raw AArch64 machine words and SP save/restore, not just MIR.
It is intentionally bounded, not a replacement for QEMU or an ARM CPU.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

MASK = (1 << 64) - 1
DIVISORS = (3, -3, 5, -5, 7, -7, 10, -10, 11, -11,
            31, -31, 127, -127, 65537, -65537,
            (1 << 32) - 1, -((1 << 32) - 1),
            (1 << 63) - 1, -((1 << 63) - 1))
VALUES = (-(1 << 63), -(1 << 63) + 1, -(1 << 62), -1234567890123456,
          -65537, -257, -128, -13, -7, -1, 0, 1, 2, 3, 7,
          13, 127, 128, 257, 65537, (1 << 32), (1 << 62), (1 << 63) - 1)


def sx(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


def a64_exec(code, n):
    if not code or len(code) % 4:
        raise AssertionError('bad A64 binary length')
    regs = [0] * 32
    regs[0] = n & MASK
    sp = 0x10000
    memory = {}
    pc = 0
    for _ in range(400):
        if pc % 4 or not 0 <= pc < len(code):
            raise AssertionError(f'invalid A64 PC {pc}')
        w = int.from_bytes(code[pc:pc + 4], 'little')
        rd, rn, rm = w & 31, (w >> 5) & 31, (w >> 16) & 31
        if w == 0xd65f03c0:
            if sp != 0x10000:
                raise AssertionError('SP was not restored on A64 RET')
            return sx(regs[0], 64)
        elif w & 0xffc00000 in (0xf9000000, 0xf9400000):
            addr = (sp if rn == 31 else regs[rn]) + (((w >> 10) & 0xfff) * 8)
            if w & (1 << 22):
                regs[rd] = memory[addr]
            else:
                memory[addr] = regs[rd]
            pc += 4
            continue
        elif w & 0xff000000 in (0xd1000000, 0x91000000):
            # 64-bit ADD/SUB immediate (SP is encoded as register 31)
            imm = (w >> 10) & 0xfff
            if w & (1 << 22):
                imm <<= 12
            lhs = sp if rn == 31 else regs[rn]
            result = (lhs - imm if w & (1 << 30) else lhs + imm) & MASK
            if rd == 31:
                sp = result
            else:
                regs[rd] = result
            pc += 4
            continue
        elif w & 0xff800000 == 0x93000000:
            # 64-bit signed bitfield move, ASR alias with imms=63.
            shift, imms = (w >> 16) & 63, (w >> 10) & 63
            if imms != 63:
                raise AssertionError('unsupported A64 SBFM encoding')
            result = sx(regs[rn], 64) >> shift
        elif w & 0xff800000 == 0xd3000000:
            # UBFM (LSR alias) with imms=63.
            shift, imms = (w >> 16) & 63, (w >> 10) & 63
            if imms != 63:
                raise AssertionError('unsupported A64 UBFM encoding')
            result = regs[rn] >> shift
        elif w & 0xff800000 in (0xd2800000, 0xf2800000, 0x92800000):
            shift = ((w >> 21) & 3) * 16
            imm = ((w >> 5) & 0xffff) << shift
            if w & 0xff800000 == 0xd2800000:  # MOVZ
                result = imm
            elif w & 0xff800000 == 0xf2800000:  # MOVK
                result = (regs[rd] & (MASK ^ (0xffff << shift))) | imm
            else:  # MOVN
                result = ~imm
        elif w & 0xffe0fc00 == 0x9bc07c00:  # UMULH
            result = (regs[rn] * regs[rm]) >> 64
        elif w & 0xffe08000 == 0x9b008000:  # MSUB
            ra = (w >> 10) & 31
            result = regs[ra] - regs[rn] * regs[rm]
        elif w & 0xffe0fc00 == 0xaa200000:  # ORN, NOT when rn=xzr
            result = ((0 if rn == 31 else regs[rn]) | (~regs[rm]))
        elif w & 0xffe0fc00 == 0xaa000000:  # ORR, MOV when rn=xzr
            result = (0 if rn == 31 else regs[rn]) | regs[rm]
        elif w & 0xffe0fc00 == 0xca000000:  # EOR
            result = (0 if rn == 31 else regs[rn]) ^ regs[rm]
        elif w & 0xffe0fc00 == 0x8b000000:  # ADD register
            result = regs[rn] + regs[rm]
        elif w & 0xffe0fc00 == 0xcb000000:  # SUB register
            result = regs[rn] - regs[rm]
        else:
            raise AssertionError(f'unsupported A64 opcode at {pc}: {w:08x}')
        if rd != 31:
            regs[rd] = result & MASK
        pc += 4
    raise AssertionError('A64 code exceeded instruction bound')


def quotient(n, d):
    q = abs(n) // abs(d)
    return -q if (n < 0) != (d < 0) else q


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    args = ap.parse_args()
    count = 0
    with tempfile.TemporaryDirectory(prefix='aster-a64-signed-') as temp:
        src, out = Path(temp) / 'in.aa', Path(temp) / 'out.bin'
        for d in DIVISORS:
            for op in ('/', '%'):
                src.write_text('module test.a64_magic; export fn value(x:i64)->i64 @abi(c) '
                               f'{{ return x {op} {d}; }}\n', encoding='utf8')
                process = subprocess.run([args.compiler, 'build', '--target', 'aarch64',
                                          '-j', '1', '-o', str(out), str(src)],
                                         capture_output=True, text=True, timeout=30)
                if process.returncode or not out.is_file():
                    raise AssertionError((d, op, process.stderr))
                data = out.read_bytes()
                for n in VALUES:
                    q = quotient(n, d)
                    expected = q if op == '/' else n - q * d
                    actual = a64_exec(data, n)
                    if actual != expected:
                        raise AssertionError((n, d, op, expected, actual))
                    count += 1
    print(f'A64 independently executed {count} signed magic BIN cases')


if __name__ == '__main__':
    main()
