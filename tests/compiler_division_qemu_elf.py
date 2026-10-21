#!/usr/bin/env python3
"""Build freestanding Linux ELF smoke runners for emitted A64 and RV64 BIN.

The ELF contains byte-exact AsterASM output via .incbin. Tests compile/link and
validate ELF even without QEMU. Pass --require-qemu for mandatory QEMU runtime
verification; never silently count a missing QEMU runtime as a pass.
"""
import argparse
import hashlib
import json
import os
import random
import shutil
import subprocess
import tempfile
from pathlib import Path
from compiler_narrow_isa_execution import sx

TYPES=[('u8',8,False),('i8',8,True),('u16',16,False),('i16',16,True),
       ('u32',32,False),('i32',32,True),('u64',64,False),('i64',64,True)]
CTYPE={('u8',8,False):'unsigned char',('i8',8,True):'signed char',
       ('u16',16,False):'unsigned short',('i16',16,True):'short',
       ('u32',32,False):'unsigned int',('i32',32,True):'int',
       ('u64',64,False):'unsigned long long',('i64',64,True):'long long'}


def ref(x,y,d,op,bits,sgn,dynamic,target):
    divisor=y if dynamic else d
    if divisor == 0:
        q=0 if target=='aarch64' else (-1 if sgn else (1<<bits)-1)
    elif sgn:
        q=(abs(x)//abs(divisor))*(-1 if (x<0)!=(divisor<0) else 1)
    else:q=x//divisor
    answer=q if op=='/' else x-q*divisor
    answer &= (1<<bits)-1
    return sx(answer,bits) if sgn else answer


def run():
    ap=argparse.ArgumentParser()
    ap.add_argument('--compiler',required=True)
    ap.add_argument('--opt-level',choices=['0','2','z'],default='0')
    ap.add_argument('--clang',default='clang')
    ap.add_argument('--readelf',default='readelf')
    ap.add_argument('--out', help='save generated ELF files for external QEMU/CPU runner')
    ap.add_argument('--require-qemu',action='store_true')
    ap.add_argument('--quick',action='store_true')
    ap.add_argument('--report',help='JSON evidence with every actual runner exit code')
    a=ap.parse_args()
    if a.out:
        root=Path(a.out).resolve();root.mkdir(parents=True,exist_ok=True)
    else:
        temp=tempfile.TemporaryDirectory(prefix='aster-division-qemu-elf-')
        root=Path(temp.name)
    rng=random.Random(0x6a3ab1)
    count=0;runtime=0;missing=set();evidence=[]
    configs=[('aarch64',False),('riscv64',True),('riscv64',False)]
    if a.require_qemu:
        unavailable=[arch for arch in ('aarch64','riscv64')
                     if not shutil.which('qemu-'+arch)]
        if unavailable:
            raise RuntimeError('Division runtime qualification FAILED: missing QEMU executable(s): '+
                               ', '.join(unavailable))
    for target,m in configs:
        triple='riscv64-linux-gnu' if target=='riscv64' else 'aarch64-linux-gnu'
        qemu=shutil.which('qemu-riscv64' if target=='riscv64' else 'qemu-aarch64')
        if qemu is None:missing.add(target)
        asm_start=('.text\n.globl _start\n_start:\nbl run_test\nmov x8, #93\nsvc #0\n'
                   if target=='aarch64' else
                   '.text\n.globl _start\n_start:\ncall run_test\nli a7, 93\necall\n')
        types=TYPES[:2] if a.quick else TYPES
        for typ,bits,sgn in types:
            for dynamic,op,policy in [(d,o,p) for d,o in ((False,'/'),(False,'%'),(True,'/'),(True,'%')) for p in ('hot','size')]:
                d=-7 if sgn else 7
                program=f'export fn aster_function(x:{typ}, y:{typ})->{typ} @abi(c) '
                program+='{ return x '+op+(' y' if dynamic else ' '+str(d))+'; }\n'
                safe='m' if m else 'i'
                label=f'{target}_{safe}_{typ}_{"dynamic" if dynamic else "constant"}_{"div" if op=="/" else "rem"}_{policy}'
                folder=root/label;folder.mkdir(parents=True,exist_ok=True)
                src=folder/'unit.aa';raw=folder/'fn.bin';asm=folder/'entry.S';main=folder/'test.c';exe=folder/'runner.elf'
                src.write_text('module division.qemu_smoke; '+program)
                cc=[a.compiler,'build','-O'+a.opt_level,'--target',target,'--emit','bin','-j','1','-o',str(raw),str(src)]
                if m:cc+=['--feature','m']
                env=os.environ.copy();env['ASTERASM_DIV_POLICY']=policy
                res=subprocess.run(cc,capture_output=True,text=True,timeout=35,env=env)
                if res.returncode:raise AssertionError(('AsterASM',label,res.stderr))
                raw_data=raw.read_bytes()
                if target=='riscv64' and not m:
                    if len(raw_data)%4:raise AssertionError(('unaligned RV64I BIN',label))
                    words=(int.from_bytes(raw_data[i:i+4],'little')
                           for i in range(0,len(raw_data),4))
                    if any((w&127) in (0x33,0x3b) and (w>>25)==1 for w in words):
                        raise AssertionError(('RV64I contains M instruction',label))
                asm.write_text(asm_start+'\n.text\n.balign 16\n.global aster_function\n'
                    'aster_function:\n.incbin "'+raw.as_posix()+'"\n')
                lo=-(1<<(bits-1)) if sgn else 0
                hi=(1<<(bits-1))-1 if sgn else (1<<bits)-1
                candidates=[lo,lo+1,-127,-7,-1,0,1,2,3,7,127,hi-1,hi]
                vals=[v for v in candidates if lo<=v<=hi]
                vals+= [rng.randint(lo,hi) for _ in range(2 if a.quick else 15)]
                records=[]
                for x in vals:
                    y= (rng.choice((1,3,7,11)) if not sgn else rng.choice((-7,-3,1,3,7))) if dynamic else 5
                    if sgn and x==lo and y==-1:y=3
                    if not lo<=y<=hi:y=1
                    want=ref(x,y,d,op,bits,sgn,dynamic,target)
                    records.append((x,y,want))
                if dynamic:
                    exceptional=[(lo,0),(0,0),(hi,0)]
                    if sgn:exceptional.extend([(lo,-1),(lo+1,-1),(-7,3),(7,-3)])
                    records.extend((x,y,ref(x,y,d,op,bits,sgn,dynamic,target)) for x,y in exceptional)
                def scalar(v):return '(T)0x{:x}ULL'.format(v&((1<<bits)-1))
                elements=',\n'.join('    {'+','.join(map(scalar,values))+'}' for values in records)
                main.write_text('typedef '+CTYPE[(typ,bits,sgn)]+' T;\n'
                    'extern T aster_function(T, T);\n'
                    'struct item {T x,y,expected;};\n'
                    'static volatile T sink;\n'
                    '__attribute__((noinline)) int run_test(void) {\n'
                    '  static const struct item v[] = {\n'+elements+'\n  };\n'
                    '  for (unsigned i=0; i<sizeof(v)/sizeof(v[0]);i++) {\n'
                    '    T got = aster_function(v[i].x,v[i].y);\n'
                    '    sink=got;\n'
                    '    if (got != v[i].expected) return (int)i+1;\n'
                    '  }\n  return 0;\n}\n')
                clang=[a.clang,'--target='+triple,'-std=c17','-O1','-ffreestanding',
                       '-fno-builtin','-fno-stack-protector','-nostdlib','-static',
                       '-fuse-ld=lld','-Wl,-e,_start','-Wl,--build-id=none',
                       '-Wl,-z,max-page-size=4096','-o',str(exe),str(asm),str(main)]
                if target=='riscv64':clang+=['-march=rv64im' if m else '-march=rv64i','-mabi=lp64']
                result=subprocess.run(clang,capture_output=True,text=True,timeout=35)
                if result.returncode:raise AssertionError(('clang cross link',label,result.stderr))
                elf_data=exe.read_bytes()
                if raw_data not in elf_data:
                    raise AssertionError(('raw BIN bytes absent from linked ELF',label))
                elf=subprocess.run([a.readelf,'-h',str(exe)],capture_output=True,text=True,check=True)
                expected_machine='AArch64' if target=='aarch64' else 'RISC-V'
                if 'EXEC (Executable file)' not in elf.stdout or expected_machine not in elf.stdout:
                    raise AssertionError(('invalid ELF header',label,elf.stdout))
                interp=subprocess.run([a.readelf,'-l',str(exe)],capture_output=True,text=True,check=True)
                if 'INTERP' in interp.stdout or 'DYNAMIC' in interp.stdout:
                    raise AssertionError(('not static',label))
                count+=1
                item={'label':label,'target':target,'m':m,'type':typ,'dynamic':dynamic,'op':op,
                      'policy':policy,'samples':len(records),'binary_bytes':len(raw_data),
                      'binary_sha256':hashlib.sha256(raw_data).hexdigest(),
                      'elf_sha256':hashlib.sha256(elf_data).hexdigest(),'exit_code':None}
                if qemu is not None:
                    qemu_cmd=[qemu]
                    if target=='riscv64' and not m:qemu_cmd+=['-cpu','rv64,m=false']
                    proc=subprocess.run(qemu_cmd+[str(exe)],capture_output=True,text=True,timeout=25)
                    if proc.returncode:
                        raise AssertionError(('QEMU mismatch/crash',label,proc.returncode,proc.stderr))
                    runtime+=1
                    item['exit_code']=proc.returncode
                evidence.append(item)
    if a.require_qemu and missing:
        raise RuntimeError('Required division runtime is unavailable: required QEMU unavailable for '+', '.join(sorted(missing)))
    if a.report:
        Path(a.report).write_text(json.dumps({'cross_linked':count,'executed':runtime,'records':evidence},indent=2)+'\n')
    print(f'PASS: {count} freestanding Linux ELF cross-linked; {runtime} QEMU executions; '
          f'QEMU unavailable for {sorted(missing)}')
    if a.out:print('ELF test runners:',root)

if __name__=='__main__':run()
