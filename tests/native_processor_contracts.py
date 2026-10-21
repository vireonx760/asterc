#!/usr/bin/env python3
"""Native CPU control: exact source bytes, independent assembler and dumper round trips.

This suite does not execute privileged instructions on the host. System emulation
is a separate mandatory qualification command, native_processor_qemu.py.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from isa_inventory import macro_rows

ROOT = Path(__file__).resolve().parents[1]


def run(command, expected=0):
    p = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                       encoding='utf-8', errors='replace', timeout=45)
    assert p.returncode == expected, (command, p.returncode, p.stdout, p.stderr)
    return p


def assemble(clang, arch, text, root, rv_march='rv64ih_zicsr_zifencei_svinval'):
    source, obj, raw = (root / name for name in ('oracle.s', 'oracle.o', 'oracle.bin'))
    source.write_text(('.intel_syntax noprefix\n' if arch == 'x86_64' else '') + '.text\n' + text + '\n')
    triple = {'x86_64': 'x86_64-none-elf', 'aarch64': 'aarch64-none-elf', 'riscv64': 'riscv64-none-elf'}[arch]
    flags = ['-march='+rv_march, '-mabi=lp64'] if arch == 'riscv64' else []
    run([clang, '--target=' + triple, *flags, '-c', source, '-o', obj])
    if arch == 'riscv64':
        # LLVM retains R_RISCV_BRANCH relocations even for same-section forward
        # labels. Resolve them independently; objcopy on ET_REL is not a byte
        # oracle for those instructions. Keep every requested 32-bit form.
        linked = root / 'oracle.elf'
        linker = str(Path(clang).with_name('ld.lld' + ('.exe' if str(clang).endswith('.exe') else '')))
        run([linker, '-m', 'elf64lriscv', '--no-relax', '--image-base=0', '-Ttext=0', '-e', '0', obj, '-o', linked])
        obj = linked
    objcopy = str(Path(clang).with_name('llvm-objcopy' + ('.exe' if str(clang).endswith('.exe') else '')))
    run([objcopy, '--only-section=.text', '-O', 'binary', obj, raw])
    return raw.read_bytes()


def cases():
    result = []
    def add(arch, key, body, asm, features=()):
        result.append((arch, key, body, asm, features))
    for name in ('cpuid', 'rdtsc', 'rdtscp', 'rdmsr', 'wrmsr', 'xgetbv', 'xsetbv',
                 'pause', 'lfence', 'mfence', 'sfence', 'clts', 'invd', 'wbinvd', 'swapgs', 'stac', 'clac'):
        feature = ('rdtscp',) if name == 'rdtscp' else ('xsave',) if name in ('xgetbv', 'xsetbv') else ('smap',) if name in ('stac', 'clac') else ()
        add('x86_64', 'X86_' + name.upper(), name + ';', name, feature)
    for bank, numbers in [('cr', (0, 2, 3, 4, 8)), ('dr', (0, 1, 2, 3, 6, 7))]:
        for number in numbers:
            for write in (False, True):
                for gpr in ('rax', 'r10'):
                    special = bank + str(number)
                    dst, src = (special, gpr) if write else (gpr, special)
                    key = 'X86_' + ('WRITE_' if write else 'READ_') + bank.upper()
                    add('x86_64', key, f'mov {{dst:{dst},src:{src}}};', f'mov {dst}, {src}')
    for name in ('sgdt', 'sidt', 'invlpg'):
        for address in ('rax', 'rbp', 'r12', 'r13'):
            add('x86_64', 'X86_' + name.upper(), f'{name} {{addr:{address}}};', f'{name} [{address}]')
    for name in ('sldt', 'str', 'lldt', 'ltr'):
        for register in ('ax', 'r10w'):
            role = 'dst' if name in ('sldt', 'str') else 'src'
            add('x86_64', 'X86_' + name.upper(), f'{name} {{{role}:{register}}};', f'{name} {register}')
    for name in ('nop', 'yield', 'wfe', 'wfi', 'sev', 'sevl'):
        add('aarch64', 'A64_' + name.upper(), name + ';', name)
    for name in ('dmb', 'dsb', 'isb'):
        for option in ((15,) if name == 'isb' else (1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15)):
            add('aarch64', 'A64_' + name.upper(), f'{name} {{option:{option}}};', f'{name} #{option}')
    for name in ('svc', 'hvc', 'smc', 'brk'):
        for value in (0, 65535):
            add('aarch64', 'A64_' + name.upper(), f'{name} {{imm:{value}}};', f'{name} #{value}')
    for name in ('sys', 'sysl'):
        role = 'dst' if name == 'sysl' else 'src'
        for values in ((0, 7, 5, 0), (7, 15, 15, 7)):
            a, b, c, d = values
            operands = f'{{{role}:x0,op1:{a},crn:{b},crm:{c},op2:{d}}}'
            asm = f'sysl x0, #{a}, c{b}, c{c}, #{d}' if name == 'sysl' else f'sys #{a}, c{b}, c{c}, #{d}, x0'
            add('aarch64', 'A64_' + name.upper(), f'{name} {operands};', asm)
    for line in macro_rows(ROOT / 'include/asterasm/detail/system_registers.inc'):
        match = re.fullmatch(r'AST_SYSTEM_REGISTER\((\w+), "([\w]+)", (0x[0-9a-f]+)u\)', line)
        if not match:
            continue
        architecture, name, value = match.groups()
        code = int(value, 16)
        if architecture == 'AARCH64':
            for spelling in (name, value):
                # Numeric selectors use the architectural generic spelling in LLVM.
                generic = f's{code >> 14}_{code >> 11 & 7}_c{code >> 7 & 15}_c{code >> 3 & 15}_{code & 7}'
                add('aarch64', 'A64_MRS', f'mrs {{dst:x0,sys:{spelling}}};', f'mrs x0, {name if spelling == name else generic}')
                # LLVM rejects writes to known read-only registers; use generic
                # encoded spelling, as supported by the source numeric contract.
                add('aarch64', 'A64_MSR', f'msr {{sys:{spelling},src:x1}};', f'msr {generic}, x1')
        else:
            for spelling in (name, value):
                add('riscv64', 'RV_CSRRS', f'csrrs {{dst:a0,csr:{spelling},src:zero}};', f'csrrs a0, {value}, zero', ('zicsr',))
    for name in ('csrrw', 'csrrs', 'csrrc', 'csrrwi', 'csrrsi', 'csrrci'):
        immediate = name.endswith('i')
        source = 'imm:31' if immediate else 'src:a1'
        add('riscv64', 'RV_' + name.upper(), f'{name} {{dst:a0,csr:mscratch,{source}}};',
            f'{name} a0, mscratch, ' + ('31' if immediate else 'a1'), ('zicsr',))
        if name in ('csrrs', 'csrrc', 'csrrsi', 'csrrci'):
            source = 'imm:0' if immediate else 'src:zero'
            add('riscv64', 'RV_' + name.upper(), f'{name} {{dst:zero,csr:cycle,{source}}};',
                f'{name} zero, cycle, ' + ('0' if immediate else 'zero'), ('zicsr',))
    for pred, succ in ((15, 15), (3, 3), (0, 0)):
        spelling = lambda v: ''.join(c for c, bit in [('i', 8), ('o', 4), ('r', 2), ('w', 1)] if v & bit) or '0'
        add('riscv64', 'RV_FENCE', f'fence {{pred:{pred},succ:{succ}}};', f'fence {spelling(pred)}, {spelling(succ)}')
    add('riscv64', 'RV_FENCE_I', 'fence.i;', 'fence.i', ('zifencei',))
    for src, asid in (('zero', 'zero'), ('a0', 'a1')):
        add('riscv64', 'RV_SFENCE_VMA', f'sfence.vma {{addr:{src},asid:{asid}}};', f'sfence.vma {src}, {asid}')
    for name in ('wfi', 'ecall', 'ebreak'):
        add('riscv64', 'RV_' + name.upper(), name + ';', name)
    for arch, name in (('aarch64', 'eret'), ('riscv64', 'mret'), ('riscv64', 'sret')):
        add(arch, ('A64_' if arch == 'aarch64' else 'RV_') + name.upper(), name + ';', name)
    for field in ('daifset', 'daifclr', 'spsel'):
        for value in range(2 if field == 'spsel' else 16):
            add('aarch64', 'A64_' + field.upper(), f'msr {{pstate:{field},imm:{value}}};', f'msr {field}, #{value}')
    for name in ('hfence.vvma', 'hfence.gvma', 'sinval.vma', 'hinval.vvma', 'hinval.gvma'):
        features = (('h',) if name.startswith('hfence') else ('h', 'svinval') if name.startswith('hinval') else ('svinval',))
        for address in ('zero', 'a0', 't6'):
            for space in ('zero', 'a1', 't6'):
                add('riscv64', 'RV_' + name.upper().replace('.', '_'),
                    f'{name} {{addr:{address},{"vmid" if name.endswith("gvma") else "asid"}:{space}}};',
                    f'{name} {address}, {space}', features)
    for name in ('sfence.w.inval', 'sfence.inval.ir'):
        add('riscv64', 'RV_' + name.upper().replace('.', '_'), name + ';', name, ('svinval',))
    add('aarch64', 'A64_ERET',
        'cbz {reg:x0,target:done}; sub {dst:sp,lhs:sp,rhs:16}; add {dst:sp,lhs:sp,rhs:16}; label {name:done}; eret;',
        'cbz x0,1f\nsub sp,sp,#16\nadd sp,sp,#16\n1: eret')
    for name in ('mret', 'sret'):
        add('riscv64', 'RV_' + name.upper(),
            f'beq {{lhs:a0,rhs:zero,target:done}}; addi {{dst:sp,lhs:sp,rhs:-16}}; '
            f'addi {{dst:sp,lhs:sp,rhs:16}}; label {{name:done}}; {name};',
            f'beq a0,zero,1f\naddi sp,sp,-16\naddi sp,sp,16\n1: {name}')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True)
    p.add_argument('--dumper', required=True)
    p.add_argument('--clang', required=True)
    p.add_argument('--out')
    args = p.parse_args()
    records = []
    corpus = cases()
    inventory = set(re.findall(r'^AST_SYSTEM_FORM\((\w+),', (ROOT / 'include/asterasm/detail/system_instructions.inc').read_text(), re.M))
    assert {x[1] for x in corpus} == inventory, inventory - {x[1] for x in corpus}
    with tempfile.TemporaryDirectory(prefix='aster-native-processor-') as temporary:
        root = Path(temporary)
        source, binary = root / 'input.aa', root / 'input.bin'
        for index, (arch, key, body, asm, features) in enumerate(corpus):
            exception_return = key in ('A64_ERET', 'RV_MRET', 'RV_SRET')
            if arch == 'x86_64':
                body = 'push {src:rbx}; ' + body + ' pop {dst:rbx};'
                asm = 'push rbx\n' + asm + '\npop rbx'
            ending = '' if exception_return else 'ret;'
            source.write_text(f'module processor.forms; export fn run() @naked {{ unsafe exact {arch} {{ {body} {ending} }} }}')
            expected = assemble(args.clang, arch, asm + ('' if exception_return else '\nret'), root)
            for level in ('0', '2', 'z'):
                command = [args.compiler, 'build', '--target', arch, '-O' + level, '-j', '1', '-o', binary, source]
                for feature in features:
                    command += ['--feature', feature]
                run(command)
                actual = binary.read_bytes()
                assert actual == expected, (index, key, body, level, actual.hex(), expected.hex())
                if level == '0':
                    payload = json.loads(run([args.dumper, '--target', arch, '--strict', '--format', 'json', binary]).stdout)
                    assert payload['unknown'] == 0
                    texts = [item.get('assembly',item['mnemonic']) for item in payload['instructions']]
                    rebuilt = assemble(args.clang, arch, '\n'.join(texts), root)
                    assert rebuilt == actual, (key, texts, actual.hex(), rebuilt.hex())
            records.append({'target': arch, 'form': key, 'source': body, 'features': features,
                            'bytes': expected.hex(), 'sha256': hashlib.sha256(expected).hexdigest()})
            if (index + 1) % 64 == 0:
                print(f'PASS: {index + 1}/{len(corpus)} source/oracle/dump cases', flush=True)
        manual_modes = 0
        for arch in ('x86_64', 'aarch64', 'riscv64'):
            selected = {}
            for item in corpus:
                if item[0] == arch and item[1] not in ('A64_ERET', 'RV_MRET', 'RV_SRET'):
                    selected.setdefault(item[1], item)
            body = ' '.join(item[2] for item in selected.values())
            asm = '\n'.join(item[3] for item in selected.values())
            features = sorted({feature for item in selected.values() for feature in item[4]})
            if arch == 'x86_64':
                body = 'push {src:rbx}; ' + body + ' pop {dst:rbx};'
                asm = 'push rbx\n' + asm + '\npop rbx'
            expected = assemble(args.clang, arch, asm + '\nret', root)
            for mode in ('native', 'exact', 'unsafe exact'):
                for abi in (('sysv', 'win64') if arch == 'x86_64' else (None,)):
                    for level in ('0', '2', 'z'):
                        source.write_text(f'module processor.modes; export fn run() @naked {{ {mode} {arch} {{ {body} ret; }} }}')
                        argv = [args.compiler, 'build', '--target', arch, '-O' + level, '-j', '1', '-o', binary, source]
                        if abi: argv += ['--abi', abi]
                        for feature in features: argv += ['--feature', feature]
                        run(argv)
                        assert binary.read_bytes() == expected, (arch, mode, abi, level)
                        manual_modes += 1
        for arch, name in (('aarch64', 'eret'), ('riscv64', 'mret'), ('riscv64', 'sret')):
            expected = assemble(args.clang, arch, name, root)
            for frame in ('@naked', '@frame(validate)'):
                for mode in ('native', 'exact', 'unsafe exact'):
                    for level in ('0', '2', 'z'):
                        source.write_text(f'module processor.returns; export fn run() {frame} {{ {mode} {arch} {{ {name}; }} }}')
                        run([args.compiler, 'build', '--target', arch, '-O' + level, '-j', '1', '-o', binary, source])
                        assert binary.read_bytes() == expected, (arch, name, frame, mode, level)
                        manual_modes += 1
        negative = {
            'x86_64': ['mov {dst:rax,src:cr1};', 'mov {dst:rax,src:cr9};', 'mov {dst:eax,src:cr3};',
                       'mov {dst:dr4,src:rax};', 'mov {dst:rsp,src:cr3};', 'rdmsr {dst:eax};',
                       'sidt {addr:eax};', 'invlpg {addr:0};', 'ltr {src:eax};', 'cpuid;'],
            'aarch64': ['mrs {dst:w0,sys:sctlr_el1};', 'msr {sys:0x7fff,src:x0};', 'mrs {dst:sp,sys:currentel};',
                        'mrs {dst:x18,sys:currentel};', 'mrs {dst:x30,sys:currentel};', 'dmb {option:0};',
                        'dsb {option:4};', 'isb {option:11};', 'svc {imm:65536};', 'sys {src:x0,op1:8,crn:0,crm:0,op2:0};'],
            'riscv64': ['csrrw {dst:a0,csr:cycle,src:zero};', 'csrrsi {dst:a0,csr:cycle,imm:1};',
                        'csrrwi {dst:a0,csr:mscratch,imm:32};', 'csrrs {dst:sp,csr:mscratch,src:zero};',
                        'csrrs {dst:a0,csr:4096,src:zero};', 'fence {pred:16,succ:0};', 'sfence.vma {addr:0,asid:zero};']}
        rejections = 0
        for arch, bodies in negative.items():
            for body in bodies:
                # CPUID without saving RBX must fail ABI preservation.
                source.write_text(f'module processor.invalid; export fn run() @naked {{ unsafe exact {arch} {{ {body} ret; }} }}')
                binary.unlink(missing_ok=True)
                run([args.compiler, 'build', '--target', arch, '--feature', 'zicsr' if arch == 'riscv64' else 'sse2' if arch == 'x86_64' else 'fp16',
                     '-j', '1', '-o', binary, source], expected=1)
                assert not binary.exists(), body
                rejections += 1
        for arch, body, feature in [('x86_64', 'xgetbv;', 'xsave'), ('x86_64', 'stac;', 'smap'),
                                     ('x86_64', 'rdtscp;', 'rdtscp'), ('riscv64', 'csrrs {dst:a0,csr:mscratch,src:zero};', 'zicsr'),
                                     ('riscv64', 'fence.i;', 'zifencei')]:
            source.write_text(f'module processor.feature; export fn run() @naked {{ unsafe exact {arch} {{ {body} ret; }} }}')
            binary.unlink(missing_ok=True)
            run([args.compiler, 'build', '--target', arch, '-j', '1', '-o', binary, source], expected=1)
            assert not binary.exists(), feature
            rejections += 1
        for arch, body in (
                ('aarch64', 'eret; ret;'), ('aarch64', 'eret {imm:0};'),
                ('aarch64', 'sub {dst:sp,lhs:sp,rhs:16}; eret;'),
                ('aarch64', 'mrs {dst:x19,sys:currentel}; eret;'),
                ('aarch64', 'msr {pstate:daifset,imm:16}; ret;'),
                ('aarch64', 'msr {pstate:spsel,imm:2}; ret;'),
                ('aarch64', 'msr {pstate:unknown,imm:0}; ret;'),
                ('aarch64', 'msr {pstate:daifclr,imm:0,sys:daif}; ret;'),
                ('riscv64', 'mret; ret;'), ('riscv64', 'sret; fence {pred:3,succ:3};'),
                ('riscv64', 'addi {dst:sp,lhs:sp,rhs:-16}; mret;'),
                ('riscv64', 'csrrs {dst:s0,csr:mscratch,src:zero}; sret;'),
                ('riscv64', 'mret {src:zero};'),
                ('riscv64', 'hfence.gvma {addr:a0,vmid:32}; ret;'),
                ('riscv64', 'hfence.gvma {addr:a0,asid:a1}; ret;'),
                ('aarch64', 'cbz {reg:x0,target:done}; sub {dst:sp,lhs:sp,rhs:16}; label {name:done}; eret;'),
                ('riscv64', 'beq {lhs:a0,rhs:zero,target:done}; addi {dst:sp,lhs:sp,rhs:-16}; label {name:done}; mret;')):
            source.write_text(f'module processor.invalid_return; export fn run() @naked {{ native {arch} {{ {body} }} }}')
            binary.unlink(missing_ok=True)
            argv = [args.compiler, 'build', '--target', arch, '-j', '1', '-o', binary, source]
            if arch == 'riscv64': argv += ['--feature', 'zicsr', '--feature', 'h']
            run(argv, expected=1)
            assert not binary.exists(), body
            rejections += 1
        for body, features in [('hfence.vvma {addr:zero,asid:zero};', ()),
                               ('sinval.vma {addr:zero,asid:zero};', ()),
                               ('hinval.gvma {addr:zero,vmid:zero};', ('h',)),
                               ('hinval.vvma {addr:zero,asid:zero};', ('svinval',))]:
            source.write_text(f'module processor.invalid_feature; export fn run() @naked {{ native riscv64 {{ {body} ret; }} }}')
            binary.unlink(missing_ok=True)
            argv = [args.compiler, 'build', '--target', 'riscv64', '-j', '1', '-o', binary, source]
            for feature in features: argv += ['--feature', feature]
            run(argv, expected=1)
            assert not binary.exists(), body
            rejections += 1
        # Opaque bytes retain their old unsafe/exact/naked boundary. Native CPU
        # forms do not inherit the raw-byte restriction.
        for mode in ('native', 'exact'):
            source.write_text(f'module processor.boundary; export fn run() @naked {{ {mode} x86_64 {{ bytes {{0xc3}}; }} }}')
            binary.unlink(missing_ok=True)
            run([args.compiler, 'build', '--target', 'x86_64', '-j', '1', '-o', binary, source], expected=1)
            assert not binary.exists()
            rejections += 1
    report = {'instruction_forms': len(inventory), 'oracle_cases': len(records), 'exact_binaries': len(records) * 3,
              'levels': ['O0', 'O2', 'Oz'], 'manual_modes': manual_modes,
              'rejections': rejections, 'records': records}
    if args.out: Path(args.out).write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(inventory)} system forms, {len(records)} independent byte/dump round trips, '
          f'{len(records) * 3} exact binaries, {manual_modes} native/exact/manual variants and {rejections} rejected contracts')


if __name__ == '__main__':
    main()
