"""Compile the short-circuit logical-binary fixture across constrained x86-64 homes.

This regression specifically exercises conditional-branch edge copies where a
live-through value may be rematerialized on both sides of an edge.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--compiler', required=True)
p.add_argument('--case', type=int, required=True)
args = p.parse_args()

abis = ('sysv', 'win64')
limits = (0, 2)
promotions = (True, False)
cases = [(abi, limit, promote) for abi in abis for limit in limits for promote in promotions]
if args.case < 0 or args.case >= len(cases):
    raise SystemExit(f'invalid case {args.case}; expected 0..{len(cases)-1}')

abi, limit, promote = cases[args.case]
source = Path(__file__).parent / 'compiler_e2e' / 'logical_binary.aa'
with tempfile.TemporaryDirectory(prefix='aster-logical-binary-') as tmp:
    output = Path(tmp) / 'image.bin'
    cmd = [args.compiler, 'build', '--target', 'x86_64', '--abi', abi,
           '--emit', 'bin', '--no-red-zone', '--reg-limit', str(limit),
           '-j', '1', '-o', str(output), str(source)]
    if not promote:
        cmd.append('--no-promote')
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if proc.returncode != 0:
        raise SystemExit(
            f'logical_binary compile failed: abi={abi} reg_limit={limit} '
            f'promote={int(promote)}\n{proc.stderr}')
    if not output.is_file() or output.stat().st_size == 0:
        raise SystemExit('logical_binary compile produced no output image')
