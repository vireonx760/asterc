"""Exercise native port I/O and descriptor-table loads without host device access."""
import argparse, subprocess, sys, tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',required=True);p.add_argument('--python-path',required=True)
a=p.parse_args();sys.path.insert(0,a.python_path)
import unicorn as u
from unicorn import x86_const as x
count=0
with tempfile.TemporaryDirectory(prefix='aster-native-system-') as temporary:
 temporary=Path(temporary);source=temporary/'test.aa';image=temporary/'test.bin'
 for abi in ['sysv','win64']:
  for limit in [0,2]:
   for promote in [True,False]:
    for bits,accumulator in [(8,'al'),(16,'ax'),(32,'eax')]:
     for port_kind in ['register','immediate']:
      value=0x1234abcd & ((1<<bits)-1);port=0x1234 if port_kind=='register' else 0xe9
      setup='mov {dst:dx,src:port};' if port_kind=='register' else ''
      operand='dx' if port_kind=='register' else '0xe9'
      source.write_text(f'''module native.system;
export fn run(port:u16)->u{bits} @abi(c) {{
 let result:u{bits};
 native x86_64 inputs(port) outputs(result) {{
  {setup}
  in {{dst:{accumulator},port:{operand}}};
  mov {{dst:result,src:{accumulator}}};
  out {{src:{accumulator},port:{operand}}};
 }}
 return result;
}}''')
      command=[a.compiler,'build','--target','x86_64','--abi',abi,'--reg-limit',str(limit),'--no-red-zone','-j','1','-o',str(image),str(source)]
      if not promote:command+=['--no-promote']
      proc=subprocess.run(command,capture_output=True,text=True,timeout=30);assert proc.returncode==0,proc.stderr
      c=u.Uc(u.UC_ARCH_X86,u.UC_MODE_64);c.mem_map(0x100000,0x100000);c.mem_map(0x400000,0x100000)
      c.mem_write(0x100000,image.read_bytes());c.mem_write(0x47fff8,(0x1f0000).to_bytes(8,'little'));c.reg_write(x.UC_X86_REG_RSP,0x47fff8)
      saved=[x.UC_X86_REG_RBX,x.UC_X86_REG_RBP,x.UC_X86_REG_R12,x.UC_X86_REG_R13,x.UC_X86_REG_R14,x.UC_X86_REG_R15]
      if abi=='win64':saved += [x.UC_X86_REG_RDI,x.UC_X86_REG_RSI]
      for index,register in enumerate(saved):c.reg_write(register,0xabc000+index)
      c.reg_write(x.UC_X86_REG_RCX if abi=='win64' else x.UC_X86_REG_RDI,port)
      seen=[]
      def read(cpu,number,size,userdata):
       assert number==port and size==bits//8,(number,size);seen.append(('in',number,size));return value
      def write(cpu,number,size,output,userdata):
       assert (number,size,output)==(port,bits//8,value),(number,size,output);seen.append(('out',number,size))
      c.hook_add(u.UC_HOOK_INSN,read,None,1,0,x.UC_X86_INS_IN);c.hook_add(u.UC_HOOK_INSN,write,None,1,0,x.UC_X86_INS_OUT)
      c.emu_start(0x100000,0x1f0000,timeout=1000000,count=10000)
      assert c.reg_read(x.UC_X86_REG_RIP)==0x1f0000 and c.reg_read(x.UC_X86_REG_RSP)==0x480000
      assert c.reg_read(x.UC_X86_REG_RAX)&((1<<bits)-1)==value and len(seen)==2
      for index,register in enumerate(saved):assert c.reg_read(register)==0xabc000+index
      count+=1
    for operation,register in [('lidt',x.UC_X86_REG_IDTR),('lgdt',x.UC_X86_REG_GDTR)]:
     source.write_text(f'''module native.tables;
export fn run(address:usize) @abi(c) {{ native x86_64 inputs(address) {{ {operation} {{addr:address}}; }} }}''')
     command=[a.compiler,'build','--target','x86_64','--abi',abi,'--reg-limit',str(limit),'--no-red-zone','-j','1','-o',str(image),str(source)]
     if not promote:command+=['--no-promote']
     proc=subprocess.run(command,capture_output=True,text=True,timeout=30);assert proc.returncode==0,proc.stderr
     c=u.Uc(u.UC_ARCH_X86,u.UC_MODE_64);c.mem_map(0x100000,0x100000);c.mem_map(0x400000,0x100000)
     c.mem_write(0x100000,image.read_bytes());c.mem_write(0x47fff8,(0x1f0000).to_bytes(8,'little'));c.reg_write(x.UC_X86_REG_RSP,0x47fff8)
     c.mem_write(0x410000,b'\xff\x0f'+(0x412000).to_bytes(8,'little'))
     c.reg_write(x.UC_X86_REG_RCX if abi=='win64' else x.UC_X86_REG_RDI,0x410000)
     c.emu_start(0x100000,0x1f0000,timeout=1000000,count=10000)
     descriptor=c.reg_read(register);assert descriptor[1:3]==(0x412000,4095),descriptor
     assert c.reg_read(x.UC_X86_REG_RSP)==0x480000;count+=1
 for instruction in ['in {dst:bl,port:dx};','in {dst:rax,port:dx};','out {src:al,port:256};','out {src:al,port:edx};','lidt {addr:eax};']:
  source.write_text('module native.bad; export fn run() { native x86_64 { '+instruction+' } }')
  image.unlink(missing_ok=True)
  proc=subprocess.run([a.compiler,'build','--target','x86_64','-j','1','-o',str(image),str(source)],capture_output=True,text=True,timeout=30)
  assert proc.returncode!=0 and not image.exists(),instruction
print(f'{count} native system executions and 5 invalid-form rejections passed.')
