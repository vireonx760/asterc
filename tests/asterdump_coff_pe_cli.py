#!/usr/bin/env python3
"""PE32+/COFF reader: independent Windows-target Clang/LLD oracle and mutations.

All constructed container fields are intentionally byte-level rather than
mirroring the C parser's internal data structures. No Windows host is needed.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def run(dumper, arch, file, *opts):
    return subprocess.run([dumper, '--target', arch, '--format', 'json', *opts, str(file)],
                          text=True, capture_output=True, timeout=20)


def patch(payload, at, fmt, value):
    blob = bytearray(payload)
    struct.pack_into(fmt, blob, at, value)
    return bytes(blob)


def stub_obj(arch):
    """One fully-defined .text symbol and one external COFF relocation."""
    a64 = arch == 'aarch64'
    code = bytes.fromhex('00000094c0035fd6' if a64 else 'e800000000c3')
    sec = struct.pack('<8sIIIIIIHHI', b'.text\0\0\0', 0, 0, len(code), 60,
                      60 + len(code), 0, 1, 0, 0x60000020)
    symbols_offset = 60 + len(code) + 10
    hdr = struct.pack('<HHIIIHH', 0xaa64 if a64 else 0x8664, 1, 0,
                      symbols_offset, 2, 0, 0)
    relocation = struct.pack('<IIH', 0 if a64 else 1, 1, 3 if a64 else 4)
    syms = (struct.pack('<8sIhHBB', b'demo\0\0\0\0', 0, 1, 0, 2, 0) +
            struct.pack('<8sIhHBB', b'external', 0, 0, 0, 2, 0))
    return hdr + sec + code + relocation + syms + struct.pack('<I', 4)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--dumper', required=True)
    p.add_argument('--clang')
    p.add_argument('--lld-link')
    args = p.parse_args()
    count = 0
    with tempfile.TemporaryDirectory(prefix='aster-pe-coff-') as tmp:
        d = Path(tmp)
        for arch in ('x86_64', 'aarch64'):
            code = stub_obj(arch)
            obj = d / f'{arch}.obj'
            obj.write_bytes(code)
            result = run(args.dumper, arch, obj, '--strict')
            assert result.returncode == 0, result.stderr
            data = json.loads(result.stdout)
            assert data['container'] == 'coff-object'
            assert data['sections'][0]['name'] == '.text'
            assert data['sections'][0]['address'] == 0
            assert data['sections'][0]['instructions'][0]['relocation_pending'] is True
            assert data['relocations'][0]['symbol'] == 'external'
            assert data['relocations'][0]['type_name'] == (
                'IMAGE_REL_ARM64_BRANCH26' if arch == 'aarch64' else 'IMAGE_REL_AMD64_REL32')
            assert data['recognized'] == 2 and data['unknown'] == 0
            assert len(data['symbols']) == 1 and data['symbols'][0]['name'] == 'demo'
            count += 8
            assert run(args.dumper, 'aarch64' if arch == 'x86_64' else 'x86_64', obj).returncode == 2
            count += 1
            # COFF cannot read outside the file, accept arbitrary machine
            # values, reference nonexistent/auxiliary symbols or use unbounded
            # name offsets. Every adversarial mutation must fail closed.
            sympos = struct.unpack_from('<I', code, 8)[0]
            wrong = [
                code[:19],
                patch(code, 2, '<H', 0xffff),
                patch(code, 8, '<I', 0xfffffffe),
                patch(code, 12, '<I', 0xffffffff),
                patch(code, 16, '<H', 0xffff),
                patch(code, 20 + 16, '<I', 0xfffffffe),
                patch(code, 20 + 20, '<I', 0xfffffffe),
                patch(code, 20 + 24, '<I', 0xfffffffe),
                patch(code, 20 + 32, '<H', 0xffff),
                patch(code, 20 + 36, '<I', 0x61000020),
                patch(code, 60 + (8 if arch == 'aarch64' else 6) + 4, '<I', 0xffffffff),
                patch(code, sympos + 17, '<B', 2),
                patch(code, sympos + 18, '<I', 0xffffffff),
            ]
            for i, bad in enumerate(wrong):
                broken = d / f'bad-{arch}-{i}.obj'
                broken.write_bytes(bad)
                r = run(args.dumper, arch, broken)
                assert r.returncode == 2, (arch, i, r.returncode, r.stdout, r.stderr)
                count += 1
            # Long symbol (COFF string table) without NUL is invalid.
            altered = bytearray(code)
            altered[sympos + 18:sympos + 26] = struct.pack('<II', 0, 5)
            altered.extend(b'abc')
            altered[-7:-3] = struct.pack('<I', 7)
            extra = d / f'bad-name-{arch}.obj'
            extra.write_bytes(altered)
            assert run(args.dumper, arch, extra).returncode == 2
            count += 1
            if args.clang:
                target = 'aarch64-pc-windows-msvc' if arch == 'aarch64' else 'x86_64-pc-windows-msvc'
                asm = d / f'oracle-{arch}.s'
                asm.write_text('.text\n.globl demo\ndemo:\n' +
                               (' bl external\n ret\n' if arch == 'aarch64'
                                else ' call external\n ret\n'), encoding='ascii')
                real = d / f'oracle-{arch}.obj'
                built = subprocess.run([args.clang, '-target', target, '-c', str(asm), '-o', str(real)],
                                       text=True, capture_output=True, timeout=30)
                assert built.returncode == 0, built.stderr
                output = run(args.dumper, arch, real, '--strict')
                assert output.returncode == 0, output.stderr
                oracle = json.loads(output.stdout)
                assert oracle['relocations'][0]['type_name'] == data['relocations'][0]['type_name']
                assert oracle['relocations'][0]['symbol'] == 'external'
                assert oracle['recognized'] == 2
                count += 4
                # COFF long names are represented by decimal offsets into
                # the string table; section/symbol names must not be cut at
                # eight bytes or mistaken for executable raw data.
                if arch == 'x86_64':
                    long_src = d / 'long-coff.s'
                    long_obj = d / 'long-coff.obj'
                    long_src.write_text('.section .text$very_long_name,"xr"\n'
                                        '.globl lengthy_symbol_name\n'
                                        'lengthy_symbol_name: ret\n', encoding='ascii')
                    built = subprocess.run([args.clang, '-target', target, '-c', str(long_src), '-o', str(long_obj)],
                                           capture_output=True, text=True, timeout=30)
                    assert built.returncode == 0, built.stderr
                    result = run(args.dumper, arch, long_obj, '--strict')
                    assert result.returncode == 0, result.stderr
                    parsed = json.loads(result.stdout)
                    assert parsed['sections'][0]['name'] == '.text$very_long_name'
                    assert any(s['name'] == 'lengthy_symbol_name' for s in parsed['symbols'])
                    count += 4
                if args.lld_link:
                    exeasm = d / f'exec-{arch}.s'
                    exeasm.write_text('.text\n.globl demo\ndemo:\n ret\n', encoding='ascii')
                    exeobj = d / f'exec-{arch}.obj'
                    exe = d / f'exec-{arch}.exe'
                    built = subprocess.run([args.clang, '-target', target, '-c', str(exeasm), '-o', str(exeobj)],
                                           text=True, capture_output=True, timeout=30)
                    assert built.returncode == 0, built.stderr
                    linked = subprocess.run([args.lld_link, '/entry:demo', '/subsystem:console', '/nodefaultlib',
                                             str(exeobj), '/out:'+str(exe)],
                                            text=True, capture_output=True, timeout=30)
                    assert linked.returncode == 0, linked.stderr
                    r = run(args.dumper, arch, exe, '--strict')
                    assert r.returncode == 0, r.stderr
                    data = json.loads(r.stdout)
                    assert data['container'] == 'pe32+-image'
                    assert data['sections'][0]['address'] >= 0x1000
                    assert data['recognized'] >= 1
                    assert data['unknown'] == 0
                    count += 5
                    pe = exe.read_bytes()
                    peoff = struct.unpack_from('<I', pe, 0x3c)[0]
                    for i, (at, fmt, v) in enumerate(((0x3c, '<I', 0xfffffff0),
                                                        (peoff, '<I', 0),
                                                        (peoff+4, '<H', 0),
                                                        (peoff+6, '<H', 0xffff),
                                                        (peoff+20, '<H', 1),
                                                        (peoff+24, '<H', 0x10b),
                                                        (peoff+24+24, '<Q', 0xfffffffffffff000))):
                        bad = d / f'pe-bad-{arch}-{i}.exe'
                        bad.write_bytes(patch(pe, at, fmt, v))
                        result = run(args.dumper, arch, bad)
                        assert result.returncode == 2, (arch, i, result.stderr)
                        count += 1
    print(f'COFF/PE strict parse: {count} positive/negative assertions passed')


if __name__ == '__main__':
    main()
