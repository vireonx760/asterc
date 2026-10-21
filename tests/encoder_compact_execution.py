#!/usr/bin/env python3
"""Check compact 64-bit immediates against an independent x86 CPU, including flags."""
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
    for row in corpus.splitlines():
        arch,assembly,encoded=row.split('\t')
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
    print(f'PASS: {count} compact immediate CPU/flags boundary checks')


if __name__=='__main__':main()
