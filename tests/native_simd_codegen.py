#!/usr/bin/env python3
import argparse, pathlib, subprocess, tempfile, sys

IDENTITY = r'''module tests.native_simd_identity;
export fn identity(x: vec<4,u32>) -> vec<4,u32> @abi(c) {
    let out: vec<4,u32>;
    native x86_64 inputs(x) outputs(out) { mov { dst: out, src: x }; }
    return out;
}
'''
HARD = r'''module tests.native_simd_hard;
export fn hard(x: vec<4,u32>) -> vec<4,u32> @abi(c) {
    let out: vec<4,u32>;
    native x86_64 inputs(x) outputs(out) {
        mov { dst: xmm6, src: x };
        mov { dst: out, src: xmm6 };
    }
    return out;
}
'''
MEMORY = r'''module tests.native_simd_memory;
export fn readv(p: ptr<vec<4,u32>>) -> vec<4,u32> @abi(c) {
    let out: vec<4,u32>;
    native x86_64 inputs(p) outputs(out) { mov { dst: out, src: mem<vec<4,u32>>[p] }; }
    return out;
}
export fn writev(p: ptr<vec<4,u32>>, x: vec<4,u32>) -> void @abi(c) {
    native x86_64 inputs(p, x) { mov { dst: mem<vec<4,u32>>[p], src: x }; }
}
'''
YMM = r'''module tests.native_simd_ymm;
export fn raw() -> void @naked {
    exact x86_64 { mov { dst: ymm0, src: ymm1 }; ret; }
}
'''

def run(compiler: str, case: int) -> int:
    source, extra, should_pass = {
        0: (IDENTITY, ["--abi", "sysv"], True),
        1: (HARD, ["--abi", "win64"], True),
        2: (MEMORY, ["--abi", "sysv"], True),
        3: (YMM, [], False),
        4: (YMM, ["--feature", "avx"], False),
    }[case]
    with tempfile.TemporaryDirectory(prefix="aster-native-simd-") as td:
        root = pathlib.Path(td)
        src = root / "case.aa"; out = root / "case.bin"
        src.write_text(source, encoding="utf-8")
        p = subprocess.run([compiler, "build", "--target", "x86_64", *extra,
                            "--emit", "bin", "-o", str(out), str(src)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        text = p.stdout + p.stderr
        if should_pass:
            if p.returncode != 0 or not out.exists() or not out.read_bytes():
                sys.stderr.write(text); return 1
            # The supported 128-bit native transfer contract must reach legacy
            # SSE MOVDQU encoding (F3 0F 6F/7F), not silently scalarize away.
            data = out.read_bytes()
            if b"\xf3\x0f\x6f" not in data and b"\xf3\x0f\x7f" not in data:
                sys.stderr.write("native XMM case emitted no MOVDQU encoding\n"); return 1
            return 0
        if p.returncode == 0:
            sys.stderr.write("unsupported YMM native form unexpectedly compiled\n"); return 1
        if case == 3 and "unavailable target feature" not in text:
            sys.stderr.write(text); return 1
        if case == 4 and "unsupported explicit instruction form" not in text and "unsupported operands" not in text:
            sys.stderr.write(text); return 1
        return 0

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--case", type=int, required=True, choices=range(5))
    a = ap.parse_args()
    return run(a.compiler, a.case)

if __name__ == "__main__":
    raise SystemExit(main())
