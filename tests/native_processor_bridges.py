#!/usr/bin/env python3
"""Execute managed native/exact CPU instructions with live SSA and ABI canaries."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unicorn as u
from unicorn import x86_const as x

MASK = (1 << 64) - 1


def execute(code, abi, arguments, reference=False):
    cpu = u.Uc(u.UC_ARCH_X86, u.UC_MODE_64)
    cpu.mem_map(0x100000, 0x100000); cpu.mem_map(0x400000, 0x100000)
    cpu.mem_write(0x100000, code); cpu.mem_write(0x47fff8, (0x1f0000).to_bytes(8, 'little'))
    cpu.reg_write(x.UC_X86_REG_RSP, 0x47fff8)
    registers = [x.UC_X86_REG_RCX, x.UC_X86_REG_RDX, x.UC_X86_REG_R8] if abi == 'win64' else [x.UC_X86_REG_RDI, x.UC_X86_REG_RSI, x.UC_X86_REG_RDX]
    for r, value in zip(registers, arguments): cpu.reg_write(r, value)
    preserved = [x.UC_X86_REG_RBX, x.UC_X86_REG_RBP, x.UC_X86_REG_R12, x.UC_X86_REG_R13, x.UC_X86_REG_R14, x.UC_X86_REG_R15]
    if abi == 'win64': preserved += [x.UC_X86_REG_RDI, x.UC_X86_REG_RSI]
    for j, r in enumerate(preserved): cpu.reg_write(r, 0xabc000 + j)
    cpu.reg_write(x.UC_X86_REG_CR3, 0x123000)
    if reference:
        cpu.reg_write(x.UC_X86_REG_RAX, arguments[0]); cpu.reg_write(x.UC_X86_REG_RCX, 0)
    cpu.emu_start(0x100000, 0x1f0000, timeout=1000000, count=100000)
    assert cpu.reg_read(x.UC_X86_REG_RIP) == 0x1f0000 and cpu.reg_read(x.UC_X86_REG_RSP) == 0x480000
    if not reference:
        for j, r in enumerate(preserved): assert cpu.reg_read(r) == 0xabc000 + j, (abi, r)
    return cpu.reg_read(x.UC_X86_REG_RAX)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True); p.add_argument('--out')
    args = p.parse_args(); binaries = calls = 0
    inputs = [(0, 0, 0), (1, 0x123456789abcdef0, 0xfeed1234), (7, MASK, 1), (0, 123, 456)]
    cpuid = {leaf: execute(bytes.fromhex('0fa2c3'), 'sysv', (leaf,), reference=True) & 0xffffffff for leaf, _, _ in inputs}
    with tempfile.TemporaryDirectory(prefix='aster-native-bridge-') as temporary:
        root = Path(temporary); source = root / 'unit.aa'; raw = root / 'unit.bin'
        for operation in ('cpuid', 'control'):
            body = ('mov {dst:eax,src:leaf}; mov {dst:ecx,src:0}; cpuid; mov {dst:answer,src:eax};'
                    if operation == 'cpuid' else 'mov {dst:rax,src:cr3}; mov {dst:answer,src:rax};')
            for mode in ('native', 'exact', 'unsafe exact'):
                source.write_text('module processor.bridge; export fn run(leaf:u32,a:u64,b:u64)->u64 @abi(c) {'
                                  'let live:u64=a*17+b; let answer:' + ('u32' if operation == 'cpuid' else 'u64') + '; ' +
                                  mode + ' x86_64 inputs(leaf) outputs(answer) {' + body +
                                  '} return live+cast<u64>(answer); }')
                for abi in ('sysv', 'win64'):
                    for level in ('0', '2', 'z'):
                        for limit in (0, 2):
                            for promote in (False, True):
                                argv = [args.compiler, 'build', '--target', 'x86_64', '--abi', abi, '--no-red-zone',
                                        '-O' + level, '--reg-limit', str(limit), '-j', '1', '-o', str(raw), str(source)]
                                if not promote: argv += ['--no-promote']
                                result = subprocess.run(argv, capture_output=True, text=True, timeout=30)
                                assert result.returncode == 0, (argv, result.stderr)
                                code = raw.read_bytes(); binaries += 1
                                for leaf, a, b in inputs:
                                    expected = (a * 17 + b + (cpuid[leaf] if operation == 'cpuid' else 0x123000)) & MASK
                                    actual = execute(code, abi, (leaf, a, b))
                                    assert actual == expected, (operation, mode, abi, level, limit, promote, leaf, actual, expected)
                                    calls += 1
    report = {'binaries': binaries, 'guarded_calls': calls, 'modes': ['native', 'exact', 'unsafe exact'],
              'abis': ['sysv', 'win64'], 'levels': ['O0', 'O2', 'Oz'], 'unicorn': u.__version__}
    if args.out: Path(args.out).write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {binaries} managed native/exact CPU binaries, {calls} executions with live SSA and ABI canaries')


if __name__ == '__main__': main()
