#!/usr/bin/env python3
"""Native exact symbolic direct-call fixups, independent raw-bin decoding.

The compiler's C fixup resolver emits final BINs for defined symbols. The
optional Rust ELF object writer is not assumed installed by this test.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def signed(v, n):
    mask = (1 << n) - 1
    v &= mask
    return (v ^ (1 << (n - 1))) - (1 << (n - 1))


def call_targets(arch, code):
    words = struct.unpack('<' + 'I' * (len(code) // 4), code)
    results = []
    for n, word in enumerate(words):
        pc = n * 4
        if arch == 'aarch64' and (word & 0xfc000000) == 0x94000000:
            results.append((pc, pc + 4 * signed(word & 0x03ffffff, 26), 4))
        if arch == 'riscv64' and (word & 0x7f) == 0x17 and ((word >> 7) & 31) == 1:
            if n + 1 >= len(words):
                raise AssertionError('orphan AUIPC')
            lo = words[n+1]
            if (lo & 0x000fffff) != 0x000080e7:
                raise AssertionError(f'bad RV JALR linkage: {lo:08x}')
            hi_imm = signed(word & 0xfffff000, 32)
            low_imm = signed(lo >> 20, 12)
            results.append((pc, pc + hi_imm + low_imm, 8))
    return results


def snippet(arch, variant):
    arm = arch == 'aarch64'
    alloc = 'sub {dst:sp,lhs:sp,rhs:16};' if arm else 'addi {dst:sp,lhs:sp,rhs:-16};'
    free = 'add {dst:sp,lhs:sp,rhs:16};' if arm else 'addi {dst:sp,lhs:sp,rhs:16};'
    save = 'str {src:x30,offset:0};' if arm else 'sd {src:ra,offset:0};'
    load = 'ldr {dst:x30,offset:0};' if arm else 'ld {dst:ra,offset:0};'
    call = 'bl {symbol:callee};' if arm else 'jal {dst:ra,symbol:callee};'
    bad_call = 'bl {target:callee};' if arm else 'jal {dst:ra,target:callee};'
    unknown = 'bl {symbol:does_not_exist};' if arm else 'jal {dst:ra,symbol:does_not_exist};'
    bad_reg = 'bl {symbol:callee,dst:x0};' if arm else 'jal {dst:a0,symbol:callee};'
    volatile = 'add {dst:x0,lhs:x0,rhs:x1};' if arm else 'add {dst:a0,lhs:a0,rhs:a1};'
    if variant == 'normal':
        return f'{alloc} {save} {call} {load} {free} ret;'
    if variant == 'double':
        return f'{alloc} {save} {call} {call} {load} {free} ret;'
    if variant == 'unresolved':
        return f'{alloc} {save} {unknown} {load} {free} ret;'
    if variant == 'missing_save':
        return f'{alloc} {call} {free} ret;'
    if variant == 'restore_before':
        return f'{alloc} {save} {load} {call} {free} ret;'
    if variant == 'missing_restore':
        return f'{alloc} {save} {call} {free} ret;'
    if variant == 'volatile_read':
        return f'{alloc} {save} {call} {volatile} {load} {free} ret;'
    if variant == 'bad_target':
        return f'{alloc} {save} {bad_call} {load} {free} ret;'
    if variant == 'bad_reg':
        return f'{alloc} {save} {bad_reg} {load} {free} ret;'
    if variant == 'duplicate_role':
        extra = 'bl {target:callee,symbol:callee};' if arm else 'jal {dst:ra,target:callee,symbol:callee};'
        return f'{alloc} {save} {extra} {load} {free} ret;'
    if variant == 'local_mix':
        b = 'b {target:exit};' if arm else 'beq {lhs:a0,rhs:a0,target:exit};'
        return f'{alloc} {save} {call} {b} label {{name:exit}}; {load} {free} ret;'
    raise ValueError(variant)


def source_text(arch, variant, defined):
    if defined:
        before = 'export fn callee(a:u64,b:u64)->u64 { return a+b; }\n'
    else:
        before = 'extern fn callee(a:u64,b:u64)->u64;\n'
    return ('module test.native_symbol;\n' + before +
            'export fn wrapper(a:u64,b:u64)->u64 @naked {\n' +
            f'unsafe exact {arch} {{ {snippet(arch,variant)} }}\n}}\n')


def compile_source(binary, arch, body, output, base=None):
    cmd = [binary, 'build', '--target', arch, '--emit', 'bin', '-j', '1', '-o', str(output)]
    if base is not None:
        cmd.extend(['--base', str(base)])
    cmd.append(str(body))
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=50)


def oracle(clang, readelf, arch, folder):
    if not clang or not readelf:
        return
    asm = folder/'oracle.s'
    obj = folder/'oracle.o'
    asm.write_text('.text\n.globl _start\n_start:\n' +
                   ('  bl callee\n  ret\n' if arch=='aarch64' else
                    '  .option norelax\n  call callee\n  ret\n'), encoding='utf-8')
    cmd = [clang, '-target', 'aarch64-none-elf' if arch=='aarch64' else 'riscv64-none-elf']
    if arch=='riscv64':
        cmd.extend(['-march=rv64i', '-mabi=lp64'])
    proc = subprocess.run(cmd + ['-c', str(asm), '-o', str(obj)], capture_output=True, text=True)
    if proc.returncode:
        raise AssertionError(f'independent assembler rejected ISA: {proc.stderr}')
    reloc = subprocess.run([readelf, '-r', str(obj)],capture_output=True,text=True,check=True).stdout
    wanted = 'R_AARCH64_CALL26' if arch=='aarch64' else 'R_RISCV_CALL_PLT'
    if wanted not in reloc:
        raise AssertionError(f'{arch}: oracle relocation missing {wanted}: {reloc}')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--compiler', required=True)
    p.add_argument('--target', choices=('aarch64','riscv64'), required=True)
    p.add_argument('--clang')
    p.add_argument('--readelf')
    args = p.parse_args()
    cases = [
        ('normal', True, True), ('double', True, True),
        ('unresolved', False, False), ('missing_save', True, False),
        ('restore_before', True, False), ('missing_restore', True, False),
        ('volatile_read', True, False), ('bad_target', True, False),
        ('bad_reg', True, False), ('duplicate_role', True, False),
    ]
    # Slice 16: a local conditional branch after the RV64 two-word
    # AUIPC/JALR symbol call is now valid; a0 is the declared result.
    cases.append(('local_mix', True, True))
    with tempfile.TemporaryDirectory(prefix='aster-symbol-call-') as root:
        folder = Path(root)
        oracle(args.clang, args.readelf, args.target, folder)
        for case, defined, accepted in cases:
            src = folder/(case+'.aa')
            out = folder/(case+'.bin')
            src.write_text(source_text(args.target, case, defined), encoding='utf-8')
            run = compile_source(args.compiler, args.target, src, out)
            if not accepted:
                if run.returncode == 0 or out.exists():
                    raise AssertionError(f'{args.target} {case} wrongly accepted: {run.stdout} {run.stderr}')
                continue
            if run.returncode or not out.exists():
                raise AssertionError(f'{args.target} {case} wrongly rejected: {run.stderr}')
            raw = out.read_bytes()
            if len(raw)%4:
                raise AssertionError(f'{args.target} {case} malformed size {len(raw)}')
            offsets = call_targets(args.target,raw)
            count = 2 if case=='double' else 1
            if len(offsets)!=count or not all(dest==0 and size==(4 if args.target=='aarch64' else 8)
                                               for _,dest,size in offsets):
                raise AssertionError(f'{args.target} {case} broken fixups: {offsets} {raw.hex()}')
            # The entire raw image should be position independent if all
            # references point to definitions within that same source module.
            other = folder/(case+'.other.bin')
            again = compile_source(args.compiler, args.target, src, other,base=0x80000000)
            if again.returncode or other.read_bytes()!=raw:
                raise AssertionError(f'{args.target} {case} image depends on base')
    print(f'{args.target}: {len(cases)} symbolic-call cases PASS, Clang relocation oracle '+
          ('PASS' if args.clang and args.readelf else 'SKIPPED'))


if __name__ == '__main__':
    main()
