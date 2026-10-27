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
            ('0fafc1', 'imul eax, ecx'), ('480faf07', 'imul rax, [rdi]'),
            ('0fbcc1', 'bsf eax, ecx'), ('480fbc07', 'bsf rax, [rdi]'),
            ('0fbdc1', 'bsr eax, ecx'), ('480fbd07', 'bsr rax, [rdi]'),
            ('4d0faf4c2410', 'imul r9, [r12+0x10]'),
            ('bc01000000', 'mov esp, 0x00000001'),
            ('bd01000000', 'mov ebp, 0x00000001'),
            ('48bc0100000000000000', 'mov rsp, 0x1'),
            ('48bd0100000000000000', 'mov rbp, 0x1'),
            ('6689c0', 'mov ax, ax'), ('4088e0', 'mov al, spl'),
            ('88e0', 'mov al, ah'), ('41b1ff', 'mov r9b, 0xff'),
            ('66b93412', 'mov cx, 0x1234'),
            ('480f45c1', 'cmovne rax, rcx'), ('410f97c1', 'seta r9b'),
            ('4c0fbec4', 'movsx r8, spl'), ('4d63c1', 'movsxd r8, r9d'),
            ('4d6bc17f', 'imul r8, r9, 127'),
            ('4d69c178563412', 'imul r8, r9, 305419896'),
            ('48f7d8', 'neg rax'), ('f6d4', 'not ah'),
            ('49d3e0', 'shl r8, cl'), ('48d1e8', 'shr rax, 1'),
            ('41ffd1', 'call r9'), ('41ffe1', 'jmp r9'),
            ('99', 'cdq'), ('4899', 'cqo'), ('0f0b', 'ud2'),
            ('f34d0fb8c1', 'popcnt r8, r9'),
            ('f366450fb8c1', 'popcnt r8w, r9w'),
            ('664d0f6ec1', 'movq xmm8, r9'),
            ('664d0f7ec1', 'movq r9, xmm8'),
            ('f24d0f2ac1', 'cvtsi2sd xmm8, r9'),
            ('f24d0f2cc1', 'cvttsd2si r8, xmm9'),
            ('f2450f58c1', 'addsd xmm8, xmm9'),
            ('f3450f10c1', 'movss xmm8, xmm9'),
            ('66450f2ec1', 'ucomisd xmm8, xmm9'),
            ('480fbae83f', 'bts rax, 63'),
            ('f04c0fc108', 'lock xadd [rax], r9'),
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

        # Every condition is emitted by compiler branch/select lowering.
        conditions = 'o no b ae e ne be a s ns p np l ge le g'.split()
        for cc, suffix in enumerate(conditions):
            cases = [(bytes([0x70+cc, 0x32]), 'j'+suffix+' 0x34'),
                     (bytes([0x70+cc, 0xfe]), 'j'+suffix+' 0x0'),
                     (bytes([0x0f, 0x80+cc, 0x32, 0, 0, 0]), 'j'+suffix+' 0x38'),
                     (bytes([0x0f, 0x40+cc, 0xc1]), 'cmov'+suffix+' eax, ecx'),
                     (bytes([0x0f, 0x90+cc, 0xc0]), 'set'+suffix+' al')]
            for code, expected in cases:
                data = validate(args.dumper, code, directory, True)
                assert data['unknown'] == 0 and len(data['instructions']) == 1, data
                assert data['instructions'][0]['mnemonic'] == expected, data
                # A missing displacement/operand must not become a shorter
                # instruction that consumes bytes from the next boundary.
                for size in range(1, len(code)):
                    cut = validate(args.dumper, code[:size], directory, True)
                    assert not cut['instructions'][0]['recognized'], (code, size, cut)
                checks += len(code)
        nops = ['90', '6690', '0f1f00', '0f1f4000', '0f1f440000',
                '660f1f440000', '0f1f8000000000', '0f1f840000000000']
        nops += ['66'*count + '0f1f840000000000' for count in range(1, 8)]
        stream = b''.join(bytes.fromhex(code) for code in nops) + bytes.fromhex('733290c3')
        data = validate(args.dumper, stream, directory, True)
        assert data['unknown'] == 0, data
        assert [i['size'] for i in data['instructions']] == list(range(1,16)) + [2,1,1], data
        assert [i['mnemonic'] for i in data['instructions']][-3:] == ['jae 0xac','nop','ret'], data
        for code in nops:
            blob = bytes.fromhex(code)
            for size in range(1, len(blob)):
                cut = validate(args.dumper, blob[:size], directory, True)
                assert not cut['instructions'][0]['recognized'], (code, size, cut)
                checks += 1
        overlong = validate(args.dumper, bytes.fromhex('66'*8+'0f1f840000000000'), directory, True)
        assert not overlong['instructions'][0]['recognized'], overlong
        checks += len(nops) + 2

        # AF is SCAS, not the escaped 0F AF IMUL opcode. Unsupported
        # one-byte forms must not consume the following byte as ModRM.
        # BC/BD without 0F are MOV immediates, including truncated forms.
        for code in ('afc190', '48af07', 'af', 'af07', 'bc', 'bd',
                     'bcc190', 'bdc190', '48bcc190', '48bdc190'):
            data = validate(args.dumper, bytes.fromhex(code), directory, True)
            first = data['instructions'][0]
            assert data['unknown'] > 0 and first['size'] == 1, (code, data)
            assert not first['recognized'], (code, data)
            checks += 1
        for rex in range(0x40, 0x50):
            code = bytes([rex, 0xaf, 0xc1, 0x90])
            data = validate(args.dumper, code, directory, True)
            assert data['instructions'][0]['size'] == 1, data
            assert not data['instructions'][0]['recognized'], data
            assert not data['instructions'][1]['recognized'], data
            assert data['instructions'][1]['offset'] == 1, data
            checks += 1
        # Unknown-byte recovery must preserve the next real instruction.
        data = validate(args.dumper, bytes.fromhex('af0fafc190'), directory, True)
        assert [(i['size'], i['mnemonic']) for i in data['instructions']] == [
            (1, '.byte 0xaf'), (3, 'imul eax, ecx'), (1, 'nop')], data
        checks += 1

        for invalid in ('48', '0f', '0faf', 'c4', '48e8', '48b800',
                        '4889', '488904', '0f84ffff', '48c3', '6666c3'):
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
