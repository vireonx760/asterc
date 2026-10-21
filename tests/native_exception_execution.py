#!/usr/bin/env python3
"""Real exception entry/return and privileged state checks in QEMU guests."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile

from native_processor_qemu import command, other_kernel


def cases():
    for enabled in (False, True):
        # Current-EL SPx synchronous vector. The independent runner raises SVC;
        # AsterASM owns the actual handler and ERET, including the only return.
        arm = '''cbz {reg:x0,target:frame}; sub {dst:sp,lhs:sp,rhs:16};
                 add {dst:sp,lhs:sp,rhs:16}; label {name:frame};
                 sub {dst:sp,lhs:sp,rhs:16}; str {src:x19,offset:0};
                 mrs {dst:x19,sys:currentel};
                 mrs {dst:x0,sys:esr_el1}; mrs {dst:x1,sys:spsr_el1};
                 mrs {dst:x2,sys:elr_el1}; mrs {dst:x3,sys:daif};
                 ldr {dst:x19,offset:0}; add {dst:sp,lhs:sp,rhs:16}; eret;'''
        mask = 0x340 if enabled else 0x3c0
        runner = f'''mrs x9,currentel
cmp x9,#4
b.ne failed
mrs x9,spsel
cmp x9,#1
b.ne failed
b main
.balign 2048
vectors:
.space 512
b aster_entry
.space 1532
main:
adr x9,vectors
msr vbar_el1,x9
isb
msr daifset,#15
''' + ('msr daifclr,#2\n' if enabled else '') + f'''mov x19,#0x1234
mov x0,#{int(enabled)}
svc #0x42
resumed:
mov x9,#0x1234
cmp x19,x9
b.ne failed
mov x9,sp
adrp x10,stack_top
add x10,x10,:lo12:stack_top
cmp x9,x10
b.ne failed
mov x9,#0x42
movk x9,#0x5600,lsl #16
cmp x0,x9
b.ne failed
and x9,x1,#0x3ff
cmp x9,#{mask | 5}
b.ne failed
adr x9,resumed
cmp x2,x9
b.ne failed
and x9,x3,#0x3c0
cmp x9,#0x3c0
b.ne failed
mrs x9,daif
and x9,x9,#0x3c0
cmp x9,#{mask}
b.ne failed
'''
        yield 'aarch64', 'svc-eret', enabled, arm, runner, (), None
        for supervisor in (False, True):
            prefix = 's' if supervisor else 'm'
            code = f'''beq {{lhs:a0,rhs:zero,target:frame}};
                       addi {{dst:sp,lhs:sp,rhs:-16}}; addi {{dst:sp,lhs:sp,rhs:16}};
                       label {{name:frame}}; addi {{dst:sp,lhs:sp,rhs:-16}};
                       sd {{src:s0,offset:0}}; csrrs {{dst:s0,csr:{prefix}status,src:zero}};
                       csrrs {{dst:a0,csr:{prefix}cause,src:zero}};
                       csrrs {{dst:a1,csr:{prefix}status,src:zero}};
                       csrrs {{dst:t0,csr:{prefix}epc,src:zero}};
                       addi {{dst:t0,lhs:t0,rhs:4}};
                       csrrw {{dst:zero,csr:{prefix}epc,src:t0}};
                       ld {{dst:s0,offset:0}}; addi {{dst:sp,lhs:sp,rhs:16}};
                       {prefix}ret;'''
            # TOR PMP grants S-mode physical memory access; delegate only S-mode
            # ECALL. Startup transitions to S with its own independent MRET.
            runner = '''la t0,aster_entry
csrw stvec,t0
li t0,-1
csrw pmpaddr0,t0
li t0,15
csrw pmpcfg0,t0
li t0,512
csrw medeleg,t0
la t0,supervisor_start
csrw mepc,t0
li t0,0x1800
csrc mstatus,t0
li t0,0x800
csrs mstatus,t0
mret
supervisor_start:
''' if supervisor else 'la t0,aster_entry\ncsrw mtvec,t0\n'
            irq = 2 if supervisor else 8
            previous_irq = irq << 4
            runner += f'''li s1,{irq if enabled else 0}
li t0,{irq}
csrc {prefix}status,t0
csrs {prefix}status,s1
li s0,0x1234
li a0,{int(enabled)}
ecall
li t0,0x1234
bne s0,t0,failed
la t0,stack_top
bne sp,t0,failed
li t0,{9 if supervisor else 11}
bne a0,t0,failed
andi t0,a1,{previous_irq}
li t1,{previous_irq if enabled else 0}
bne t0,t1,failed
csrr t0,{prefix}status
andi t0,t0,{irq}
bne t0,s1,failed
'''
            yield 'riscv64', 'ecall-' + prefix + 'ret', enabled, code, runner, ('zicsr',), None
    code = '''mrs {dst:x1,sys:daif}; msr {pstate:daifset,imm:15};
              mrs {dst:x0,sys:daif}; msr {pstate:daifclr,imm:3};
              mrs {dst:x2,sys:daif}; msr {sys:daif,src:x1}; ret;'''
    yield 'aarch64', 'pstate-interrupt-mask', None, code, '''mrs x19,daif
bl aster_entry
and x9,x0,#0x3c0
cmp x9,#0x3c0
b.ne failed
and x9,x2,#0x3c0
cmp x9,#0x300
b.ne failed
mrs x9,daif
cmp x9,x19
b.ne failed
''', (), None
    code = '''msr {pstate:spsel,imm:0}; mrs {dst:x0,sys:spsel};
              msr {pstate:spsel,imm:1}; mrs {dst:x1,sys:spsel}; ret;'''
    yield 'aarch64', 'pstate-stack-selection', None, code, '''mov x9,sp
msr sp_el0,x9
bl aster_entry
cbnz x0,failed
cmp x1,#1
b.ne failed
mov x9,sp
adrp x10,stack_top
add x10,x10,:lo12:stack_top
cmp x9,x10
b.ne failed
''', (), None
    code = '''sfence.w.inval; sinval.vma {addr:zero,asid:zero}; sfence.inval.ir;
              hfence.vvma {addr:zero,asid:zero}; hfence.gvma {addr:zero,vmid:zero};
              hinval.vvma {addr:zero,asid:zero}; hinval.gvma {addr:zero,vmid:zero}; ret;'''
    yield 'riscv64', 'hypervisor-invalidation', None, code, '''la t0,failed
csrw mtvec,t0
call aster_entry
''', ('h', 'svinval'), 'rv64,h=true,svinval=true'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True)
    p.add_argument('--clang', required=True)
    p.add_argument('--out')
    args = p.parse_args()
    records = []
    with tempfile.TemporaryDirectory(prefix='aster-exception-execution-') as temporary:
        root = Path(temporary)
        source, raw = root / 'handler.aa', root / 'handler.bin'
        for arch, name, enabled, code, runner, features, cpu in cases():
            for mode in ('native', 'exact', 'unsafe exact'):
                for level in ('0', '2', 'z'):
                    frame = '@frame(validate)' if level == '2' else '@naked'
                    source.write_text(f'module processor.exception; export fn run() {frame} {{ {mode} {arch} {{ {code} }} }}')
                    argv = [args.compiler, 'build', '--target', arch, '-O' + level, '-j', '1', '-o', raw, source]
                    for feature in features:
                        argv += ['--feature', feature]
                    command(argv)
                    image, result = other_kernel(arch, runner, raw, root, args.clang, rv_cpu=cpu)
                    records.append({'target': arch, 'case': name, 'interrupts_enabled': enabled, 'mode': mode,
                                    'level': level, 'frame': frame, 'cpu': cpu, 'exit_code': result.returncode,
                                    'bytes': raw.read_bytes().hex(),
                                    'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest()})
            print(f'PASS: {arch} {name} interrupts={enabled}', flush=True)
    if args.out:
        Path(args.out).write_text(json.dumps({'qemu_runs': len(records), 'records': records}, indent=2) + '\n')
    print(f'PASS: {len(records)} QEMU exception-return/PSTATE/H/Svinval executions')


if __name__ == '__main__':
    main()
