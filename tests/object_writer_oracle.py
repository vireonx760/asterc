"""Source -> C encoder -> Rust C API -> object/flat, checked with LLVM tools.

LLD is used only as an independent relocation oracle here. Production ELF
linker selection remains mold. No linker is embedded in the compiler library.
"""
import argparse
import pathlib
import subprocess
import tempfile


def run(command):
    return subprocess.run(command, check=True, capture_output=True, text=True, timeout=45).stdout


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--compiler', required=True)
    p.add_argument('--readobj', required=True)
    p.add_argument('--linker', required=True)
    args = p.parse_args()
    count = 0
    with tempfile.TemporaryDirectory(prefix='aster-object-oracle-', dir=pathlib.Path.cwd()) as temp:
        root = pathlib.Path(temp)
        for mode in range(6):
            compiler = [args.compiler, str(mode), '2', '1', 'core']
            functions = [(int(address), bytes.fromhex(data)) for _, address, data in
                         (line.split('\t') for line in run(compiler).splitlines())]
            base = functions[0][0]
            reference = bytearray(functions[-1][0] + len(functions[-1][1]) - base)
            for address, data in functions: reference[address-base:address-base+len(data)] = data
            for fmt in ([1, 4] if mode >= 3 else [1, 2, 3, 4]):
                output = root / f'abi{mode}_format{fmt}.o'
                run([*compiler, str(fmt), str(output)])
                first = output.read_bytes()
                run([*compiler, str(fmt), str(output)])
                assert output.read_bytes() == first, 'Object bytes must be deterministic'
                if fmt == 4:
                    assert first == reference, f'Rust/C flat fixups disagree for ABI {mode}'
                else:
                    info = run([args.readobj, '--file-headers', '--sections', '--symbols', '--relocations', str(output)])
                    assert 'sum9' in info and 'call9' in info and '.text' in info, info
                    assert 'Relocations [' in info, info
                    if fmt == 1:
                        linked = root / f'abi{mode}.bin'
                        run([args.linker, '--no-relax', '--image-base=0', '-Ttext=0x100000', '--entry=add', '--oformat=binary', '-o', str(linked), str(output)])
                        actual = linked.read_bytes()
                        assert actual == reference, f'LLD/C relocation mismatch for ABI {mode}: {len(actual)} vs {len(reference)} bytes'
                count += 1
        print(f'{count} source-to-file configurations: LLVM inspection, ELF link oracle, C/Rust FLAT agreement, determinism passed')


if __name__ == '__main__':
    try: main()
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f'{exc.cmd}\n{exc.stderr}') from exc
