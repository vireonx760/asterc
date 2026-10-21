#!/usr/bin/env python3
"""Execute emitted typed AsterASM code on QEMU virt without a guest OS.

The tiny independently assembled startup supplies SP and the device/runtime
exit mechanism. Arithmetic, calls and spill frames are emitted by asterc.
ARM uses semihosting write/exit; RV uses the UART and SiFive test finisher.
No KVM, host ISA execution, kernel image or target C runtime is assumed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

SOURCE = '''module qual.baremetal;
export fn entry(x:u64,y:u64)->u64 @abi(c) {
    let live:u64=x+y;
    let answer:u64=helper(x,y);
    return answer+live;
}
fn helper(x:u64,y:u64)->u64 { return x/7+y%3; }
'''


def startup(arm, raw):
    if arm:
        text = '''.text
.globl _start
_start:
adrp x9, stack_top
add x9,x9,:lo12:stack_top
mov sp,x9
mov x0,#70
mov x1,#5
bl aster_entry
cmp x0,#87
b.ne failed
mov x0,#4
adr x1,message
hlt #0xf000
adr x1,exit_block
str xzr,[x1,#8]
failed:
mov x0,#0x20
adr x1,exit_block
hlt #0xf000
b .
'''
    else:
        text = '''.option norelax
.text
.globl _start
_start:
la sp,stack_top
li a0,70
li a1,5
call aster_entry
li t0,87
bne a0,t0,failed
la t0,message
li t1,0x10000000
print_loop:
lbu t2,0(t0)
beqz t2,passed
sb t2,0(t1)
addi t0,t0,1
j print_loop
passed:
li t0,0x100000
li t1,0x5555
sw t1,0(t0)
j .
failed:
li t0,0x100000
li t1,0x3333
sw t1,0(t0)
j .
'''
    text += '\n.balign 16\naster_entry:\n.incbin "'+raw.as_posix()+'"\n'
    text += 'message: .asciz "ASTER-SYSTEM-PASS\\n"\n.balign 8\n'
    text += 'exit_block: .quad 0x20026,1\n.section .bss\n.balign 16\n.space 65536\nstack_top:\n'
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--opt-level',choices=['0','2','z'],default='0')
    parser.add_argument('--clang', default='clang')
    parser.add_argument('--out')
    args = parser.parse_args()
    for tool in (args.clang,'ld.lld','qemu-system-aarch64','qemu-system-riscv64'):
        if not shutil.which(tool):
            raise RuntimeError('Mandatory bare-metal QEMU tool missing: '+tool)
    records=[]
    with tempfile.TemporaryDirectory(prefix='aster-baremetal-') as tmp:
        root=Path(tmp);source=root/'unit.aa';raw=root/'unit.bin';asm=root/'start.S';elf=root/'kernel.elf'
        source.write_text(SOURCE)
        for arch,m in [('aarch64',False),('riscv64',True),('riscv64',False)]:
            arm=arch=='aarch64'
            for policy in ('hot','size'):
                for limit in (0,2):
                    env=os.environ.copy();env['ASTERASM_DIV_POLICY']=policy
                    compile=[args.compiler,'build','-O'+args.opt_level,'--target',arch,'--emit','bin',
                             '--no-red-zone','--reg-limit',str(limit),'-j','1','-o',str(raw),str(source)]
                    if m:compile+=['--feature','m']
                    result=subprocess.run(compile,env=env,capture_output=True,text=True,timeout=30)
                    if result.returncode:raise AssertionError(result.stderr)
                    asm.write_text(startup(arm,raw))
                    link=[args.clang,'--target='+('aarch64-none-elf' if arm else 'riscv64-none-elf'),
                          '-nostdlib','-static','-fuse-ld=lld','-Wl,-e,_start',
                          '-Wl,--build-id=none','-Wl,-Ttext='+('0x40080000' if arm else '0x80000000'),
                          '-o',str(elf),str(asm)]
                    if not arm:link+=['-march=rv64im' if m else '-march=rv64i','-mabi=lp64']
                    result=subprocess.run(link,capture_output=True,text=True,timeout=30)
                    if result.returncode:raise AssertionError(result.stderr)
                    qemu=['qemu-system-'+arch,'-accel','tcg','-machine','virt','-m','128M','-nic','none',
                          '-display','none','-monitor','none','-serial','stdio','-kernel',str(elf)]
                    if arm:qemu+=['-cpu','cortex-a53','-semihosting-config','enable=on,target=native']
                    else:
                        qemu+=['-bios','none']
                        if not m:qemu+=['-cpu','rv64,m=false']
                    result=subprocess.run(qemu,capture_output=True,text=True,timeout=15)
                    if result.returncode or 'ASTER-SYSTEM-PASS' not in result.stdout+result.stderr:
                        raise AssertionError((arch,m,policy,limit,result.returncode,result.stdout,result.stderr))
                    records.append({'target':arch,'m':m,'policy':policy,'reg_limit':limit,
                                    'exit_code':result.returncode,'marker':'ASTER-SYSTEM-PASS',
                                    'binary_bytes':raw.stat().st_size,
                                    'binary_sha256':hashlib.sha256(raw.read_bytes()).hexdigest(),
                                    'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest()})
    if args.out:Path(args.out).write_text(json.dumps({'records':records},indent=2)+'\n')
    print(f'PASS: {len(records)} actual QEMU TCG bare-metal executions, no guest OS')


if __name__=='__main__':
    main()
