#!/usr/bin/env python3
"""Legacy x86 density, complete semantic shape corpus and cdecl CPU canaries."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from legacy_cases import source_case, TYPES, ARITH, CMP, SHIFT, DIV
import unicorn as uc
from unicorn import x86_const as x


def execute(data, bits, parameters, width):
    base, stack, stop = (0x1000, 0xf000, 0xf800) if bits == 16 else (0x100000, 0x480000, 0x1f0000)
    cpu = uc.Uc(uc.UC_ARCH_X86, uc.UC_MODE_16 if bits == 16 else uc.UC_MODE_32)
    cpu.mem_map(0, 0x500000); cpu.mem_write(base, data)
    cpu.mem_write(stack - 4, struct.pack('<I', stop) + parameters); cpu.reg_write(x.UC_X86_REG_ESP, stack - 4)
    preserved = [x.UC_X86_REG_EBX, x.UC_X86_REG_ESI, x.UC_X86_REG_EDI, x.UC_X86_REG_EBP]
    for n, reg in enumerate(preserved): cpu.reg_write(reg, 0xabc000 + n * 0x101)
    cpu.emu_start(base, stop, timeout=2000000, count=1000000)
    assert cpu.reg_read(x.UC_X86_REG_EIP) == stop and cpu.reg_read(x.UC_X86_REG_ESP) == stack
    for n, reg in enumerate(preserved): assert cpu.reg_read(reg) == 0xabc000 + n * 0x101
    result = cpu.reg_read(x.UC_X86_REG_EAX) | (cpu.reg_read(x.UC_X86_REG_EDX) << 32)
    return result & ((1 << width) - 1)


def oracle(index):
    if index >= 128: return None
    if index < 48: ty, op = TYPES[index // 6], ARITH[index % 6]
    elif index < 96: ty, op = TYPES[(index - 48) // 6], CMP[(index - 48) % 6]
    elif index < 112: ty, op = TYPES[(index - 96) // 2], SHIFT[(index - 96) % 2]
    else: ty, op = TYPES[(index - 112) // 2], DIV[(index - 112) % 2]
    bits = int(ty[1:]); mask = (1 << bits) - 1
    a = -17 if ty[0] == 'i' else mask - 17; b = 3 if op in SHIFT else 7
    quotient = (-(abs(a) // b) if a < 0 else a // b)
    expected = {'+': lambda: a + b, '-': lambda: a - b, '*': lambda: a * b,
                '&': lambda: a & b, '|': lambda: a | b, '^': lambda: a ^ b,
                '==': lambda: a == b, '!=': lambda: a != b, '<': lambda: a < b, '<=': lambda: a <= b,
                '>': lambda: a > b, '>=': lambda: a >= b, '<<': lambda: a << b, '>>': lambda: a >> b,
                '/': lambda: quotient, '%': lambda: a - quotient * b}[op]()
    result_bits = 8 if op in CMP else bits
    parameters = b''.join((v & mask).to_bytes(max(4, bits // 8), 'little') for v in (a, b))
    return parameters, result_bits, int(expected) & ((1 << result_bits) - 1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True); p.add_argument('--previous'); p.add_argument('--out')
    p.add_argument('--object-writer', action='store_true')
    args = p.parse_args(); records = []; executions = artifacts = 0
    with tempfile.TemporaryDirectory(prefix='aster-legacy-size-') as temporary:
        root = Path(temporary); source = root / 'case.aa'; raw = root / 'case.bin'
        for bits in (16, 32):
            for promote in (True, False):
                for semantic in range(139):
                    source.write_text('module legacy.size;\n' + source_case(semantic) + '\n'); variants = []
                    for level in ('0', '2', 'z'):
                        argv = ['build', '--target', f'x86_{bits}', '--abi', 'cdecl', '-O' + level,
                                '--base', '0x1000' if bits == 16 else '0x100000', '-j', '1', '-o', str(raw), str(source)]
                        if not promote: argv += ['--no-promote']
                        result = subprocess.run([args.compiler, *argv], capture_output=True, text=True, timeout=45)
                        assert result.returncode == 0, (bits, semantic, level, result.stderr)
                        data = raw.read_bytes(); artifacts += 1
                        expected = oracle(semantic)
                        if expected:
                            parameters, width, value = expected
                            assert execute(data, bits, parameters, width) == value, (bits, promote, semantic, level)
                            executions += 1
                        previous = None
                        if args.previous:
                            result = subprocess.run([args.previous, *argv], capture_output=True, text=True, timeout=45)
                            assert result.returncode == 0, result.stderr
                            previous = raw.stat().st_size
                            assert len(data) <= previous, (bits, promote, semantic, level, len(data), previous)
                        variants.append({'level': level, 'bytes': len(data), 'previous': previous})
                    assert variants[2]['bytes'] <= variants[1]['bytes'], (bits, promote, semantic, variants)
                    records.append({'bits': bits, 'promote': promote, 'shape': semantic, 'variants': variants})
                print('PASS: legacy', bits, 'promote', promote, flush=True)
        if args.object_writer:
            source.write_text('module legacy.object_size; section ".data" @writable {global value:u32=7;} '
                              'export fn run(x:u32)->u32 {return leaf(x)+value;} fn leaf(x:u32)->u32 {return x*17+9;}')
            for bits in (16, 32):
                for fmt in ('elf', 'coff', 'macho'):
                    sizes = []
                    for level in ('2', 'z', 's'):
                        argv = [args.compiler, 'build', '--target', f'x86_{bits}', '--abi', 'cdecl', '--emit', 'obj', '--format', fmt,
                                '-O' + level, '-j', '1', '-o', str(raw), str(source)]
                        result = subprocess.run(argv, capture_output=True, text=True, timeout=45)
                        assert result.returncode == 0, result.stderr
                        sizes.append(raw.read_bytes()); artifacts += 1
                    assert len(sizes[1]) <= len(sizes[0]) and sizes[1] == sizes[2], (bits, fmt)
    if args.out: Path(args.out).write_text(json.dumps({'artifacts': artifacts, 'guarded_cpu_calls': executions, 'records': records,
        'compiler_sha256': hashlib.sha256(Path(args.compiler).read_bytes()).hexdigest(),
        'previous_sha256': hashlib.sha256(Path(args.previous).read_bytes()).hexdigest() if args.previous else None}, indent=2) + '\n')
    print(f'PASS: {artifacts} legacy artifacts, {executions} cdecl/stack/callee-save CPU checks; Oz <= O2')


if __name__ == '__main__': main()
