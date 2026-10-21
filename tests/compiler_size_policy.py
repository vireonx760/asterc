#!/usr/bin/env python3
"""Size mode compares real artifacts across policies, formats and explicit attributes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from compiler_mir_inline import corpus
from compiler_division_unicorn import PROFILES


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True)
    p.add_argument('--object-writer', action='store_true')
    p.add_argument('--out')
    args = p.parse_args(); records = []
    selected = corpus()[:9]
    selected += [('attributes', 'module density.policy; export fn run(x:u64,y:u64)->u64 @abi(c) {'
                  'return helper(x)+kept(y); } fn helper(x:u64)->u64 @inline { return (x^7)+3; } '
                  'fn kept(x:u64)->u64 @noinline { return x+5; }', None, 64, False)]
    with tempfile.TemporaryDirectory(prefix='aster-size-policy-') as temporary:
        root = Path(temporary); source = root / 'unit.aa'; output = root / 'unit.obj'
        for profile, (arch, abi, with_m) in enumerate(PROFILES):
            formats = ['bin']
            if args.object_writer or arch in ('aarch64', 'riscv64'): formats += ['elf']
            if args.object_writer and arch != 'riscv64': formats += ['coff', 'macho']
            for name, text, _, _, _ in selected:
                # This focused object test is single-module. Cross-module CPU
                # cases retain their full harness in compiler_mir_inline.py.
                if 'import ' in text: continue
                source.write_text(text)
                for fmt in formats:
                    for disabled in (False, True):
                        variants = []
                        for level in ('2', 'z', 's'):
                            argv = [args.compiler, 'build', '--target', arch, '--abi', abi, '-O' + level,
                                    '--no-red-zone', '--stats', '--dump-mir', '-j', '1', '-o', str(output), str(source)]
                            if with_m: argv += ['--feature', 'm']
                            if disabled: argv += ['--no-inline']
                            if fmt != 'bin': argv += ['--emit', 'obj', '--format', fmt]
                            result = subprocess.run(argv, capture_output=True, text=True, timeout=45)
                            assert result.returncode == 0, (name, profile, fmt, disabled, level, result.stderr)
                            data = output.read_bytes()
                            if level != '2':
                                match = re.search(r'size policy: size-bytes=(\d+) speed-bytes=(\d+) selected=(size|balanced)', result.stderr)
                                assert match and len(data) == min(int(match[1]), int(match[2])), (name, fmt, result.stderr, len(data))
                            if name == 'attributes' and not disabled:
                                assert re.search(r'forced=[1-9][0-9]* selected=explicit-policy', result.stderr)
                            variants.append({'level': level, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
                        assert variants[1]['bytes'] <= variants[0]['bytes'], (name, profile, fmt, disabled, variants)
                        assert variants[1] | {'level': 's'} == variants[2], ('Os/Oz aliases diverged', variants)
                        records.append({'name': name, 'profile': profile, 'format': fmt, 'disabled': disabled, 'variants': variants})
    if args.out: Path(args.out).write_text(json.dumps({'records': records}, indent=2) + '\n')
    print(f'PASS: {len(records) * 3} actual binary/object size-policy artifacts; Oz <= O2 and Os == Oz')


if __name__ == '__main__': main()
