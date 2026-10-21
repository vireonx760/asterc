#!/usr/bin/env python3
"""Manual SIMD: independent LLVM bytes, strict dump/reassembly and rejected forms."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from native_processor_contracts import assemble, run
from isa_inventory import macro_rows

ROOT = Path(__file__).resolve().parents[1]


def forms():
    result = []
    for line in macro_rows(ROOT / 'include/asterasm/detail/vector_instructions.inc'):
        match = re.match(r'AST_VECTOR_(PAIR|INTEGER|EXT|UNARY|MOVE)\(([A-Z0-9_]+), "(\w+)"', line)
        if not match: continue
        kind, key, name = match.groups()
        for vex in (False, True):
            shape = 'move' if kind == 'MOVE' else 'unary' if kind == 'UNARY' else 'binary'
            feature = 'avx' if vex else 'sse41' if kind == 'EXT' else 'sse2'
            wide = 'avx2' if kind in ('INTEGER', 'EXT') else 'avx'
            result.append(('X86_' + ('V' if vex else '') + key, ('v' if vex else '') + name, shape, vex, feature, wide))
    assert len({f[0] for f in result}) == len(result) and len(result) >= 180
    return result


def cases():
    for key, name, shape, vex, feature, wide in forms():
        for bits in ((128, 256) if vex else (128,)):
            registers = ('ymm' if bits == 256 else 'xmm')
            for memory in (False, True):
                dst, lhs, rhs = (registers + str(i) for i in (15, 14, 13))
                source = f'mem<vec<{bits // 32},u32>>[r13+r10*4-129]' if memory else rhs
                asm_source = f'{registers}word ptr [r13+r10*4-129]' if memory else rhs
                if shape == 'binary' and vex:
                    body = f'{name} {{dst:{dst},lhs:{lhs},rhs:{source}}};'
                    asm = f'{name} {dst}, {lhs}, {asm_source}'
                else:
                    body = f'{name} {{dst:{dst},src:{source}}};'
                    asm = f'{name} {dst}, {asm_source}'
                yield key, bits, body, asm, (wide if bits == 256 else feature)
            if shape == 'move':
                yield key, bits, f'{name} {{dst:mem<vec<{bits // 32},u32>>[r12],src:{registers}3}};', \
                    f'{name} {registers}word ptr [r12], {registers}3', (wide if bits == 256 else feature)
    for bits in (128, 256):
        reg = 'xmm' if bits == 128 else 'ymm'; name = 'paddd' if bits == 128 else 'vpaddd'
        for base in ('rax', 'rbp', 'rsp', 'r12', 'r13'):
            for offset in (-2147483648, -129, -128, -1, 0, 1, 127, 128, 2147483647):
                address = base + (('+' if offset >= 0 else '') + str(offset) if offset else '')
                operands = f'dst:{reg}0,' + (f'lhs:{reg}1,rhs:' if bits == 256 else 'src:')
                body = f'{name} {{{operands}mem<vec<{bits // 32},u32>>[{address}]}};'
                asm = f'{name} {reg}0, ' + (f'{reg}1, ' if bits == 256 else '') + f'{reg}word ptr [{address}]'
                yield 'address_boundaries', bits, body, asm, 'avx2' if bits == 256 else 'sse2'


def evex_forms():
    result = []
    for line in macro_rows(ROOT / 'include/asterasm/detail/vector_instructions.inc'):
        match = re.match(r'AST_VECTOR_EFLOAT\(([A-Z0-9_]+), "(\w+)"', line)
        if match:
            key, name = match.groups()
            result += [('X86_E' + key + s.upper(), name + s, 'BINARY', 'avx512f') for s in ('ps', 'pd')]
        match = re.match(r'AST_VECTOR_EBYTE\(([A-Z0-9_]+), "(\w+)"', line)
        if match: result.append(('X86_E' + match[1], match[2], 'BINARY', 'avx512bw'))
        match = re.match(r'AST_VECTOR_EVEX\(([A-Z0-9_]+), "(\w+)", (\w+), .*AST_TARGET_FEATURE_X86_(\w+)\)', line)
        if match: result.append(('X86_E' + match[1], match[2], match[3], match[4].lower()))
    return result


def evex_cases():
    for key, name, shape, feature in evex_forms():
        for bits, kind in ((128, 'xmm'), (256, 'ymm'), (512, 'zmm')):
            features = (feature, 'avx512vl') if bits < 512 else (feature,)
            for masked, zero, memory in ((0, 0, False), (1, 0, False), (7, 1, True)):
                dst, lhs, rhs = (kind + str(i) for i in ((3, 4, 5) if masked == 1 else (31, 30, 29)))
                source = f'mem<vec<{bits // 32},u32>>[r13+r10*8-129]' if memory else rhs
                asm_source = f'{kind}word ptr [r13+r10*8-129]' if memory else rhs
                extra = f',mask:k{masked},zero:{zero}' if masked else ''
                suffix = f' {{k{masked}}}' + (' {z}' if zero else '') if masked else ''
                if shape == 'BINARY':
                    body = f'{name} {{dst:{dst},lhs:{lhs},rhs:{source}{extra}}};'
                    asm = f'{name} {dst}{suffix}, {lhs}, {asm_source}'
                else:
                    body = f'{name} {{dst:{dst},src:{source}{extra}}};'
                    asm = f'{name} {dst}{suffix}, {asm_source}'
                yield key, bits, body, asm, features
            if shape == 'MOVE':
                yield key, bits, f'{name} {{dst:mem<vec<{bits // 32},u32>>[r12+64],src:{kind}31,mask:k2}};', \
                    f'{name} {kind}word ptr [r12+64] {{k2}}, {kind}31', features
    for bits, kind in ((128, 'xmm'), (256, 'ymm'), (512, 'zmm')):
        # Isolate every extension bit instead of testing only all-low/all-high
        # registers, where swapped R/R'/B/X/V' fields could accidentally agree.
        for operand in range(3):
            for register in (0, 1, 7, 8, 15, 16, 23, 24, 31):
                indices = [0, 1, 2]; indices[operand] = register
                dst, lhs, rhs = (kind + str(n) for n in indices)
                yield 'evex_register_bits', bits, f'vpaddd {{dst:{dst},lhs:{lhs},rhs:{rhs},mask:k4}};', \
                    f'vpaddd {dst} {{k4}}, {lhs}, {rhs}', ('avx512f', 'avx512vl')
        for offset in (-2147483648, -129 * bits // 8, -128 * bits // 8, -1, 0, 1, 127 * bits // 8, 128 * bits // 8, 2147483647):
            address = 'r13' + (('+' if offset >= 0 else '') + str(offset) if offset else '')
            yield 'evex_disp8_boundaries', bits, f'vpaddd {{dst:{kind}31,lhs:{kind}30,rhs:mem<vec<{bits // 32},u32>>[{address}]}};', \
                f'vpaddd {kind}31, {kind}30, {kind}word ptr [{address}]', ('avx512f', 'avx512vl')
        for base in ('rax', 'r8', 'r13'):
            for index in ('rcx', 'r10', 'r12'):
                yield 'evex_sib_bits', bits, f'vpaddd {{dst:{kind}16,lhs:{kind}8,rhs:mem<vec<{bits // 32},u32>>[{base}+{index}*8+64],mask:k4}};', \
                    f'vpaddd {kind}16 {{k4}}, {kind}8, {kind}word ptr [{base}+{index}*8+64]', ('avx512f', 'avx512vl')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True); p.add_argument('--dumper', required=True)
    p.add_argument('--clang', required=True); p.add_argument('--out'); p.add_argument('--proof')
    args = p.parse_args(); records = []; rejected = 0; binaries = 0
    if args.proof:
        inventory = [line.split('\t') for line in run([args.proof, '--inventory']).stdout.splitlines()[1:]]
        expected = Counter(f[1] for f in forms()) + Counter(f[1] for f in evex_forms())
        assert Counter(row[2] for row in inventory if row[1] == 'x86_64') == expected
    with tempfile.TemporaryDirectory(prefix='aster-manual-vector-') as temporary:
        root = Path(temporary); source = root / 'case.aa'; raw = root / 'case.bin'
        # Batch independent single-instruction cases by their minimum feature
        # requirements. This preserves every byte/shape check while avoiding
        # thousands of redundant compiler/LLVM process startups.
        groups = {}
        for case in (*cases(), *evex_cases()):
            feature = case[4]; feature = (feature,) if isinstance(feature, str) else feature
            groups.setdefault(feature, []).append(case)
        for features, group in groups.items():
            for start in range(0, len(group), 64):
                chunk = group[start:start + 64]
                source.write_text('module vector.forms; export fn run() @naked { unsafe exact x86_64 { ' +
                                  ' '.join(c[2] for c in chunk) + ' ret; } }')
                expected = assemble(args.clang, 'x86_64', '\n'.join(c[3] for c in chunk) + '\nret', root)
                for level in ('0', '2', 'z'):
                    argv = [args.compiler, 'build', '--target', 'x86_64', '-O' + level, '-j', '1', '-o', raw, source]
                    for feature in features: argv += ['--feature', feature]
                    run(argv); binaries += 1
                    assert raw.read_bytes() == expected, (features, start, level, raw.read_bytes().hex(), expected.hex())
                payload = json.loads(run([args.dumper, '--target', 'x86_64', '--strict', '--format', 'json', raw]).stdout)
                assert payload['unknown'] == 0 and len(payload['instructions']) == len(chunk) + 1, payload
                rebuilt = assemble(args.clang, 'x86_64', '\n'.join(i['mnemonic'] for i in payload['instructions']), root)
                assert rebuilt == expected, (features, start, payload)
                for case, instruction in zip(chunk, payload['instructions']):
                    key, bits, body, asm, feature = case
                    records.append({'form': key, 'bits': bits, 'source': body, 'assembly': asm, 'feature': feature,
                                    'bytes': instruction['bytes']})
                print(f'PASS: {len(records)} LLVM/source/dump SIMD cases', flush=True)
        # Every existing source mode and both ABI profiles use the same
        # one-instruction lowering. Low registers avoid manual ABI saves here.
        body = ' '.join((f'{name} {{dst:xmm0,lhs:xmm1,rhs:xmm2}};' if vex and shape == 'binary' else
                         f'{name} {{dst:xmm0,src:xmm2}};') for _, name, shape, vex, _, _ in forms())
        asm = '\n'.join((f'{name} xmm0, xmm1, xmm2' if vex and shape == 'binary' else
                         f'{name} xmm0, xmm2') for _, name, shape, vex, _, _ in forms())
        expected = assemble(args.clang, 'x86_64', asm + '\nret', root)
        modes = 0
        for mode in ('native', 'exact', 'unsafe exact'):
            for abi in ('sysv', 'win64'):
                for level in ('0', '2', 'z'):
                    source.write_text(f'module vector.modes; export fn run() @naked {{ {mode} x86_64 {{ {body} ret; }} }}')
                    run([args.compiler, 'build', '--target', 'x86_64', '--abi', abi, '--feature', 'avx2', '--feature', 'sse41',
                         '-O' + level, '-j', '1', '-o', raw, source])
                    assert raw.read_bytes() == expected, (mode, abi, level); modes += 1
        body = ' '.join((f'{name} {{dst:zmm0,lhs:zmm1,rhs:zmm2,mask:k1}};' if shape == 'BINARY' else
                         f'{name} {{dst:zmm0,src:zmm2,mask:k1}};') for _, name, shape, _ in evex_forms())
        asm = '\n'.join((f'{name} zmm0 {{k1}}, zmm1, zmm2' if shape == 'BINARY' else
                         f'{name} zmm0 {{k1}}, zmm2') for _, name, shape, _ in evex_forms())
        expected = assemble(args.clang, 'x86_64', asm + '\nret', root)
        for mode in ('native', 'exact', 'unsafe exact'):
            for abi in ('sysv', 'win64'):
                for level in ('0', '2', 'z'):
                    source.write_text(f'module vector.evex_modes; export fn run() @naked {{ {mode} x86_64 {{ {body} ret; }} }}')
                    run([args.compiler, 'build', '--target', 'x86_64', '--abi', abi, '--feature', 'avx512bw', '--feature', 'avx512dq',
                         '-O' + level, '-j', '1', '-o', raw, source])
                    assert raw.read_bytes() == expected, (mode, abi, level); modes += 1
        negatives = [
            ('paddd {dst:xmm0,src:xmm1,rhs:xmm2};', ()),
            ('paddd {dst:ymm0,src:ymm1};', ('avx2',)),
            ('vpaddd {dst:ymm0,lhs:ymm1,rhs:ymm2};', ('avx',)),
            ('vaddps {dst:xmm0,lhs:xmm1,rhs:xmm2};', ()),
            ('pmulld {dst:xmm0,src:xmm1};', ()),
            ('vpaddd {dst:ymm0,lhs:xmm1,rhs:ymm2};', ('avx2',)),
            ('vpaddd {dst:ymm0,lhs:ymm1,rhs:mem<vec<4,u32>>[rax]};', ('avx2',)),
            ('movdqu {dst:mem<vec<4,u32>>[rax],src:mem<vec<4,u32>>[rdx]};', ()),
            ('paddd {dst:xmm16,src:xmm0};', ('avx512f',)),
            ('vpaddd {dst:ymm0,lhs:ymm1,rhs:mem<vec<8,u32>>[rax+rsp*2]};', ('avx2',)),
            ('vpaddd {dst:ymm0,lhs:ymm1,rhs:mem<vec<8,u32>>[rax+2147483648]};', ('avx2',)),
            ('paddd {dst:rax,src:rbx};', ()),
            ('paddd {dst:xmm0,dst:xmm1};', ()),
            ('vpaddd {dst:zmm0,lhs:zmm1,rhs:zmm2,mask:k0,zero:1};', ('avx512f',)),
            ('vpaddd {dst:xmm0,lhs:xmm1,rhs:xmm2,mask:k1};', ('avx512f',)),
            ('vpaddb {dst:zmm0,lhs:zmm1,rhs:zmm2};', ('avx512f',)),
            ('vpmullq {dst:zmm0,lhs:zmm1,rhs:zmm2};', ('avx512f',)),
            ('vmovdqu32 {dst:mem<vec<16,u32>>[rax],src:zmm0,mask:k1,zero:1};', ('avx512f',)),
            ('vpaddd {dst:zmm0,lhs:zmm1,rhs:zmm2,mask:rax};', ('avx512f',)),
        ]
        for body, features in negatives:
            source.write_text(f'module vector.invalid; export fn run() @naked {{ exact x86_64 {{ {body} ret; }} }}')
            command = [args.compiler, 'build', '--target', 'x86_64', '-j', '1', '-o', raw, source]
            for feature in features: command += ['--feature', feature]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            assert result.returncode != 0, (body, features); rejected += 1
        # Reserved VEX.vvvv and W/non-canonical prefixes cannot be silently
        # decoded as a different instruction or read past a truncated buffer.
        for data in (b'\xc5\xe8\x6f\xc0', b'\xc4\xe1\xf9\x6f\xc0', b'\xc5', b'\xc4\xe1', b'\xf3\x0f\x6f'):
            raw.write_bytes(data)
            result = subprocess.run([args.dumper, '--target', 'x86_64', '--strict', '--format', 'json', raw],
                                    capture_output=True, text=True, timeout=15)
            assert result.returncode != 0, (data.hex(), result.stdout); rejected += 1
    report = {'schemas': len(forms()) + len(evex_forms()), 'oracle_cases': len(records), 'binaries': binaries,
              'instruction_encodings': len(records) * 3,
              'source_modes': modes, 'rejections': rejected, 'records': records}
    if args.out: Path(args.out).write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {report["schemas"]} SIMD schemas, {len(records)} LLVM/dump cases, {binaries} batched binaries, {modes} mode/ABI builds, {rejected} rejections')


if __name__ == '__main__': main()
