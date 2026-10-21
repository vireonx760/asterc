"""Executable oracle for aggregate field reads and IEEE floating negation."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(args):
    p = subprocess.run(list(map(str, args)), text=True, capture_output=True, timeout=60)
    if p.returncode:
        raise RuntimeError(f'{args}\nexit={p.returncode}\n{p.stdout}{p.stderr}')
    return p.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--clang', required=True)
    args = parser.parse_args()
    fixture = Path(__file__).parent / 'compiler_e2e/aggregate_field_read.aa'
    with tempfile.TemporaryDirectory(prefix='aster-editor-regression-') as tmp:
        root = Path(tmp)
        host = root / 'host.c'
        host.write_text('''#include <stdint.h>
#include <string.h>
extern int field_read_test(void);
extern float negate32(float);
extern double negate64(double);
int main(void) {
    int result = field_read_test();
    if (result) return result;
    const uint32_t bits32[] = {0, 0x80000000, 0x3fa00000, 0xbfa00000, 0x7f800000, 0xff800000, 0x7fc12345, 1};
    const uint64_t bits64[] = {0, UINT64_C(0x8000000000000000), UINT64_C(0x3ff4000000000000), UINT64_C(0xbff4000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8123456789abc), 1};
    for (int i = 0; i < 8; ++i) {
        float x; uint32_t actual32; memcpy(&x, &bits32[i], 4); x = negate32(x); memcpy(&actual32, &x, 4);
        if (actual32 != (bits32[i] ^ UINT32_C(0x80000000))) return 20+i;
        double y; uint64_t actual64; memcpy(&y, &bits64[i], 8); y = negate64(y); memcpy(&actual64, &y, 8);
        if (actual64 != (bits64[i] ^ UINT64_C(0x8000000000000000))) return 40+i;
    }
    return 0;
}
''', encoding='utf-8')
        for mode, extra in [('default', []), ('spilled', ['--reg-limit', '2']), ('unpromoted', ['--no-promote'])]:
            obj = root / (mode + '.obj')
            run([args.compiler, 'build', '--target', 'x86_64', '--abi', 'win64',
                 '--emit', 'obj', '--format', 'coff', *extra, '-o', obj, fixture])
            exe = root / (mode + '.exe')
            run([args.clang, '-fuse-ld=lld', '-Wall', '-Wextra', '-Werror', host, obj, '-o', exe])
            run([exe])
        for arch, abi in [('x86_64', 'sysv'), ('aarch64', 'aapcs64')]:
            run([args.compiler, 'build', '--target', arch, '--abi', abi,
                 '--emit', 'obj', '--format', 'elf', '-o', root / (abi + '.o'), fixture])
    print('PASS: aggregate fields, pointer chains, indexed arrays, f32/f64 sign bits; 3 Win64 execution modes, SysV/AAPCS64 object generation')


if __name__ == '__main__':
    main()
