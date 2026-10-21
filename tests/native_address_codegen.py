#!/usr/bin/env python3
import argparse, pathlib, subprocess, tempfile, sys
CASES = {
0: ("rbx + rcx * 8 + 16", True, bytes.fromhex("48 8b 44 cb 10 c3")),
1: ("r9 + r10 * 4 - 8", True, None),
2: ("rbx + rcx * 3", False, None),
3: ("rbx - rcx", False, None),
}
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--compiler',required=True); ap.add_argument('--case',type=int,choices=CASES,required=True); a=ap.parse_args()
    expr,ok,expected=CASES[a.case]
    source=f'''module tests.native_address_{a.case};\nexport fn raw() -> u64 @naked {{ exact x86_64 {{ mov {{ dst: rax, src: mem<u64>[{expr}] }}; ret; }} }}\n'''
    with tempfile.TemporaryDirectory(prefix='aster-native-address-') as td:
        td=pathlib.Path(td); src=td/'case.aa'; out=td/'case.bin'; src.write_text(source)
        p=subprocess.run([a.compiler,'build','--target','x86_64','--emit','bin','-o',str(out),str(src)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        if ok:
            if p.returncode or not out.exists(): sys.stderr.write(p.stdout+p.stderr); return 1
            if expected is not None and out.read_bytes()!=expected:
                sys.stderr.write(f'expected {expected.hex()}, got {out.read_bytes().hex()}\n'); return 1
            return 0
        if p.returncode==0:
            sys.stderr.write('unsupported native address unexpectedly compiled\n'); return 1
        return 0
if __name__=='__main__': raise SystemExit(main())
