"""Execute copy/select/diamond/loop code across every supported ISA/ABI."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',required=True);p.add_argument('--python-path',required=True)
args=p.parse_args();sys.path.insert(0,args.python_path)
import unicorn as uc
from unicorn import x86_const as x,arm64_const as a,riscv_const as r

MASK=(1<<64)-1
profiles=[('x86_16','cdecl'),('x86_32','cdecl'),('x86_64','sysv'),('x86_64','win64'),
          ('aarch64','aapcs64'),('riscv64','lp64'),('riscv64','lp64f'),('riscv64','lp64d')]

def execute(data,target):
    if target.startswith('x86'):
        bits=int(target.split('_')[1]);width=8 if bits==64 else 4
        cpu=uc.Uc(uc.UC_ARCH_X86,{16:uc.UC_MODE_16,32:uc.UC_MODE_32,64:uc.UC_MODE_64}[bits])
        pc,sp,ret=(x.UC_X86_REG_RIP,x.UC_X86_REG_RSP,x.UC_X86_REG_RAX) if bits==64 else (x.UC_X86_REG_EIP,x.UC_X86_REG_ESP,x.UC_X86_REG_EAX)
        stack,stop=(0xf000,0xf800) if bits==16 else (0x480000,0x1f0000)
    elif target=='aarch64':
        cpu=uc.Uc(uc.UC_ARCH_ARM64,uc.UC_MODE_ARM);pc,sp,ret=a.UC_ARM64_REG_PC,a.UC_ARM64_REG_SP,a.UC_ARM64_REG_X0
        stack,stop,width=0x480000,0x1f0000,0;cpu.reg_write(a.UC_ARM64_REG_X30,stop)
    else:
        cpu=uc.Uc(uc.UC_ARCH_RISCV,uc.UC_MODE_RISCV64);pc,sp,ret=r.UC_RISCV_REG_PC,r.UC_RISCV_REG_X2,r.UC_RISCV_REG_X10
        stack,stop,width=0x480000,0x1f0000,0;cpu.reg_write(r.UC_RISCV_REG_X1,stop)
    base=0x1000 if target=='x86_16' else 0x100000
    cpu.mem_map(0,0x500000);cpu.mem_write(base,data);cpu.reg_write(sp,stack-width)
    if width:cpu.mem_write(stack-width,stop.to_bytes(width,'little'))
    cpu.emu_start(base,stop,timeout=2000000,count=1000000)
    assert cpu.reg_read(pc)==stop and cpu.reg_read(sp)==stack,(target,'entry/return contract')
    result=cpu.reg_read(ret)
    if target in ['x86_16','x86_32']:result|=cpu.reg_read(x.UC_X86_REG_EDX)<<32
    return result&MASK

count=0
with tempfile.TemporaryDirectory(prefix='aster-codegen-cleanup-') as directory:
    root=Path(directory);source=root/'main.aa';output=root/'image.bin'
    for value in [0,1,0xfeedc0de812345e7]:
        source.write_text(f'''module cleanup.execution;
section ".data" @writable {{global input:u64=0x{value:x};}}
export fn run()->u64 @abi(c) {{
  let x:u64=load.u64 [&input];
  let a:u64=x^0x123456789abcdef0;
  let b:u64=x+0x102030405060708;
  let p:u64=select(x!=0,a,b);
  let q:u64=choose(x<17,p,b);
  let result:u64=0;let i:u64=0;
  while i<3 {{result=result+select(i==1,q,b);i=i+1;}}
  return result^x;
}}
fn choose(flag:bool,a:u64,b:u64)->u64 @abi(c) @noinline {{
  let result:u64;if flag {{result=a;}} else {{result=b;}}return result;
}}
''',encoding='utf-8')
        a_value=value^0x123456789abcdef0;b_value=(value+0x102030405060708)&MASK
        selected=a_value if value else b_value
        chosen=selected if value<17 else b_value
        expected=((2*b_value+chosen)&MASK)^value
        for target,abi in profiles:
            for opt in ['-O0','-O2','-Oz']:
                for pressure in [False,True]:
                    reference=None
                    for workers in ['1','8']:
                        command=[args.compiler,'build',opt,'--target',target,'--abi',abi,
                                 '--base','0x1000' if target=='x86_16' else '0x100000',
                                 '--no-red-zone','-j',workers,'-o',str(output),str(source)]
                        if pressure:command+=['--reg-limit','2','--no-promote']
                        if target=='riscv64':command+=['--feature','m','--feature','f','--feature','d']
                        proc=subprocess.run(command,capture_output=True,text=True,timeout=60)
                        assert proc.returncode==0,(target,abi,opt,pressure,proc.stderr)
                        data=output.read_bytes()
                        assert reference is None or reference==data,(target,abi,opt,pressure,'parallel bytes')
                        reference=data
                        actual=execute(data,target)
                        assert actual==expected,(target,abi,opt,pressure,value,hex(actual),hex(expected))
                        count+=1
print(f'PASS: {count} CPU executions; all eight ISA/ABI profiles, 64-bit masks, aliases, diamonds, loops, O0/O2/Oz, pressure and 1/8 workers')
