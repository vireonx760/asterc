"""Exercise the actual CLI, data layout, fixups, memory accesses and stack contract."""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--compiler', required=True)
p.add_argument('--opt-level',choices=['0','2'],default='0')
p.add_argument('--python-path', required=True)
args = p.parse_args()
sys.path.insert(0, args.python_path)
import unicorn as uc
from unicorn import x86_const as x, arm64_const as a, riscv_const as r

SOURCE = Path(__file__).parent/'compiler_e2e'

def execute(data, mode, argument, entry=0x100000):
    if mode < 2:
        cpu = uc.Uc(uc.UC_ARCH_X86, uc.UC_MODE_64)
        arg, ret, sp = (x.UC_X86_REG_RCX if mode else x.UC_X86_REG_RDI), x.UC_X86_REG_RAX, x.UC_X86_REG_RSP
        pc = x.UC_X86_REG_RIP
    elif mode == 2:
        cpu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        arg, ret, sp, pc = a.UC_ARM64_REG_X0, a.UC_ARM64_REG_X0, a.UC_ARM64_REG_SP, a.UC_ARM64_REG_PC
        cpu.reg_write(a.UC_ARM64_REG_X30, 0x1f0000)
    else:
        cpu = uc.Uc(uc.UC_ARCH_RISCV, uc.UC_MODE_RISCV64)
        arg, ret, sp, pc = r.UC_RISCV_REG_X10, r.UC_RISCV_REG_X10, r.UC_RISCV_REG_X2, r.UC_RISCV_REG_PC
        cpu.reg_write(r.UC_RISCV_REG_X1, 0x1f0000)
    cpu.mem_map(0x100000, 0x100000)
    cpu.mem_map(0x400000, 0x100000)
    cpu.mem_write(0x100000, data)
    cpu.reg_write(sp, 0x480000 - (8 if mode < 2 else 0))
    if mode < 2: cpu.mem_write(0x47fff8, (0x1f0000).to_bytes(8, 'little'))
    cpu.reg_write(arg, argument)
    below_sp = []
    def write(cpu, access, address, size, value, user):
        if 0x400000 <= address < cpu.reg_read(sp): below_sp.append(address)
    cpu.hook_add(uc.UC_HOOK_MEM_WRITE, write)
    def invalid(cpu, access, address, size, value, user):
        print(f'mode={mode} PC={cpu.reg_read(pc):#x} invalid address={address:#x} size={size}', flush=True)
        if mode >= 3:
            print({i: hex(cpu.reg_read(r.UC_RISCV_REG_X0+i)) for i in range(32)}, flush=True)
        return False
    cpu.hook_add(uc.UC_HOOK_MEM_INVALID, invalid)
    cpu.emu_start(entry, 0x1f0000, timeout=1000000, count=100000)
    assert cpu.reg_read(pc) == 0x1f0000
    assert cpu.reg_read(sp) == 0x480000
    return cpu.reg_read(ret), below_sp

count = 0
with tempfile.TemporaryDirectory(prefix='aster-cli-runtime-') as tmp:
    output = Path(tmp)/'image.bin'
    targets = [('x86_64', 'sysv'), ('x86_64', 'win64'), ('aarch64', 'aapcs64'),
               ('riscv64', 'lp64'), ('riscv64', 'lp64f'), ('riscv64', 'lp64d')]
    for mode, (target, abi) in enumerate(targets):
        for limit in [0, 2]:
            for promote in [True, False]:
                cmd = [args.compiler, 'build', '-O'+args.opt_level, '--target', target, '--abi', abi, '--emit', 'bin', '--no-red-zone',
                       '--reg-limit', str(limit), '-j', '1', '-o', str(output), str(SOURCE/'runtime_globals.aa')]
                if target == 'riscv64': cmd += ['--feature', 'm', '--feature', 'a', '--feature', 'f', '--feature', 'd']
                if not promote: cmd += ['--no-promote']
                proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, (mode, limit, promote, proc.stderr)
                for value in [0, 41, 0x8000000000000000]:
                    try:
                        result, below = execute(output.read_bytes(), mode, value)
                    except Exception as error:
                        raise AssertionError((mode, limit, promote, value, output.read_bytes().hex())) from error
                    assert result == value + 84, (mode, limit, promote, value, result)
                    assert not below, (mode, below)
                    count += 1
                for source, expected in [('nullary_call', 42), ('constant_expression_cast', 4095), ('function_address', None), ('derived_addresses', 42), ('comptime_layout', 42), ('atomic_types',42), ('macro_runtime',42), ('indirect_pointer',42), ('logical_binary',42), ('runtime_generic_sizeof',12), ('byte_mul',415), ('immediate_ops',511), ('inline_qualified_pointer',42)]:
                    extra_cmd = [str(SOURCE/f'{source}.aa') if part == str(SOURCE/'runtime_globals.aa') else part for part in cmd]
                    proc = subprocess.run(extra_cmd, capture_output=True, text=True, timeout=30)
                    assert proc.returncode == 0, (source, mode, proc.stderr)
                    data = output.read_bytes()
                    result, below = execute(data, mode, 0)
                    if expected is None:
                        assert 0x100000 < result < 0x100000 + len(data), (mode, result)
                        result, below = execute(data, mode, 0, result)
                        expected = 42
                        count += 1
                    assert result == expected, (source, mode, result, expected)
                    count += 1
    for disable in [False, True]:
        cmd = [args.compiler, 'build', '-O'+args.opt_level, '--target', 'x86_64', '--abi', 'sysv', '--emit', 'bin',
               '-j', '1', '-o', str(output), str(SOURCE/'red_zone.aa')]
        if disable: cmd += ['--no-red-zone']
        subprocess.run(cmd, check=True, capture_output=True, timeout=30)
        result, below = execute(output.read_bytes(), 0, 41)
        assert result == 42
        assert bool(below) == (not disable), (disable, below)
        count += 1
print(f'{count} CLI image executions passed (globals, BSS, pointer fixups, no-red-zone).')
