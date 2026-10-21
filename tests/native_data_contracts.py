#!/usr/bin/env python3
"""A64 operand forms, all logical masks/field encodings, flags and conditions."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import tempfile
from native_processor_qemu import command,other_kernel
from native_processor_contracts import assemble
from native_binding_execution import scalar_call,u,arm

MASK=(1 << 64)-1
ADD='add sub adds subs'.split();LOGIC='and orr eor ands bic orn eon bics'.split()
SELECT='csel csinc csinv csneg'.split();COMPARE=['ccmn','ccmp'];NAMES=ADD+LOGIC+SELECT+COMPARE
CONDS='eq ne cs cc mi pl vs vc hi ls ge lt gt le al nv'.split();SHIFTS='lsl lsr asr ror'.split()
EXTENDS='uxtb uxth uxtw uxtx sxtb sxth sxtw sxtx'.split()

def signed(x,bits):
    x &= (1 << bits)-1
    return x-(1 << bits) if x >> (bits-1) else x

def rotate(x,r,bits):
    r %= bits
    return ((x >> r) | (x << ((bits-r) % bits))) & ((1 << bits)-1)

def masks(bits):
    out=set()
    for size in (2,4,8,16,32,64):
        if size>bits:break
        for ones in range(1,size):
            for r in range(size):
                element=rotate((1 << ones)-1,r,size)
                out.add(sum(element << j for j in range(0,bits,size)))
    assert len(out)==(1302 if bits==32 else 5334)
    return sorted(out)

def condition(cc,flags):
    n,z,c,v=((flags >> j)&1 for j in (3,2,1,0))
    base=[z,c,n,v,c and not z,n==v,(n==v) and not z,True][cc//2]
    return bool(not base if cc & 1 and cc!=15 else base)

def fixtures(smoke=False):
    out=[]
    for bits in ([64] if smoke else [32,64]):
        for name in ADD+LOGIC:
            for shift in range(3 if name in ADD else 4):
                for amount in ([1] if smoke else [0,1,bits-1]):out.append(dict(name=name,bits=bits,kind='shift',mode=shift,amount=amount))
        for name in ADD:
            for mode in range(8):
                for amount in ([1] if smoke else [0,4]):out.append(dict(name=name,bits=bits,kind='extend',mode=mode,amount=amount))
            for imm in ([4096] if smoke else [0,4095,4096,0xfff000]):out.append(dict(name=name,bits=bits,kind='imm',imm=imm))
            out.append(dict(name=name,bits=bits,kind='extend',mode=2,amount=2,sp=True))
            out.append(dict(name=name,bits=bits,kind='imm',imm=17,sp=True))
        for name in SELECT:
            for cc in range(2 if smoke else 16):out.append(dict(name=name,bits=bits,kind='select',cond=cc))
        for name in COMPARE:
            for cc in range(2 if smoke else 16):
                for imm in ([None] if smoke else [None,0,31]):
                    for nzcv in ([15] if smoke else [0,15]):out.append(dict(name=name,bits=bits,kind='compare',cond=cc,nzcv=nzcv,imm=imm))
        for name in ('cmp','cmn'):
            out += [dict(name=name,bits=bits,kind='shift',mode=2,amount=bits-1),dict(name=name,bits=bits,kind='imm',imm=4096),
                    dict(name=name,bits=bits,kind='extend',mode=6,amount=4)]
    for bits in ([64] if smoke else [32,64]):
        out += [dict(name='and',bits=bits,kind='mask',imm=((1 << bits)-1)^255,literal='~255'),
                dict(name='orr',bits=bits,kind='mask',imm=((1 << bits)-1)^255,literal='-256'),
                dict(name='eor',bits=bits,kind='mask',imm=255,literal='0o377')]
    return out

def source(c,d=None,a=None,b=None):
    n,bits,kind=c['name'],c['bits'],c['kind'];p='x' if bits==64 else 'w'
    d=d or p+'0';a=a or ('sp' if bits==64 else 'wsp') if c.get('sp') else a or p+'0'
    b=b or ('x1' if bits==64 and c.get('mode',0)%4==3 and kind=='extend' else 'w1' if kind=='extend' else p+'1')
    roles=(f'lhs:{a},rhs:' if n in COMPARE+['cmp','cmn'] else f'dst:{d},lhs:{a},rhs:')
    roles+=c.get('literal',str(c.get('imm'))) if kind in ('imm','mask') or kind=='compare' and c['imm'] is not None else b
    if kind=='shift':roles+=f',shift:{SHIFTS[c["mode"]]},amount:{c["amount"]}'
    if kind=='extend':roles+=f',extend:{EXTENDS[c["mode"]]},amount:{c["amount"]}'
    if kind in ('compare','select'):roles+=f',cond:{CONDS[c["cond"]]}'
    if kind=='compare':roles+=f',nzcv:{c["nzcv"]}'
    return n+' {'+roles+'};'

def assembly(c):
    n,bits,kind=c['name'],c['bits'],c['kind'];p='x' if bits==64 else 'w';a=('sp' if bits==64 else 'wsp') if c.get('sp') else p+'0'
    b=('x1' if bits==64 and c.get('mode',0)%4==3 else 'w1') if kind=='extend' else p+'1'
    operands=(f'{a}, ' if n in COMPARE+['cmp','cmn'] else f'{p}0, {a}, ')
    operands+=f'#{c["imm"]}' if kind in ('imm','mask') or kind=='compare' and c['imm'] is not None else b
    if kind=='shift':operands+=f', {SHIFTS[c["mode"]]} #{c["amount"]}'
    if kind=='extend':operands+=f', {EXTENDS[c["mode"]]} #{c["amount"]}'
    if kind=='compare':operands+=f', #{c["nzcv"]}'
    if kind in ('compare','select'):operands+=f', {CONDS[c["cond"]]}'
    return n+' '+operands

def reference(c,a,b,flags):
    bits=c['bits'];mask=(1 << bits)-1;n=c['name'];kind=c['kind'];x=a & mask;y=b & mask
    if c.get('sp'):x=0x4ff000 & mask
    if kind in ('imm','mask'):y=c['imm']
    if kind=='shift':
        shift=c['amount'];mode=c['mode']
        y=([lambda:y << shift,lambda:y >> shift,lambda:signed(y,bits) >> shift,lambda:rotate(y,shift,bits)][mode]()) & mask
    if kind=='extend':
        size=min([8,16,32,64][c['mode']%4],bits);y=b & ((1 << size)-1)
        if c['mode']>=4:y=signed(y,size)
        y=(y << c['amount']) & mask
    if kind=='select':
        result=x if condition(c['cond'],flags) else y if n=='csel' else y+1 if n=='csinc' else ~y if n=='csinv' else -y
        return result & mask,flags << 28
    if kind=='compare':
        if not condition(c['cond'],flags):return a,c['nzcv'] << 28
        y=b & mask if c['imm'] is None else c['imm'];n='adds' if n=='ccmn' else 'subs'
    if n in ('cmp','cmn'):n='subs' if n=='cmp' else 'adds'
    if n in ADD:
        subtract=n.startswith('sub');right=(~y & mask) if subtract else y
        total=x+right+int(subtract);result=total & mask
        if n.endswith('s'):
            overflow=bool((x ^ result) & (right ^ result) & (1 << (bits-1)))
            flags=(int(result >> (bits-1)) << 3)|(int(result==0) << 2)|(int(total>mask) << 1)|int(overflow)
    else:
        if n in ('bic','orn','eon','bics'):y=~y & mask
        result=(x & y if n in ('and','ands','bic','bics') else x | y if n in ('orr','orn') else x ^ y) & mask
        if n in ('ands','bics'):flags=(int(result >> (bits-1)) << 3)|(int(result==0) << 2)
    if kind=='compare' or c['name'] in ('cmp','cmn'):result=a
    return result,flags << 28

def execute(code,a,b,flags):
    cpu=u.Uc(u.UC_ARCH_ARM64,u.UC_MODE_ARM);cpu.mem_map(0x100000,0x100000);cpu.mem_map(0x400000,0x100000);cpu.mem_write(0x100000,code)
    cpu.reg_write(arm.UC_ARM64_REG_SP,0x4ff000);cpu.reg_write(arm.UC_ARM64_REG_LR,0x1f0000)
    for reg,v in zip((arm.UC_ARM64_REG_X0,arm.UC_ARM64_REG_X1,arm.UC_ARM64_REG_X2),(a,b,flags << 28)):cpu.reg_write(reg,v & MASK)
    saved=[getattr(arm,'UC_ARM64_REG_X'+str(j)) for j in range(19,30)]
    for j,r in enumerate(saved):cpu.reg_write(r,0xabcdef120000+j)
    cpu.reg_write(arm.UC_ARM64_REG_NZCV,flags << 28);cpu.emu_start(0x100000,0x1f0000,timeout=1000000,count=100000)
    assert cpu.reg_read(arm.UC_ARM64_REG_PC)==0x1f0000 and cpu.reg_read(arm.UC_ARM64_REG_SP)==0x4ff000
    for j,r in enumerate(saved):assert cpu.reg_read(r)==0xabcdef120000+j
    return cpu.reg_read(arm.UC_ARM64_REG_X0),cpu.reg_read(arm.UC_ARM64_REG_NZCV)

def managed(c,mode):
    typ='u32' if c['bits']==32 else 'u64'
    body='msr {sys:nzcv,src:flags};mov {dst:out,src:a};'+source(c,'out','a','b')+'mrs {dst:state,sys:nzcv};'
    return ('module data.managed;export fn run(a:'+typ+',b:'+typ+',flags:'+typ+')->'+typ+' {let out:'+typ+';let state:'+typ+';let live:'+typ+'=flags*17+9;'+mode+
            ' aarch64 inputs(a,b,flags) outputs(out,state) {'+body+'}return out^state^live;}')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for x in ('compiler','clang'):p.add_argument('--'+x,required=True)
    p.add_argument('--dumper');p.add_argument('--out');p.add_argument('--qemu',action='store_true');p.add_argument('--smoke',action='store_true');p.add_argument('--python-path')
    args=p.parse_args();modes=('native',) if args.smoke else ('native','exact','unsafe exact');levels=('0',) if args.smoke else ('0','2','z')
    values=[(0,0),(MASK,1),(1 << 63,MASK),(0x7fffffffffffffff,1),(0x1122334480000000,0xffffffff)];records=[];compiled=calls=rejects=0
    corpus=fixtures(args.smoke)
    with tempfile.TemporaryDirectory(prefix='aster-data-') as temp:
        work=Path(temp);src=work/'case.aa';raw=work/'case.bin'
        def build(text,level='0',pressure=0,promote=True):
            nonlocal compiled
            src.write_text(text);argv=[args.compiler,'build','--target','aarch64','-O'+level,'--reg-limit',str(pressure),'-j','1','-o',raw,src]
            if not promote:argv+=['--no-promote']
            command(argv);compiled+=1;return raw.read_bytes()
        if args.qemu:
            # Retain every condition and extension type, and boundary shifts.
            selected=[c for c in corpus if c['kind'] in ('select','extend') and c.get('amount',4)==4 or c['kind']=='compare' and c['nzcv']==15 and c['imm'] is None or c['kind']=='shift' and c['amount']==c['bits']-1 or c['kind']=='imm' and c['imm']==4096]
            selected += [dict(name=n,bits=b,kind='mask',imm=v) for n in ('and','orr','eor','ands') for b in (32,64) for v in masks(b)[::500]]
            if args.smoke:selected=corpus[:8]
            for mode in modes:
                for level in levels:
                    binary=bytearray();entries=[]
                    for c in selected:
                        code=build('module data.run;export fn run() @naked {'+mode+' aarch64 {msr {sys:nzcv,src:x2};'+source(c)+'mrs {dst:x3,sys:nzcv};ret;}}',level)
                        assert len(code)==16
                        entries.append((c,len(binary)));binary.extend(code)
                    library=work/'functions.bin';library.write_bytes(binary)
                    runner=['mov x28,sp']+[f'ldr x{j},={0x123450+j}' for j in range(19,28)]
                    for c,offset in entries:
                        for flags in (0,6,9,15):
                            for a,b in values[:2]:
                                want,state=reference(c,a,b,flags)
                                runner += [f'ldr x0,={a}',f'ldr x1,={b}',f'ldr x2,={flags << 28}',f'ldr x16,=aster_entry+{offset}','blr x16',
                                           f'ldr x16,={want}','cmp x0,x16','b.ne failed',f'ldr x16,={state}','cmp x3,x16','b.ne failed','mov x17,sp','cmp x17,x28','b.ne failed']
                                for j in range(19,28):runner += [f'ldr x16,={0x123450+j}',f'cmp x{j},x16','b.ne failed']
                                runner += ['b 1f','.ltorg','1:'];calls+=1
                    elf,_=other_kernel('aarch64','\n'.join(runner)+'\n',library,work,args.clang)
                    records.append({'mode':mode,'level':level,'binaries':len(entries),'calls':len(entries)*8,'sha256':hashlib.sha256(binary).hexdigest(),'kernel_sha256':hashlib.sha256(elf.read_bytes()).hexdigest()})
                    print('PASS: A64 data QEMU',mode,level,flush=True)
            report={'instruction_schemas':18,'qemu_runs':len(records),'compiled_binaries':compiled,'guarded_calls':calls,'records':records}
        else:
            mask_cases=[dict(name=n,bits=bits,kind='mask',imm=v) for bits in ([64] if args.smoke else [32,64]) for n in ('and','orr','eor','ands') for v in (masks(bits)[::1000] if args.smoke else masks(bits))]
            all_cases=corpus+mask_cases;blob=bytearray();code_by_case={}
            for start in range(0,len(all_cases),1024):
                batch=all_cases[start:start+1024];want=assemble(args.clang,'aarch64','\n'.join(assembly(c) for c in batch)+'\nret',work)
                for mode in modes:
                    for level in levels:
                        code=build('module data.words;export fn run() @naked {'+mode+' aarch64 {'+''.join(source(c) for c in batch)+'ret;}}',level)
                        assert code==want,('data batch',start,mode,level,next((j for j in range(0,min(len(code),len(want)),4) if code[j:j+4]!=want[j:j+4]),None))
                for j,c in enumerate(batch):code_by_case[id(c)]=want[j*4:j*4+4]+bytes.fromhex('c0035fd6')
                blob.extend(code)
                print('PASS: A64 operand/mask words',min(start+1024,len(all_cases)),'/',len(all_cases),flush=True)
            raw.write_bytes(blob);dump=json.loads(command([args.dumper,'--target','aarch64','--strict','--format','json',raw]).stdout)
            assert assemble(args.clang,'aarch64','\n'.join(i['assembly'] for i in dump['instructions']),work)==blob
            # Logical fields are decoded independently from the encoding
            # algorithm: choose element length from N:NOT(imms), then tile.
            valid=[];words=[]
            for bits in (32,64):
                for op in range(4):
                    for fields in range(8192):
                        n=fields >> 12;s=fields & 63;r=(fields >> 6)&63;selector=(n << 6)|((~s)&63);length=selector.bit_length()-1
                        legal=length>=1 and (1 << length)<=bits and (s & ((1 << length)-1))!=((1 << length)-1)
                        word=(0x12000000 | ((bits==64) << 31) | (op << 29) | (n << 22) | (r << 16) | (s << 10) | 32)
                        words.append(word);valid.append(legal)
            raw.write_bytes(b''.join(w.to_bytes(4,'little') for w in words));dump=json.loads(command([args.dumper,'--target','aarch64','--strict','--format','json',raw],expected=3).stdout)
            assert [i['recognized'] for i in dump['instructions']]==valid
            recognized=[i['assembly'] for i in dump['instructions'] if i['recognized']]
            assert assemble(args.clang,'aarch64','\n'.join(recognized),work)==b''.join(w.to_bytes(4,'little') for w,v in zip(words,valid) if v)
            for c in corpus+mask_cases[::97]:
                for flags in (range(16) if c['kind'] in ('select','compare') and not args.smoke else (0,15)):
                    for a,b in values:
                        got=execute(code_by_case[id(c)],a,b,flags);want=reference(c,a,b,flags)
                        assert got==want,(c,a,b,flags,got,want);calls+=1
            selected=[c for c in corpus if not c.get('sp') and (c['kind'] in ('select','compare') and c.get('cond') in (0,1,14,15) and (c['kind']!='compare' or c['imm'] is None and c['nzcv']==15) or c['kind']=='shift' and c['mode']==0 and c['amount']==1 or c['kind']=='extend' and c['mode'] in (2,6) and c['amount']==4)]
            selected+=mask_cases[::1000]
            for c in selected:
                for mode in modes:
                    for level in levels:
                        for pressure,promote in ([(2,False)] if args.smoke else [(0,True),(2,False)]):
                            code=build(managed(c,mode),level,pressure,promote)
                            for flags in (0,15):
                                for a,b in values[:3]:
                                    want,state=reference(c,a,b,flags);mask=(1 << c['bits'])-1
                                    if c['name'] in COMPARE+['cmp','cmn']:want=a & mask
                                    expected=(want ^ state ^ (((flags << 28)*17+9)&mask)) & mask
                                    assert scalar_call('aarch64',code,(a,b,flags << 28))==expected,(c,mode,level,flags,a,b)
                                    calls+=1
                            records.append({'case':c,'mode':mode,'level':level,'pressure':pressure,'bytes':len(code),'sha256':hashlib.sha256(code).hexdigest()})
            bad=['add {dst:x0,lhs:x1,rhs:x2,shift:ror,amount:1};','add {dst:w0,lhs:w1,rhs:w2,shift:lsl,amount:32};',
                 'sub {dst:x0,lhs:x1,rhs:w2,extend:sxtw,amount:5};','and {dst:x0,lhs:x1,rhs:x2,extend:uxtw,amount:0};',
                 'and {dst:x0,lhs:x1,rhs:0};','orr {dst:x0,lhs:x1,rhs:0xffffffffffffffff};','eor {dst:x0,lhs:x1,rhs:0x12345678};',
                 'orr {dst:xzr,lhs:x1,rhs:255};','adds {dst:x0,lhs:xzr,rhs:w1,extend:uxtw};',
                 'csel {dst:x0,lhs:x1,rhs:x2,cond:16};','ccmp {lhs:x0,rhs:32,nzcv:0,cond:eq};',
                 'ccmn {lhs:x0,rhs:x1,nzcv:16,cond:nv};','add {dst:sp,lhs:x0,rhs:x1,extend:uxtx};',
                 'and {dst:x0,lhs:x1,rhs:0x10000000000000000};','and {dst:x0,lhs:x1,rhs:~0};']
            for mode in modes:
                for body in bad:
                    src.write_text('module data.bad;export fn run() @naked {'+mode+' aarch64 {'+body+'ret;}}');raw.unlink(missing_ok=True)
                    failed=command([args.compiler,'build','--target','aarch64','-j','1','-o',raw,src],expected=1)
                    assert failed.stderr and not raw.exists() and "Native instruction: '"+body.split()[0]+"'" in failed.stderr;rejects+=1
            report={'instruction_schemas':18,'operand_oracle_cases':len(corpus),'logical_mask_cases':len(mask_cases),'logical_field_words':len(words),
                    'valid_logical_field_roundtrips':sum(valid),'reserved_logical_field_rejections':len(valid)-sum(valid),
                    'compiled_binaries':compiled,'managed_binaries':len(records),'guarded_calls':calls,'rejections':rejects,'records':records}
        if args.out:Path(args.out).write_text(json.dumps(report,indent=2)+'\n')
    print('PASS: A64 data',json.dumps({k:v for k,v in report.items() if k!='records'}))

if __name__=='__main__':main()
