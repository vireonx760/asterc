"""Execute CLI atomic images across ISAs, widths, orders and register pressure."""
import argparse, subprocess, sys, tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',required=True)
p.add_argument('--python-path',required=True)
p.add_argument('--mode',type=int,choices=range(6))
a=p.parse_args()
sys.path.insert(0,a.python_path)
import unicorn as uc
from unicorn import x86_const as x, arm64_const as arm, riscv_const as rv
TARGETS=[('x86_64','sysv'),('x86_64','win64'),('aarch64','aapcs64'),('riscv64','lp64'),('riscv64','lp64f'),('riscv64','lp64d')]
OPS=['load','store','rmw.xchg','rmw.add','rmw.sub','rmw.and','rmw.or','rmw.xor','cas']
BASE,STOP,DATA,STACK=0x100000,0x1f0000,0x300000,0x480000

def execute(code,mode,bits,op,initial,value,expected,offset,force_retry=False):
    if mode<2:
        c=uc.Uc(uc.UC_ARCH_X86,uc.UC_MODE_64)
        params=[x.UC_X86_REG_RDI,x.UC_X86_REG_RSI,x.UC_X86_REG_RDX] if mode==0 else [x.UC_X86_REG_RCX,x.UC_X86_REG_RDX,x.UC_X86_REG_R8]
        result,sp,pc=x.UC_X86_REG_RAX,x.UC_X86_REG_RSP,x.UC_X86_REG_RIP
        preserved=[x.UC_X86_REG_RBX,x.UC_X86_REG_RBP,x.UC_X86_REG_R12,x.UC_X86_REG_R13,x.UC_X86_REG_R14,x.UC_X86_REG_R15]
        if mode==1:preserved += [x.UC_X86_REG_RDI,x.UC_X86_REG_RSI]
    elif mode==2:
        c=uc.Uc(uc.UC_ARCH_ARM64,uc.UC_MODE_ARM)
        params=[arm.UC_ARM64_REG_X0,arm.UC_ARM64_REG_X1,arm.UC_ARM64_REG_X2]
        result,sp,pc=arm.UC_ARM64_REG_X0,arm.UC_ARM64_REG_SP,arm.UC_ARM64_REG_PC
        c.reg_write(arm.UC_ARM64_REG_X30,STOP)
        preserved=[getattr(arm,f'UC_ARM64_REG_X{i}') for i in range(19,30)]
    else:
        c=uc.Uc(uc.UC_ARCH_RISCV,uc.UC_MODE_RISCV64)
        params=[rv.UC_RISCV_REG_X10,rv.UC_RISCV_REG_X11,rv.UC_RISCV_REG_X12]
        result,sp,pc=rv.UC_RISCV_REG_X10,rv.UC_RISCV_REG_X2,rv.UC_RISCV_REG_PC
        c.reg_write(rv.UC_RISCV_REG_X1,STOP)
        preserved=[getattr(rv,f'UC_RISCV_REG_X{i}') for i in [8,9,*range(18,28)]]
    c.mem_map(BASE,0x100000);c.mem_map(DATA,0x1000);c.mem_map(0x400000,0x100000)
    c.mem_write(BASE,code)
    size=max(1,bits//8);mask=(1<<bits)-1
    memory=bytearray(b'\xa5'*32);memory[offset:offset+size]=initial.to_bytes(size,'little')
    c.mem_write(DATA,bytes(memory));c.reg_write(sp,STACK-(8 if mode<2 else 0))
    if mode<2:c.mem_write(STACK-8,STOP.to_bytes(8,'little'))
    sentinels={reg:0x123450000000+i for i,reg in enumerate(preserved)}
    for reg,v in sentinels.items():c.reg_write(reg,v)
    for reg,v in zip(params,[DATA+offset,value,expected]):c.reg_write(reg,v)
    injected=False
    def lose_reservation(cpu,address,size,user):
        nonlocal injected,initial
        if injected or size!=4:return
        instruction=int.from_bytes(cpu.mem_read(address,4),'little')
        if mode==2 and (instruction&0x3fe07c00)==0x08007c00:
            status=(instruction>>16)&31
            cpu.reg_write(getattr(arm,f'UC_ARM64_REG_W{status}'),1)
        elif mode>=3 and (instruction&0xf800007f)==0x1800002f:
            cpu.reg_write(getattr(rv,f'UC_RISCV_REG_X{(instruction>>7)&31}'),1)
        else:return
        initial=(initial+1)&mask
        cpu.mem_write(DATA+offset,initial.to_bytes(max(1,bits//8),'little'))
        cpu.reg_write(pc,address+4);injected=True
    if force_retry:c.hook_add(uc.UC_HOOK_CODE,lose_reservation)
    c.emu_start(BASE,STOP,count=100000,timeout=1000000)
    if force_retry:assert injected,(mode,bits,op,'reservation failure was not injected')
    assert c.reg_read(pc)==STOP,(mode,bits,op,'did not return',hex(c.reg_read(pc)))
    assert c.reg_read(sp)==STACK,(mode,bits,op,'stack')
    for reg,v in sentinels.items():assert c.reg_read(reg)==v,(mode,bits,op,'callee-save',reg)
    new=initial;want=initial
    if op=='store':new=value;want=42
    elif op=='rmw.xchg':new=value
    elif op=='rmw.add':new=(initial+value)&mask
    elif op=='rmw.sub':new=(initial-value)&mask
    elif op=='rmw.and':new=initial&value
    elif op=='rmw.or':new=initial|value
    elif op=='rmw.xor':new=initial^value
    elif op=='cas':want=int(initial==expected);new=value if want else initial
    memory[offset:offset+size]=new.to_bytes(size,'little')
    result_mask=255 if op=='cas' else (1<<64)-1 if op=='store' else mask
    actual=c.reg_read(result)&result_mask
    assert actual==want,(mode,bits,op,initial,value,expected,'result',actual,want)
    assert c.mem_read(DATA,32)==memory,(mode,bits,op,'memory/neighbors',c.mem_read(DATA,32).hex(),memory.hex())

count=0
with tempfile.TemporaryDirectory(prefix='aster-atomic-') as temp:
    source=Path(temp)/'atomic.aa';output=Path(temp)/'atomic.bin'
    for mode,(target,abi) in enumerate(TARGETS):
        if a.mode is not None and mode!=a.mode:continue
        for bits in [1,8,16,32,64]:
            typ='bool' if bits==1 else f'u{bits}'
            for op in OPS:
                if bits==1 and op not in ['load','store','rmw.xchg','cas']:continue
                orders=['relaxed','seq_cst']
                if op=='load':orders+=['acquire']
                elif op=='store':orders+=['release']
                else:orders+=['acquire','release','acq_rel']
                for order in orders:
                    suffix=f'@{order}';expression=f'atomic.{op}.{typ} [p]'
                    if op=='store':body=expression+f' = v {suffix}; return 42;';ret='u64'
                    elif op=='cas':body=f'return {expression}, e, v {suffix};';ret='bool'
                    else:body=f'return {expression}'+('' if op=='load' else ', v')+f' {suffix};';ret=typ
                    source.write_text(f'module test.atomics; export fn run(p:ptr<{typ}>,v:{typ},e:{typ})->{ret} @abi(c) {{ {body} }}')
                    for limit in [0,2]:
                        cmd=[a.compiler,'build','--target',target,'--abi',abi,'--no-red-zone','--reg-limit',str(limit),'-j','1','-o',str(output),str(source)]
                        if target=='riscv64':cmd+=['--feature','a','--feature','m','--feature','f','--feature','d']
                        if limit:cmd+=['--no-promote']
                        proc=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
                        assert proc.returncode==0,(mode,bits,op,order,limit,proc.stderr)
                        code=output.read_bytes();mask=(1<<bits)-1
                        values=[(0,mask,0),(mask,1,mask),(1,0,0),(1<<(bits-1),mask,1<<(bits-1))]
                        for initial,value,expected in values:
                            offsets=[0,1,2,3] if bits<=8 else [0,2] if bits==16 else [0,8]
                            for offset in offsets:
                                execute(code,mode,bits,op,initial,value,expected,offset);count+=1
                        if mode>=2 and (op=='cas' or (op.startswith('rmw.') and (mode==2 or bits<32))):
                            execute(code,mode,bits,op,0,mask,0,0,True);count+=1
            print(f'{target}/{abi} bits={bits}: passed',flush=True)
print(f'{count} atomic executions passed (operations, widths, orders, neighbors, ABI and pressure).')
