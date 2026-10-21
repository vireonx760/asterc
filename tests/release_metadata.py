#!/usr/bin/env python3
"""Check the public C/CMake/Cargo/CLI release version as one contract."""
import argparse
from pathlib import Path
import re
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True)
    p.add_argument('--dumper', required=True)
    args = p.parse_args()
    header = (ROOT/'include/asterasm/version.h').read_text(encoding='utf-8')
    parts = [int(re.search(r'#define ASTERASM_VERSION_'+name+r' (\d+)', header)[1])
             for name in ('MAJOR', 'MINOR', 'PATCH')]
    version = '.'.join(map(str, parts))
    assert '#define ASTERASM_VERSION_STRING "'+version+'"' in header
    assert re.search(r'\bVERSION\s+'+re.escape(version)+r'\b', (ROOT/'CMakeLists.txt').read_text())
    cargo = tomllib.loads((ROOT/'object-writer/Cargo.toml').read_text(encoding='utf-8'))
    lock = tomllib.loads((ROOT/'object-writer/Cargo.lock').read_text(encoding='utf-8'))
    assert cargo['package']['version'] == version
    assert next(p['version'] for p in lock['package'] if p['name']==cargo['package']['name']) == version
    for tool, name in ((args.compiler, 'asterc'), (args.dumper, 'asterdump')):
        result = subprocess.run([tool, '--version'], capture_output=True, text=True, check=True)
        assert result.stdout.strip() == name+' '+version, result.stdout
    print('PASS: C headers, CMake, Cargo/lock and both CLI versions agree:', version)


if __name__ == '__main__': main()
