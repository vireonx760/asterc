#!/usr/bin/env python3
"""Adversarial ELF64 cross-ISA disassembler tests; Clang is only an optional oracle.

Runs on little-endian synthetic ELF64 inputs even when no cross assembler is
installed. No LLVM runtime dependency in asterdump itself.
"""
import argparse
import json
import pathlib
import random
import struct
import subprocess
import sys
import tempfile


def sh(name, typ, flags, offset, size, link=0, info=0, align=1, entsize=0):
    return struct.pack('<IIQQQQIIQQ', name, typ, flags, 0, offset, size, link, info, align, entsize)


def elf64(machine, words, symbol_name=b'func', section_name=b'.text', use_rel=False):
    # Construct standard ELF64 ET_REL: NULL, .text, .strtab, .symtab,
    # .rela.text, .shstrtab. The symbol can contain JSON special characters.
    text = struct.pack('<' + 'I'*len(words), *words)
    strings = b'\0' + symbol_name + b'\0external\0'
    str_sym = 1
    str_ext = len(strings) - len(b'external\0')
    relname = b'.rel.text' if use_rel else b'.rela.text'
    sections = b'\0' + section_name + b'\0.strtab\0.symtab\0' + relname + b'\0.shstrtab\0'
    names = [0, 1, len(section_name) + 2, len(section_name) + 10,
             len(section_name) + 18, len(section_name) + 29]
    # Name offsets constructed directly to make the generator resilient to renaming
    names = [sections.index(n) if n else 0 for n in
             (b'', section_name, b'.strtab', b'.symtab', relname, b'.shstrtab')]
    symtab = b'\0'*24 + struct.pack('<IBBHQQ', str_sym, 0x12, 0, 1, 0, 4*len(words)) + \
             struct.pack('<IBBHQQ', str_ext, 0x12, 0, 0, 0, 0)
    rtyp = 283 if machine == 183 else 17
    rels = (struct.pack('<QQ', 0, (2 << 32) | rtyp) if use_rel
            else struct.pack('<QQq', 0, (2 << 32) | rtyp, -4))
    parts = [text, strings, symtab, rels, sections]
    locations = []
    data = bytearray(64)
    for chunk in parts:
        while len(data) % 8: data.append(0)
        locations.append(len(data))
        data += chunk
    while len(data) % 8: data.append(0)
    shoff = len(data)
    headers = [sh(0, 0, 0, 0, 0),
               sh(names[1], 1, 6, locations[0], len(text), align=4),
               sh(names[2], 3, 0, locations[1], len(strings)),
               sh(names[3], 2, 0, locations[2], len(symtab), link=2, info=1, align=8, entsize=24),
               sh(names[4], 9 if use_rel else 4, 0, locations[3], len(rels),
                  link=3, info=1, align=8, entsize=16 if use_rel else 24),
               sh(names[5], 3, 0, locations[4], len(sections))]
    data += b''.join(headers)
    hdr = bytearray(64)
    hdr[0:16] = b'\x7fELF\x02\x01\x01' + bytes(9)
    struct.pack_into('<HHIQQQIHHHHHH', hdr, 16, 1, machine, 1, 0, 0, shoff, 0,
                     64, 0, 0, 64, len(headers), 5)
    data[0:64] = hdr
    return bytes(data)


def run(dumper, arch, blob, dirpath, *opt):
    path = dirpath / 'sample.o'
    path.write_bytes(blob)
    return subprocess.run([dumper, '--target', arch, *opt, str(path)],
                          capture_output=True, text=True, encoding='utf-8', timeout=8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dumper', required=True)
    ap.add_argument('--clang', default='')
    args = ap.parse_args()
    n = 0
    with tempfile.TemporaryDirectory(prefix='asterdump-elf-') as tmp:
        dest = pathlib.Path(tmp)
        for arch, machine, words, fragment in (
                ('aarch64', 183, (0x14000002, 0xd65f03c0), 'b 0x8'),
                ('riscv64', 243, (0x0080006f, 0x00008067), 'jal zero, 0x8')):
            blob = elf64(machine, words, symbol_name='a"b\\cλ'.encode('utf-8'))
            proc = run(args.dumper, arch, blob, dest, '--format', 'json')
            assert proc.returncode == 0, (proc.returncode, proc.stderr)
            payload = json.loads(proc.stdout)
            assert payload['container'] == 'elf64-rel'
            assert payload['sections'][0]['instructions'][0]['mnemonic'] == fragment, payload
            assert payload['symbols'][0]['name'] == 'a"b\\cλ', payload
            assert payload['relocations'][0]['addend'] == -4, payload
            assert payload['relocations'][0]['symbol'] == 'external', payload
            assert payload['unknown'] == 0 and payload['recognized'] == 2
            rel_blob = elf64(machine, words, use_rel=True)
            rel_result = run(args.dumper, arch, rel_blob, dest, '--format', 'json')
            assert rel_result.returncode == 0, rel_result.stderr
            rel = json.loads(rel_result.stdout)['relocations'][0]
            assert rel['explicit_addend'] is False and rel['addend'] == 0, rel
            n += 2
            assert run(args.dumper, arch, blob, dest, '--strict').returncode == 0
            assert run(args.dumper, arch, blob, dest, '--base', '0x2000', '--format', 'json').returncode == 0
            # Correctly reject mismatched machine architecture.
            opposite = 'riscv64' if arch == 'aarch64' else 'aarch64'
            assert run(args.dumper, opposite, blob, dest).returncode == 2
            n += 4
            # Corrupt every high-risk header dimension, fail closed.
            for offset, value in ((4, 1), (5, 2), (16, 4), (18, 0), (40, 0xff),
                                  (58, 63), (60, 0), (62, 0xff)):
                invalid = bytearray(blob)
                invalid[offset] = value
                assert run(args.dumper, arch, invalid, dest).returncode == 2, (arch, offset)
                n += 1
            # Too-small/truncated and broken string-table and reloc target.
            for invalid in (blob[:64], blob[:-1], blob[:25], b'\x7fELF' + bytes(70)):
                assert run(args.dumper, arch, invalid, dest).returncode == 2
                n += 1
            # Mutate indexed metadata: section offsets, relocation symbol index,
            # invalid UTF-8 name and impossible symtab string offset.
            shoff = struct.unpack_from('<Q', blob, 40)[0]
            symtab_off = struct.unpack_from('<Q', blob, shoff + 3*64 + 24)[0]
            rela_off = struct.unpack_from('<Q', blob, shoff + 4*64 + 24)[0]
            strtab_off = struct.unpack_from('<Q', blob, shoff + 2*64 + 24)[0]
            corruptions = [
                (shoff + 64 + 24, (1 << 63), 8),  # .text data start beyond EOF
                (shoff + 64 + 32, (1 << 63), 8),  # .text size overflow
                (shoff + 3*64 + 56, 17, 8),      # invalid symtab entry size
                (shoff + 4*64 + 40, 999, 4),     # invalid relocation symtab link
                (shoff + 4*64 + 44, 999, 4),     # invalid relocation target section
                (shoff + 4*64 + 56, 7, 8),       # invalid relocation entry size
                (symtab_off + 24, 0xffffffff, 4),# invalid name index
                (rela_off, 0xffffffffffffffff, 8),  # offset outside text
                (rela_off + 8, (999 << 32) | 17, 8),# invalid symbol index
                (strtab_off + 1, 0xff, 1),        # invalid UTF-8
            ]
            for off, val, width in corruptions:
                trial = bytearray(blob)
                trial[off:off+width] = val.to_bytes(width, 'little')
                result = run(args.dumper, arch, trial, dest)
                assert result.returncode == 2, (arch, off, result.stdout, result.stderr)
                n += 1
            # Deterministic AFL-style byte mutations; no crash, hang or false success
            # for mutations that change critical ELF structure bytes.
            rng = random.Random(0xA57E + machine)
            for _ in range(180):
                off = rng.choice([40, 41, 42, 43, 58, 59, 60, 61, 62, 63,
                                  len(blob)-1, len(blob)-8, len(blob)-64])
                trial = bytearray(blob)
                trial[off] ^= rng.randrange(1, 256)
                result = run(args.dumper, arch, trial, dest, '--format', 'json')
                assert result.returncode in (0, 2, 3), (result.returncode, result.stderr)
                if result.returncode == 0:
                    json.loads(result.stdout)
                n += 1
            unknown = elf64(machine, [0xffffffff])
            assert run(args.dumper, arch, unknown, dest, '--strict').returncode == 3
            n += 1
        if args.clang:
            for arch, triple, options, program, needle in (
                    ('aarch64', 'aarch64-linux-gnu', [],
                     '.text\n.global entry\nentry:\n b 1f\n bl external\n1: ret\n', 'R_AARCH64_CALL26'),
                    ('riscv64', 'riscv64-linux-gnu', ['-march=rv64i', '-mabi=lp64'],
                     '.text\n.global entry\nentry:\n beq a0,a1,1f\n call external\n1: ret\n', 'R_RISCV_CALL_PLT')):
                asm = dest / 'real.s'; obj = dest / 'real.o'
                asm.write_text(program)
                compile_result = subprocess.run([args.clang, f'--target={triple}', *options,
                                                 '-c', str(asm), '-o', str(obj)],
                                                capture_output=True, text=True, timeout=15)
                assert compile_result.returncode == 0, compile_result.stderr
                p = subprocess.run([args.dumper, '--target', arch, '--format', 'json', str(obj)],
                                   capture_output=True, text=True, timeout=10)
                assert p.returncode == 0, p.stderr
                parsed = json.loads(p.stdout)
                assert needle in [r['type_name'] for r in parsed['relocations']], parsed
                assert any(i['relocation_pending'] for s in parsed['sections']
                           for i in s['instructions']), parsed
                assert any(s['name'] == 'entry' for s in parsed['symbols'])
                n += 2
            # Decode immediate shifts and masks exercised by constant-division
            # lowering and branch forms; Clang is a reference encoder, not a
            # runtime library used by the dumper.
            variants = (
                ('aarch64', 'aarch64-linux-gnu', [],
                 'lsr x0,x1,#3\nlsr w2,w3,#10\nubfm x4,x5,#0,#2\n'
                 'lsl x8,x9,#4\ncbz x0,1f\ncbnz w2,1f\nb.ne 1f\n1: ret\n',
                 ('lsr x0, x1, #3', 'lsr w2, w3, #10', 'ubfm x4, x5, #0, #2',
                  'lsl x8, x9, #4', 'cbz x0, 0x1c', 'cbnz w2, 0x1c', 'b.ne 0x1c')),
                ('riscv64', 'riscv64-linux-gnu', ['-march=rv64i', '-mabi=lp64'],
                 'srli a0,a1,3\nsrai a2,a3,10\nslli a4,a5,4\n'
                 'andi a0,a0,7\nori a0,a0,32\nxori a0,a0,12\nret\n',
                 ('srli a0, a1, #3', 'srai a2, a3, #10', 'slli a4, a5, #4',
                  'andi a0, a0, #7', 'ori a0, a0, #32', 'xori a0, a0, #12')),
            )
            for arch, triple, options, program, expected in variants:
                asm = dest / 'ops.s'; obj = dest / 'ops.o'
                asm.write_text('.text\n.global entry\nentry:\n' + program)
                built = subprocess.run([args.clang, f'--target={triple}', *options,
                                       '-c', str(asm), '-o', str(obj)],
                                       capture_output=True, text=True, timeout=15)
                assert built.returncode == 0, built.stderr
                p = subprocess.run([args.dumper, '--target', arch, '--strict', '--format', 'json', str(obj)],
                                   capture_output=True, text=True, timeout=10)
                assert p.returncode == 0, (p.stderr, p.stdout)
                insns = [i['mnemonic'] for sec in json.loads(p.stdout)['sections']
                         for i in sec['instructions']]
                assert all(x in insns for x in expected), (insns, expected)
                n += len(expected)
    print(f'asterdump ELF: {n} positive/negative/mutation checks passed')


if __name__ == '__main__':
    try: main()
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        print(f'ELF test failed: {exc}', file=sys.stderr)
        sys.exit(1)
