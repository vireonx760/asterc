# SPDX-License-Identifier: MPL-2.0
"""Check guide programs; optionally execute their self-checks on all five ISAs."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ['x86_16', 'x86_32', 'x86_64', 'aarch64', 'riscv64']


def execute(data, target, base):
    import unicorn as uc
    from unicorn import x86_const as x, arm64_const as a, riscv_const as r
    if target.startswith('x86'):
        bits = int(target.split('_')[1])
        cpu = uc.Uc(uc.UC_ARCH_X86, {16: uc.UC_MODE_16, 32: uc.UC_MODE_32, 64: uc.UC_MODE_64}[bits])
        pc, sp, result = ((x.UC_X86_REG_RIP, x.UC_X86_REG_RSP, x.UC_X86_REG_RAX) if bits == 64
                          else (x.UC_X86_REG_EIP, x.UC_X86_REG_ESP, x.UC_X86_REG_EAX))
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
    cpu.mem_map(0, 0x500000); cpu.mem_write(base, data)
    cpu.reg_write(sp, stack-width)
    if width: cpu.mem_write(stack-width, stop.to_bytes(width, 'little'))
    cpu.emu_start(base, stop, timeout=5000000, count=5000000)
    assert cpu.reg_read(pc) == stop, (target, 'did not return')
    assert cpu.reg_read(sp) == stack, (target, 'stack corrupted')
    assert cpu.reg_read(result) == 42, (target, 'self-check failed', cpu.reg_read(result))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', type=Path, required=True)
    p.add_argument('--output', type=Path, default=ROOT/'build/guide-examples')
    p.add_argument('--skip-objects', action='store_true')
    p.add_argument('--runtime', action='store_true', help='requires Python Unicorn; run examples 79вЂ“100 at O0/O2/Oz')
    a = p.parse_args(); a.compiler = a.compiler.resolve(); a.output.mkdir(parents=True, exist_ok=True)
    sources = sorted((ROOT/'examples/guide').glob('*.aa')) + [ROOT/'examples/guide/modules/main.aa']
    results = []; inputs = {}
    for source in sources:
        key = source.relative_to(ROOT).as_posix()
        inputs[key] = hashlib.sha256(source.read_bytes()).hexdigest()
        number = int(source.name.split('_')[0]) if source.name[0].isdigit() else 0
        targets = ['x86_64'] if 'native' in source.name else TARGETS
        if 'atomics' in source.name or 'vector' in source.name: targets = TARGETS[2:]
        for target in targets:
            base = 0x1000 if target == 'x86_16' else 0x100000
            options = ['--target', target, '-j', '1', '--base', hex(base), '--no-red-zone', '-I', str(ROOT/'examples/guide/modules')]
            if target == 'riscv64': options += ['--feature', 'm', '--feature', 'a', '--feature', 'f', '--feature', 'd']
            if target == 'x86_64': options += ['--feature', 'popcnt']
            modes = ['check', 'obj'] if 'extern' in source.name else ['check', 'bin', 'obj']
            if a.skip_objects: modes.remove('obj')
            for opt in (['O0', 'O2', 'Oz'] if number >= 79 else ['O0']):
                for mode in modes:
                    if mode == 'check' and opt != 'O0': continue
                    command = [str(a.compiler), 'check' if mode == 'check' else 'build', '-'+opt]+options
                    output = a.output/f'{source.stem}-{target}-{opt}.{mode}'
                    if mode != 'check': command += ['--emit', mode, '--format', 'elf', '-o', str(output)]
                    proc = subprocess.run(command+[str(source)], capture_output=True, text=True, timeout=30)
                    if proc.returncode: raise RuntimeError(f'{source.name}/{target}/{opt}/{mode}: {proc.stderr}')
                    if mode != 'check' and not output.stat().st_size: raise RuntimeError('empty output: '+str(output))
                    row = dict(source=key, target=target, opt=opt, mode=mode)
                    if a.runtime and number >= 79 and mode == 'bin':
                        execute(output.read_bytes(), target, base); row['runtime'] = 'PASS'
                    results.append(row)
        print(f'PASS {source.name}', flush=True)
    if a.runtime:
        # Execute the actual edited examples, with a no-argument wrapper that
        # checks boundary values independently of each function's implementation.
        names = ['34_signed_absolute.aa', '66_factorial.aa', '75_ring_index.aa']
        wrapper = a.output/'boundary-checks.aa'
        code = ("module examples.boundaries;\n"
                "export fn run() -> u32 @abi(c) {\n"
                " if absolute(-9223372036854775807 - 1) != 9223372036854775808 { return 1; }\n"
                " if absolute(-42) != 42 || absolute(0) != 0 { return 2; }\n"
                " if factorial(0) != 1 || factorial(20) != 2432902008176640000 || factorial(21) != 0 { return 3; }\n"
                " if next(0xffffffff, 0xffffffff) != 0 || next(0, 0) != 0 || next(30, 32) != 31 { return 4; }\n"
                " if distance(30, 4, 32) != 6 || distance(32, 0, 32) != 0 { return 5; }\n"
                " return 42; }\n")
        import re
        for name in names:
            code += re.sub(r'^module [^;]+;', '', (ROOT/'examples/guide'/name).read_text())
        wrapper.write_text(code, encoding='utf-8')
        for target in TARGETS:
            base = 0x1000 if target == 'x86_16' else 0x100000
            for opt in ['O0', 'O2', 'Oz']:
                output = a.output/f'boundaries-{target}-{opt}.bin'
                command = [str(a.compiler), 'build', '-'+opt, '--target', target,
                           '--base', hex(base), '--no-red-zone', '-o', str(output), str(wrapper)]
                if target == 'riscv64': command += ['--feature', 'm']
                subprocess.run(command, check=True, capture_output=True, timeout=30)
                execute(output.read_bytes(), target, base)
                results.append(dict(source='boundary wrapper: '+','.join(names), target=target,
                                    opt=opt, mode='bin', runtime='PASS'))
    # Include imported source hashes so evidence cannot hide a dependency edit.
    for path in (ROOT/'examples/guide/modules').rglob('*.aa'):
        inputs[path.relative_to(ROOT).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    result = dict(status='PASS', programs=len(sources), operations=len(results),
                  runtime_cases=sum(r.get('runtime') == 'PASS' for r in results), source_sha256=inputs, cases=results)
    (a.output/'results.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(f'{len(sources)} programs: {len(results)} checks/builds, {result["runtime_cases"]} runtime self-checks.')


if __name__ == '__main__':
    main()
