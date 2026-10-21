#!/usr/bin/env python3
"""Actual Rust COFF objects linked with an independent Win64 C17 caller.

Exercises scalar /%, both call directions, nine arguments, nested calls and
values live across calls, in normal/spilled/unpromoted modes and both policies.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile

TYPES = [('u8', 'uint8_t'), ('i8', 'int8_t'), ('u16', 'uint16_t'), ('i16', 'int16_t'),
         ('u32', 'uint32_t'), ('i32', 'int32_t'), ('u64', 'uint64_t'), ('i64', 'int64_t')]


def command(argv, env=None, expected=0):
    result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, env=env, timeout=60)
    if result.returncode != expected:
        raise AssertionError((list(map(str, argv)), result.returncode, result.stdout, result.stderr))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--opt-level',choices=['0','2'],default='0')
    parser.add_argument('--clang', required=True)
    parser.add_argument('--out')
    args = parser.parse_args()
    if os.name != 'nt' or platform.machine().lower() not in ('amd64', 'x86_64'):
        parser.error('this gate requires native Windows x64')
    provider = ['module qual.provider;']
    driver = ['#include <stdint.h>', '#include <inttypes.h>', '#include <stdio.h>',
              '#ifndef ASTER_CORRUPT_CALLBACK', '#define ASTER_CORRUPT_CALLBACK 0', '#endif']
    checks = []
    positive_per_run = 0
    for typ, ctype in TYPES:
        bits = int(typ[1:])
        signed = typ.startswith('i')
        divisor = -7 if signed else 7
        for op, name in (('/', 'div'), ('%', 'rem')):
            for dynamic in (False, True):
                function = ('dynamic_' if dynamic else '') + name + '_' + typ
                params = f'x:{typ},y:{typ}' if dynamic else f'x:{typ}'
                provider.append(f'export fn {function}({params})->{typ} @abi(c) '
                                f'{{return x {op} ' + ('y' if dynamic else str(divisor)) + ';}')
                driver.append(f'extern {ctype} {function}({ctype}' + (f',{ctype}' if dynamic else '') + ');')
                # Avoid implementation-defined unsigned-to-signed input casts.
                lo = -(1 << (bits - 1)) if signed else 0
                hi = (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1
                values = sorted(set([lo, lo + 1, 0, 1, 7, hi - 1, hi] + ([-127, -7, -1] if signed else [])))
                literal = lambda value: ('(-INT64_C(9223372036854775807)-1)' if value == -(1 << 63)
                                         else f'(-INT64_C({-value}))' if value < 0 else f'UINT64_C({value})')
                for value in values:
                    call = f'{function}(x' + (f',({ctype})({divisor})' if dynamic else '') + ')'
                    checks.append(f'{{{ctype} x=({ctype})({literal(value)}); '
                                  f'if ({call} != ({ctype})(x {op} ({ctype})({divisor}))) return 10;}}')
                    positive_per_run += 1
    client = '''module qual.client;
extern fn div_u64(x:u64)->u64 @abi(c);
extern fn rem_u64(x:u64)->u64 @abi(c);
extern fn callback(a:u64,b:u64,c:u64,d:u64,e:u64,f:u64,g:u64,h:u64,i:u64)->u64 @abi(c);
export fn chain(x:u64,y:u64)->u64 @abi(c) {
    let live:u64=x ^ y;
    let result:u64=callback(x,y,3,4,5,6,7,8,9);
    return result+live+div_u64(x)+rem_u64(y);
}
'''
    driver.append('''extern uint64_t chain(uint64_t,uint64_t);
static unsigned failures;
uint64_t callback(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,
                  uint64_t f,uint64_t g,uint64_t h,uint64_t i) {
    if (c!=3 || d!=4 || e!=5 || f!=6 || g!=7 || h!=8 || i!=9) ++failures;
    return div_u64(a)+rem_u64(b)+c+d+e+f+g+h+i+ASTER_CORRUPT_CALLBACK;
}
int main(void) {
''')
    driver.extend(checks)
    driver.append('''for (uint64_t x=0;x<257;++x) {
    uint64_t y=x*17+5;
    if (chain(x,y)!=2*(x/7+y%7)+(x^y)+42) return 20;
}
if (failures) return 21;
puts("ASTER-WIN64-DIV-PASS"); return 0; }
''')
    records = []
    with tempfile.TemporaryDirectory(prefix='aster-win64-div-') as temporary:
        root = Path(temporary)
        src, caller, host = root / 'provider.aa', root / 'client.aa', root / 'host.c'
        src.write_text('\n'.join(provider) + '\n')
        caller.write_text(client)
        host.write_text('\n'.join(driver) + '\n')
        # C compilation is independent of AsterASM policy/allocation settings.
        host_object = root / 'host.obj'
        command([args.clang, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-c', host, '-o', host_object])
        bad_host = root / 'bad-host.obj'
        command([args.clang, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-DASTER_CORRUPT_CALLBACK=1', '-c', host, '-o', bad_host])
        for policy in ('hot', 'size'):
            for mode, options in (('normal', []), ('spilled', ['--reg-limit', '2']),
                                  ('unpromoted', ['--no-promote'])):
                env = os.environ.copy()
                env['ASTERASM_DIV_POLICY'] = policy
                objects = []
                for source in (src, caller):
                    obj = source.with_suffix('.obj')
                    command([args.compiler, 'build', '-O'+args.opt_level, '--target', 'x86_64', '--abi', 'win64',
                             '--emit', 'obj', '--format', 'coff', '-j', '1', *options,
                             '-o', obj, source], env)
                    if obj.read_bytes()[:2] != b'\x64\x86':
                        raise AssertionError('expected actual AMD64 COFF, not embedded BIN')
                    objects.append(obj)
                executable = root / 'test.exe'
                command([args.clang, '-fuse-ld=lld', host_object, *objects, '-o', executable])
                result = command([executable])
                if result.stdout.strip() != 'ASTER-WIN64-DIV-PASS':
                    raise AssertionError(result.stdout)
                bad_executable = root / 'corrupted.exe'
                command([args.clang, '-fuse-ld=lld', bad_host, *objects, '-o', bad_executable])
                negative = command([bad_executable], expected=20)
                if 'ASTER-WIN64-DIV-PASS' in negative.stdout:
                    raise AssertionError('corrupted callback accepted')
                missing = root / 'unresolved.exe'
                unresolved = subprocess.run([args.clang, '-fuse-ld=lld', str(host_object),
                                             str(objects[1]), '-o', str(missing)],
                                            capture_output=True, text=True, timeout=60)
                if not unresolved.returncode or missing.exists() or 'undefined symbol' not in unresolved.stderr:
                    raise AssertionError(('unresolved provider accepted', unresolved.stderr))
                records.append({'policy': policy, 'mode': mode, 'exit_code': result.returncode,
                                'positive_checks': positive_per_run + 257 * 2,
                                'corrupted_callback_exit': negative.returncode,
                                'unresolved_link_exit': unresolved.returncode,
                                'provider_coff_sha256': hashlib.sha256(objects[0].read_bytes()).hexdigest(),
                                'client_coff_sha256': hashlib.sha256(objects[1].read_bytes()).hexdigest(),
                                'exe_sha256': hashlib.sha256(executable.read_bytes()).hexdigest()})
    if args.out:
        Path(args.out).write_text(json.dumps({'records': records}, indent=2) + '\n')
    print(f'PASS: {len(records)} actual COFF/Win64 C ABI executions, '
          f'{sum(r["positive_checks"] for r in records)} checks')


if __name__ == '__main__':
    main()
