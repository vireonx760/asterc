#!/usr/bin/env python3
"""Independent fixed-word NEON/RVV encoding and strict dumper round trips."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

from native_processor_contracts import run
from isa_inventory import macro_rows

ROOT = Path(__file__).resolve().parents[1]


def neon_cases():
    for line in macro_rows(ROOT / 'include/asterasm/detail/aarch64_vector_instructions.inc'):
        m = re.match(r'AST_NEON\((\w+), "(\w+)", (\w+),', line)
        if not m: continue
        key, name, shape = m.groups()
        for width in (64, 128):
            dst, lhs, rhs = (('v' if width == 128 else 'd') + str(n) for n in (31, 16, 23))
            if shape in ('LOAD', 'STORE'):
                for offset in (0, width // 8, 4095 * width // 8):
                    role = 'dst' if shape == 'LOAD' else 'src'; data = ('q' if width == 128 else 'd') + '31'
                    yield 'aarch64', key, f'{name} {{{role}:{data},addr:x0,offset:{offset}}};', f'{name} {data}, [x0, #{offset}]'
                continue
            sizes = (8,) if shape == 'LOGIC' else (32, 64) if shape == 'FLOAT' else (8, 16, 32, 64)
            for bits in sizes:
                if bits == 64 and (width == 64 or shape == 'SMALL_INTEGER'): continue
                arrangement = str(width // bits) + {8: 'b', 16: 'h', 32: 's', 64: 'd'}[bits]
                operands = f'dst:{dst},lhs:{lhs},rhs:{rhs}' + (f',bits:{bits}' if shape != 'LOGIC' else '')
                yield 'aarch64', key, f'{name} {{{operands}}};', f'{name} v31.{arrangement}, v16.{arrangement}, v23.{arrangement}'


def rvv_cases():
    for line in macro_rows(ROOT / 'include/asterasm/detail/riscv64_vector_instructions.inc'):
        m = re.match(r'AST_RVV_(PAIR|TRIPLE|SHIFT)\((\w+), "(\w+)",', line)
        if not m: continue
        shape, key, name = m.groups()
        modes = ('vv', 'vx', 'vi') if shape != 'PAIR' else ('vv', 'vx')
        for mode in modes:
            values = ('v29',) if mode == 'vv' else ('a0', 'zero') if mode == 'vx' else ('0', '31') if shape == 'SHIFT' else ('-16', '15')
            for value in values:
                for vm in (0, 1):
                    yield 'riscv64', key + '_' + mode, f'{name}.{mode} {{dst:v31,lhs:v30,rhs:{value},vm:{vm}}};', \
                        f'{name}.{mode} v31, v30, {value}' + (', v0.t' if not vm else '')
    for bits in (8, 16, 32, 64):
        for load in (True, False):
            name = ('vle' if load else 'vse') + str(bits) + '.v'; role = 'dst' if load else 'src'
            for vm in (0, 1):
                yield 'riscv64', name, f'{name} {{{role}:v31,addr:a0,vm:{vm}}};', f'{name} v31, (a0)' + (', v0.t' if not vm else '')
    groups = {0:'m1', 1:'m2', 2:'m4', 3:'m8', 5:'mf8', 6:'mf4', 7:'mf2'}
    for name in ('vsetvli', 'vsetivli'):
        for sew in (8, 16, 32, 64):
            for group, spelling in groups.items():
                for flags in (0, 64, 128, 192):
                    vtype = ((sew.bit_length() - 4) << 3) | group | flags
                    avl = '31' if name == 'vsetivli' else 'a1'
                    yield 'riscv64', name, f'{name} {{dst:a0,avl:{avl},vtype:{vtype}}};', \
                        f'{name} a0, {avl}, e{sew}, {spelling}, ' + ('ta' if flags & 64 else 'tu') + ', ' + ('ma' if flags & 128 else 'mu')
    yield 'riscv64', 'vsetvl', 'vsetvl {dst:a0,avl:a1,vtype:a2};', 'vsetvl a0,a1,a2'


def assemble(clang, arch, text, root):
    source = root / 'oracle.s'; obj = root / 'oracle.o'; raw = root / 'oracle.bin'
    source.write_text(('.option norvc\n' if arch == 'riscv64' else '') + '.text\n' + text + '\n')
    flags = ['-march=rv64gcv', '-mabi=lp64d'] if arch == 'riscv64' else []
    run([clang, '--target=' + arch + '-none-elf', *flags, '-c', source, '-o', obj])
    objcopy = str(Path(clang).with_name('llvm-objcopy' + ('.exe' if str(clang).endswith('.exe') else '')))
    run([objcopy, '--only-section=.text', '-O', 'binary', obj, raw]); return raw.read_bytes()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True); p.add_argument('--dumper', required=True)
    p.add_argument('--clang', required=True); p.add_argument('--out'); p.add_argument('--proof')
    args = p.parse_args(); records = []; binaries = modes = rejects = 0
    if args.proof:
        inventory = [line.split('\t') for line in run([args.proof, '--inventory']).stdout.splitlines()[1:]]
        expected = {(c[0], c[3].split()[0]) for c in (*neon_cases(), *rvv_cases())}
        assert {(row[1], row[2]) for row in inventory if row[1] != 'x86_64'} == expected
    with tempfile.TemporaryDirectory(prefix='aster-vector-other-') as temporary:
        root = Path(temporary); source = root / 'case.aa'; raw = root / 'case.bin'
        for arch, corpus in [('aarch64', list(neon_cases())), ('riscv64', list(rvv_cases()))]:
            features = ['--feature', 'v'] if arch == 'riscv64' else []
            for start in range(0, len(corpus), 64):
                chunk = corpus[start:start + 64]
                source.write_text(f'module vector.other; export fn run() @naked {{ unsafe exact {arch} {{ ' +
                                  ' '.join(c[2] for c in chunk) + ' ret; } }')
                expected = assemble(args.clang, arch, '\n'.join(c[3] for c in chunk) + '\nret', root)
                for level in ('0', '2', 'z'):
                    run([args.compiler, 'build', '--target', arch, *features, '-O' + level, '-j', '1', '-o', raw, source]); binaries += 1
                    assert raw.read_bytes() == expected, (arch, start, level, raw.read_bytes().hex(), expected.hex())
                payload = json.loads(run([args.dumper, '--target', arch, '--strict', '--format', 'json', raw]).stdout)
                assert payload['unknown'] == 0 and len(payload['instructions']) == len(chunk) + 1, payload
                assert assemble(args.clang, arch, '\n'.join(i['mnemonic'] for i in payload['instructions']), root) == expected
                records += [{'arch': a, 'form': key, 'source': body, 'assembly': asm} for a, key, body, asm in chunk]
                print(f'PASS: {arch} {min(start + 64, len(corpus))}/{len(corpus)} LLVM/dump vector cases', flush=True)
            for mode in ('native', 'exact', 'unsafe exact'):
                for level in ('0', '2', 'z'):
                    source.write_text(f'module vector.modes; export fn run() @naked {{ {mode} {arch} {{ ' +
                                      ' '.join(c[2] for c in corpus) + ' ret; } }')
                    run([args.compiler, 'build', '--target', arch, *features, '-O' + level, '-j', '1', '-o', raw, source]); modes += 1
                    assert raw.read_bytes() == assemble(args.clang, arch, '\n'.join(c[3] for c in corpus) + '\nret', root)
        negatives = [
            ('aarch64', 'add {dst:v0,lhs:v1,rhs:v2,bits:7};', []),
            ('aarch64', 'mul {dst:v0,lhs:v1,rhs:v2,bits:64};', []),
            ('aarch64', 'add {dst:d0,lhs:d1,rhs:d2,bits:64};', []),
            ('aarch64', 'add {dst:v8,lhs:v0,rhs:v1,bits:32};', []),
            ('aarch64', 'ldr {dst:q0,addr:x0,offset:1};', []),
            ('riscv64', 'vadd.vv {dst:v1,lhs:v2,rhs:v3};', []),
            ('riscv64', 'vadd.vv {dst:v0,lhs:v1,rhs:v2,vm:0};', ['--feature', 'v']),
            ('riscv64', 'vadd.vi {dst:v1,lhs:v2,rhs:16};', ['--feature', 'v']),
            ('riscv64', 'vsetvli {dst:a0,avl:a1,vtype:4};', ['--feature', 'v']),
            ('riscv64', 'vsetvli {dst:a0,avl:a1,vtype:256};', ['--feature', 'v']),
            ('riscv64', 'vsetvli {dst:sp,avl:a1,vtype:0};', ['--feature', 'v']),
            ('riscv64', 'vadd.vv {dst:v8,lhs:v9,rhs:v10};', ['--feature', 'v', '--abi', 'lp64v']),
        ]
        for arch, body, features in negatives:
            source.write_text(f'module vector.invalid; export fn run() @naked {{ exact {arch} {{ {body} ret; }} }}')
            result = subprocess.run([args.compiler, 'build', '--target', arch, *features, '-j', '1', '-o', raw, source], capture_output=True, text=True, timeout=30)
            assert result.returncode != 0, (arch, body); rejects += 1
    if args.out: Path(args.out).write_text(json.dumps({'oracle_cases': len(records), 'batched_binaries': binaries,
                                                       'mode_binaries': modes, 'rejections': rejects, 'records': records}, indent=2) + '\n')
    print(f'PASS: {len(records)} independent NEON/RVV cases, {binaries} batched binaries, {modes} mode builds, {rejects} rejections')


if __name__ == '__main__': main()
