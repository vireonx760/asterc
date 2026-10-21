#!/usr/bin/env python3
"""Independent A64 scalar LLVM bytes, guarded execution and rejection corpus."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import re
import tempfile

from native_processor_qemu import command, other_kernel
from native_processor_contracts import assemble
from native_binding_execution import scalar_call

ROOT = Path(__file__).resolve().parents[1]
MASK = (1 << 64)-1
BINARY = 'bic orn eon adds subs ands bics adc adcs sbc sbcs sdiv udiv lslv lsrv asrv rorv smulh umulh'.split()
UNARY = 'clz cls rbit rev16 rev32 rev'.split()
FOUR = 'madd msub'.split()
LONG = 'smaddl smsubl umaddl umsubl'.split()
WIDE = 'movz movn movk'.split()
FIELD = 'sbfm ubfm bfm'.split()
NAMES = BINARY+UNARY+FOUR+LONG+['extr']+WIDE+FIELD
ONLY64 = set(LONG+['smulh','umulh','rev32'])
FLAGS = set('adds subs ands bics adcs sbcs'.split())
READ_FLAGS = set('adc adcs sbc sbcs'.split())
RMW = {'movk','bfm'}


def signed(v,bits):
    v &= (1 << bits)-1
    return v-(1 << bits) if v >> (bits-1) else v


def ror(v,amount,bits):
    amount %= bits
    return ((v >> amount) | (v << (bits-amount))) & ((1 << bits)-1)


def reference(name,a,b,acc,bits,imm=(13,27),carry=0):
    mask = (1 << bits)-1; x,y = a & mask,b & mask
    flags = 0
    if name in ('bic','bics'): result = x & ~y
    elif name == 'orn': result = x | ~y
    elif name == 'eon': result = x ^ ~y
    elif name == 'ands': result = x & y
    elif name in ('adds','subs','adc','adcs','sbc','sbcs'):
        subtract = name in ('subs','sbc','sbcs')
        c = carry if name in READ_FLAGS else int(subtract)
        right = (~y & mask) if subtract else y
        total = x+right+c; result = total & mask
        overflow = int(((x ^ result) & (right ^ result) & (1 << (bits-1))) != 0)
        flags = (int(result >> (bits-1)) << 31) | (int(result == 0) << 30) | (int(total > mask) << 29) | (overflow << 28)
    elif name == 'sdiv':
        sx,sy = signed(x,bits),signed(y,bits)
        result = 0 if not sy else abs(sx)//abs(sy)*(-1 if (sx < 0) != (sy < 0) else 1)
    elif name == 'udiv': result = x//y if y else 0
    elif name == 'lslv': result = x << (y & (bits-1))
    elif name == 'lsrv': result = x >> (y & (bits-1))
    elif name == 'asrv': result = signed(x,bits) >> (y & (bits-1))
    elif name == 'rorv': result = ror(x,y,bits)
    elif name == 'umulh': result = (x*y) >> 64
    elif name == 'smulh': result = (signed(x,64)*signed(y,64)) >> 64
    elif name == 'clz': result = bits-x.bit_length()
    elif name == 'cls': result = bits-(x ^ (mask if x >> (bits-1) else 0)).bit_length()-1
    elif name == 'rbit': result = int(f'{x:0{bits}b}'[::-1],2)
    elif name in ('rev16','rev32','rev'):
        group = 2 if name == 'rev16' else 4 if name == 'rev32' else bits//8
        data = x.to_bytes(bits//8,'little')
        result = int.from_bytes(b''.join(data[j:j+group][::-1] for j in range(0,len(data),group)),'little')
    elif name in FOUR+LONG:
        p = (signed(a,32)*signed(b,32) if name.startswith('sm') and name in LONG else
             (a & 0xffffffff)*(b & 0xffffffff) if name in LONG else x*y)
        result = acc-p if name in ('msub','smsubl','umsubl') else acc+p
    elif name == 'extr': result = ((x << bits) | y) >> imm[0]
    elif name in WIDE:
        half,shift = imm
        result = half << shift
        if name == 'movn': result = ~result
        elif name == 'movk': result |= acc & ~(65535 << shift)
    elif name in FIELD:
        r,s = imm
        # DecodeBitMasks for bitfields: a rotated source mask followed by
        # a low destination mask. SBFM fills from the original source bit s.
        write_mask = ror((1 << (s+1))-1,r,bits)
        top_mask = (1 << (((s-r) % bits)+1))-1
        bottom = ror(x,r,bits) & write_mask
        if name == 'bfm':
            bottom |= acc & ~write_mask
            result = (acc & ~top_mask) | (bottom & top_mask)
        elif name == 'sbfm': result = ((mask if (x >> s) & 1 else 0) & ~top_mask) | (bottom & top_mask)
        else: result = bottom & top_mask
    else: raise AssertionError(name)
    result &= mask
    if name in ('ands','bics'): flags = (int(result >> (bits-1)) << 31) | (int(result == 0) << 30)
    return result,flags


def native(name,d,a,b,acc,imm=(13,27)):
    if name in UNARY: roles = f'dst:{d},src:{a}'
    elif name in WIDE: roles = f'dst:{d},imm:{imm[0]},shift:{imm[1]}'
    elif name in FIELD: roles = f'dst:{d},src:{a},immr:{imm[0]},imms:{imm[1]}'
    else:
        roles = f'dst:{d},lhs:{a},rhs:{b}'
        if name in FOUR+LONG: roles += f',acc:{acc}'
        if name == 'extr': roles += f',lsb:{imm[0]}'
    return name+' {'+roles+'};'


def assembly(name,d,a,b,acc,imm):
    if name in UNARY: operands = f'{d}, {a}'
    elif name in WIDE: operands = f'{d}, #{imm[0]}, lsl #{imm[1]}'
    elif name in FIELD: operands = f'{d}, {a}, #{imm[0]}, #{imm[1]}'
    else:
        operands = f'{d}, {a}, {b}'
        if name in FOUR+LONG: operands += f', {acc}'
        if name == 'extr': operands += f', #{imm[0]}'
    return name+' '+operands


def cases(smoke=False):
    result = []
    for name in NAMES:
        for bits in ([64] if smoke or name in ONLY64 else [32,64]):
            p = 'x' if bits == 64 else 'w'; q = 'w' if name in LONG else p
            registers = [(p+'0',q+'1',q+'2',p+'3'),(p+'17',q+'16',q+'15',p+'14'),
                         (p+'zr',q+'1',q+'zr',p+'zr'),(p+'0',q+'zr',q+'2',p+'0'),
                         (p+'0',q+'0',q+'0',p+'0')]
            immediates = [(0,0),(65535,bits-16),(0x8123,16)] if name in WIDE else [(0,0),(bits-1,bits-1),(bits-1,0),(13,27)] if name in FIELD else [(0,0),(bits-1,0),(13,0)] if name == 'extr' else [(13,27)]
            if smoke: registers,immediates = registers[:1],immediates[:1]
            for regs in registers:
                for imm in immediates: result.append((name,bits,regs,imm))
            if name in ('adds','subs') and not smoke:
                for immediate in (0,4095,4096,0xfff000):
                    result.append((name,bits,(p+'0',p+'1',str(immediate),p+'3'),(13,27)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('compiler','dumper','clang'): parser.add_argument('--'+flag,required=True)
    parser.add_argument('--out'); parser.add_argument('--smoke',action='store_true'); parser.add_argument('--qemu',action='store_true')
    parser.add_argument('--python-path')
    args = parser.parse_args()
    inventory = re.findall(r'^AST_A64_SCALAR\(\w+,\s*"([^"]+)"',(ROOT/'include/asterasm/detail/aarch64_scalar_instructions.inc').read_text(),re.M)
    assert len(inventory) == len(NAMES) == 38 and set(inventory) == set(NAMES)
    modes = ('native',) if args.smoke else ('native','exact','unsafe exact')
    levels = ('0',) if args.smoke else ('0','2','z')
    records = []; calls = rejected = reserved = call_cases = 0
    rng = random.Random(0xa64ca11)
    values = [(0,0,0),(MASK,1,MASK),(1 << 63,MASK,0x123456789abcdef0),
              ((1 << 63)-1,1,0xabcdef0123456789),(1 << 63,0,1),
              (0x80000000,0xffffffff,MASK),(0xfedcba9876543210,65,0x1122334455667788)]
    values += [(rng.getrandbits(64),rng.getrandbits(64),rng.getrandbits(64)) for _ in range(3)]
    with tempfile.TemporaryDirectory(prefix='aster-a64-scalar-') as temporary:
        work = Path(temporary); source = work/'case.aa'; raw = work/'case.bin'

        def build(text,level='0',pressure=0,promote=True):
            source.write_text(text)
            argv = [args.compiler,'build','--target','aarch64','-O'+level,'--reg-limit',str(pressure),'-j','1','-o',raw,source]
            if not promote: argv += ['--no-promote']
            command(argv); return raw.read_bytes()

        if args.qemu:
            qemu(args,work,build,modes,levels,values,records)
            report = {'instruction_names':38,'qemu_runs':len(records),'compiled_binaries':sum(r['binaries'] for r in records),
                      'guarded_calls':sum(r['calls'] for r in records),'records':records}
        else:
            corpus = cases(args.smoke)
            text = '\n'.join(assembly(name,*regs,imm)+'\nret' for name,bits,regs,imm in corpus)
            expected = assemble(args.clang,'aarch64',text,work)
            assert len(expected) == len(corpus)*8
            collected = bytearray()
            for j,(name,bits,regs,imm) in enumerate(corpus):
                body = native(name,*regs,imm)+'ret;'
                want = expected[j*8:j*8+8]
                for level in levels:
                    code = build('module scalar.a64; export fn run() @naked {unsafe exact aarch64 {'+body+'}}',level)
                    assert code == want,(name,bits,body,code.hex(),want.hex())
                collected.extend(code)
            raw.write_bytes(collected)
            dump = json.loads(command([args.dumper,'--target','aarch64','--strict','--format','json',raw]).stdout)
            rebuilt = assemble(args.clang,'aarch64','\n'.join(i['assembly'] for i in dump['instructions']),work)
            assert rebuilt == expected == collected
            # W forms cannot set N/high immediate bits, and high multiply
            # cannot change the fixed accumulator field.
            for word in (0x13400020,0x33008020,0x53018020,0x13808020,0x52c00020,0x9bc07820,0x9b407820):
                raw.write_bytes(word.to_bytes(4,'little'))
                invalid = json.loads(command([args.dumper,'--target','aarch64','--strict','--format','json',raw],expected=3).stdout)
                assert invalid['unknown'] == 1 and not invalid['instructions'][0]['recognized']; reserved += 1

            for name in NAMES:
                for bits in ([64] if args.smoke or name in ONLY64 else [32,64]):
                    for carry in ([0,1] if name in READ_FLAGS else [0]):
                        imm = (0x8123,bits-16) if name in WIDE else (bits-1,13) if name in FIELD else (13,0)
                        for mode in modes:
                            for level in levels:
                                for pressure,promote in ([(2,False)] if args.smoke else [(0,True),(2,False)]):
                                    code = build(managed(name,bits,mode,imm,carry),level,pressure,promote)
                                    for a,b,acc in values:
                                        result,flags = reference(name,a,b,acc,bits,imm,carry)
                                        expected_value = result ^ (((acc*17)+9) & ((1 << bits)-1))
                                        if name in FLAGS: expected_value ^= flags
                                        actual = scalar_call('aarch64',code,(a,b,acc))
                                        assert actual == expected_value,(name,bits,carry,mode,level,pressure,a,b,acc,actual,expected_value)
                                        calls += 1
                                    records.append({'name':name,'bits':bits,'carry':carry,'mode':mode,'level':level,
                                                    'pressure':pressure,'promote':promote,'bytes':len(code),'sha256':hashlib.sha256(code).hexdigest()})
                print('PASS: A64 scalar',name,flush=True)
            bad = ['movz {dst:w0,imm:65536,shift:0};','movk {dst:w0,imm:1,shift:32};',
                   'movn {dst:x0,imm:1,shift:64};','movz {dst:x0,imm:1,shift:1};',
                   'sbfm {dst:w0,src:w1,immr:32,imms:0};','ubfm {dst:x0,src:x1,immr:0,imms:64};',
                   'extr {dst:w0,lhs:w1,rhs:w2,lsb:32};','smulh {dst:w0,lhs:w1,rhs:w2};',
                   'rev32 {dst:w0,src:w1};','smaddl {dst:x0,lhs:x1,rhs:w2,acc:x3};',
                   'umaddl {dst:w0,lhs:w1,rhs:w2,acc:x3};','clz {dst:x0,src:w1};',
                   'adc {dst:sp,lhs:x0,rhs:x1};','adds {dst:x0,lhs:xzr,rhs:1};',
                   'subs {dst:x0,lhs:x1,rhs:4097};','adcs {dst:x0,lhs:x1,rhs:1};',
                   'clz {dst:x18,src:x0};','clz {dst:x30,src:x0};','clz {dst:x0,src:x18};']
            for mode in modes:
                for body in bad:
                    source.write_text('module scalar.bad; export fn run() @naked {'+mode+' aarch64 {'+body+'ret;}}')
                    raw.unlink(missing_ok=True)
                    failed = command([args.compiler,'build','--target','aarch64','-j','1','-o',raw,source],expected=1)
                    assert failed.stderr and not raw.exists(); rejected += 1
                for name in RMW:
                    source.write_text('module scalar.bad; export fn run(a:u64)->u64 {let out:u64; '+mode+' aarch64 inputs(a) outputs(out) {'+native(name,'out','a','a','a',(1,16))+'}return out;}')
                    raw.unlink(missing_ok=True)
                    failed = command([args.compiler,'build','--target','aarch64','-j','1','-o',raw,source],expected=1)
                    assert failed.stderr and not raw.exists(); rejected += 1
                    # An ABI call kills x1. A partial write cannot define its
                    # remaining bits; a preceding MOVZ can define them.
                    prefix = ('module scalar.call; fn callee()->u64 {return 7;} export fn run() @naked {'+mode+
                              ' aarch64 {sub {dst:sp,lhs:sp,rhs:16};str {src:x30,offset:0};bl {symbol:callee};')
                    body = native(name,'x1','x0','x0','x0',(1,16))
                    tail = 'ldr {dst:x30,offset:0};add {dst:sp,lhs:sp,rhs:16};ret;}}'
                    build(prefix+'movz {dst:x1,imm:7,shift:0};'+body+tail); call_cases += 1
                    source.write_text(prefix+body+tail); raw.unlink(missing_ok=True)
                    failed = command([args.compiler,'build','--target','aarch64','-j','1','-o',raw,source],expected=1)
                    assert failed.stderr and not raw.exists(); rejected += 1
            report = {'instruction_names':38,'oracle_cases':len(corpus),'exact_binaries':len(corpus)*len(levels),
                      'managed_binaries':len(records),'guarded_calls':calls,'rejections':rejected,
                      'initialized_call_binaries':call_cases,'reserved_word_rejections':reserved,
                      'llvm_dumper_roundtrips':len(corpus),'records':records}
            report['compiled_binaries'] = report['exact_binaries']+report['managed_binaries']+call_cases
        if args.out: Path(args.out).write_text(json.dumps(report,indent=2)+'\n')
    print('PASS: A64 scalar contracts',json.dumps({k:v for k,v in report.items() if k != 'records'}))


def managed(name,bits,mode,imm,carry):
    typ = 'u32' if bits == 32 else 'u64'; arg = 'u32' if name in LONG else typ
    body = ''
    if name in RMW: body += 'mov {dst:out,src:acc};'
    if name in READ_FLAGS: body += ('subs' if carry else 'adds')+' {dst:xzr,lhs:xzr,rhs:xzr};'
    body += native(name,'out','a','b','acc',imm)
    if name in FLAGS: body += 'mrs {dst:flags,sys:nzcv};'
    return ('module scalar.managed; export fn run(a:'+arg+',b:'+arg+',acc:'+typ+')->'+typ+' {'
            'let live:'+typ+'=acc*17+9; let out:'+typ+'; '+('let flags:'+typ+';' if name in FLAGS else '')+
            mode+' aarch64 inputs(a,b,acc) outputs(out'+(',flags' if name in FLAGS else '')+') {'+body+'} return out^live'+('^flags' if name in FLAGS else '')+';}')


def qemu(args,work,build,modes,levels,values,records):
    library = work/'functions.bin'
    for mode in modes:
        for level in levels:
            binary = bytearray(); entries = []
            for name in NAMES:
                for bits in ([64] if args.smoke or name in ONLY64 else [32,64]):
                    for carry in ([0,1] if name in READ_FLAGS else [0]):
                        for sink in (False,True):
                            p = 'x' if bits == 64 else 'w'; q = 'w' if name in LONG else p
                            imm = (0x8123,bits-16) if name in WIDE else (bits-1,13) if name in FIELD else (13,0)
                            body = native(name,p+'zr' if sink else p+'0',q+'1',q+'2',p+'3',imm)
                            if name in READ_FLAGS: body = ('subs' if carry else 'adds')+' {dst:xzr,lhs:xzr,rhs:xzr};'+body
                            code = build('module scalar.execute; export fn run() @naked {'+mode+' aarch64 {'+body+'ret;}}',level)
                            assert len(code) == (12 if name in READ_FLAGS else 8)
                            entries.append((name,bits,sink,imm,carry,len(binary))); binary.extend(code)
            library.write_bytes(binary)
            runner = ['mov x28,sp']+[f'ldr x{j},={0x123450+j}' for j in range(19,28)]
            for name,bits,sink,imm,carry,offset in entries:
                for a,b,acc in values:
                    canary = acc if name in RMW else 0xfedcba9876543210
                    expected,flags = reference(name,a,b,acc,bits,imm,carry)
                    if sink: expected = canary
                    runner += [f'ldr x0,={canary}',f'ldr x1,={a}',f'ldr x2,={b}',f'ldr x3,={acc}',
                               f'ldr x16,=aster_entry+{offset}','blr x16','mrs x5,nzcv',f'ldr x16,={expected}',
                               'cmp x0,x16','b.ne failed','mov x17,sp','cmp x17,x28','b.ne failed']
                    if name in FLAGS: runner += [f'ldr x16,={flags}','cmp x5,x16','b.ne failed']
                    for j in range(19,28): runner += [f'ldr x16,={0x123450+j}',f'cmp x{j},x16','b.ne failed']
                    # Keep literal pools within LDR-literal's signed 19-bit reach.
                    runner += ['b 1f','.ltorg','1:']
            elf,_ = other_kernel('aarch64','\n'.join(runner)+'\n',library,work,args.clang)
            records.append({'mode':mode,'level':level,'binaries':len(entries),'calls':len(entries)*len(values),
                            'sha256':hashlib.sha256(binary).hexdigest(),'kernel_sha256':hashlib.sha256(elf.read_bytes()).hexdigest()})
            print('PASS: A64 scalar QEMU',mode,'O'+level,flush=True)


if __name__ == '__main__': main()
