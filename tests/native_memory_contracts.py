#!/usr/bin/env python3
"""Independent RV64 integer memory bytes, effects, bounds, ABI and system execution."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile

from native_processor_qemu import command, other_kernel
from native_scalar_contracts import assemble, signed, MASK
import unicorn as u
from unicorn import riscv_const as rv

LOADS = {'lb':(1,True),'lbu':(1,False),'lh':(2,True),'lhu':(2,False),
         'lw':(4,True),'lwu':(4,False),'ld':(8,False)}
STORES = {'sb':1,'sh':2,'sw':4,'sd':8}
NAMES = tuple(LOADS)+tuple(STORES)
VALUES = (0,1,MASK,1 << 63,0x80000000,0x8000,0x80,0xabcdef0187654321)
CANARY = 0x123456789abcdef0


def native(name,reg='a0',base='a1',offset=-16):
    return f'{name} {{'+('dst:' if name in LOADS else 'src:')+f'{reg},base:{base},offset:{offset}'+'};'


def loaded(name,value):
    size,negative = LOADS[name]; value &= (1 << (size*8))-1
    return (signed(value,size*8) if negative else value) & MASK


def memory_call(code,name,value):
    cpu = u.Uc(u.UC_ARCH_RISCV,u.UC_MODE_RISCV64)
    cpu.mem_map(0x100000,0x100000); cpu.mem_map(0x400000,0x100000); cpu.mem_map(0x800000,0x2000)
    cpu.mem_write(0x100000,code)
    before = bytearray(b'\xa5'*0x2000); before[0x1000:0x1008] = value.to_bytes(8,'little')
    cpu.mem_write(0x800000,bytes(before)); cpu.reg_write(rv.UC_RISCV_REG_SP,0x4ff000)
    cpu.reg_write(rv.UC_RISCV_REG_RA,0x1f0000)
    payload = value ^ MASK; seed = 0x3141592653589793
    for reg,item in zip((rv.UC_RISCV_REG_A0,rv.UC_RISCV_REG_A1,rv.UC_RISCV_REG_A2),(0x801010,payload,seed)): cpu.reg_write(reg,item)
    for j in range(12): cpu.reg_write(getattr(rv,'UC_RISCV_REG_S'+str(j)),CANARY+j)
    cpu.emu_start(0x100000,0x1f0000,timeout=1000000,count=100000)
    assert cpu.reg_read(rv.UC_RISCV_REG_PC) == 0x1f0000 and cpu.reg_read(rv.UC_RISCV_REG_SP) == 0x4ff000
    for j in range(12): assert cpu.reg_read(getattr(rv,'UC_RISCV_REG_S'+str(j))) == CANARY+j
    expected = (loaded(name,value) if name in LOADS else payload) ^ ((seed*17+9)&MASK)
    assert cpu.reg_read(rv.UC_RISCV_REG_A0) == expected,(name,value,cpu.reg_read(rv.UC_RISCV_REG_A0),expected)
    if name in STORES: before[0x1000:0x1000+STORES[name]] = payload.to_bytes(8,'little')[:STORES[name]]
    assert bytes(cpu.mem_read(0x800000,0x2000)) == before,(name,'memory or guard corruption')


def qemu(args,work,modes,levels):
    source = work/'case.aa'; raw = work/'case.bin'; library = work/'functions.bin'; records = []; calls = 0
    for mode in modes:
        for level in levels:
            binary = bytearray(); entries = []
            for name in NAMES:
                for zero in (False,True):
                    source.write_text('module memory.execute; export fn run() @naked {'+mode+' riscv64 {'+native(name,'zero' if zero else 'a0')+'ret;}}')
                    command([args.compiler,'build','--target','riscv64','-O'+level,'-j','1','-o',raw,source])
                    code = raw.read_bytes(); assert len(code) == 8
                    entries.append((name,zero,len(binary))); binary.extend(code)
            library.write_bytes(binary)
            runner = ['mv s11,sp']+[f'li s{j},{CANARY+j}' for j in range(11)]
            for name,zero,offset in entries:
                for value in VALUES:
                    payload = value ^ MASK
                    runner += ['la t2,data_center',f'li t0,{value}','sd t0,0(t2)',
                               f'li t0,{CANARY}','sd t0,-8(t2)','sd t0,8(t2)',f'li a0,{payload}',
                               'addi a1,t2,16',f'la t0,aster_entry+{offset}','jalr ra,0(t0)']
                    expected = loaded(name,value) if name in LOADS and not zero else payload
                    if name in STORES:
                        size = STORES[name]; mask = (1 << (8*size))-1
                        expected_memory = (value & ~mask) | ((0 if zero else payload)&mask)
                    else: expected_memory = value
                    runner += [f'li t0,{expected}','bne a0,t0,failed','bne sp,s11,failed',
                               'la t2,data_center','ld t1,0(t2)',f'li t0,{expected_memory}','bne t0,t1,failed',
                               f'li t0,{CANARY}','ld t1,-8(t2)','bne t0,t1,failed','ld t1,8(t2)','bne t0,t1,failed']
                    for j in range(11): runner += [f'li t0,{CANARY+j}',f'bne s{j},t0,failed']
                    calls += 1
            runner += ['j memory_checks_done','.balign 8','data_before: .quad 0','data_center: .quad 0',
                       'data_after: .quad 0','memory_checks_done:']
            elf,_ = other_kernel('riscv64','\n'.join(runner)+'\n',library,work,args.clang)
            records.append({'mode':mode,'level':level,'binaries':len(entries),'calls':len(entries)*len(VALUES),
                            'sha256':hashlib.sha256(binary).hexdigest(),'kernel_sha256':hashlib.sha256(elf.read_bytes()).hexdigest()})
            print('PASS: memory QEMU',mode,'O'+level,flush=True)
    return {'instruction_forms':11,'qemu_runs':len(records),'guarded_calls':calls,
            'compiled_binaries':sum(r['binaries'] for r in records),'records':records}


def contracts(args,work,modes,levels):
    source = work/'case.aa'; raw = work/'case.bin'; records = []; calls = rejected = roundtrips = 0
    def build(text,level='0',limit=0,promote=True):
        source.write_text(text)
        argv = [args.compiler,'build','--target','riscv64','-O'+level,'--reg-limit',str(limit),'-j','1','-o',raw,source]
        if not promote: argv += ['--no-promote']
        command(argv); return raw.read_bytes()
    variants = (('a0','a1'),) if args.smoke else (('a0','a1'),('t6','t5'),('zero','a1'),('a0','gp'))
    offsets = (-16,) if args.smoke else (-2048,-17,-1,0,1,17,2047)
    oracle = [(name,reg,base,off) for name in NAMES for reg,base in variants for off in offsets]
    expected = assemble(args.clang,'riscv64','\n'.join(f'{name} {reg},{off}({base})\nret' for name,reg,base,off in oracle),work)
    actual = bytearray()
    for index,(name,reg,base,off) in enumerate(oracle):
        for level in levels:
            code = build('module memory.exact; export fn run() @naked { unsafe exact riscv64 {'+native(name,reg,base,off)+'ret;}}',level)
            assert code == expected[index*8:index*8+8],(name,reg,base,off,code.hex())
            records.append({'kind':'oracle','name':name,'level':level,'bytes':code.hex()})
        actual.extend(code)
    raw.write_bytes(actual)
    dump = json.loads(command([args.dumper,'--target','riscv64','--strict','--format','json',raw]).stdout)
    rebuilt = assemble(args.clang,'riscv64','\n'.join(i['assembly'] for i in dump['instructions']),work)
    assert rebuilt == actual == expected; roundtrips = len(oracle)
    for name in NAMES:
        for mode in modes:
            body = native(name,'out' if name in LOADS else 'value','p')
            bindings = 'inputs(p) outputs(out)' if name in LOADS else 'inputs(p,value)'
            text = ('module memory.managed; export fn run(p:ptr<u8>,value:u64,seed:u64)->u64 {'
                    'let live:u64=seed*17+9; '+('let out:u64; ' if name in LOADS else '')+mode+' riscv64 '+bindings+' {'+body+'} return '+('out' if name in LOADS else 'value')+'^live;}')
            for level in levels:
                for limit in ((2,) if args.smoke else (0,2)):
                    for promote in ((False,) if args.smoke else (False,True)):
                        code = build(text,level,limit,promote)
                        for value in VALUES: memory_call(code,name,value); calls += 1
                        records.append({'kind':'managed','name':name,'mode':mode,'level':level,'limit':limit,'promote':promote,'bytes':len(code)})
        print('PASS: memory',name,flush=True)
    # Explicit stack forms retain initialization, full-width restoration and natural alignment.
    for store,load,size in (('sb','lb',1),('sh','lh',2),('sw','lw',4),('sd','ld',8)):
        body = f'addi {{dst:sp,lhs:sp,rhs:-16}}; {store} {{src:a0,offset:0}}; {load} {{dst:a0,base:x2,offset:0}}; addi {{dst:sp,lhs:sp,rhs:16}}; ret;'
        reference = assemble(args.clang,'riscv64',f'addi sp,sp,-16\n{store} a0,0(sp)\n{load} a0,0(sp)\naddi sp,sp,16\nret',work)
        for mode in modes:
            for frame in ('@naked','@frame(validate)'):
                code = build('module memory.stack; export fn run() '+frame+' {'+mode+' riscv64 {'+body+'}}')
                assert code == reference; records.append({'kind':'stack','name':store+'/'+load,'mode':mode,'frame':frame})
    for reg in ('sp','ra','gp','tp'):
        reference = assemble(args.clang,'riscv64',f'addi a0,{reg},-17\nret',work)
        for mode in modes:
            for level in levels:
                code = build('module memory.registers; export fn run() @naked {'+mode+' riscv64 {addi {dst:a0,lhs:'+reg+',rhs:-17}; ret;}}',level)
                assert code == reference; records.append({'kind':'register-read','register':reg,'mode':mode,'level':level})
    invalid = [native(name,offset=off) for name in NAMES for off in (-2049,2048)]
    invalid += [native(name,base='f0') for name in NAMES]
    invalid += ['ld {dst:ra,base:a1,offset:0};','lb {dst:gp,base:a1,offset:0};',
                'addi {dst:sp,lhs:sp,rhs:-16}; ld {dst:a0,offset:0}; addi {dst:sp,lhs:sp,rhs:16};',
                'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:s0,offset:0}; sb {src:a0,offset:1}; ld {dst:s0,offset:0}; addi {dst:sp,lhs:sp,rhs:16};',
                'addi {dst:sp,lhs:sp,rhs:-16}; sb {src:s0,offset:0}; add {dst:s0,lhs:a0,rhs:a1}; addi {dst:sp,lhs:sp,rhs:16};',
                'addi {dst:sp,lhs:sp,rhs:-16}; sw {src:a0,offset:0}; ld {dst:a0,offset:0}; addi {dst:sp,lhs:sp,rhs:16};',
                'addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0}; jal {dst:ra,target:leaf}; lb {dst:a0,base:a1,offset:0}; ld {dst:ra,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret; label {name:leaf}; ret;']
    for mode in modes:
        for body in invalid:
            source.write_text('module memory.bad; export fn run() @naked {'+mode+' riscv64 {'+body+('' if body.rstrip().endswith('ret;') else 'ret;')+'}}')
            raw.unlink(missing_ok=True)
            failure = command([args.compiler,'build','--target','riscv64','-j','1','-o',raw,source],expected=1)
            assert failure.stderr and not raw.exists(); rejected += 1
    return {'instruction_forms':11,'compiled_binaries':len(records),'oracle_cases':len(oracle),'llvm_dumper_roundtrips':roundtrips,
            'guarded_calls':calls,'rejections':rejected,'records':records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',required=True); parser.add_argument('--dumper'); parser.add_argument('--clang',required=True)
    parser.add_argument('--out'); parser.add_argument('--python-path'); parser.add_argument('--qemu',action='store_true'); parser.add_argument('--smoke',action='store_true')
    args = parser.parse_args()
    if not args.qemu and not args.dumper: parser.error('--dumper is required for byte/contracts qualification')
    modes = ('native',) if args.smoke else ('native','exact','unsafe exact'); levels = ('0',) if args.smoke else ('0','2','z')
    with tempfile.TemporaryDirectory(prefix='aster-native-memory-') as temporary:
        report = (qemu if args.qemu else contracts)(args,Path(temporary),modes,levels)
    if args.out: Path(args.out).write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS: memory {"QEMU" if args.qemu else "contracts"}: {report["compiled_binaries"]} binaries, {report["guarded_calls"]} guarded calls')


if __name__ == '__main__': main()
