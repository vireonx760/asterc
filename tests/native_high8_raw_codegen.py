#!/usr/bin/env python3
"""x86 high-8/REX and unsafe-exact raw byte regression matrix."""
import argparse, pathlib, subprocess, sys, tempfile

CASES = {
0: (r'''module t.h8rr; export fn raw()->void @naked { unsafe exact x86_64 {
 mov { dst: ah, src: bl }; mov { dst: ch, src: ah }; mov { dst: dl, src: dh }; ret; } }''', True, bytes.fromhex('88dc88e588f2c3'), 'sysv'),
1: (r'''module t.h8imm; export fn raw()->void @naked { unsafe exact x86_64 {
 mov { dst: ah, src: 127 }; ret; } }''', True, bytes.fromhex('b47fc3'), 'sysv'),
2: (r'''module t.h8load; export fn raw()->void @naked { unsafe exact x86_64 {
 mov { dst: ah, src: mem<u8>[rbx] }; ret; } }''', True, bytes.fromhex('8a23c3'), 'sysv'),
3: (r'''module t.h8store; export fn raw()->void @naked { unsafe exact x86_64 {
 mov { dst: mem<u8>[rbx], src: ah }; ret; } }''', True, bytes.fromhex('8823c3'), 'sysv'),
4: (r'''module t.h8spl; export fn raw()->void @naked { unsafe exact x86_64 {
 mov { dst: ah, src: spl }; ret; } }''', False, None, 'sysv'),
5: (r'''module t.h8r8; export fn raw()->void @naked { unsafe exact x86_64 {
 mov { dst: ah, src: r8b }; ret; } }''', False, None, 'sysv'),
6: (r'''module t.h8memr8; export fn raw()->void @naked { unsafe exact x86_64 {
 mov { dst: ah, src: mem<u8>[r8] }; ret; } }''', False, None, 'sysv'),
7: (r'''module t.rawexact; export fn raw()->void @naked { unsafe exact x86_64 {
 bytes { 0x90, 0xc3 }; } }''', True, bytes.fromhex('90c3'), 'sysv'),
8: (r'''module t.rawvalidate; export fn raw()->void @frame(validate) { unsafe exact x86_64 {
 bytes { 0x90, 0xc3 }; } }''', False, None, 'sysv'),
}

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--compiler',required=True); ap.add_argument('--case',type=int,required=True,choices=range(9)); a=ap.parse_args()
    source, should_pass, expected, abi=CASES[a.case]
    with tempfile.TemporaryDirectory(prefix='aster-high8-raw-') as td:
        root=pathlib.Path(td); src=root/'case.aa'; out=root/'case.bin'; src.write_text(source,encoding='utf-8')
        p=subprocess.run([a.compiler,'build','--target','x86_64','--abi',abi,'--emit','bin','-j','1','-o',str(out),str(src)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=30)
        text=p.stdout+p.stderr
        if should_pass:
            if p.returncode or not out.exists(): sys.stderr.write(text); return 1
            data=out.read_bytes()
            if data != expected:
                sys.stderr.write(f'case {a.case}: expected {expected.hex()}, got {data.hex()}\n{text}'); return 1
            return 0
        if p.returncode == 0:
            sys.stderr.write(f'case {a.case}: invalid high8/raw form unexpectedly compiled\n'); return 1
        if a.case in (4,5,6) and 'high-8 register' not in text and 'unsupported' not in text:
            sys.stderr.write(text); return 1
        return 0

if __name__=='__main__': raise SystemExit(main())
