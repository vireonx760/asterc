#!/usr/bin/env python3
"""Slice 11: differential forward-CFG ABI-state validation on final machine code."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from native_manual_cross_isa import assemble


def generate(target):
    arm = target == 'aarch64'
    cond = 'cbz {reg:x0,target:alternative};' if arm else 'beq {lhs:a0,rhs:a1,target:alternative};'
    jump = 'b {target:joined};' if arm else 'jal {dst:zero,target:joined};'
    alloc16 = 'sub {dst:sp,lhs:sp,rhs:16};' if arm else 'addi {dst:sp,lhs:sp,rhs:-16};'
    free16 = 'add {dst:sp,lhs:sp,rhs:16};' if arm else 'addi {dst:sp,lhs:sp,rhs:16};'
    alloc32 = 'sub {dst:sp,lhs:sp,rhs:32};' if arm else 'addi {dst:sp,lhs:sp,rhs:-32};'
    free32 = 'add {dst:sp,lhs:sp,rhs:32};' if arm else 'addi {dst:sp,lhs:sp,rhs:32};'
    store19 = 'str {src:x19,offset:0};' if arm else 'sd {src:s1,offset:0};'
    load19 = 'ldr {dst:x19,offset:0};' if arm else 'ld {dst:s1,offset:0};'
    store20 = 'str {src:x20,offset:8};' if arm else 'sd {src:s2,offset:8};'
    load20 = 'ldr {dst:x20,offset:8};' if arm else 'ld {dst:s2,offset:8};'
    mutate19 = 'add {dst:x19,lhs:x0,rhs:x1};' if arm else 'add {dst:s1,lhs:a0,rhs:a1};'
    mutate20 = 'add {dst:x20,lhs:x0,rhs:x1};' if arm else 'add {dst:s2,lhs:a0,rhs:a1};'
    volatile = 'eor {dst:x0,lhs:x0,rhs:x1};' if arm else 'xor {dst:a0,lhs:a0,rhs:a1};'
    link_store = 'str {src:x30,offset:0};' if arm else 'sd {src:ra,offset:0};'
    link_load = 'ldr {dst:x30,offset:0};' if arm else 'ld {dst:ra,offset:0};'
    cases = [
        # Branch skips balanced stack activity on one path, then both paths join.
        (True, f'{cond} {alloc16} {free16} label {{name:alternative}}; ret;',
         ('cbz x0,12\nsub sp,sp,#16\nadd sp,sp,#16\nret' if arm else
          'beq a0,a1,12\naddi sp,sp,-16\naddi sp,sp,16\nret')),
        (True, f'{cond} {alloc16} {free16} {jump} label {{name:alternative}}; '
         f'{alloc32} {free32} label {{name:joined}}; ret;', None),
        # Same protected slot on both paths, then a write+restore after join.
        (True, f'{alloc32} {cond} {store19} {jump} label {{name:alternative}}; '
         f'{store19} label {{name:joined}}; {mutate19} {load19} {free32} ret;', None),
        # Different store order is semantically equivalent at the join.
        (True, f'{alloc32} {cond} {store19} {store20} {jump} '
         f'label {{name:alternative}}; {store20} {store19} label {{name:joined}}; '
         f'{mutate19} {mutate20} {load19} {load20} {free32} ret;', None),
        # Each branch restores its saved register *before* the join.
        (True, f'{cond} {alloc32} {store19} {mutate19} {load19} {free32} {jump} '
         f'label {{name:alternative}}; {alloc32} {store20} {mutate20} {load20} {free32} '
         f'label {{name:joined}}; ret;', None),
        (True, f'{alloc16} {cond} {link_store} {link_load} {jump} '
         f'label {{name:alternative}}; {link_store} {link_load} '
         f'label {{name:joined}}; {free16} ret;', None),
        # Joins are forbidden when depths or slot provenance differ.
        (False, f'{cond} {alloc16} label {{name:alternative}}; ret;', None),
        (False, f'{alloc16} {cond} {free16} label {{name:alternative}}; ret;', None),
        (False, f'{alloc16} {cond} {store19} label {{name:alternative}}; '
         f'{mutate19} {load19} {free16} ret;', None),
        (False, f'{alloc16} {cond} {store19} {jump} '
         f'label {{name:alternative}}; {link_store} label {{name:joined}}; '
         f'{free16} ret;', None),
        (False, f'{alloc32} {cond} {store19} {jump} '
         f'label {{name:alternative}}; {store20} label {{name:joined}}; '
         f'{mutate19} {load19} {free32} ret;', None),
        (False, f'{alloc32} {cond} {store19} {jump} '
         f'label {{name:alternative}}; {store19} {load19} label {{name:joined}}; '
         f'{mutate19} {load19} {free32} ret;', None),
        (False, f'{alloc16} {cond} {link_store} {jump} '
         f'label {{name:alternative}}; {volatile} label {{name:joined}}; '
         f'{link_load} {free16} ret;', None),
        (False, f'{cond} {alloc16} {free16} label {{name:alternative}}; '
         f'{store19} ret;', None),
    ]
    # Additional structured diamond stress tests with different branch depth.
    for layers in (1, 2, 4, 8, 12, 20):
        seq = []
        for i in range(layers):
            seq += [cond.replace('alternative', f'a{i}'), alloc16, free16,
                    jump.replace('joined', f'j{i}'), f'label {{name:a{i}}};',
                    alloc32, free32, f'label {{name:j{i}}};']
        cases.append((True, ' '.join(seq) + ' ret;', None))
    # A balanced loop has a stable canonical state at its header.
    cases.append((True, f'label {{name:alternative}}; {volatile} '
                  f'{cond} ret;', None))
    return cases


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--target', choices=('aarch64', 'riscv64'), required=True)
    parser.add_argument('--clang')
    a = parser.parse_args()
    suite = generate(a.target)
    for idx, (expected, code, assembly) in enumerate(suite):
        with tempfile.TemporaryDirectory(prefix='aster-flow-') as tmp:
            src = Path(tmp)/'test.aa'
            dst = Path(tmp)/'test.bin'
            src.write_text(f'module g1.flow;\nexport fn f(a:u64,b:u64)->u64 @naked {{\n'
                           f'unsafe exact {a.target} {{ {code} }}\n}}\n', encoding='utf8')
            proc = subprocess.run([a.compiler, 'build', '--target', a.target,
                                   '--emit', 'bin', '-j', '1', '-o', str(dst), str(src)],
                                  capture_output=True, text=True, timeout=30)
            if expected and (proc.returncode or not dst.is_file()):
                raise AssertionError(f'{a.target}[{idx}] false rejection: {proc.stdout} {proc.stderr}')
            if not expected and (proc.returncode == 0 or dst.exists()):
                raise AssertionError(f'{a.target}[{idx}] false acceptance: {code}')
            if expected:
                data = dst.read_bytes()
                if not data or len(data) % 4:
                    raise AssertionError(f'{a.target}[{idx}] bad binary size')
                if assembly and a.clang:
                    ref = assemble(a.clang, a.target, assembly, tmp)
                    if data != ref:
                        raise AssertionError(f'{a.target}[{idx}] bad encoding {data.hex()} != {ref.hex()}')
    print(f'{a.target}: {len(suite)} path-sensitive CFG cases pass')


if __name__ == '__main__':
    main()
