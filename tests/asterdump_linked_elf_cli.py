#!/usr/bin/env python3
"""Differential and adversarial ET_EXEC / ET_DYN section-backed ELF64 tests."""
import argparse
import json
import pathlib
import struct
import subprocess
import tempfile

from asterdump_elf_cli import elf64


def linked_fixture(machine, typ, addr, dynamic_reloc=False):
    # ELF64 ET_EXEC/ET_DYN: symbol st_value and relocation r_offset are
    # virtual addresses, unlike ET_REL where they are section-relative.
    raw = bytearray(elf64(machine, [0x14000002, 0xd65f03c0] if machine == 183
                            else [0x0080006f, 0x00008067]))
    shoff = struct.unpack_from('<Q', raw, 40)[0]
    sym = struct.unpack_from('<Q', raw, shoff + 3 * 64 + 24)[0]
    rela = struct.unpack_from('<Q', raw, shoff + 4 * 64 + 24)[0]
    struct.pack_into('<H', raw, 16, typ)
    struct.pack_into('<Q', raw, shoff + 64 + 16, addr)
    struct.pack_into('<Q', raw, sym + 24 + 8, addr + 4)
    struct.pack_into('<Q', raw, rela, addr)
    if dynamic_reloc:
        # SHT_DYNSYM and dynamic RELA section with sh_info = SHN_UNDEF.
        struct.pack_into('<I', raw, shoff + 3 * 64 + 4, 11)
        struct.pack_into('<I', raw, shoff + 4 * 64 + 44, 0)
    return bytes(raw)


def sectionless_fixture(machine, typ=2):
    # Only ELF header + a single executable PT_LOAD, no section table.
    text = (b'\x02\x00\x00\x14\xc0\x03\x5f\xd6' if machine == 183
            else b'\x6f\x00\x80\x00\x67\x80\x00\x00')
    hdr = bytearray(0x100) + bytearray(text)
    hdr[:16] = b'\x7fELF\x02\x01\x01' + bytes(9)
    struct.pack_into('<HHIQQQIHHHHHH', hdr, 16, typ, machine, 1,
                     0x1000, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', hdr, 64, 1, 5, 0x100, 0x1000, 0x1000,
                     len(text), len(text) + 16, 16)
    return bytes(hdr)


def dump(binary, blob, arch, temp, *flags):
    f = temp / 'input.elf'
    f.write_bytes(blob)
    return subprocess.run([binary, '--target', arch, '--format', 'json', *flags, str(f)],
                          capture_output=True, text=True, timeout=12)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dumper', required=True)
    ap.add_argument('--clang', default='')
    args = ap.parse_args()
    checks = 0
    with tempfile.TemporaryDirectory(prefix='aster-linked-elf-') as tmp:
        tmp = pathlib.Path(tmp)
        for machine, arch in ((183, 'aarch64'), (243, 'riscv64')):
            for typ in (2, 3):
                for dynamic in (False, True):
                    addr = 0x10000 if typ == 2 else 0x2000
                    blob = linked_fixture(machine, typ, addr, dynamic)
                    proc = dump(args.dumper, blob, arch, tmp)
                    assert proc.returncode == 0, proc.stderr
                    obj = json.loads(proc.stdout)
                    assert obj['container'] == ('elf64-exec' if typ == 2 else 'elf64-dyn')
                    assert obj['sections'][0]['address'] == addr
                    assert obj['symbols'][0]['offset'] == 4
                    assert obj['relocations'][0]['offset'] == 0
                    assert obj['relocations'][0]['symbol'] == 'external'
                    assert obj['sections'][0]['instructions'][0]['relocation_pending']
                    assert obj['unknown'] == 0
                    checks += 7
                    based = dump(args.dumper, blob, arch, tmp, '--base', '0x100')
                    assert based.returncode == 0
                    assert json.loads(based.stdout)['sections'][0]['address'] == addr + 0x100
                    checks += 2
                    # Invalid ELF encoding, section-table bounds, virtual
                    # address overflow, bad relocation and symbol address.
                    shoff = struct.unpack_from('<Q', blob, 40)[0]
                    sym = struct.unpack_from('<Q', blob, shoff + 3 * 64 + 24)[0]
                    rela = struct.unpack_from('<Q', blob, shoff + 4 * 64 + 24)[0]
                    bads = []
                    for off, fmt, val in ((16, '<H', 4), (40, '<Q', (1 << 64) - 10),
                                          (shoff + 64 + 16, '<Q', (1 << 64) - 4),
                                          (rela, '<Q', addr + 16),
                                          (sym + 24, '<I', 0xffffffff)):
                        broken = bytearray(blob)
                        struct.pack_into(fmt, broken, off, val)
                        bads.append(bytes(broken))
                    for broken in bads:
                        p = dump(args.dumper, broken, arch, tmp)
                        # Dynamic relocation at a non-code address may safely
                        # be ignored; all others must reject outright.
                        if dynamic and broken == bads[3]:
                            assert p.returncode == 0
                        else:
                            assert p.returncode == 2, p.stderr
                        checks += 1
        for machine, arch in ((183, 'aarch64'), (243, 'riscv64')):
            for typ in (2, 3):
                blob = sectionless_fixture(machine, typ)
                p = dump(args.dumper, blob, arch, tmp, '--strict')
                assert p.returncode == 0, (p.stderr, p.stdout)
                obj = json.loads(p.stdout)
                assert obj['container'] == ('elf64-exec' if typ == 2 else 'elf64-dyn')
                assert obj['program_headers_only'] is True
                assert obj['symbols'] == [] and obj['relocations'] == []
                assert obj['sections'][0]['address'] == 0x1000
                assert obj['sections'][0]['name'] == 'PT_LOAD[PF_X]'
                assert obj['recognized'] == 2
                checks += 7
                # Wrong format, bad offsets/ranges, or invalid permissions
                # cannot turn arbitrary bytes into interpreted machine code.
                corruptions = ((54, '<H', 0), (56, '<H', 0), (32, '<Q', (1 << 64) - 4),
                               (64 + 4, '<I', 0), (64 + 32, '<Q', 0x10000000),
                               (64 + 40, '<Q', 4), (64 + 48, '<Q', 3),
                               (64 + 16, '<Q', (1 << 64) - 4),
                               (64 + 8, '<Q', 0x101))
                for offset, fmt, value in corruptions:
                    trial = bytearray(blob)
                    struct.pack_into(fmt, trial, offset, value)
                    p = dump(args.dumper, trial, arch, tmp)
                    assert p.returncode == 2, (offset, p.stderr)
                    checks += 1
        if args.clang:
            # Relocations must be associated only with their owning section:
            # offset 1 in .text.beta must not taint byte 1 in .text.alpha.
            multi = tmp / 'multitext.s'
            multi_obj = tmp / 'multitext.o'
            multi.write_text('.section .text.alpha,"ax",@progbits\n'
                             'alpha: ret\n ret\n ret\n'
                             '.section .text.beta,"ax",@progbits\n'
                             'beta: call external\n ret\n', encoding='ascii')
            built = subprocess.run([args.clang, '--target=x86_64-unknown-linux-gnu',
                                    '-c', str(multi), '-o', str(multi_obj)],
                                   capture_output=True, text=True, timeout=30)
            assert built.returncode == 0, built.stderr
            p = subprocess.run([args.dumper, '--target', 'x86_64', '--format', 'json', str(multi_obj)],
                               capture_output=True, text=True, timeout=15)
            assert p.returncode == 0, p.stderr
            obj = json.loads(p.stdout)
            alpha = next(s for s in obj['sections'] if s['name'] == '.text.alpha')
            beta = next(s for s in obj['sections'] if s['name'] == '.text.beta')
            assert all(not i['relocation_pending'] for i in alpha['instructions'])
            assert any(i['relocation_pending'] for i in beta['instructions'])
            assert len(obj['relocations']) == 1
            checks += 4
            asm = tmp / 'linked.s'
            exe = tmp / 'linked-exec'
            shared = tmp / 'linked-shared.so'
            asm.write_text('.text\n.globl _start\n_start:\n mov $60,%rax\n xor %rdi,%rdi\n syscall\n', encoding='ascii')
            for typ, options, output in ((2, ['-nostdlib', '-no-pie', '-Wl,--image-base=0x400000'], exe),
                                         (3, ['-nostdlib', '-shared'], shared)):
                p = subprocess.run([args.clang, '--target=x86_64-unknown-linux-gnu', '-fuse-ld=lld',
                                    *options, str(asm), '-o', str(output)],
                                   capture_output=True, text=True, timeout=30)
                assert p.returncode == 0, p.stderr
                p = subprocess.run([args.dumper, '--target', 'x86_64', '--format', 'json', str(output)],
                                   capture_output=True, text=True, timeout=15)
                assert p.returncode == 0, p.stderr
                obj = json.loads(p.stdout)
                assert obj['container'] == ('elf64-exec' if typ == 2 else 'elf64-dyn')
                assert any(x['name'] == '_start' for x in obj['symbols'])
                text = next(s for s in obj['sections'] if s['name'] == '.text')
                assert text['address'] >= (0x400000 if typ == 2 else 0x1000)
                checks += 4
                # A genuinely linked image can be sectionless. The original
                # program-header table remains authoritative for PT_LOAD.
                trimmed = bytearray(output.read_bytes())
                struct.pack_into('<Q', trimmed, 40, 0)
                for off in (58, 60, 62):
                    struct.pack_into('<H', trimmed, off, 0)
                p = dump(args.dumper, trimmed, 'x86_64', tmp)
                assert p.returncode == 0, p.stderr
                obj = json.loads(p.stdout)
                assert obj['program_headers_only'] is True
                assert any(s['name'] == 'PT_LOAD[PF_X]' for s in obj['sections'])
                checks += 2
    print(f'asterdump linked ELF: {checks} assertions passed')


if __name__ == '__main__':
    main()
