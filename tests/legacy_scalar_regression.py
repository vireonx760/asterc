#!/usr/bin/env python3
import argparse, pathlib, subprocess, tempfile

FLOAT_SOURCE = r'''module test.legacy_float_casts;
export fn i32_to_f64(a:i32)->f64 @abi(c) { return cast<f64>(a); }
export fn u32_to_f32(a:u32)->f32 @abi(c) { return cast<f32>(a); }
export fn u64_to_f64(a:u64)->f64 @abi(c) { return cast<f64>(a); }
export fn f32_to_i32(a:f32)->i32 @abi(c) { return cast<i32>(a); }
export fn f64_to_u32(a:f64)->u32 @abi(c) { return cast<u32>(a); }
export fn f64_to_u64(a:f64)->u64 @abi(c) { return cast<u64>(a); }
export fn widen(a:f32)->f64 @abi(c) { return cast<f64>(a); }
export fn narrow(a:f64)->f32 @abi(c) { return cast<f32>(a); }
'''

CHECKED_SOURCE = r'''module test.legacy_checked;
export fn run(a:u32,b:u32)->u32 @abi(c) {
    let r = add.checked(a,b);
    if r[1] { return 0; }
    return r[0];
}
'''

ATOMIC_SOURCE = r'''module test.legacy_atomic;
export fn run(p:ptr<u64>)->u64 @abi(c) {
    return atomic.load.u64 [p] @acquire;
}
'''

NATIVE_SOURCE = r'''module test.legacy_native;
export fn run(x:u64)->u64 @abi(c) {
    let out:u64;
    native x86_64 inputs(x) outputs(out) { mov { dst:out, src:x }; }
    return out;
}
'''

def invoke(compiler, target, src, out):
    return subprocess.run([compiler, 'build', '--target', target, '--abi', 'cdecl', '--emit', 'bin', '-j', '1', '-o', str(out), str(src)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='aster-legacy-scalar-') as td:
        root = pathlib.Path(td)
        for target in ('x86_32', 'x86_16'):
            for name, text in (('float', FLOAT_SOURCE), ('checked', CHECKED_SOURCE)):
                src = root / f'{target}-{name}.aa'; src.write_text(text, encoding='utf-8')
                out1 = root / f'{target}-{name}-1.bin'; out2 = root / f'{target}-{name}-2.bin'
                p = invoke(args.compiler, target, src, out1)
                if p.returncode:
                    raise SystemExit(f'{target}/{name} failed:\n{p.stdout}{p.stderr}')
                p = invoke(args.compiler, target, src, out2)
                if p.returncode:
                    raise SystemExit(f'{target}/{name} repeat failed:\n{p.stdout}{p.stderr}')
                a, b = out1.read_bytes(), out2.read_bytes()
                if not a or a != b:
                    raise SystemExit(f'{target}/{name}: empty or nondeterministic output')
                if name == 'float':
                    # x87 integer-load / control-word / integer-store opcode bytes.
                    if b'\xdf' not in a or b'\xd9' not in a:
                        raise SystemExit(f'{target}/float: expected x87 conversion opcodes are absent')
            for name, text in (('atomic', ATOMIC_SOURCE), ('native', NATIVE_SOURCE)):
                src = root / f'{target}-{name}.aa'; src.write_text(text, encoding='utf-8')
                out = root / f'{target}-{name}.bin'
                p = invoke(args.compiler, target, src, out)
                if p.returncode == 0:
                    raise SystemExit(f'{target}/{name}: unsupported baseline operation unexpectedly compiled')
                diag = p.stdout + p.stderr
                if 'target:' not in diag or 'is not legal' not in diag:
                    raise SystemExit(f'{target}/{name}: operation did not fail at Target legalization:\n{diag}')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
