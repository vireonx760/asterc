#!/usr/bin/env python3
"""End-to-end differential smoke tests for the independent LLVM-free decoder.

Checks architecture-specific stack ops, preserved-register transfers, strict
unknown handling, alignment errors, JSON schema, and nonzero signed offsets.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

SAMPLES = {
    'aarch64': (
        'sub {dst:sp,lhs:sp,rhs:32}; str {src:x19,offset:0}; '
        'add {dst:x19,lhs:x0,rhs:x1}; ldr {dst:x19,offset:0}; '
        'str {src:w0,offset:8}; ldr {dst:w1,offset:8}; '
        'add {dst:sp,lhs:sp,rhs:32}; ret;',
        ('sub sp, sp, #32', 'str x19, [sp, #0]', 'add x19, x0, x1',
         'ldr x19, [sp, #0]', 'str w0, [sp, #8]', 'ldr w1, [sp, #8]',
         'add sp, sp, #32', 'ret'),
    ),
    'riscv64': (
        'addi {dst:sp,lhs:sp,rhs:-32}; sd {src:s0,offset:0}; '
        'xor {dst:s0,lhs:a0,rhs:a1}; ld {dst:s0,offset:0}; '
        'sd {src:a0,offset:8}; ld {dst:a1,offset:8}; '
        'addi {dst:sp,lhs:sp,rhs:32}; ret;',
        ('addi sp, sp, -32', 'sd s0, 0(sp)', 'xor s0, a0, a1',
         'ld s0, 0(sp)', 'sd a0, 8(sp)', 'ld a1, 8(sp)',
         'addi sp, sp, 32', 'ret'),
    ),
}


def run(*args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=40)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--dumper', required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='asterdump-') as tmp:
        path = pathlib.Path(tmp)
        count = 0
        for arch, (body, expected) in SAMPLES.items():
            src = path / 'raw.aa'
            binary = path / 'raw.bin'
            src.write_text('module dump.example;\nexport fn raw(a:u64,b:u64)->u64 @naked {\n'
                           f'unsafe exact {arch} {{ {body} }}\n}}\n', encoding='utf-8')
            result = run(args.compiler, 'build', '--target', arch, '--emit', 'bin',
                         '-o', str(binary), str(src))
            assert result.returncode == 0, result.stderr
            result = run(args.dumper, '--target', arch, '--format', 'json', '--strict',
                         '--base', '0x4000', str(binary))
            assert result.returncode == 0, result.stderr
            dump = json.loads(result.stdout)
            assert dump['recognized'] == len(expected) and dump['unknown'] == 0, dump
            assert [item['mnemonic'] for item in dump['instructions']] == list(expected), dump
            assert [item['offset'] for item in dump['instructions']] == [0x4000 + 4*i for i in range(len(expected))]
            text_result = run(args.dumper, '--target', arch, '--strict', str(binary))
            assert text_result.returncode == 0 and all(item in text_result.stdout for item in expected)
            count += 4
        unknown = path / 'unknown.bin'
        unknown.write_bytes(b'\xff\xff\xff\xff')
        for arch in SAMPLES:
            result = run(args.dumper, '--target', arch, '--format', 'json', str(unknown))
            assert result.returncode == 0 and json.loads(result.stdout)['unknown'] == 1
            result = run(args.dumper, '--target', arch, '--strict', str(unknown))
            assert result.returncode == 3 and '.word 0xffffffff' in result.stdout
            count += 2
        unknown.write_bytes(b'\x13\x00\x00')
        assert run(args.dumper, '--target', 'riscv64', str(unknown)).returncode == 2
        assert run(args.dumper, '--target', 'x86_64', str(unknown)).returncode == 0
        assert run(args.dumper, '--target', 'aarch64', '--base', '0xffffffffffffffff', str(unknown)).returncode == 2
        count += 3
    print(f'asterdump: {count} CLI/integration assertions passed')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        sys.stderr.write(str(exc) + '\n')
        sys.exit(1)
