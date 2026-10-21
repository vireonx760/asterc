"""Compile one deterministic x86_16/x86_32 MIR->legacy-backend regression case.

The 139 semantic source shapes are crossed with both code modes and both MIR
promotion settings by CTest (556 independently named regression gates).
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', required=True)
p.add_argument('--case', type=int, required=True)
args = p.parse_args()

from legacy_cases import source_case

if not 0 <= args.case < 556:
    raise SystemExit('case must be in [0, 555]')
semantic = args.case % 139
mode_slot = args.case // 139
bits = 16 if mode_slot < 2 else 32
promote = (mode_slot % 2) == 0
code = 'module legacy.matrix;\n' + source_case(semantic) + '\n'

with tempfile.TemporaryDirectory(prefix=f'aster-legacy-{bits}-{semantic}-') as tmp:
    tmp = Path(tmp)
    source = tmp/'case.aa'
    output = tmp/'case.bin'
    source.write_text(code, encoding='utf-8')
    command = [args.compiler, 'build', '--target', f'x86_{bits}', '--abi', 'cdecl',
               '--emit', 'bin', '--base', '0x1000' if bits == 16 else '0x100000',
               '-j', '1', '-o', str(output), str(source)]
    if not promote:
        command.append('--no-promote')
    proc = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if proc.returncode:
        raise SystemExit(f'{command}\n{proc.stdout}{proc.stderr}')
    data = output.read_bytes()
    if not data:
        raise SystemExit('compiler produced an empty image')
    # Every semantic case must be deterministic under the same options. Recheck
    # a quarter of the matrix to keep the suite fast while covering both modes
    # and promotion settings.
    if semantic % 4 == 0:
        second = tmp/'second.bin'
        command2 = [str(second) if part == str(output) else part for part in command]
        proc = subprocess.run(command2, capture_output=True, text=True, timeout=30)
        if proc.returncode or second.read_bytes() != data:
            raise SystemExit(f'non-deterministic legacy output\n{proc.stdout}{proc.stderr}')
