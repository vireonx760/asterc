#!/usr/bin/env python3
"""Public CLI dump contracts for HIR/MIR/target/ABI/machine/physical stages."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    args = ap.parse_args()
    source = 'module dumps.smoke; export fn f(x:u64)->u64 @abi(c) { return x + 1; }\n'
    with tempfile.TemporaryDirectory(prefix='aster-dump-') as tmp:
        root = Path(tmp)
        source_file = root / 'source.aa'
        source_file.write_text(source, encoding='utf-8')
        expectations = {'--dump-hir': 'hir', '--dump-mir': 'mir',
                        '--dump-target': 'target', '--dump-abi': 'abi',
                        '--dump-machine': 'machine stage=',
                        '--dump-regalloc': 'regalloc functions='}
        # Each backend must expose the same well-defined inspection stages.
        for arch in ('x86_64', 'aarch64', 'riscv64'):
            for option, marker in expectations.items():
                dest = root / f'{arch}.bin'
                command = [args.compiler, 'build', '--target', arch, option,
                           '-o', str(dest), str(source_file)]
                proc = subprocess.run(command, capture_output=True, text=True, timeout=30)
                if proc.returncode or marker not in proc.stdout.lower() or not dest.is_file():
                    raise AssertionError((command, proc.stdout[:1000], proc.stderr[:1000]))
        # Legacy backend deliberately bypasses machine IR; don't claim its
        # native-code representation can be dumped from that nonexistent stage.
        legacy = subprocess.run([args.compiler, 'build', '--target', 'x86_16',
                                 '--dump-machine', '-o', str(root / 'legacy.bin'), str(source_file)],
                                capture_output=True, text=True, timeout=30)
        if legacy.returncode == 0 or 'unavailable' not in legacy.stderr:
            raise AssertionError((legacy.stdout, legacy.stderr))
    print('dump CLI: 18 target/stage combinations and legacy error path passed')


if __name__ == '__main__':
    main()
