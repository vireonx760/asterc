#!/usr/bin/env python3
"""Policy/report environment reads must preserve semantics on UCRT and POSIX."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    args = parser.parse_args()
    count = 0
    with tempfile.TemporaryDirectory(prefix='aster-policy-env-') as temporary:
        root = Path(temporary)
        source, binary = root / 'case.aa', root / 'case.bin'
        source.write_text('module test.policy; export fn f(x:u64)->u64 @abi(c) {return x/7;}\n')
        for target in ('x86_64', 'aarch64', 'riscv64'):
            binaries = {}
            for policy in (None, '', 'hot', 'size', 'SIZE', 'size-extra', 'size' + 'x' * 4096):
                for report in (None, '1', '0', '1' + 'x' * 4096):
                    env = os.environ.copy()
                    env.pop('ASTERASM_DIV_POLICY', None)
                    env.pop('ASTERASM_DIV_REPORT', None)
                    if policy is not None:
                        env['ASTERASM_DIV_POLICY'] = policy
                    if report is not None:
                        env['ASTERASM_DIV_REPORT'] = report
                    command = [args.compiler, 'build', '--target', target, '-j', '1', '-o', str(binary), str(source)]
                    if target == 'riscv64':
                        command += ['--feature', 'm']
                    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
                    if result.returncode:
                        raise AssertionError((target, policy, report, result.stderr))
                    data = binary.read_bytes()
                    key = 'size' if policy == 'size' else 'hot'
                    if key in binaries and data != binaries[key]:
                        raise AssertionError(('environment changes code unexpectedly', target, policy, report))
                    binaries[key] = data
                    if ('[aster-div]' in result.stderr) != (report == '1'):
                        raise AssertionError(('report must match exactly 1', target, policy, report, result.stderr))
                    count += 1
            if binaries['size'] == binaries['hot']:
                raise AssertionError(('test must exercise different selections', target))
    print(f'PASS: {count} exact policy/report environment combinations')


if __name__ == '__main__':
    main()
