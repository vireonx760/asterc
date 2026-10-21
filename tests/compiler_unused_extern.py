"""Unused header declarations must not become mandatory linker symbols."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(args):
    result = subprocess.run([str(x) for x in args], check=True, capture_output=True,
                            text=True, timeout=60)
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--clang', required=True)
    parser.add_argument('--readobj', required=True)
    args = parser.parse_args()
    fixture = Path(__file__).parent / 'compiler_e2e/unused_extern.aa'
    with tempfile.TemporaryDirectory(prefix='aster-unused-extern-') as tmp:
        root = Path(tmp)
        for arch, abi, fmt in [('x86_64','win64','coff'), ('x86_64','sysv','elf'),
                               ('aarch64','aapcs64','elf'), ('riscv64','lp64','elf')]:
            obj = root / f'{abi}.obj'
            run([args.compiler, 'build', '--target', arch, '--abi', abi,
                 '--emit', 'obj', '--format', fmt, '-o', obj, fixture])
            symbols = run([args.readobj, '--symbols', '--relocations', obj])
            assert 'missing_unused' not in symbols, symbols
            for name in ['needed_external', 'address_external', 'use_external', 'take_address']:
                assert name in symbols, name
        host = root / 'host.c'
        host.write_text('''#include <stdint.h>
uint64_t needed_external(uint64_t x) { return x + 40; }
uint64_t address_external(uint64_t x) { return x; }
extern uint64_t use_external(uint64_t);
extern uintptr_t take_address(void);
int main(void) {
    return use_external(1) != 42 || take_address() != (uintptr_t)&address_external;
}
''')
        exe = root / 'check.exe'
        run([args.clang, '-fuse-ld=lld', host, root / 'win64.obj', '-o', exe])
        run([exe])
    print('PASS: unused extern pruned on 4 ABIs; Win64 call/address link and run')


if __name__ == '__main__':
    main()
