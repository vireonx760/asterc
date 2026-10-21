"""Execute runtime literals and verify parallel output and object emission."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', required=True)
p.add_argument('--python-path', required=True)
p.add_argument('--object-writer', action='store_true')
args = p.parse_args()
sys.path.insert(0, args.python_path)
import unicorn as uc
from unicorn import x86_const as x, arm64_const as a, riscv_const as r


def execute(data, target, abi, base):
    if target.startswith('x86'):
        bits = int(target.split('_')[1])
        cpu = uc.Uc(uc.UC_ARCH_X86, {16: uc.UC_MODE_16, 32: uc.UC_MODE_32,
                                   64: uc.UC_MODE_64}[bits])
        pc, sp, result = ((x.UC_X86_REG_RIP, x.UC_X86_REG_RSP, x.UC_X86_REG_RAX)
                          if bits == 64 else
                          (x.UC_X86_REG_EIP, x.UC_X86_REG_ESP, x.UC_X86_REG_EAX))
        stack, stop = (0xf000, 0xf800) if bits == 16 else (0x480000, 0x1f0000)
        width = 8 if bits == 64 else 4
    elif target == 'aarch64':
        cpu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        pc, sp, result = a.UC_ARM64_REG_PC, a.UC_ARM64_REG_SP, a.UC_ARM64_REG_X0
        stack, stop, width = 0x480000, 0x1f0000, 0
        cpu.reg_write(a.UC_ARM64_REG_X30, stop)
    else:
        cpu = uc.Uc(uc.UC_ARCH_RISCV, uc.UC_MODE_RISCV64)
        pc, sp, result = r.UC_RISCV_REG_PC, r.UC_RISCV_REG_X2, r.UC_RISCV_REG_X10
        stack, stop, width = 0x480000, 0x1f0000, 0
        cpu.reg_write(r.UC_RISCV_REG_X1, stop)
    cpu.mem_map(0, 0x500000)
    cpu.mem_write(base, data)
    cpu.reg_write(sp, stack - width)
    if width:
        cpu.mem_write(stack - width, stop.to_bytes(width, 'little'))
    cpu.emu_start(base, stop, timeout=2000000, count=1000000)
    assert cpu.reg_read(pc) == stop, (target, abi, 'did not return')
    assert cpu.reg_read(sp) == stack, (target, abi, 'stack corrupted')
    return cpu.reg_read(result) & 255


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, (command, result.stdout, result.stderr)


profiles = [('x86_16', 'cdecl'), ('x86_32', 'cdecl'), ('x86_64', 'sysv'),
            ('x86_64', 'win64'), ('aarch64', 'aapcs64'), ('riscv64', 'lp64'),
            ('riscv64', 'lp64f'), ('riscv64', 'lp64d')]
fixture = Path(__file__).parent / 'hir_cases/runtime_strings'
executions = objects = 0
with tempfile.TemporaryDirectory(prefix='aster-string-literals-') as tmp:
    tmp = Path(tmp)
    repro = tmp / 'repro.aa'
    # Raw images start at the first function; object callers resolve `run`.
    repro.write_text('''module repro.c_string_hir;
export fn run() -> u8 @abi(c) { return first_byte(c"hello"); }
fn first_byte(p: ptr<const u8>) -> u8 @abi(c) { return load.u8 [p]; }
''', encoding='utf-8')
    for target, abi in profiles:
        for opt in ['-O0', '-O2', '-Oz']:
            for source, expected in [(repro, 104), (fixture / 'main.aa', 42)]:
                base = 0x1000 if target == 'x86_16' else 0x100000
                command = [args.compiler, 'build', opt, '--target', target,
                           '--abi', abi, '--base', hex(base), '--no-red-zone',
                           '-I', str(fixture), str(source)]
                if target == 'riscv64':
                    command += ['--feature', 'm', '--feature', 'f', '--feature', 'd']
                images = []
                for workers in ['1', '8']:
                    output = tmp / f'image-{workers}.bin'
                    run(command + ['-j', workers, '--emit', 'bin', '-o', str(output)])
                    data = output.read_bytes()
                    actual = execute(data, target, abi, base)
                    assert actual == expected, (target, abi, opt, source, actual, expected)
                    executions += 1
                    images.append(data)
                assert images[0] == images[1], (target, abi, opt, 'parallel image differs')
                if not args.object_writer:
                    continue
                formats = ['elf']
                if target.startswith('x86') or target == 'aarch64':
                    formats += ['coff']
                if target in ['x86_64', 'aarch64']:
                    formats += ['macho']
                for fmt in formats:
                    outputs = []
                    for workers in ['1', '8']:
                        output = tmp / f'object-{workers}.o'
                        run(command + ['-j', workers, '--emit', 'obj', '--format', fmt,
                                       '-o', str(output)])
                        outputs.append(output.read_bytes())
                        objects += 1
                    assert outputs[0] == outputs[1], (target, abi, opt, fmt, 'parallel object differs')
print(f'PASS: {executions} string image executions, {objects} objects; '
      '8 ABI/ISA profiles, O0/O2/Oz, 1/8 workers, byte-identical parallel output.')
