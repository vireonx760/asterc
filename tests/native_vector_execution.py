#!/usr/bin/env python3
"""Execute managed SSE/VEX islands under QEMU, checking YMM aliases and live SSA."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile

from native_processor_qemu import command, x86_kernel

ENABLE = '''
mov rax,cr0
and rax,-13
or rax,2
mov cr0,rax
mov rax,cr4
or rax,0x40600
mov cr4,rax
xor ecx,ecx
mov eax,7
xor edx,edx
xsetbv
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True); p.add_argument('--clang', required=True); p.add_argument('--out')
    args = p.parse_args(); records = []
    with tempfile.TemporaryDirectory(prefix='aster-vector-execute-') as temporary:
        root = Path(temporary); source = root / 'case.aa'; raw = root / 'case.bin'
        for variant, bits in [('sse', 128), ('vex128', 128), ('vex256', 256)]:
            reg = 'ymm0' if bits == 256 else 'xmm0'; lanes = bits // 32
            transfer = 'movdqu' if variant == 'sse' else 'vmovdqu'
            operation = 'paddd {dst:xmm0,src:xmm0};' if variant == 'sse' else f'vpaddd {{dst:{reg},lhs:{reg},rhs:{reg}}};'
            body = f'mov {{dst:rax,src:p}}; {transfer} {{dst:{reg},src:mem<vec<{lanes},u32>>[rax]}}; ' + operation + \
                   f' {transfer} {{dst:mem<vec<{lanes},u32>>[rax],src:{reg}}};'
            for mode in ('native', 'exact', 'unsafe exact'):
                source.write_text('module vector.execute; export fn run(p:ptr<u32>,seed:u64)->u64 @abi(c) {'
                                  'let answer:u64=seed*17+9; ' + mode + ' x86_64 inputs(p) { ' + body + ' } return answer; }')
                for abi in ('sysv', 'win64'):
                    arg, seed = ('rdi', 'esi') if abi == 'sysv' else ('rcx', 'edx')
                    oracle = ENABLE + 'vmovdqu ymm0,ymmword ptr [rip+canary]\n' + \
                        f'lea {arg},[rip+values]\nmov {seed},11\ncall aster_entry\ncmp rax,196\njne failed\n' + \
                        'vmovdqu ymm1,ymmword ptr [rip+canary]\nvpxor ymm1,ymm1,ymm0\nvptest ymm1,ymm1\njnz failed\n'
                    for index in range(8):
                        expected = (index + 1) * (2 if index < lanes else 1)
                        oracle += f'cmp dword ptr [rip+values+{index * 4}],{expected}\njne failed\n'
                    oracle += '''jmp checked
.balign 32
canary: .long 0x12345678,0xaabbccdd,0x56781234,0xff00ff00,0x11223344,0xdeadbeef,0x01020304,0x98765432
values: .long 1,2,3,4,5,6,7,8
checked:
'''
                    for level in ('0', '2', 'z'):
                        for pressure in (0, 2):
                            argv = [args.compiler, 'build', '--target', 'x86_64', '--abi', abi, '-O' + level,
                                    '--no-red-zone', '--reg-limit', str(pressure), '-j', '1', '-o', raw, source]
                            if variant != 'sse': argv += ['--feature', 'avx2']
                            command(argv)
                            disk, _ = x86_kernel(oracle, raw, root, args.clang, abi, args.compiler)
                            records.append({'variant': variant, 'mode': mode, 'abi': abi, 'level': level, 'reg_limit': pressure,
                                            'bytes': raw.stat().st_size, 'sha256': hashlib.sha256(raw.read_bytes()).hexdigest(),
                                            'disk_sha256': hashlib.sha256(disk.read_bytes()).hexdigest()})
                print('PASS:', variant, mode, flush=True)
    if args.out: Path(args.out).write_text(json.dumps({'qemu_runs': len(records), 'records': records}, indent=2) + '\n')
    print(f'PASS: {len(records)} QEMU SSE/AVX2 runs; full YMM alias, memory results, live SSA and ABI canaries')


if __name__ == '__main__': main()
