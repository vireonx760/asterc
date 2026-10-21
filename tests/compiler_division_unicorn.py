#!/usr/bin/env python3
"""Independent CPU qualification of scalar division and its ABI boundary.

160 separately addressable source/target cases; two policies per case. Every
invocation uses RX code, an NX guarded stack, poisoned nonvolatile registers,
an instruction budget and an independently computed, UB-free integer oracle.
Unicorn is test-only. No hand-written ISA interpreter is used by this suite.
"""
import argparse
import itertools
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile

TYPES = [(prefix + str(bits), bits, prefix == 'i')
         for bits in (8, 16, 32, 64) for prefix in ('u', 'i')]
PROFILES = [('x86_64', 'sysv', False), ('x86_64', 'win64', False),
            ('aarch64', 'aapcs64', False), ('riscv64', 'lp64', True),
            ('riscv64', 'lp64', False)]
CASES = list(itertools.product(range(len(PROFILES)), TYPES, (False, True), ('/', '%')))
MASK64 = (1 << 64) - 1
BASE, STOP, STACK, SP = 0x100000, 0x1f0000, 0x400000, 0x408000


def normalize(value, bits, signed):
    value &= (1 << bits) - 1
    return value - (1 << bits) if signed and value >> (bits - 1) else value


def oracle(x, y, bits, signed, op, target):
    """The explicitly documented DEV target-dependent exceptional contract."""
    lo = -(1 << (bits - 1))
    if y == 0:
        if target == 'x86_64':
            return None  # integer #DE, never an ordinary arithmetic result
        quotient = 0 if target == 'aarch64' else (-1 if signed else (1 << bits) - 1)
    elif signed and x == lo and y == -1:
        if target == 'x86_64' and bits >= 32:
            return None
        quotient = -x
    else:
        quotient = abs(x) // abs(y)
        if signed and (x < 0) != (y < 0):
            quotient = -quotient
    value = quotient if op == '/' else x - quotient * y
    return normalize(value, bits, signed)


def execute(code, profile, xval, yval, bits, signed, expect_fault=False, enable_fp=False):
    import unicorn as uc
    from unicorn import x86_const as x, arm64_const as a, riscv_const as r
    target, abi, _ = PROFILES[profile]
    if target == 'x86_64':
        cpu = uc.Uc(uc.UC_ARCH_X86, uc.UC_MODE_64)
        args = [x.UC_X86_REG_RCX, x.UC_X86_REG_RDX] if abi == 'win64' else [x.UC_X86_REG_RDI, x.UC_X86_REG_RSI]
        result, sp, pc = x.UC_X86_REG_RAX, x.UC_X86_REG_RSP, x.UC_X86_REG_RIP
        names = ['RBX', 'RBP', 'R12', 'R13', 'R14', 'R15']
        if abi == 'win64':
            names += ['RSI', 'RDI']
        saved = [getattr(x, 'UC_X86_REG_' + n) for n in names]
        entry_sp = SP - 8
    elif target == 'aarch64':
        cpu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        if enable_fp:cpu.reg_write(a.UC_ARM64_REG_CPACR_EL1,3<<20)
        args = [a.UC_ARM64_REG_X0, a.UC_ARM64_REG_X1]
        result, sp, pc = a.UC_ARM64_REG_X0, a.UC_ARM64_REG_SP, a.UC_ARM64_REG_PC
        saved = [getattr(a, f'UC_ARM64_REG_X{i}') for i in range(19, 30)]
        cpu.reg_write(a.UC_ARM64_REG_X30, STOP)
        entry_sp = SP
    else:
        cpu = uc.Uc(uc.UC_ARCH_RISCV, uc.UC_MODE_RISCV64)
        if enable_fp:cpu.reg_write(r.UC_RISCV_REG_MSTATUS,3<<13)
        args = [r.UC_RISCV_REG_A0, r.UC_RISCV_REG_A1]
        result, sp, pc = r.UC_RISCV_REG_A0, r.UC_RISCV_REG_SP, r.UC_RISCV_REG_PC
        saved = [getattr(r, f'UC_RISCV_REG_X{i}') for i in (8, 9, *range(18, 28))]
        cpu.reg_write(r.UC_RISCV_REG_RA, STOP)
        entry_sp = SP
    length = (len(code) + 4095) & ~4095
    cpu.mem_map(BASE, length, uc.UC_PROT_READ | uc.UC_PROT_WRITE)
    cpu.mem_write(BASE, code)
    cpu.mem_protect(BASE, length, uc.UC_PROT_READ | uc.UC_PROT_EXEC)
    cpu.mem_map(STOP, 4096, uc.UC_PROT_READ | uc.UC_PROT_EXEC)
    cpu.mem_map(STACK, 0x10000, uc.UC_PROT_READ | uc.UC_PROT_WRITE)
    cpu.reg_write(sp, entry_sp)
    if target == 'x86_64':
        cpu.mem_write(entry_sp, STOP.to_bytes(8, 'little'))
    guard_at = entry_sp + (40 if abi == 'win64' else 8 if target == 'x86_64' else 0)
    guard = bytes(range(64))
    cpu.mem_write(guard_at, guard)
    sentinels = {reg: 0x9a873412ccdd0000 + n * 0x101 for n, reg in enumerate(saved)}
    for reg, value in sentinels.items():
        cpu.reg_write(reg, value)
    for reg, value in zip(args, (xval, yval)):
        # The RV psABI sign-extends a 32-bit value to XLEN even for u32.
        if target == 'riscv64' and bits == 32:
            value = normalize(value, 32, True)
        cpu.reg_write(reg, value & MASK64)
    instructions = [0]
    def instruction(cpu, address, size, user):
        instructions[0] += 1
    cpu.hook_add(uc.UC_HOOK_CODE, instruction)
    try:
        cpu.emu_start(BASE, STOP, timeout=1000000, count=50000)
    except uc.UcError as error:
        if expect_fault and target == 'x86_64' and error.errno == uc.UC_ERR_EXCEPTION:
            return None, instructions[0]
        raise AssertionError(f'{target}/{abi} PC={cpu.reg_read(pc):#x}: {error}') from error
    if expect_fault:
        raise AssertionError('expected integer divide exception did not occur')
    if cpu.reg_read(pc) != STOP:
        raise AssertionError('instruction/time budget exhausted or wrong return address')
    if cpu.reg_read(sp) != SP:
        raise AssertionError('stack pointer was not restored')
    for reg, value in sentinels.items():
        if cpu.reg_read(reg) != value:
            raise AssertionError(f'callee-saved register {reg} corrupted')
    if bytes(cpu.mem_read(guard_at, len(guard))) != guard:
        raise AssertionError('caller stack guard overwritten')
    return normalize(cpu.reg_read(result), bits, signed), instructions[0]


def run_case(compiler, index, opt_level='0'):
    profile, (typ, bits, signed), dynamic, op = CASES[index]
    target, abi, with_m = PROFILES[profile]
    lo = -(1 << (bits - 1)) if signed else 0
    hi = (1 << (bits - int(signed))) - 1
    divisor = -7 if signed else 7
    edges = [lo, lo + 1, -127, -7, -1, 0, 1, 2, 7, 127, hi - 1, hi]
    edges = sorted(set(v for v in edges if lo <= v <= hi))
    rng = random.Random(0x6a3a0000 + index)
    samples = [(v, divisor) for v in edges]
    samples += [(rng.randint(lo, hi), rng.choice([-7, -3, -1, 1, 3, 7] if signed else [1, 3, 7, hi])) for _ in range(16)]
    if dynamic:
        samples += [(v, 0) for v in (lo, 0, hi)]
        if signed:
            samples += [(lo, -1), (lo + 1, -1), (-7, 3), (7, -3), (-7, -3)]
    else:
        samples = [(v, divisor) for v, _ in samples]
    records = []
    with tempfile.TemporaryDirectory(prefix='aster-unicorn-div-') as tmp:
        root = Path(tmp); source, output = root / 'unit.aa', root / 'unit.bin'
        rhs = 'y' if dynamic else str(divisor)
        source.write_text(f'module qual.unicorn; export fn f(x:{typ},y:{typ})->{typ} @abi(c) {{ return x {op} {rhs}; }}\n')
        for policy in ('hot', 'size'):
            command = [compiler, 'build', '-O'+opt_level, '--target', target, '--abi', abi, '--emit', 'bin', '--no-red-zone', '-j', '1', '-o', str(output), str(source)]
            if with_m:
                command += ['--feature', 'm']
            env = os.environ.copy(); env['ASTERASM_DIV_POLICY'] = policy
            compiled = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
            if compiled.returncode:
                raise AssertionError((index, policy, compiled.stderr))
            code = output.read_bytes()
            if target == 'riscv64' and not with_m:
                if len(code) % 4 or any((w & 127) in (0x33, 0x3b) and w >> 25 == 1 for w in
                                       (int.from_bytes(code[i:i+4], 'little') for i in range(0, len(code), 4))):
                    raise AssertionError('RV64I emitted an M instruction')
            total_instructions = faults = 0
            for xval, yval in samples:
                want = oracle(xval, yval, bits, signed, op, target)
                got, instructions = execute(code, profile, xval, yval, bits, signed, want is None)
                if got != want:
                    raise AssertionError((index, target, abi, with_m, typ, dynamic, op, policy, xval, yval, got, want))
                total_instructions += instructions
                faults += want is None
            records.append({'case': index, 'target': target, 'abi': abi, 'm': with_m,
                            'type': typ, 'dynamic': dynamic, 'op': op, 'policy': policy,
                            'binary_bytes': len(code), 'calls': len(samples),
                            'expected_faults': faults, 'instructions': total_instructions})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--opt-level',choices=['0','2','z'],default='0')
    parser.add_argument('--python-path', default='')
    parser.add_argument('--case', type=int, choices=range(len(CASES)))
    parser.add_argument('--out')
    args = parser.parse_args()
    if args.python_path:
        sys.path.insert(0, args.python_path)
    records = []
    for index in (range(len(CASES)) if args.case is None else [args.case]):
        records.extend(run_case(args.compiler, index, args.opt_level))
    if args.out:
        import unicorn
        Path(args.out).write_text(json.dumps({'unicorn': unicorn.__version__, 'records': records}, indent=2) + '\n')
    print(f'PASS: {len(records)//2} Unicorn cases, {len(records)} policy variants, '
          f'{sum(r["calls"] for r in records)} calls, {sum(r["expected_faults"] for r in records)} expected exceptions')


if __name__ == '__main__':
    main()
