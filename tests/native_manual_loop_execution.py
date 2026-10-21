#!/usr/bin/env python3
"""Execute true signed-backedge code using an ISA decoder independent of asterc.

The step-limited interpreter catches encodings that a structural CFG proof
alone could miss. This is not a hardware or QEMU qualification test.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
from native_manual_cfg_semantics import execute


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--compiler', required=True)
    p.add_argument('--target', choices=('aarch64', 'riscv64'), required=True)
    p.add_argument('--dumper')
    a = p.parse_args()
    if a.target == 'aarch64':
        cases = [
            ('label {name:start}; eor {dst:x0,lhs:x0,rhs:x1}; '
             'cbnz {reg:x0,target:start}; ret;', [(0,1,0), (1,1,0), (0,2,0), (2,2,0)]),
            ('label {name:start}; cbz {reg:x0,target:start}; ret;', [(1,3,1), (2,4,2)]),
        ]
    else:
        cases = [
            ('label {name:start}; xor {dst:a0,lhs:a0,rhs:a1}; '
             'beq {lhs:a0,rhs:a1,target:start}; ret;', [(0,1,0), (1,1,0), (0,2,0), (2,2,0)]),
            ('label {name:start}; beq {lhs:a0,rhs:a1,target:start}; ret;', [(1,3,1), (2,4,2)]),
        ]
    count = 0
    with tempfile.TemporaryDirectory(prefix='aster-native-loop-runtime-') as tmp:
        for n, (body, samples) in enumerate(cases):
            path = Path(tmp)/f'{n}.aa'
            out = Path(tmp)/f'{n}.bin'
            path.write_text('module test.loop_runtime;\nexport fn raw(a:u64,b:u64)->u64 @naked {\n'
                            f'unsafe exact {a.target} {{ {body} }}\n}}\n', encoding='utf-8')
            result = subprocess.run([a.compiler, 'build', '--target', a.target, '--emit', 'bin',
                                     '-j', '1', '-o', str(out), str(path)],
                                    capture_output=True, text=True, timeout=30)
            if result.returncode or not out.is_file():
                raise AssertionError(result.stderr)
            data = out.read_bytes()
            if a.dumper:
                dump = subprocess.run([a.dumper, '--target', a.target, '--format', 'text',
                                       '--strict', str(out)], capture_output=True, text=True)
                if dump.returncode or '.word' in dump.stdout or '.byte' in dump.stdout:
                    raise AssertionError(f'disassembler rejected loop: {dump.stdout} {dump.stderr}')
            for x, y, expected in samples:
                actual = execute(a.target, data, x, y)
                if actual != expected:
                    raise AssertionError(f'{a.target} ({x},{y}) -> {actual}, expected {expected}')
                count += 1
    print(f'{a.target}: {count} independently interpreted loop executions pass')


if __name__ == '__main__':
    main()
