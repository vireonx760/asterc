#!/usr/bin/env python3
"""Run native CPU-control instructions at real guest privilege under QEMU TCG.

No guest OS, KVM, host instruction execution or target libc is required. The
startup/oracle is assembled independently; only the tested function is AsterASM.
"""
import argparse
import hashlib
import json
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COOKIE = 0x12345678


def command(argv, expected=0):
    argv = [str(x) for x in argv]
    argv[0] = shutil.which(argv[0]) or argv[0]
    p = subprocess.run(argv, capture_output=True, text=True,
                       encoding='utf-8', errors='replace', timeout=25)
    assert p.returncode == expected, (argv, p.returncode, p.stdout, p.stderr)
    return p


def x86_kernel(body, raw, root, clang, abi, compiler, run_emulator=True):
    # Compile the same all-AsterASM bootstrap shipped with the example. The
    # independently assembled guest below remains the instruction-result oracle.
    spec = importlib.util.spec_from_file_location('aster_minikernel_build', ROOT/'examples/minikernel/build.py')
    loader = importlib.util.module_from_spec(spec); spec.loader.exec_module(loader)
    boot = root/'boot.bin'; prefix_path = root/'boot-prefix.bin'
    if not prefix_path.exists():
        boot_bytes, prefix = loader.build_boot(compiler, root)
        prefix_path.write_bytes(prefix)
    else:
        boot_bytes, prefix = boot.read_bytes(), prefix_path.read_bytes()
    asm = root / 'start.s'; elf = root / 'start.elf'; kernel = root / 'kernel.bin'; disk = root / 'disk.img'
    asm.write_text('''.intel_syntax noprefix
.text
.globl _start
_start:
mov rsp, 0x8fff0
mov rbx, 0x1234abcd
''' + ('sub rsp, 32\n' if abi == 'win64' else '') + body + '''
cmp rbx, 0x1234abcd
jne failed
mov eax, 0x10
jmp finished
failed:
mov eax, 0x11
finished:
mov dx, 0xf4
out dx, eax
cli
hlt
.balign 16
aster_entry:
.incbin "''' + raw.as_posix() + '''"
''')
    command([clang, '--target=x86_64-unknown-linux-gnu', '-nostdlib', '-static', '-fuse-ld=lld',
             '-Wl,--image-base=0', '-Wl,-Ttext='+hex(loader.KERNEL_BASE), '-Wl,-e,_start', '-Wl,--build-id=none', '-o', elf, asm])
    command(['llvm-objcopy', '-O', 'binary', '--only-section=.text', elf, kernel])
    data = kernel.read_bytes()
    assert len(data) <= loader.KERNEL_CAPACITY
    disk.write_bytes(boot_bytes + prefix + data + bytes(loader.KERNEL_CAPACITY - len(data)))
    result = command(['qemu-system-x86_64', '-accel', 'tcg', '-cpu', 'max', '-machine', 'pc', '-m', '32M',
                      '-drive', f'format=raw,file={disk}', '-nic', 'none', '-display', 'none', '-monitor', 'none',
                      '-serial', 'none', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], expected=33) if run_emulator else None
    return disk, result


def other_kernel(arch, body, raw, root, clang, rvv_bits=None, rv_cpu=None, a64_cpu='cortex-a53'):
    arm = arch == 'aarch64'
    asm = root / 'start.s'; elf = root / 'kernel.elf'
    start = '''.text
.globl _start
_start:
adrp x9,stack_top
add x9,x9,:lo12:stack_top
mov sp,x9
''' if arm else '''.option norelax
.text
.globl _start
_start:
la sp,stack_top
'''
    finish = '''
mov x0,#0x20
adr x1,passed_block
hlt #0xf000
failed:
mov x0,#0x20
adr x1,failed_block
hlt #0xf000
b .
.balign 8
passed_block: .quad 0x20026,0
failed_block: .quad 0x20026,1
''' if arm else '''
li t0,0x100000
li t1,0x5555
sw t1,0(t0)
j .
failed:
li t0,0x100000
li t1,0x13333
sw t1,0(t0)
j .
'''
    asm.write_text(start + body + finish + '\n.balign 16\naster_entry:\n.incbin "' + raw.as_posix() +
                   '"\n.section .bss\n.balign 16\n.space 65536\nstack_top:\n')
    flags = [] if arm else ['-march=' + ('rv64gcv' if rvv_bits else 'rv64i_zicsr_zifencei'), '-mabi=lp64']
    command([clang, '--target=' + ('aarch64-none-elf' if arm else 'riscv64-none-elf'), *flags,
             '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-Wl,--build-id=none',
             '-Wl,-Ttext=' + ('0x40080000' if arm else '0x80000000'), '-o', elf, asm])
    qemu = ['qemu-system-' + arch, '-accel', 'tcg', '-machine', 'virt', '-m', '128M', '-nic', 'none',
            '-display', 'none', '-monitor', 'none', '-serial', 'none', '-kernel', elf]
    qemu += ['-cpu', a64_cpu, '-semihosting-config', 'enable=on,target=native'] if arm else ['-bios', 'none']
    if rvv_bits: qemu += ['-cpu', f'rv64,v=true,vlen={rvv_bits},elen=64']
    elif rv_cpu: qemu += ['-cpu', rv_cpu]
    result = command(qemu)
    return elf, result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True)
    p.add_argument('--clang', required=True)
    p.add_argument('--out')
    args = p.parse_args()
    for tool in ('qemu-system-x86_64', 'qemu-system-aarch64', 'qemu-system-riscv64', 'llvm-objcopy', 'ld.lld'):
        assert shutil.which(tool), 'Mandatory test dependency missing: ' + tool
    tests = [
        ('x86_64', 'control', 'mov {dst:rax,src:cr3}; mov {dst:cr3,src:rax};',
         'call aster_entry\ncmp rax,0x70000\njne failed\n', ()),
        ('x86_64', 'debug', 'mov {dst:r10,src:dr0}; mov {dst:dr0,src:rdi}; mov {dst:rax,src:dr0}; mov {dst:dr0,src:r10};',
         f'mov edi,{COOKIE}\ncall aster_entry\ncmp rax,{COOKIE}\njne failed\n', ()),
        ('x86_64', 'msr', 'rdmsr; wrmsr;',
         f'mov ecx,0xc0000101\nmov eax,{COOKIE}\nxor edx,edx\nwrmsr\ncall aster_entry\ncmp eax,{COOKIE}\njne failed\ntest edx,edx\njne failed\n', ()),
        ('x86_64', 'xstate', 'xgetbv; xsetbv;',
         'mov rax,cr4\nor rax,0x40000\nmov cr4,rax\nxor ecx,ecx\nxgetbv\nmov r12d,eax\nmov r13d,edx\ncall aster_entry\ncmp eax,r12d\njne failed\ncmp edx,r13d\njne failed\n', ('xsave',)),
        ('x86_64', 'cpuid', 'push {src:rbx}; cpuid; pop {dst:rbx};',
         'push rbx\nxor eax,eax\ncpuid\nmov r12d,eax\npop rbx\nxor eax,eax\ncall aster_entry\ncmp eax,r12d\njne failed\n', ()),
        ('aarch64', 'register', 'mrs {dst:x0,sys:sctlr_el1}; msr {sys:sctlr_el1,src:x0}; isb {option:15};',
         'mrs x19,sctlr_el1\nbl aster_entry\ncmp x0,x19\nb.ne failed\n', ()),
        ('aarch64', 'translation', 'msr {sys:tpidr_el1,src:x0}; mrs {dst:x0,sys:tpidr_el1}; sys {src:xzr,op1:0,crn:8,crm:7,op2:0}; dsb {option:15}; isb {option:15};',
         f'mov x0,#{COOKIE & 65535}\nmovk x0,#{COOKIE >> 16},lsl #16\nmov x19,x0\nbl aster_entry\ncmp x0,x19\nb.ne failed\nmrs x1,tpidr_el1\ncmp x1,x19\nb.ne failed\n', ()),
        ('riscv64', 'csr', 'csrrw {dst:t0,csr:mscratch,src:a0}; csrrs {dst:a0,csr:mscratch,src:zero}; csrrw {dst:zero,csr:mscratch,src:t0}; fence {pred:15,succ:15}; fence.i; sfence.vma {addr:zero,asid:zero};',
         f'li a0,{COOKIE}\ncsrr s0,mscratch\ncall aster_entry\nli t0,{COOKIE}\nbne a0,t0,failed\ncsrr t0,mscratch\nbne t0,s0,failed\n', ('zicsr', 'zifencei')),
        ('riscv64', 'csr-immediate', 'csrrs {dst:t0,csr:mscratch,src:zero}; csrrwi {dst:a0,csr:mscratch,imm:31}; csrrci {dst:a0,csr:mscratch,imm:16}; csrrsi {dst:a0,csr:mscratch,imm:0}; csrrw {dst:zero,csr:mscratch,src:t0};',
         'call aster_entry\nli t0,15\nbne a0,t0,failed\n', ('zicsr',)),
    ]
    for kind in ('cpuid', 'control'):
        body = ('mov {dst:eax,src:leaf}; mov {dst:ecx,src:0}; cpuid; mov {dst:answer,src:eax};'
                if kind == 'cpuid' else 'mov {dst:rax,src:cr3}; mov {dst:answer,src:rax};')
        oracle = ('push rbx\nxor eax,eax\ncpuid\nmov r12d,eax\npop rbx\nadd r12,200\n'
                  if kind == 'cpuid' else 'mov r12,0x700c8\n')
        tests.append(('x86_64', kind + '-bridge', body, oracle, ()))
    records = []
    with tempfile.TemporaryDirectory(prefix='aster-native-processor-qemu-') as temporary:
        root = Path(temporary); source = root / 'unit.aa'; raw = root / 'unit.bin'
        for arch, name, code, startup, features in tests:
            for abi in (('sysv', 'win64') if arch == 'x86_64' else (None,)):
                for level in ('0', '2', 'z'):
                    mode = {'0': 'native', '2': 'exact', 'z': 'unsafe exact'}[level]
                    runner = startup
                    if name.endswith('-bridge'):
                        answer_type = 'u32' if name.startswith('cpuid') else 'u64'
                        source.write_text('module processor.kernel; export fn run(leaf:u32,a:u64,b:u64)->u64 @abi(c) {'
                                          'let live:u64=a*17+b; let answer:' + answer_type + '; ' + mode +
                                          ' x86_64 inputs(leaf) outputs(answer) {' + code + '} return live+cast<u64>(answer); }')
                        runner += ('xor ecx,ecx\nmov edx,11\nmov r8d,13\n' if abi == 'win64' else
                                   'xor edi,edi\nmov esi,11\nmov edx,13\n')
                        runner += 'call aster_entry\ncmp rax,r12\njne failed\n'
                    else:
                        source.write_text(f'module processor.kernel; export fn run() @naked {{ {mode} {arch} {{ {code} ret; }} }}')
                    compile = [args.compiler, 'build', '--target', arch, '-O' + level, '--no-red-zone', '-j', '1', '-o', raw, source]
                    if abi: compile += ['--abi', abi]
                    for feature in features: compile += ['--feature', feature]
                    command(compile)
                    image, result = x86_kernel(runner, raw, root, args.clang, abi, args.compiler) if arch == 'x86_64' else other_kernel(arch, runner, raw, root, args.clang)
                    records.append({'target': arch, 'abi': abi, 'level': level, 'mode': mode, 'case': name, 'exit_code': result.returncode,
                                    'bytes': raw.read_bytes().hex(), 'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest()})
                    print(f'PASS {arch}/{abi or "default"}/O{level}: {name}', flush=True)
    if args.out: Path(args.out).write_text(json.dumps({'records': records}, indent=2) + '\n')
    print(f'PASS: {len(records)} QEMU TCG privileged CPU-control executions, no guest OS')


if __name__ == '__main__':
    main()
