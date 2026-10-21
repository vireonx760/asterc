#!/usr/bin/env python3
"""QEMU executes manual NEON lanes and RVV across SEW, AVL and physical VLEN."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile

from native_processor_qemu import command, other_kernel


def values(bits, count):
    mask = (1 << bits) - 1
    left = [((mask - 1) + i * 17) & mask for i in range(count)]
    right = [(i + 3) & mask for i in range(count)]
    return left, right


def packed(values, bits): return b''.join(v.to_bytes(bits // 8, 'little') for v in values)
def blob(name, data): return '.balign 16\n' + name + ': .byte ' + ','.join(str(v) for v in data) + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True); p.add_argument('--clang', required=True); p.add_argument('--out')
    args = p.parse_args(); records = []
    with tempfile.TemporaryDirectory(prefix='aster-vector-other-cpu-') as temporary:
        root = Path(temporary); source = root / 'case.aa'; raw = root / 'case.bin'
        for bits in (8, 16, 32, 64):
            left, right = values(bits, 128 // bits); mask = (1 << bits) - 1
            expected = packed([(a + b) & mask for a, b in zip(left, right)], bits)
            body = f'ldr {{dst:q0,addr:x0}}; ldr {{dst:q1,addr:x1}}; add {{dst:v0,lhs:v0,rhs:v1,bits:{bits}}}; str {{src:q0,addr:x0}};'
            oracle = '''mov x9,#0x300000
msr cpacr_el1,x9
isb
mov x19,#0x123
movi v8.16b,#0x5a
adr x0,left
adr x1,right
bl aster_entry
cmp x19,#0x123
b.ne failed
fmov x9,d8
mov x10,#0x5a5a
movk x10,#0x5a5a,lsl #16
movk x10,#0x5a5a,lsl #32
movk x10,#0x5a5a,lsl #48
cmp x9,x10
b.ne failed
adr x11,left
ldp x9,x10,[x11]
adr x11,expected
ldp x12,x13,[x11]
cmp x9,x12
b.ne failed
cmp x10,x13
b.ne failed
b checked
''' + blob('left', packed(left, bits)) + blob('right', packed(right, bits)) + blob('expected', expected) + 'checked:\n'
            for mode in ('native', 'exact', 'unsafe exact'):
                source.write_text(f'module vector.neon_cpu; export fn run() @naked {{ {mode} aarch64 {{ {body} ret; }} }}')
                for level in ('0', '2', 'z'):
                    command([args.compiler, 'build', '--target', 'aarch64', '-O' + level, '-j', '1', '-o', raw, source])
                    elf, _ = other_kernel('aarch64', oracle, raw, root, args.clang)
                    records.append({'target': 'aarch64', 'bits': bits, 'mode': mode, 'level': level,
                                    'code_sha256': hashlib.sha256(raw.read_bytes()).hexdigest(), 'kernel_sha256': hashlib.sha256(elf.read_bytes()).hexdigest()})
            print('PASS: NEON', bits, flush=True)
        for bits in (8, 16, 32, 64):
            vtype = ((bits.bit_length() - 4) << 3) | 192
            body = f'vsetvli {{dst:a3,avl:a2,vtype:{vtype}}}; vle{bits}.v {{dst:v1,addr:a0}}; vle{bits}.v {{dst:v2,addr:a1}}; ' + \
                   f'vadd.vv {{dst:v3,lhs:v1,rhs:v2}}; vse{bits}.v {{src:v3,addr:a0}};'
            for vlen in (128, 256, 512):
                count = vlen // bits; left, right = values(bits, count); mask = (1 << bits) - 1
                oracle = f'li t0,0x6600\ncsrs mstatus,t0\ncsrr t0,vlenb\nli t1,{vlen // 8}\nbne t0,t1,failed\n'
                data = ''
                for index, avl in enumerate((0, max(1, count - 1), count + 5)):
                    vl = min(avl, count)
                    expected = packed([((a + b) & mask) if i < vl else a for i, (a, b) in enumerate(zip(left, right))], bits)
                    data += blob('left' + str(index), packed(left, bits)) + blob('expected' + str(index), expected)
                    oracle += f'la a0,left{index}\nla a1,right\nli a2,{avl}\ncall aster_entry\nli t0,{vl}\nbne a3,t0,failed\n'
                    for offset in range(0, vlen // 8, 8):
                        oracle += f'la t0,left{index}\nld t1,{offset}(t0)\nla t0,expected{index}\nld t2,{offset}(t0)\nbne t1,t2,failed\n'
                oracle += 'j checked\n' + data + blob('right', packed(right, bits)) + 'checked:\n'
                for mode in ('native', 'exact', 'unsafe exact'):
                    source.write_text(f'module vector.rvv_cpu; export fn run() @naked {{ {mode} riscv64 {{ {body} ret; }} }}')
                    for level in ('0', '2', 'z'):
                        command([args.compiler, 'build', '--target', 'riscv64', '--feature', 'v', '-O' + level, '-j', '1', '-o', raw, source])
                        elf, _ = other_kernel('riscv64', oracle, raw, root, args.clang, rvv_bits=vlen)
                        records.append({'target': 'riscv64', 'bits': bits, 'vlen': vlen, 'avl': [0, max(1, count - 1), count + 5],
                                        'mode': mode, 'level': level, 'code_sha256': hashlib.sha256(raw.read_bytes()).hexdigest(),
                                        'kernel_sha256': hashlib.sha256(elf.read_bytes()).hexdigest()})
                print('PASS: RVV', bits, 'VLEN', vlen, flush=True)
    if args.out: Path(args.out).write_text(json.dumps({'qemu_runs': len(records), 'records': records}, indent=2) + '\n')
    print(f'PASS: {len(records)} NEON/RVV QEMU runs; element carries, inactive memory, returned VL and physical VLEN')


if __name__ == '__main__': main()
