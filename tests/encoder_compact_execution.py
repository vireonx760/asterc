#!/usr/bin/env python3
"""Check immediate and BSWAP encodings against an independent x86 CPU, including flags."""
import argparse
import subprocess
import sys


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test',required=True)
    parser.add_argument('--python-path',default='')
    args=parser.parse_args()
    if args.python_path:sys.path.insert(0,args.python_path)
    import unicorn as uc
    from unicorn import x86_const as x
    values=[0,(1<<32)-1,1<<32,-(1<<31),-1,-(1<<31)-1]
    expected={}
    mask=(1<<64)-1
    for wide,narrow,reg in (('rax','eax',x.UC_X86_REG_RAX),('r9','r9d',x.UC_X86_REG_R9)):
        for value in values:
            if 0<=value<1<<32:assembly=f'mov {narrow}, {value:#x}'
            elif -(1<<31)<=value<0:assembly=f'mov {wide}, {value}'
            else:assembly=f'movabs {wide}, {value&mask:#x}'
            expected[assembly]=(reg,value&mask)
    count=0
    corpus=subprocess.run([args.test,'--corpus'],capture_output=True,text=True,check=True,timeout=60).stdout
    bswap_count = 0
    names = ['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi',
             'r8','r9','r10','r11','r12','r13','r14','r15']
    registers = [getattr(x, 'UC_X86_REG_' + name.upper()) for name in names]
    narrow_names = ['eax','ecx','edx','ebx','esp','ebp','esi','edi'] + [f'r{i}d' for i in range(8,16)]
    bswap_expected = {f'bswap {name}': (index, bits)
                      for bits, views in [(32, narrow_names), (64, names)]
                      for index, name in enumerate(views)}
    for row in corpus.splitlines():
        arch,assembly,encoded=row.split('\t')
        if arch == 'x86_64' and assembly in bswap_expected:
            index, bits = bswap_expected.pop(assembly)
            code = bytes.fromhex(encoded)
            for value in [0, 0x1122334455667788, mask]:
                for flags in (0x202,0x203,0x2d6,0xad7):
                    cpu = uc.Uc(uc.UC_ARCH_X86, uc.UC_MODE_64)
                    cpu.mem_map(0x100000,4096,uc.UC_PROT_ALL)
                    cpu.mem_write(0x100000,code)
                    before = [(0x8765432100000000 + i * 0x1020304) & mask for i in range(16)]
                    before[index] = value
                    for register, initial in zip(registers, before):
                        cpu.reg_write(register, initial)
                    cpu.reg_write(x.UC_X86_REG_RFLAGS,flags)
                    saved_flags = cpu.reg_read(x.UC_X86_REG_RFLAGS)
                    cpu.emu_start(0x100000,0x100000+len(code),count=1)
                    wanted = before[:]
                    wanted[index] = int.from_bytes((value & ((1 << bits)-1)).to_bytes(bits//8,'little'),'big')
                    actual = [cpu.reg_read(register) for register in registers]
                    assert actual == wanted, (assembly, actual, wanted)
                    assert cpu.reg_read(x.UC_X86_REG_RFLAGS) == saved_flags, (assembly, 'flags changed')
                    bswap_count += 1
        if arch!='x86_64' or assembly not in expected:continue
        reg,want=expected.pop(assembly);code=bytes.fromhex(encoded)
        for flags in (0x202,0x203,0x2d6,0xad7):
            cpu=uc.Uc(uc.UC_ARCH_X86,uc.UC_MODE_64)
            cpu.mem_map(0x100000,4096,uc.UC_PROT_ALL);cpu.mem_write(0x100000,code)
            cpu.reg_write(reg,mask);cpu.reg_write(x.UC_X86_REG_RFLAGS,flags)
            saved_flags=cpu.reg_read(x.UC_X86_REG_RFLAGS)
            cpu.emu_start(0x100000,0x100000+len(code),count=1)
            assert cpu.reg_read(reg)==want,(assembly,hex(cpu.reg_read(reg)),hex(want))
            assert cpu.reg_read(x.UC_X86_REG_RFLAGS)==saved_flags,(assembly,'flags changed')
            count+=1
    assert not expected,expected
    assert not bswap_expected, bswap_expected
    print(f'PASS: {count} compact immediate and {bswap_count} BSWAP CPU/flags checks (all 16 GPRs, 32/64 bits)')


if __name__=='__main__':main()
