#!/usr/bin/env python3
"""Exercise generated A64/RV64I forward DAGs in a minimal independent ISA interpreter.

Not a replacement for QEMU or real hardware. The decoder is intentionally
limited to the instruction forms used here and fails on unknown machine words.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

MASK64 = (1 << 64) - 1


def sx(value, bits):
    value &= (1 << bits) - 1
    return (value ^ (1 << (bits - 1))) - (1 << (bits - 1))


def execute(arch, data, a, b):
    if len(data) % 4:
        raise AssertionError('unaligned code')
    ins = list(struct.unpack('<' + 'I' * (len(data)//4), data))
    regs = [0] * 32
    regs[0 if arch == 'aarch64' else 10] = a & MASK64
    regs[1 if arch == 'aarch64' else 11] = b & MASK64
    pc = 0
    for _step in range(10000):
        if pc < 0 or pc % 4 or pc // 4 >= len(ins):
            raise AssertionError(f'PC out of image: {pc}')
        word = ins[pc//4]
        if arch == 'aarch64':
            if word == 0xd65f03c0:
                return regs[0]
            if (word & 0x7c000000) == 0x14000000:
                pc += sx(word & 0x03ffffff, 26) * 4
                continue
            if (word & 0x7e000000) == 0x34000000:
                index = word & 31
                bits = 64 if word & (1 << 31) else 32
                val = regs[index] & ((1 << bits) - 1)
                condition = (val != 0) if word & (1 << 24) else (val == 0)
                pc += sx((word >> 5) & 0x7ffff, 19) * 4 if condition else 4
                continue
            signature = word & 0x7fe0fc00
            if signature in (0x0b000000, 0x4a000000):
                dst, lhs, rhs = word & 31, (word >> 5) & 31, (word >> 16) & 31
                value = regs[lhs] + regs[rhs] if signature == 0x0b000000 else regs[lhs] ^ regs[rhs]
                if not (word & (1 << 31)):
                    value &= 0xffffffff
                regs[dst] = value & MASK64
                pc += 4
                continue
        else:
            opcode = word & 127
            if word == 0x00008067:
                return regs[10]
            if opcode == 0x6f:
                imm = ((word >> 31) & 1) << 20 | ((word >> 21) & 1023) << 1 | ((word >> 20) & 1) << 11 | (word & 0xff000)
                if (word >> 7) & 31:
                    raise AssertionError('unexpected jal link')
                pc += sx(imm, 21)
                continue
            if opcode == 0x63:
                rs1, rs2 = (word >> 15) & 31, (word >> 20) & 31
                f = (word >> 12) & 7
                if f not in (0, 1):
                    raise AssertionError('unsupported branch funct3')
                imm = ((word >> 31) & 1) << 12 | ((word >> 7) & 1) << 11 | ((word >> 25) & 63) << 5 | ((word >> 8) & 15) << 1
                condition = (regs[rs1] == regs[rs2]) if f == 0 else (regs[rs1] != regs[rs2])
                pc += sx(imm, 13) if condition else 4
                continue
            if opcode == 0x33:
                dst, rs1, rs2 = (word >> 7) & 31, (word >> 15) & 31, (word >> 20) & 31
                if word >> 25:
                    raise AssertionError('unsupported RV arithmetic funct7')
                f = (word >> 12) & 7
                if f == 0:
                    regs[dst] = (regs[rs1] + regs[rs2]) & MASK64
                elif f == 4:
                    regs[dst] = regs[rs1] ^ regs[rs2]
                else:
                    raise AssertionError('unsupported RV arithmetic funct3')
                pc += 4
                continue
        raise AssertionError(f'unknown instruction {word:08x} at {pc}')
    raise AssertionError('exceeded instruction budget')


def programs(arch):
    if arch == 'aarch64':
        return [
            ('cbz {reg:x0,target:done}; add {dst:x0,lhs:x0,rhs:x1}; label {name:done}; ret;',
             lambda a, b: a if a == 0 else (a+b)&MASK64),
            ('cbnz {reg:w0,target:done}; add {dst:x0,lhs:x0,rhs:x1}; label {name:done}; ret;',
             lambda a, b: a if a & 0xffffffff else (a+b)&MASK64),
            ('cbz {reg:x0,target:else_path}; add {dst:x0,lhs:x0,rhs:x1}; b {target:done}; '
             'label {name:else_path}; eor {dst:x0,lhs:x0,rhs:x1}; label {name:done}; ret;',
             lambda a, b: (a ^ b) if a == 0 else (a+b)&MASK64),
        ]
    return [
        ('beq {lhs:a0,rhs:a1,target:done}; add {dst:a0,lhs:a0,rhs:a1}; label {name:done}; ret;',
         lambda a, b: a if a == b else (a+b)&MASK64),
        ('bne {lhs:a0,rhs:a1,target:done}; add {dst:a0,lhs:a0,rhs:a1}; label {name:done}; ret;',
         lambda a, b: a if a != b else (a+b)&MASK64),
        ('beq {lhs:a0,rhs:a1,target:else_path}; add {dst:a0,lhs:a0,rhs:a1}; jal {dst:zero,target:done}; '
         'label {name:else_path}; xor {dst:a0,lhs:a0,rhs:a1}; label {name:done}; ret;',
         lambda a, b: (a ^ b) if a == b else (a+b)&MASK64),
    ]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--dumper')
    parser.add_argument('--target', choices=('aarch64','riscv64'), required=True)
    args = parser.parse_args()
    values = ((0,0), (0,9), (8,0), (2,3), (6,6), (1,1),
              (0xffffffff00000000, 11), (MASK64,2))
    with tempfile.TemporaryDirectory(prefix='aster-cfg-run-') as directory:
        base = Path(directory)
        for i, (native, model) in enumerate(programs(args.target)):
            src, out = base/f'{i}.aa', base/f'{i}.bin'
            src.write_text('module test.cfg_semantics;\nexport fn raw(a:u64,b:u64)->u64 @naked {\n'
                           f'unsafe exact {args.target} {{ {native} }}\n}}\n', encoding='utf8')
            process = subprocess.run([args.compiler,'build','--target',args.target,'--emit','bin',
                                      '-j','1','-o',str(out),str(src)], capture_output=True,
                                     text=True, timeout=35)
            if process.returncode or not out.is_file():
                raise AssertionError(f'{args.target} [{i}] compile failed: {process.stderr}')
            code = out.read_bytes()
            if args.dumper:
                dump = subprocess.run([args.dumper, '--target', args.target,
                                       '--format', 'text', '--strict', str(out)],
                                      capture_output=True, text=True, timeout=15)
                if dump.returncode or '.word' in dump.stdout or '.byte' in dump.stdout:
                    raise AssertionError(f'{args.target} [{i}] unexpected dumper result: {dump.stdout} {dump.stderr}')
            for a,b in values:
                result, wanted = execute(args.target, code, a,b), model(a,b)
                if result != wanted:
                    raise AssertionError(f'{args.target} [{i}] ({a},{b}): {result} != {wanted}')
    print(f'{args.target}: {len(programs(args.target)) * len(values)} encoded-control-flow executions passed')


if __name__ == '__main__':
    main()
