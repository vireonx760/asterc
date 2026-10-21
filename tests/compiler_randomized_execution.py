#!/usr/bin/env python3
"""Seeded expression/PHI/loop differential execution across optimization policies."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
import tempfile

from compiler_division_unicorn import execute

MASK = (1 << 64)-1


def expression(rng, depth):
    if not depth:
        name = rng.choice(('x', 'y', str(rng.randrange(1, 65536))))
        return name, lambda x, y: x if name == 'x' else y if name == 'y' else int(name)
    op = rng.choice(('+', '-', '*', '^', '&', '|', '>>', '<<', '/', '%'))
    left, left_fn = expression(rng, depth-1)
    if op in ('>>', '<<', '/', '%'):
        value = rng.randrange(1, 63) if op in ('>>', '<<') else rng.choice((3, 7, 31, 255, 65537))
        right, right_fn = str(value), lambda x, y: value
    else:
        right, right_fn = expression(rng, depth-1)

    def oracle(x, y):
        a, b = left_fn(x, y), right_fn(x, y)
        if op == '+': result = a+b
        elif op == '-': result = a-b
        elif op == '*': result = a*b
        elif op == '^': result = a^b
        elif op == '&': result = a&b
        elif op == '|': result = a|b
        elif op == '>>': result = a >> b
        elif op == '<<': result = a << b
        elif op == '/': result = a//b
        else: result = a % b
        return result & MASK
    return '('+left+op+right+')', oracle


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True)
    p.add_argument('--cases', type=int, default=64)
    p.add_argument('--out', type=Path)
    args = p.parse_args()
    rng = random.Random(0x517A37)
    records = []
    with tempfile.TemporaryDirectory(prefix='aster-randomized-') as temporary:
        root = Path(temporary)
        source, binary = root/'unit.aa', root/'unit.bin'
        for case in range(args.cases):
            text, reference = expression(rng, 3)
            multiplier, shift = rng.randrange(1, 32)*2+1, rng.randrange(1, 64)
            body = 'let v:u64='+text+';if (x&1)!=0 {v=v^y;}else{v=v+x;}'
            body += f'let i:u64=0;while i<3 {{v=(v*{multiplier})^(y>>{shift});i=i+1;}}return v;'
            program = 'module randomized;export fn run(x:u64,y:u64)->u64 @abi(c){'+body+'}'
            source.write_text(program, encoding='utf-8')
            inputs = [(0, 0), (MASK, 1), (1 << 63, MASK), (1, 1 << 63)]
            inputs += [(rng.getrandbits(64), rng.getrandbits(64)) for _ in range(12)]
            expected = []
            for x, y in inputs:
                value = reference(x, y)
                value = (value ^ y) if x & 1 else (value+x) & MASK
                for _ in range(3):
                    value = ((value*multiplier) ^ (y >> shift)) & MASK
                expected.append(value)
            for profile, target, abi in ((0, 'x86_64', 'sysv'), (1, 'x86_64', 'win64'),
                                          (2, 'aarch64', 'aapcs64'), (3, 'riscv64', 'lp64')):
                sizes = {}
                for level in ('0', '2', 'z'):
                    argv = [args.compiler, 'build', '--target', target, '--abi', abi, '-O'+level,
                            '--no-red-zone', '-j', '1', '-o', str(binary), str(source)]
                    if target == 'riscv64': argv += ['--feature', 'm']
                    compiled = subprocess.run(argv, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
                    if compiled.returncode:
                        raise AssertionError((case, program, argv, compiled.stderr))
                    code = binary.read_bytes()
                    for (x, y), wanted in zip(inputs, expected):
                        result, _ = execute(code, profile, x, y, 64, False)
                        if result != wanted:
                            if args.out:
                                args.out.parent.mkdir(parents=True, exist_ok=True)
                                args.out.with_suffix('.failure.aa').write_text(program, encoding='utf-8')
                            raise AssertionError((case, program, profile, level, x, y, result, wanted))
                    sizes[level] = len(code)
                    records.append(dict(case=case, profile=profile, level=level, bytes=len(code),
                                        source_sha256=hashlib.sha256(program.encode()).hexdigest(),
                                        binary_sha256=hashlib.sha256(code).hexdigest(), executions=len(inputs)))
                assert sizes['z'] <= sizes['2'], (case, profile, sizes)
            print('PASS: randomized case', case, flush=True)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(dict(seed=0x517A37, records=records), indent=2)+'\n', encoding='utf-8')
    print('PASS:', len(records), 'binaries;', sum(r['executions'] for r in records), 'independent CPU/oracle comparisons')


if __name__ == '__main__':
    main()
