# SPDX-License-Identifier: Apache-2.0
"""Execute raw entry order and callbacks, then resolve exports from C objects."""
import argparse
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile


PROFILES = [('x86_16', 'cdecl'), ('x86_32', 'cdecl'), ('x86_64', 'sysv'),
            ('x86_64', 'win64'), ('aarch64', 'aapcs64'), ('riscv64', 'lp64'),
            ('riscv64', 'lp64f'), ('riscv64', 'lp64d')]


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True,
                            text=True, timeout=60)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return result.stdout


def execute(data, target, base, expected):
    import unicorn as uc
    from unicorn import x86_const as x, arm64_const as a, riscv_const as r
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
    assert cpu.reg_read(pc) == stop, (target, 'did not return')
    assert cpu.reg_read(sp) == stack, (target, 'stack corrupted')
    assert cpu.reg_read(result) & 0xffffffff == expected, (target, expected, cpu.reg_read(result))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--python-path', default='')
    parser.add_argument('--object-writer', action='store_true')
    parser.add_argument('--clang', default='')
    args = parser.parse_args()
    if args.python_path:
        sys.path.insert(0, args.python_path)
    guide = Path(__file__).resolve().parents[1] / 'examples/guide/65_function_pointer.aa'
    first = 'fn helper() -> u32 { return 17; }\n'
    exported = 'export fn run() -> u32 @abi(c) { return 42; }\n'
    callback = '''export fn run() -> u32 @abi(c) {
    if call(0) != 0 || call(1) != 2 || call(21) != 42 { return 1; }
    if call(2147483647) != 4294967294 { return 2; }
    if call(2147483648) != 0 { return 3; }
    if call(4294967295) != 4294967294 { return 4; }
    if call(3735928559) != 3176889822 { return 5; }
    if swap(0x1122334455667788) != 0x8877665544332211 { return 6; }
    return 42;
}
'''
    swap = guide.with_name('53_byte_swap64.aa').read_text(encoding='utf-8').split(';', 1)[1]
    cases = [('private-first', first + exported, 17),
             ('export-first', exported + first, 42),
             ('callback', callback + guide.read_text(encoding='utf-8').split(';', 1)[1] + swap, 42)]
    executions = objects = native = 0
    with tempfile.TemporaryDirectory(prefix='aster-entry-contract-') as temporary:
        # Spaces in source/output paths are also part of the CLI contract.
        root = Path(temporary) / 'source and objects'
        root.mkdir()
        sources = {}
        for name, body, _ in cases:
            source = root / (name + '.aa')
            source.write_text('module regression.entry;\n' + body, encoding='utf-8')
            sources[name] = source
        for target, abi in PROFILES:
            base = 0x1000 if target == 'x86_16' else 0x100000
            options = ['--target', target, '--abi', abi, '--base', hex(base), '--no-red-zone']
            if target == 'riscv64':
                options += ['--feature', 'm', '--feature', 'f', '--feature', 'd']
            run([args.compiler, 'check', '--target', target, '--abi', abi, guide])
            for opt in ['-O0', '-O2', '-Oz']:
                for name, _, expected in cases:
                    command = [args.compiler, 'build', opt, *options, sources[name]]
                    # Keep callback and wrapper calls even under optimizing policies.
                    if name == 'callback':
                        command += ['--no-inline']
                    reference = None
                    for workers in ['1', '8']:
                        output = root / ('image-' + workers + '.bin')
                        extra = ['--dump-mir'] if name == 'callback' and opt == '-O0' else []
                        dump = run(command + ['-j', workers, '-o', output, *extra])
                        if extra:
                            assert 'g.call_indirect' in dump, dump
                        data = output.read_bytes()
                        assert reference is None or reference == data, (target, abi, opt, name)
                        reference = data
                        execute(data, target, base, expected)
                        executions += 1
                    if not args.object_writer:
                        continue
                    formats = ['elf']
                    if target != 'riscv64':
                        formats += ['coff']
                    if target in ['x86_64', 'aarch64']:
                        formats += ['macho']
                    for fmt in formats:
                        reference = None
                        for workers in ['1', '8']:
                            output = root / ('object-' + workers + '.o')
                            run(command + ['-j', workers, '--emit', 'obj', '--format', fmt, '-o', output])
                            data = output.read_bytes()
                            assert reference is None or reference == data, (target, abi, opt, name, fmt)
                            reference = data
                            objects += 1
        if args.object_writer and args.clang and platform.machine().lower() in ['amd64', 'x86_64']:
            abi, fmt = ('win64', 'coff') if os.name == 'nt' else ('sysv', 'elf')
            host = root / 'host.c'
            for opt in ['-O0', '-O2', '-Oz']:
                for name, _, _ in cases:
                    # The unchanged callback guide starts with double(), while
                    # the C caller must resolve the later exported call().
                    source = guide if name == 'callback' else sources[name]
                    declaration = 'extern uint32_t run(void);'
                    body = 'return run() != 42;'
                    if name == 'callback':
                        declaration = 'extern uint32_t call(uint32_t);'
                        body = '''const uint32_t inputs[] = {0, 1, 21, 0x7fffffff,
                            0x80000000, 0xffffffff, 0xdeadbeef};
                        for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
                            if (call(inputs[i]) != inputs[i] * UINT32_C(2)) return 1;
                        }
                        return 0;'''
                    host.write_text('#include <stdint.h>\n' + declaration + '\nint main(void) {' + body + '}\n',
                                    encoding='utf-8')
                    obj = root / 'original.o'
                    run([args.compiler, 'build', opt, '--target', 'x86_64', '--abi', abi,
                         '--emit', 'obj', '--format', fmt, '-o', obj, source])
                    exe = root / ('host.exe' if os.name == 'nt' else 'host')
                    run([args.clang, '-std=c17', '-Wall', '-Wextra', '-Werror', '-fuse-ld=lld',
                         host, obj, '-o', exe])
                    run([exe])
                    native += 1
    print(f'PASS: {executions} raw-image executions, {objects} deterministic objects, '
          f'{native} native C link/execution cases; entry order and callbacks, '
          '8 ISA/ABI profiles, O0/O2/Oz, 1/8 workers.')


if __name__ == '__main__':
    main()
