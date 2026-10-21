#!/usr/bin/env python3
"""Deterministic byte mutation stress for the COFF/PE disassembler.

A mutated document may remain valid; that is fine. The safety property is
never to crash/hang or emit invalid JSON on successful parse.
"""
import argparse
import json
import random
import subprocess
from pathlib import Path
import tempfile

from asterdump_coff_pe_cli import stub_obj


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dumper', required=True)
    p = parser.parse_args()
    rng = random.Random(0xA517C0FF)
    checks = 0
    with tempfile.TemporaryDirectory(prefix='aster-coff-fuzz-') as d:
        f = Path(d)/'mutation.obj'
        for arch in ('x86_64', 'aarch64'):
            original = stub_obj(arch)
            for k in range(144):
                n = len(original)
                if k % 4 == 0:
                    blob = original[:rng.randrange(n + 1)]
                elif k % 4 == 1:
                    blob = bytearray(original)
                    for _ in range(rng.randrange(1, 6)):
                        blob[rng.randrange(n)] ^= rng.randrange(1, 256)
                elif k % 4 == 2:
                    blob = bytearray(original)
                    i = rng.randrange(n)
                    blob[i:] = bytes([rng.randrange(256) for _ in range(n-i)])
                else:
                    blob = original + bytes([rng.randrange(256) for _ in range(rng.randrange(1, 32))])
                f.write_bytes(blob)
                res = subprocess.run([p.dumper, '--target', arch, '--format', 'json', '--strict', str(f)],
                                     capture_output=True, text=True, errors='replace', timeout=8)
                assert res.returncode in (0, 2, 3), (arch, k, res.returncode, res.stderr)
                if res.returncode in (0, 3):
                    parsed = json.loads(res.stdout)
                    assert parsed.get('target') == arch and 'recognized' in parsed
                checks += 1
    print(f'COFF mutation: {checks} cases without crashes or invalid successful JSON')


if __name__ == '__main__':
    main()
