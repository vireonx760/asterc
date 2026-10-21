#!/usr/bin/env python3
"""x86-64 BIN/ELF decoding, variable instruction widths, and malformed-input tests.

Runtime decoder is C17-only; Clang is an optional independent assembler oracle.
"""
import argparse
import json
import pathlib
import random
import subprocess
import tempfile


def run(dumper, arch, blob, directory, *args):
    path = directory / 'input.bin'
    path.write_bytes(blob)
    return subprocess.run([dumper, '--target', arch, '--format', 'json', *args, str(path)],
                          text=True, capture_output=True, timeout=10)


def validate(dumper, blob, directory, strict=False):
    result = run(dumper, 'x86_64', blob, directory, *(['--strict'] if strict else []))
    assert result.returncode in (0, 3), (result.returncode, result.stderr)
    data = json.loads(result.stdout)
    instructions = data['instructions']
    assert data['recognized'] + data['unknown'] == len(instructions)
    assert ''.join(i['bytes'] for i in instructions) == blob.hex(), (blob, instructions)
    offset = 0
    for instr in instructions:
        assert instr['offset'] == offset
        assert 1 <= instr['size'] <= 15
        offset += instr['size']
    assert offset == len(blob)
    if strict:
        assert result.returncode == (3 if data['unknown'] else 0)
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dumper', required=True)
    ap.add_argument('--clang', default='')
    args = ap.parse_args()
    checks = 0
    with tempfile.TemporaryDirectory(prefix='asterdump-x86-') as tmp:
        directory = pathlib.Path(tmp)
        instructions = [
            ('90', 'nop'), ('c3', 'ret'), ('cc', 'int3'),
            ('53', 'push rbx'), ('5b', 'pop rbx'),
            ('4154', 'push r12'), ('415c', 'pop r12'),
            ('4889d8', 'mov rax, rbx'), ('4989c0', 'mov r8, rax'),
            ('4d01d1', 'add r9, r10'), ('4829d8', 'sub rax, rbx'),
            ('4821d8', 'and rax, rbx'), ('4809d8', 'or rax, rbx'),
            ('4831d8', 'xor rax, rbx'), ('480fafc3', 'imul rax, rbx'),
            ('4883c003', 'add rax, #3'),
            ('488900', 'mov [rax], rax'),
            ('488b442408', 'mov rax, [rsp+0x8]'),
            ('488b45f8', 'mov rax, [rbp-0x8]'),
            ('488d0500000000', 'lea rax, [rip]'),
            ('4b8b448c10', 'mov rax, [r12+r9*4+0x10]'), ('4883e8ff', 'sub rax, #-1'),
            ('48b88877665544332211', 'mov rax, 0x1122334455667788'),
            ('41b801000000', 'mov r8d, 0x00000001'),
            ('eb02', 'jmp 0x4'), ('e900000000', 'jmp 0x5'),
            ('e800000000', 'call 0x5'), ('7402', 'je 0x4'),
            ('7502', 'jne 0x4'), ('0f8400000000', 'je 0x6'),
        ]
        for code, expected in instructions:
            blob = bytes.fromhex(code)
            data = validate(args.dumper, blob, directory, True)
            assert data['recognized'] == 1 and data['unknown'] == 0
            assert data['instructions'][0]['mnemonic'] == expected, (code, data)
            checks += 1

        for invalid in ('48', '0f', '0faf', 'c4', '48e8', '48b800',
                        '4889', '488904', '0f84ffff', '48c3', '666690'):
            data = validate(args.dumper, bytes.fromhex(invalid), directory, True)
            assert data['unknown'] > 0, invalid
            checks += 1

        # Deterministic mutation corpus; most random encodings must be unknown.
        rng = random.Random(0xA57E12)
        for count in range(1, 512):
            blob = bytes(rng.randrange(256) for _ in range(count))
            validate(args.dumper, blob, directory, True)
            checks += 1

        # All 0..255 single bytes terminate and preserve byte ownership.
        for value in range(256):
            validate(args.dumper, bytes([value]), directory, True)
            checks += 1

        clang = pathlib.Path(args.clang) if args.clang else None
        if clang and clang.exists():
            asm = directory / 'reference.s'
            obj = directory / 'reference.o'
            asm.write_text('''\
.text
.globl testcall
testcall:
  push %rbx
  mov %rdi, %rax
  add %rbx, %rax
  call extfunction
  pop %rbx
  ret
''', encoding='ascii')
            build = subprocess.run([str(clang), '-target', 'x86_64-unknown-linux-gnu',
                                    '-c', str(asm), '-o', str(obj)],
                                   text=True, capture_output=True, timeout=15)
            assert build.returncode == 0, build.stderr
            result = subprocess.run([args.dumper, '--target', 'x86_64', '--format', 'json',
                                     '--strict', str(obj)], text=True, capture_output=True, timeout=10)
            assert result.returncode == 0, result.stderr
            data = json.loads(result.stdout)
            inst = data['sections'][0]['instructions']
            assert [e['mnemonic'].split()[0] for e in inst] == [
                'push', 'mov', 'add', 'call', 'pop', 'ret'], inst
            assert inst[3]['relocation_pending'] is True
            assert data['relocations'][0]['type_name'] == 'R_X86_64_PLT32'
            assert data['relocations'][0]['symbol'] == 'extfunction'
            assert data['relocations'][0]['addend'] == -4
            checks += 1
            binary = obj.read_bytes()
            for pos in range(64):
                corrupt = bytearray(binary)
                corrupt[pos] ^= 0xff
                path = directory / 'bad.o'
                path.write_bytes(corrupt)
                output = subprocess.run([args.dumper, '--target', 'x86_64', '--format', 'json',
                                         str(path)], capture_output=True, text=True, timeout=10)
                assert output.returncode in (0, 2, 3), (pos, output.stderr)
                if output.returncode in (0, 3):
                    json.loads(output.stdout)
                checks += 1
            # Wrong target must fail ELF identification rather than silently
            # decoding x86 machine bytes as a different ISA.
            wrong = subprocess.run([args.dumper, '--target', 'aarch64', str(obj)],
                                   capture_output=True, text=True, timeout=10)
            assert wrong.returncode == 2
            checks += 1
    print(f'asterdump x86 validation: {checks} checks PASS')

if __name__ == '__main__':
    main()
