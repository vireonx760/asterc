#!/usr/bin/env python3
"""Legacy x86 atomic feature/encoding contracts for the 386/i486/i586 tiers."""
import argparse
import pathlib
import subprocess
import tempfile

BASELINE = r'''module test.legacy_atomic_baseline;
export fn ld(p:ptr<u32>)->u32 @abi(c) { return atomic.load.u32 [p] @acquire; }
export fn st(p:ptr<u32>,v:u32)->void @abi(c) { atomic.store.u32 [p] = v @seq_cst; }
export fn xg(p:ptr<u32>,v:u32)->u32 @abi(c) { return atomic.rmw.xchg.u32 [p], v @acq_rel; }
'''
U32_FULL = r'''module test.legacy_atomic_u32;
export fn ld(p:ptr<u32>)->u32 @abi(c) { return atomic.load.u32 [p] @acquire; }
export fn st(p:ptr<u32>,v:u32)->void @abi(c) { atomic.store.u32 [p] = v @seq_cst; }
export fn xg(p:ptr<u32>,v:u32)->u32 @abi(c) { return atomic.rmw.xchg.u32 [p], v @acq_rel; }
export fn ad(p:ptr<u32>,v:u32)->u32 @abi(c) { return atomic.rmw.add.u32 [p], v @acq_rel; }
export fn xo(p:ptr<u32>,v:u32)->u32 @abi(c) { return atomic.rmw.xor.u32 [p], v @relaxed; }
export fn ca(p:ptr<u32>,e:u32,d:u32)->bool @abi(c) { return atomic.cas.u32 [p], e, d @success(acq_rel) @failure(acquire); }
'''
U64_FULL = r'''module test.legacy_atomic_u64;
export fn ld(p:ptr<u64>)->u64 @abi(c) { return atomic.load.u64 [p] @acquire; }
export fn st(p:ptr<u64>,v:u64)->void @abi(c) { atomic.store.u64 [p] = v @seq_cst; }
export fn xg(p:ptr<u64>,v:u64)->u64 @abi(c) { return atomic.rmw.xchg.u64 [p], v @acq_rel; }
export fn ad(p:ptr<u64>,v:u64)->u64 @abi(c) { return atomic.rmw.add.u64 [p], v @acq_rel; }
export fn xo(p:ptr<u64>,v:u64)->u64 @abi(c) { return atomic.rmw.xor.u64 [p], v @relaxed; }
export fn ca(p:ptr<u64>,e:u64,d:u64)->bool @abi(c) { return atomic.cas.u64 [p], e, d @success(acq_rel) @failure(acquire); }
'''
CAS32 = r'''module test.legacy_atomic_need_i486;
export fn ca(p:ptr<u32>,e:u32,d:u32)->bool @abi(c) { return atomic.cas.u32 [p], e, d @seq_cst; }
'''


def run(compiler, target, text, feature=None):
    with tempfile.TemporaryDirectory(prefix='aster-legacy-atomic-') as td:
        td = pathlib.Path(td)
        src, out = td/'main.aa', td/'main.bin'
        src.write_text(text, encoding='utf-8')
        cmd = [compiler, 'build', '--target', target, '--abi', 'cdecl', '--emit', 'bin', '-j', '1']
        if feature:
            cmd += ['--feature', feature]
        cmd += ['-o', str(out), str(src)]
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return p, out.read_bytes() if out.exists() else b''


def expect_ok(p, data, what):
    if p.returncode or not data:
        raise SystemExit(f'{what} failed:\n{p.stdout}{p.stderr}')


def expect_feature_reject(p, feature, what):
    if p.returncode == 0:
        raise SystemExit(f'{what}: unexpectedly compiled without {feature}')
    diag = p.stdout + p.stderr
    if 'target:' not in diag or 'is not legal' not in diag:
        raise SystemExit(f'{what}: did not fail at Target feature legalization:\n{diag}')


def has_cmpxchg(data):
    # Prefix order differs in code16; looking for the opcode pair avoids
    # mistaking address/operand-size prefixes for missing LOCK semantics.
    return b'\x0f\xb1' in data and b'\xf0' in data


def has_cmpxchg8b(data):
    return b'\x0f\xc7' in data and b'\xf0' in data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--case', required=True, type=int)
    a = ap.parse_args()
    if not 0 <= a.case < 10:
        raise SystemExit('case must be in 0..9')
    target = 'x86_32' if a.case % 2 == 0 else 'x86_16'
    group = a.case // 2
    if group == 0:
        p, data = run(a.compiler, target, BASELINE)
        expect_ok(p, data, f'{target} 386 atomic load/store/xchg')
    elif group == 1:
        p, _ = run(a.compiler, target, CAS32)
        expect_feature_reject(p, 'i486', f'{target} 386 CAS')
    elif group == 2:
        p, data = run(a.compiler, target, U32_FULL, 'i486')
        expect_ok(p, data, f'{target} i486 u32 atomics')
        if not has_cmpxchg(data):
            raise SystemExit(f'{target} i486 atomics contain no CMPXCHG encoding')
    elif group == 3:
        p, _ = run(a.compiler, target, U64_FULL, 'i486')
        expect_feature_reject(p, 'i586/cmpxchg8b', f'{target} i486 u64 atomics')
    else:
        p, data = run(a.compiler, target, U64_FULL, 'i586')
        expect_ok(p, data, f'{target} i586 u64 atomics')
        if not has_cmpxchg8b(data):
            raise SystemExit(f'{target} i586 atomics contain no CMPXCHG8B encoding')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
