#!/usr/bin/env python3
import argparse, pathlib, subprocess, tempfile, sys

SOURCES = {
0: r'''module tests.nt64;
export fn f(p: ptr<u64>, v: u64) -> void @abi(c) { unsafe { store.u64 [p] = v @nontemporal; } }
''',
1: r'''module tests.nt32;
export fn f(p: ptr<u64>, v: u64) -> void @abi(c) { unsafe { store.u64 [p] = v @nontemporal @align(8); } }
''',
2: r'''module tests.vol;
export fn f(p: ptr<u64>, v: u64) -> void @abi(c) { unsafe { store.u64 [p] = v @volatile; } }
''',
3: r'''module tests.nt_a64;
export fn f(p: ptr<u64>, v: u64) -> void @abi(c) { unsafe { store.u64 [p] = v @nontemporal; } }
''',
4: r'''module tests.nt_rv;
export fn f(p: ptr<u64>, v: u64) -> void @abi(c) { unsafe { store.u64 [p] = v @nontemporal; } }
''',
5: r'''module tests.nt_expr;
export fn f(p: ptr<u64>, a: u64, b: u64) -> void @abi(c) { unsafe { store.u64 [p] = a + b @nontemporal; } }
''',
}

def main() -> int:
    ap=argparse.ArgumentParser(); ap.add_argument('--compiler', required=True); ap.add_argument('--case', type=int, required=True, choices=range(6)); a=ap.parse_args()
    target = 'aarch64' if a.case == 3 else 'riscv64' if a.case == 4 else 'x86_64'
    abi = 'aapcs64' if target == 'aarch64' else 'lp64' if target == 'riscv64' else 'sysv'
    with tempfile.TemporaryDirectory(prefix='aster-memory-hint-') as td:
        root=pathlib.Path(td); src=root/'case.aa'; out=root/'case.bin'; src.write_text(SOURCES[a.case], encoding='utf-8')
        p=subprocess.run([a.compiler,'build','--target',target,'--abi',abi,'--emit','bin','-o',str(out),str(src)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        if p.returncode != 0 or not out.exists() or not out.read_bytes():
            sys.stderr.write(p.stdout+p.stderr); return 1
        data=out.read_bytes()
        if a.case in (0,5):
            if b'\x0f\xc3' not in data:
                sys.stderr.write(f'x86-64 nontemporal u64 store emitted no MOVNTI: {data.hex()}\n'); return 1
        elif a.case == 1:
            if b'\x0f\xc3' not in data:
                sys.stderr.write(f'x86-64 aligned nontemporal store emitted no MOVNTI: {data.hex()}\n'); return 1
        elif a.case == 2:
            # The ordinary volatile store must remain a normal MOV, not silently
            # become a streaming store merely because both are memory effects.
            if b'\x0f\xc3' in data:
                sys.stderr.write(f'volatile store unexpectedly emitted MOVNTI: {data.hex()}\n'); return 1
        return 0

if __name__ == '__main__': raise SystemExit(main())
