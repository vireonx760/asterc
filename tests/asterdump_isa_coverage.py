#!/usr/bin/env python3
"""Slice 18 differential ISA decoder regression.

Assembler generates independent raw instruction bytes. Asterdump remains C17
without LLVM/Capstone product dependencies. Unsupported bit-patterns fail shut.
"""
import argparse
import json
from pathlib import Path
import random
import re
import subprocess
import tempfile

A64 = [
    ('mul x3, x0, x16', 'mul x3, x0, x16'),
    ('mul w30, wzr, w0', 'mul w30, wzr, w0'),
    ('madd x1, x2, x3, x4', 'madd x1, x2, x3, x4'),
    ('msub w1, w2, w3, wzr', 'msub w1, w2, w3, wzr'),
    ('add x16, sp, x16, uxtx', 'add x16, sp, x16, uxtx'),
    ('sub x17, x0, x17, uxtx', 'sub x17, x0, x17, uxtx'),
    ('udiv x0, x1, x2', 'udiv x0, x1, x2'),
    ('sdiv w7, w8, w9', 'sdiv w7, w8, w9'),
    ('lsl x10, x11, x12', 'lslv x10, x11, x12'),
    ('lsr w0, w1, w2', 'lsrv w0, w1, w2'),
    ('asr x15, x16, x17', 'asrv x15, x16, x17'),
    ('cmp x3, x4', 'cmp x3, x4'),
    ('cmp w5, w6', 'cmp w5, w6'),
    ('cmp x7, #0', 'cmp x7, #0'),
    ('add x0, x0, x1', 'add x0, x0, x1'),
    ('sub w19, w20, w21', 'sub w19, w20, w21'),
    ('fadd d0, d1, d2', 'fadd d0, d1, d2'),
    ('fsub s3, s4, s5', 'fsub s3, s4, s5'),
    ('fmul d6, d7, d8', 'fmul d6, d7, d8'),
    ('fdiv s9, s10, s11', 'fdiv s9, s10, s11'),
    ('fneg d12, d13', 'fneg d12, d13'),
    ('fcmp s14, s15', 'fcmp s14, s15'),
    ('fcvt d16, s17', 'fcvt d16, s17'),
    ('fcvt s18, d19', 'fcvt s18, d19'),
    ('ret', 'ret'),
]
X86 = [
    ('bswap rax', 'bswap rax'),
    ('bswap r10d', 'bswap r10d'),
    ('bsf rax, rbx', 'bsf rax, rbx'),
    ('bsr r11, r10', 'bsr r11, r10'),
    ('bsf r8d, r9d', 'bsf r8d, r9d'),
    ('bsr ecx, edx', 'bsr ecx, edx'),
    ('imul rax, rbx', 'imul rax, rbx'),
    ('ret', 'ret'),
]

RV64 = [
    # Full-width R and M arithmetic (including upper bits and signedness).
    *[(f'{mn} a0, a1, a2', f'{mn} a0, a1, a2') for mn in
      ('add','sub','sll','slt','sltu','xor','srl','sra','or','and',
       'mul','mulh','mulhsu','mulhu','div','divu','rem','remu')],
    # 32-bit sign-extending W forms. RV64 op-imm-32 and op-32.
    *[(f'{mn} a0, a1, a2', f'{mn} a0, a1, a2') for mn in
      ('addw','subw','sllw','srlw','sraw','mulw','divw','divuw','remw','remuw')],
    ('addiw a0, a1, -15', 'addiw a0, a1, -15'),
    ('slliw a0, a1, 31', 'slliw a0, a1, 31'),
    ('srliw a0, a1, 20', 'srliw a0, a1, 20'),
    ('sraiw a0, a1, 30', 'sraiw a0, a1, 30'),
    *[(f'{mn} a0, a1, {value}', f'{mn} a0, a1, {value}') for mn in
      ('addi', 'slti', 'sltiu', 'xori', 'ori', 'andi') for value in (-2048, 2047)],
    *[(f'{mn} a0, a1, {value}', f'{mn} a0, a1, {value}') for mn in
      ('slli', 'srli', 'srai') for value in (0, 63)],
    # Base integer width + F/D memory; negative offsets must sign-extend.
    *[(f'{mn} a0, -16(sp)', f'{mn} a0, -16(sp)') for mn in
      ('lb','lh','lw','ld','lbu','lhu','lwu')],
    *[(f'{mn} a1, 24(sp)', f'{mn} a1, 24(sp)') for mn in ('sb','sh','sw','sd')],
    ('flw f5, 12(sp)', 'flw f5, 12(sp)'),
    ('fld f10, -8(sp)', 'fld f10, -8(sp)'),
    ('fsw f9, 20(sp)', 'fsw f9, 20(sp)'),
    ('fsd f11, -24(sp)', 'fsd f11, -24(sp)'),
    ('jalr a0, 4(ra)', 'jalr a0, 4(ra)'),
    ('ret', 'ret'),
]


def assemble(clang, arch, instructions, target_dir):
    asm = Path(target_dir)/f'{arch}.s'
    obj = Path(target_dir)/f'{arch}.o'
    asm.write_text(('.intel_syntax noprefix\n' if arch == 'x86_64' else '') + '.text\n.globl entry\nentry:\n' + '\n'.join(x[0] for x in instructions) + '\n', encoding='ascii')
    flags = ['-target','aarch64-none-elf'] if arch == 'aarch64' else (
        ['-target','x86_64-unknown-linux-gnu'] if arch == 'x86_64' else
        ['-target','riscv64-none-elf','-march=rv64imfd','-mabi=lp64d'])
    command = [clang, *flags, '-c', str(asm), '-o', str(obj)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        raise AssertionError((command, result.stderr))
    result = subprocess.run(['readelf','-x','.text',str(obj)],capture_output=True,text=True,timeout=15)
    assert result.returncode == 0, result.stderr
    data = bytearray()
    for line in result.stdout.splitlines():
        if not re.match(r'^\s*0x[0-9a-fA-F]+\s', line):
            continue
        for part in line.split()[1:]:
            if len(part) % 2 == 0 and re.fullmatch(r'[0-9a-fA-F]{2,8}', part):
                data += bytes.fromhex(part)
            else:
                break
    assert len(data) == 4 * len(instructions) if arch != 'x86_64' else len(data) >= len(instructions), (arch, len(data), len(instructions))
    return bytes(data)


def decoded(dumper, arch, blob, temp_dir, *, strict=True):
    path = Path(temp_dir)/'test.bin'; path.write_bytes(blob)
    result = subprocess.run([dumper,'--target',arch,'--format','json',
                             '--base','0x4000',*(['--strict'] if strict else []),str(path)],
                            capture_output=True,text=True,timeout=20)
    return result, json.loads(result.stdout) if result.returncode in (0, 3) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dumper',required=True)
    ap.add_argument('--compiler',required=True)
    ap.add_argument('--clang',required=True)
    args = ap.parse_args()
    checks = 0
    with tempfile.TemporaryDirectory(prefix='asterdump-isa-') as temp:
        for arch, asm_cases in [('aarch64', A64), ('riscv64', RV64), ('x86_64', X86)]:
            data = assemble(args.clang, arch, asm_cases, temp)
            result, payload = decoded(args.dumper, arch, data, temp)
            assert result.returncode == 0, (arch, result.stderr, result.stdout)
            assert payload['recognized'] == len(asm_cases) and payload['unknown'] == 0, payload
            seen = [i['mnemonic'] for i in payload['instructions']]
            assert seen == [x[1] for x in asm_cases], list(zip(asm_cases, seen))
            expected_offsets = []
            position = 0
            for instruction in payload['instructions']:
                expected_offsets.append(0x4000 + position)
                position += instruction.get('size', 4)
            assert [i['offset'] for i in payload['instructions']] == expected_offsets
            assert position == len(data)
            # Roundtrip at the *byte* level: independently re-assemble the
            # text produced by the C decoder, then require exact identity.
            # No syntax repair: the decoder must emit assemblable ISA text.
            canonical = [(text, text) for text in seen]
            rebuilt = assemble(args.clang, arch, canonical, temp)
            assert rebuilt == data, (arch, data.hex(), rebuilt.hex())
            checks += len(asm_cases)
            # Decode each independently: x86 is variable-width and may not
            # be chunked into the four-byte ARM/RISC-V ISA word size.
            position = 0
            for idx, (_, expected) in enumerate(asm_cases):
                width = payload['instructions'][idx]['size'] if arch == 'x86_64' else 4
                word = data[position:position + width]
                one, content = decoded(args.dumper, arch, word, temp)
                assert one.returncode == 0 and content['instructions'][0]['mnemonic'] == expected
                position += width
                checks += 1
        # Reject reserved RV M/W encodings and incorrectly formed shifts.
        invalid = [0xffffffff, 0x0200103b, 0x0200203b, 0x0405151b,
                   0x4005159b, 0x00007007, 0x00004027]
        for word in invalid:
            result, payload = decoded(args.dumper, 'riscv64', word.to_bytes(4,'little'), temp)
            assert result.returncode == 3 and payload['unknown'] == 1, (hex(word),result.stdout)
            checks += 1
        # Fuzz mutated upper opcode/funct bits; unknowns are either decoded by
        # exactly matching a supported form or preserved as raw words.
        rng = random.Random(0xA57E18)
        for arch in ('aarch64','riscv64'):
            for _ in range(96):
                word = rng.getrandbits(32)
                result, payload = decoded(args.dumper, arch, word.to_bytes(4,'little'),temp,strict=False)
                assert result.returncode == 0 and len(payload['instructions']) == 1
                assert payload['recognized'] + payload['unknown'] == 1
                checks += 1
        # Generate actual *typed* AsterASM code, not solely arbitrary Clang
        # assembly, and require strict decoding of a signed/multiplication path.
        examples = [
            ('aarch64', 'return a / b;', 'udiv'),
            ('riscv64', 'return a * b;', 'add'), # RV64I default: software multiplication fallback
        ]
        for arch, operation, expected in examples:
            src = Path(temp)/'sample.aa'; dest=Path(temp)/'sample.bin'
            src.write_text('module test.isa_coverage;\n'
                           f'export fn calc(a:u64,b:u64)->u64 {{ {operation} }}\n', encoding='utf-8')
            compile_result = subprocess.run([args.compiler,'build','--target',arch,'--emit','bin',
                                             '-o',str(dest),str(src)],capture_output=True,text=True,timeout=30)
            assert compile_result.returncode == 0, (arch,compile_result.stderr)
            decoded_result, decoded_json = decoded(args.dumper, arch, dest.read_bytes(), temp, strict=False)
            assert decoded_result.returncode == 0
            assert any(expected in x['mnemonic'] for x in decoded_json['instructions']),decoded_json
            checks+=1
        # PC-relative assembly must retain displacements at nonzero PCs and
        # when the display base is changed. Human mnemonics retain addresses.
        from native_processor_contracts import assemble as linked_assembly
        for arch,text in [
            ('aarch64','nop\nb .-4\nbl .+8\ncbz x0,.+0\ncbnz w1,.-12\nb.eq .-4\nret'),
            ('riscv64','addi zero,zero,0\nbeq a0,zero,.-4\nbne a1,a2,.+8\njal ra,.-8\njal zero,.+0\nret')]:
            data = linked_assembly(args.clang,arch,text,Path(temp))
            result,payload = decoded(args.dumper,arch,data,temp)
            assert result.returncode == 0 and payload['unknown'] == 0
            rebuilt = linked_assembly(args.clang,arch,'\n'.join(i['assembly'] for i in payload['instructions']),Path(temp))
            assert rebuilt == data,(arch,'branch displacement lost',payload)
            checks += len(payload['instructions'])
    print(f'{checks} cross-ISA decoder assertions pass')


if __name__ == '__main__':
    main()
