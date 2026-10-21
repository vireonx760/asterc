#!/usr/bin/env python3
"""Independent bounded execution of local leaf-call machine bytes.

Only decodes the handful of A64/RV64I opcodes used by this regression.  This
is *not* hardware, operating-system or full ABI emulation.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

MASK = (1 << 64) - 1


def sx(value, width):
    value &= (1 << width) - 1
    return (value ^ (1 << (width - 1))) - (1 << (width - 1))


def execute(arch, data, x, y):
    if not data or len(data) % 4:
        raise AssertionError('invalid executable image length')
    words = struct.unpack('<' + 'I' * (len(data)//4), data)
    registers = [0] * 32
    link_reg = 30 if arch == 'aarch64' else 1
    first = 0 if arch == 'aarch64' else 10
    second = 1 if arch == 'aarch64' else 11
    registers[first], registers[second] = x & MASK, y & MASK
    registers[link_reg] = 0x10000000
    initial_sp = 0x2000
    registers[2] = initial_sp if arch == 'riscv64' else registers[2]
    sp = initial_sp
    cells = {}
    pc = 0
    for _ in range(10000):
        if pc % 4 or not 0 <= pc//4 < len(words):
            raise AssertionError(f'invalid PC {pc:x}')
        word = words[pc//4]
        if arch == 'aarch64':
            if word == 0xd65f03c0:
                target = registers[30]
                if target == 0x10000000:
                    if sp != initial_sp or cells.get(initial_sp - 16) != 0x10000000:
                        raise AssertionError('stack/link not recovered')
                    return registers[0]
                pc = target
                continue
            if word & 0xfc000000 == 0x94000000:
                registers[30] = pc + 4
                pc += sx(word & 0x3ffffff, 26) * 4
                continue
            if word & 0xffc00000 in (0xd1000000, 0x91000000):
                assert word & 31 == 31 and (word >> 5) & 31 == 31
                amount = (word >> 10) & 0xfff
                if word & (1 << 22): amount <<= 12
                sp += -amount if word & 0x40000000 else amount
                pc += 4
                continue
            if word & 0xffc00000 in (0xf9000000, 0xf9400000):
                assert (word >> 5) & 31 == 31
                addr = sp + ((word >> 10) & 0xfff) * 8
                rt = word & 31
                if word & 0x00400000: registers[rt] = cells[addr]
                else: cells[addr] = registers[rt]
                pc += 4
                continue
            if word & 0xff200000 in (0x8b000000, 0xca000000):
                rd, rn, rm = word & 31, (word >> 5) & 31, (word >> 16) & 31
                registers[rd] = ((registers[rn] + registers[rm]) if
                                 (word & 0xff200000) == 0x8b000000 else
                                 (registers[rn] ^ registers[rm])) & MASK
                pc += 4
                continue
        else:
            op = word & 0x7f
            if word == 0x00008067:
                target = registers[1]
                if target == 0x10000000:
                    if registers[2] != initial_sp or cells.get(initial_sp - 16) != 0x10000000:
                        raise AssertionError('stack/link not recovered')
                    return registers[10]
                pc = target
                continue
            if op == 0x6f:
                imm = ((word >> 31) << 20) | (((word >> 21) & 0x3ff) << 1) | (((word >> 20) & 1) << 11) | (word & 0xff000)
                rd = (word >> 7) & 31
                assert rd == 1
                registers[rd] = pc + 4
                pc += sx(imm, 21)
                continue
            if op == 0x13:
                rd, rs = (word >> 7) & 31, (word >> 15) & 31
                assert (word >> 12) & 7 == 0
                registers[rd] = (registers[rs] + sx(word >> 20, 12)) & MASK
                pc += 4
                continue
            if op == 0x23:
                assert (word >> 12) & 7 == 3
                rs, rt = (word >> 15) & 31, (word >> 20) & 31
                imm = ((word >> 25) << 5) | ((word >> 7) & 31)
                cells[(registers[rs] + sx(imm, 12)) & MASK] = registers[rt]
                pc += 4
                continue
            if op == 0x03:
                assert (word >> 12) & 7 == 3
                rd, rs = (word >> 7) & 31, (word >> 15) & 31
                registers[rd] = cells[(registers[rs] + sx(word >> 20, 12)) & MASK]
                pc += 4
                continue
            if op == 0x33:
                opfun = (word >> 12) & 7
                assert opfun in (0, 4) and word >> 25 == 0
                rd, lhs, rhs = (word >> 7) & 31, (word >> 15) & 31, (word >> 20) & 31
                registers[rd] = ((registers[lhs] + registers[rhs]) if opfun == 0 else
                                 (registers[lhs] ^ registers[rhs])) & MASK
                pc += 4
                continue
        raise AssertionError(f'unknown {arch} code {word:08x} at {pc:x}')
    raise AssertionError('local call did not terminate')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--target', choices=('aarch64', 'riscv64'), required=True)
    args = ap.parse_args()
    arm = args.target == 'aarch64'
    body = ('sub {dst:sp,lhs:sp,rhs:16}; str {src:x30,offset:0}; bl {target:leaf}; '
            'ldr {dst:x30,offset:0}; add {dst:sp,lhs:sp,rhs:16}; ret; '
            'label {name:leaf}; add {dst:x0,lhs:x0,rhs:x1}; ret;' if arm else
            'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0}; jal {dst:ra,target:leaf}; '
            'ld {dst:ra,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret; '
            'label {name:leaf}; add {dst:a0,lhs:a0,rhs:a1}; ret;')
    with tempfile.TemporaryDirectory(prefix='aster-local-exec-') as temp:
        source, binary = Path(temp)/'main.aa', Path(temp)/'main.bin'
        source.write_text(f'module local.call;\nexport fn f(a:u64,b:u64)->u64 @naked {{\n'
                          f'unsafe exact {args.target} {{ {body} }}\n}}\n')
        result = subprocess.run([args.compiler, 'build', '--target', args.target,
                                 '--emit', 'bin', '-j', '1', '-o', str(binary), str(source)],
                                capture_output=True, text=True, timeout=60)
        assert result.returncode == 0 and binary.exists(), result.stderr
        data = binary.read_bytes()
        values = (0, 1, 7, 19, 255, 2**32, 2**63, MASK)
        for x in values:
            for y in values:
                answer = execute(args.target, data, x, y)
                assert answer == (x+y)&MASK, (args.target, x, y, answer)
        print(f'{args.target}: {len(values)**2} interpreted local call executions passed')


if __name__ == '__main__':
    main()
