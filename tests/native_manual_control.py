#!/usr/bin/env python3
"""End-to-end contracts for explicit x86-64 manual stack/control flow."""
import argparse, pathlib, subprocess, sys, tempfile

SOURCES = {
0: r'''module tests.manual_saved;
export fn raw() -> u64 @naked { unsafe exact x86_64 {
  push { src: rbx }; mov { dst: rbx, src: 7 }; mov { dst: rax, src: rbx }; pop { dst: rbx }; ret;
} }''',
1: r'''module tests.manual_unbalanced;
export fn raw() -> void @naked { unsafe exact x86_64 { push { src: rbx }; ret; } }''',
2: r'''module tests.manual_clobber;
export fn raw() -> void @naked { unsafe exact x86_64 { mov { dst: rbx, src: 7 }; ret; } }''',
3: r'''module tests.manual_sysv_call;
export fn helper() -> void @naked { unsafe exact x86_64 { ret; } }
export fn raw() -> void @naked { unsafe exact x86_64 {
  sub { dst: rsp, src: 8 }; call { target: helper }; add { dst: rsp, src: 8 }; ret;
} }''',
4: r'''module tests.manual_sysv_bad_call;
export fn helper() -> void @naked { unsafe exact x86_64 { ret; } }
export fn raw() -> void @naked { unsafe exact x86_64 { call { target: helper }; ret; } }''',
5: r'''module tests.manual_win_call;
export fn helper() -> void @naked { unsafe exact x86_64 { ret; } }
export fn raw() -> void @naked { unsafe exact x86_64 {
  sub { dst: rsp, src: 40 }; call { target: helper }; add { dst: rsp, src: 40 }; ret;
} }''',
6: r'''module tests.manual_win_bad_shadow;
export fn helper() -> void @naked { unsafe exact x86_64 { ret; } }
export fn raw() -> void @naked { unsafe exact x86_64 {
  sub { dst: rsp, src: 8 }; call { target: helper }; add { dst: rsp, src: 8 }; ret;
} }''',
7: r'''module tests.manual_indirect_jmp;
export fn raw() -> void @naked { unsafe exact x86_64 {
  mov { dst: rax, src: 0 }; jmp { target: rax };
} }''',
8: r'''module tests.manual_direct_jmp;
export fn helper() -> void @naked { unsafe exact x86_64 { ret; } }
export fn raw() -> void @naked { unsafe exact x86_64 { jmp { target: helper }; } }''',
9: r'''module tests.manual_bad_restore;
export fn raw() -> void @naked { unsafe exact x86_64 {
  push { src: rbx }; pop { dst: rbp }; ret;
} }''',
}
PASS = {0, 3, 5, 7, 8}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--case', required=True, type=int, choices=range(10))
    a = ap.parse_args()
    abi = 'win64' if a.case in (5, 6) else 'sysv'
    with tempfile.TemporaryDirectory(prefix='aster-manual-control-') as td:
        root = pathlib.Path(td); src = root/'case.aa'; out = root/'case.bin'
        src.write_text(SOURCES[a.case], encoding='utf-8')
        p = subprocess.run([a.compiler, 'build', '--target', 'x86_64', '--abi', abi,
                            '--emit', 'bin', '-j', '1', '-o', str(out), str(src)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
        text = p.stdout + p.stderr
        if a.case in PASS:
            if p.returncode or not out.exists() or not out.read_bytes():
                sys.stderr.write(text); return 1
            return 0
        if p.returncode == 0:
            sys.stderr.write(f'case {a.case}: invalid manual control-flow unexpectedly compiled\n')
            return 1
        if 'Physical verification failed' not in text and 'unsupported' not in text:
            sys.stderr.write(text); return 1
        return 0

if __name__ == '__main__':
    raise SystemExit(main())
