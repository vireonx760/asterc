"""Verify RV64I integer */% lowering is self-contained without the M extension."""
from pathlib import Path
import argparse
import struct
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', required=True)
args = p.parse_args()

SOURCE = r'''module rv.baseline;
export fn mu64(a:u64,b:u64)->u64 { return a*b; }
export fn du64(a:u64,b:u64)->u64 { return a/b; }
export fn ru64(a:u64,b:u64)->u64 { return a%b; }
export fn mi64(a:i64,b:i64)->i64 { return a*b; }
export fn di64(a:i64,b:i64)->i64 { return a/b; }
export fn ri64(a:i64,b:i64)->i64 { return a%b; }
export fn mu32(a:u32,b:u32)->u32 { return a*b; }
export fn di32(a:i32,b:i32)->i32 { return a/b; }
export fn checked(a:u64,b:u64)->u64 {
    let r:(u64,bool)=mul.checked(a,b);
    if r[1] { return 0xffffffffffffffff; }
    return r[0];
}
export fn wrapped(a:u64,b:u64)->u64 { return mul.wrap(a,b); }
'''

# RV M-extension integer arithmetic has opcode OP/OP-32 and funct7=0000001.
def contains_m_arithmetic(data: bytes) -> bool:
    for off in range(0, len(data) - 3, 4):
        word, = struct.unpack_from('<I', data, off)
        opcode = word & 0x7f
        funct7 = (word >> 25) & 0x7f
        funct3 = (word >> 12) & 7
        if opcode in (0x33, 0x3b) and funct7 == 1 and funct3 <= 7:
            return True
    return False

with tempfile.TemporaryDirectory(prefix='aster-rv64i-') as td:
    td = Path(td)
    src = td/'main.aa'
    src.write_text(SOURCE, encoding='utf-8')
    base1, base2, ext = td/'base1.bin', td/'base2.bin', td/'m.bin'
    common = [args.compiler, 'build', '--target', 'riscv64', '--emit', 'bin', '-j', '1']
    for out in (base1, base2):
        proc = subprocess.run(common + ['-o', str(out), str(src)], capture_output=True, text=True, timeout=30)
        if proc.returncode:
            raise SystemExit(proc.stdout + proc.stderr)
    if base1.read_bytes() != base2.read_bytes():
        raise SystemExit('RV64I software arithmetic output is not deterministic')
    base = base1.read_bytes()
    if not base or contains_m_arithmetic(base):
        raise SystemExit('baseline RV64I output contains an RV64M arithmetic instruction')
    proc = subprocess.run(common + ['--feature', 'm', '-o', str(ext), str(src)], capture_output=True, text=True, timeout=30)
    if proc.returncode:
        raise SystemExit(proc.stdout + proc.stderr)
    m = ext.read_bytes()
    if not contains_m_arithmetic(m):
        raise SystemExit('RV64M fast path did not emit an M-extension arithmetic instruction')
    if len(base) >= len(m) * 8:
        raise SystemExit(f'RV64I software lowering unexpectedly bloated: baseline={len(base)}, M={len(m)}')
