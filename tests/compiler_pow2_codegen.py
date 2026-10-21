#!/usr/bin/env python3
"""Power-of-two unsigned div/rem: inspect codegen and execute on x86-64."""
import argparse
import ctypes
import mmap
from pathlib import Path
import platform
import subprocess
import tempfile

CASES = [
    ('u8', '/', 2, 'LSHR', 'UDIV'),
    ('u16', '%', 8, 'AND', 'UREM'),
    ('u32', '/', 128, 'LSHR', 'UDIV'),
    ('u64', '/', 8, 'LSHR', 'UDIV'),
    ('u64', '%', 8, 'AND', 'UREM'),
    ('u64', '/', 1, '', 'UDIV'),
    ('u64', '%', 1, '', 'UREM'),
    ('u64', '/', 9223372036854775808, 'LSHR', 'UDIV'),
    ('u64', '%', 9223372036854775808, 'UDIV', ''),
    # Regression: Slice22 accidentally transformed *signed* division by 2^k
    # into unsigned AND (remainder), violating negative-number semantics.
    ('i8', '/', 8, 'SDIV', ''),
    ('i16', '/', 8, 'SDIV', ''),
    ('i32', '/', 8, 'SDIV', ''),
    ('i64', '/', 8, 'SDIV', ''),
    ('i8', '%', 8, 'SDIV', ''),
    ('i16', '%', 8, 'SDIV', ''),
    ('i32', '%', 8, 'SDIV', ''),
    ('i64', '%', 8, 'SDIV', ''),
    ('i8', '/', -8, 'SDIV', ''),
    ('i16', '%', -8, 'SDIV', ''),
    ('u64', '/', 7, 'UMUL_WIDE', 'UDIV'),
]


def run() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    args = ap.parse_args()
    can_execute = platform.system() == 'Linux' and platform.machine() == 'x86_64'
    with tempfile.TemporaryDirectory(prefix='aster-pow2-') as tmp:
        root = Path(tmp)
        count = 0
        for typ, op, divisor, expected_op, excluded_op in CASES:
            source = root / 'test.aa'
            source.write_text(f'module test.pow2; export fn value(x:{typ})->{typ} @abi(c) '
                              f'{{ return x {op} {divisor}; }}\n', encoding='utf-8')
            dest = root / 'test.bin'
            proc = subprocess.run([args.compiler, 'build', '--target', 'x86_64',
                                   '--abi', 'sysv', '--dump-regalloc', '-o', str(dest), str(source)],
                                  capture_output=True, text=True, timeout=30)
            if proc.returncode or not dest.is_file():
                raise AssertionError((typ, op, divisor, proc.stdout, proc.stderr))
            for marker, should_exist in ((expected_op, True), (excluded_op, False)):
                if marker and (f'AST_MACHINE_X86_{marker}' in proc.stdout) != should_exist:
                    raise AssertionError((typ, op, divisor, marker, proc.stdout))
            if can_execute:
                data = dest.read_bytes()
                with mmap.mmap(-1, max(len(data), mmap.PAGESIZE),
                               prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC) as memory:
                    memory.write(data)
                    address = ctypes.addressof(ctypes.c_char.from_buffer(memory))
                    ctype = {'u8': ctypes.c_uint8, 'u16': ctypes.c_uint16,
                             'u32': ctypes.c_uint32, 'u64': ctypes.c_uint64,
                             'i8': ctypes.c_int8, 'i16': ctypes.c_int16,
                             'i32': ctypes.c_int32, 'i64': ctypes.c_int64}[typ]
                    function = ctypes.CFUNCTYPE(ctype, ctype)(address)
                    maximum = (1 << {'u8':8, 'u16':16, 'u32':32, 'u64':64, 'i8':8, 'i16':16, 'i32':32, 'i64':64}[typ]) - 1
                    bits = {'u8':8,'u16':16,'u32':32,'u64':64,
                            'i8':8,'i16':16,'i32':32,'i64':64}[typ]
                    signed = typ.startswith('i')
                    for value in (0, 1, 7, 127, maximum,
                                  -(1 << (bits-1)) if signed else 0,
                                  -1 if signed else maximum):
                        if signed and value > (1 << (bits-1))-1:
                            value = -1
                        if signed and value < -(1 << (bits-1)):
                            continue
                        result = function(value)
                        q = (abs(value) // abs(divisor)) * (
                            -1 if (value < 0) != (divisor < 0) else 1)
                        if op == '/':
                            wanted = q
                        else:
                            wanted = value - q * divisor
                        if result != wanted:
                            raise AssertionError((typ, op, divisor, value, result, wanted))
                        count += 1
        print(f'pow2 codegen: {len(CASES)} shapes; {count} native executions')


if __name__ == '__main__':
    run()
