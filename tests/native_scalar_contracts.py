#!/usr/bin/env python3
"""Independent RV64I/M arithmetic bytes, execution and source-contract corpus."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import re
import tempfile

from native_processor_qemu import command
from native_processor_contracts import assemble as reference_assemble
from native_binding_execution import scalar_call

ROOT = Path(__file__).resolve().parents[1]
MASK = (1 << 64) - 1
REG = ('add sub sll slt sltu xor srl sra or and addw subw sllw srlw sraw '
       'mul mulh mulhsu mulhu div divu rem remu mulw divw divuw remw remuw').split()
IMM = 'addi slti sltiu xori ori andi addiw'.split()
SHIFT = 'slli srli srai'.split()
WORD_SHIFT = 'slliw srliw sraiw'.split()
UPPER = ['lui','auipc']
NAMES = REG + IMM + SHIFT + WORD_SHIFT + UPPER
M_NAMES = set('mul mulh mulhsu mulhu div divu rem remu mulw divw divuw remw remuw'.split())


def assemble(clang,arch,text,work):
    return reference_assemble(clang,arch,text,work,rv_march='rv64imh_zicsr_zifencei_svinval')


def signed(value, bits=64):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def reference(name, a, b, pc):
    word = name.endswith('w') and name not in UPPER
    bits = 32 if word else 64
    mask = (1 << bits) - 1
    x,y = a & mask,b & mask
    sx,sy = signed(x,bits),signed(y,bits)
    if name in ('lui','auipc'): result = signed(b << 12,32) + (pc if name == 'auipc' else 0)
    elif name in ('add','addi','addw','addiw'): result = x + b
    elif name in ('sub','subw'): result = x - y
    elif name in ('sll','slli','sllw','slliw'): result = x << (y & (bits-1))
    elif name in ('srl','srli','srlw','srliw'): result = x >> (y & (bits-1))
    elif name in ('sra','srai','sraw','sraiw'): result = sx >> (y & (bits-1))
    elif name in ('slt','slti'): result = int(signed(a) < signed(b))
    elif name in ('sltu','sltiu'): result = int(a < (b & MASK))
    elif name in ('and','andi'): result = a & b
    elif name in ('or','ori'): result = a | b
    elif name in ('xor','xori'): result = a ^ b
    elif name in ('mul','mulw'): result = x * y
    elif name == 'mulh': result = (signed(a) * signed(b)) >> 64
    elif name == 'mulhsu': result = (signed(a) * b) >> 64
    elif name == 'mulhu': result = (a*b) >> 64
    elif name in ('div','divw','rem','remw'):
        quotient = -1 if sy == 0 else abs(sx) // abs(sy) * (-1 if (sx < 0) != (sy < 0) else 1)
        result = quotient if name.startswith('div') else sx if sy == 0 else sx - quotient * sy
    elif name in ('divu','divuw','remu','remuw'):
        result = (mask if y == 0 else x // y) if name.startswith('div') else x if y == 0 else x % y
    else: raise AssertionError(name)
    return (signed(result,32) if word else result) & MASK


def native(name,dst,lhs,rhs):
    return f'{name} {{dst:{dst},imm:{rhs}}};' if name in UPPER else f'{name} {{dst:{dst},lhs:{lhs},rhs:{rhs}}};'


def asm(name,dst,lhs,rhs): return f'{name} {dst}, {rhs}' if name in UPPER else f'{name} {dst}, {lhs}, {rhs}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',required=True); parser.add_argument('--dumper',required=True)
    parser.add_argument('--clang',required=True); parser.add_argument('--out'); parser.add_argument('--smoke',action='store_true')
    parser.add_argument('--python-path')
    args = parser.parse_args()
    inventory = set(re.findall(r'^AST_RV_SCALAR\(\w+,\s*"([^"]+)"',
                              (ROOT/'include/asterasm/detail/riscv64_scalar_instructions.inc').read_text(),re.M))
    assert len(NAMES) == len(set(NAMES)) == 43 and inventory == set(NAMES),(inventory-set(NAMES),set(NAMES)-inventory)
    records = []; calls = rejected = roundtrips = manual_modes = 0
    modes = ('native',) if args.smoke else ('native','exact','unsafe exact')
    levels = ('0',) if args.smoke else ('0','2','z')
    pressures = (2,) if args.smoke else (0,2)
    promotions = (False,) if args.smoke else (False,True)
    rng = random.Random(0x5ca1a2)
    operands = [(0,0),(1,1),(MASK,0),(1 << 63,MASK),((1 << 63)-1,1 << 63),
                (1 << 63,0),(0xabcdef0187654321,0xfedcba9876543210),(MASK,65)]
    operands += [(rng.getrandbits(64),rng.getrandbits(64)) for _ in range(2)]
    with tempfile.TemporaryDirectory(prefix='aster-native-scalar-') as temporary:
        work = Path(temporary); source = work/'case.aa'; raw = work/'case.bin'
        oracle = {}
        for name in NAMES:
            values = [-2048,-1,0,1,2047] if name in IMM else [0,1,31,63] if name in SHIFT else [0,1,31] if name in WORD_SHIFT else [0,1,0x7ffff,0x80000,0xfffff] if name in UPPER else [None]
            registers = [('a0','a1','a2'),('t6','t5','t4'),('zero','a0','a1')]
            if name in REG: registers += [('a0','zero','a1'),('a0','a0','a0')]
            if args.smoke: values,registers = values[:1],registers[:1]
            oracle[name] = [(dst,lhs,rhs if imm is None else str(imm)) for imm in values for dst,lhs,rhs in registers]
        oracle_text = '\n'.join(asm(name,*registers)+'\nret' for name in NAMES for registers in oracle[name])
        expected_blob = assemble(args.clang,'riscv64',oracle_text,work)
        assert len(expected_blob) == 8*sum(map(len,oracle.values()))
        actual_blob = bytearray(); oracle_index = 0

        def build(text,level='0',pressure=0,promote=True,features=True):
            source.write_text(text)
            argv = [args.compiler,'build','--target','riscv64','-O'+level,'--reg-limit',str(pressure),'-j','1','-o',raw,source]
            if features: argv += ['--feature','m']
            if not promote: argv += ['--no-promote']
            command(argv)
            return raw.read_bytes()

        for name in NAMES:
            for dst,lhs,rhs in oracle[name]:
                body = native(name,dst,lhs,rhs)
                expected = expected_blob[oracle_index*8:oracle_index*8+8]; oracle_index += 1
                for level in levels:
                    code = build('module scalar.exact; export fn run() @naked { unsafe exact riscv64 {'+body+'ret;}}',level)
                    assert code == expected,(name,body,level,code.hex(),expected.hex())
                actual_blob.extend(code)
                records.append({'kind':'exact-oracle','name':name,'source':body,'bytes':expected.hex()})
            immediate = -17 if name in IMM else 31 if name in SHIFT+WORD_SHIFT else 0x80001 if name in UPPER else None
            for mode in modes:
                body = native(name,'out','a','b' if immediate is None else str(immediate))
                text = ('module scalar.managed; export fn run(a:u64,b:u64,seed:u64)->u64 {'
                        'let live:u64=seed*17+9; let out:u64; '+mode+' riscv64 inputs(a,b) outputs(out) {'+body+'} return out^live;}')
                for level in levels:
                    for pressure in pressures:
                        for promote in promotions:
                            code = build(text,level,pressure,promote)
                            pc = 0
                            if name == 'auipc':
                                offsets = [j for j in range(0,len(code),4) if int.from_bytes(code[j:j+4],'little') & 127 == 0x17]
                                assert len(offsets) == 1,offsets
                                pc = 0x100000+offsets[0]
                            for a,b in operands:
                                seed = 0x3141592653589793
                                expected = reference(name,a,b if immediate is None else immediate,pc) ^ ((seed*17+9) & MASK)
                                actual = scalar_call('riscv64',code,(a,b,seed))
                                assert actual == expected,(name,mode,level,pressure,promote,a,b,actual,expected)
                                calls += 1
                            records.append({'kind':'managed','name':name,'mode':mode,'level':level,'reg_limit':pressure,
                                            'promote':promote,'bytes':len(code),'sha256':hashlib.sha256(code).hexdigest()})
            print('PASS: scalar',name,flush=True)

        raw.write_bytes(actual_blob)
        dump = json.loads(command([args.dumper,'--target','riscv64','--strict','--format','json',raw]).stdout)
        rebuilt = assemble(args.clang,'riscv64','\n'.join(i['assembly'] for i in dump['instructions']),work)
        assert rebuilt == actual_blob == expected_blob
        roundtrips = oracle_index

        # Every opcode in all source modes and both explicit frame policies.
        body = ' '.join(native(name,'a0','a1',1 if name not in REG else 'a2') for name in NAMES)+' ret;'
        text = '\n'.join(asm(name,'a0','a1',1 if name not in REG else 'a2') for name in NAMES)+'\nret'
        expected = assemble(args.clang,'riscv64',text,work)
        for mode in modes:
            for frame in ('@naked','@frame(validate)'):
                code = build('module scalar.modes; export fn run() '+frame+' {'+mode+' riscv64 {'+body+'}}')
                assert code == expected; manual_modes += 1
        bad = [native(name,'a0','a1',v) for names,values in [(IMM,[-2049,2048]),(SHIFT,[-1,64]),(WORD_SHIFT,[-1,32]),(UPPER,[-1,0x100000])] for name in names for v in values]
        bad += ['add {dst:sp,lhs:a0,rhs:a1};','add {dst:gp,lhs:a0,rhs:a1};','add {dst:ra,lhs:a0,rhs:a1};']
        for mode in modes:
            for body in bad + [native(name,'a0','a1','a2') for name in sorted(M_NAMES)]:
                source.write_text('module scalar.bad; export fn run() @naked {'+mode+' riscv64 {'+body+'ret;}}')
                raw.unlink(missing_ok=True)
                feature = [] if body.split()[0] in M_NAMES else ['--feature','m']
                failure = command([args.compiler,'build','--target','riscv64','-j','1','-o',raw,source,*feature],expected=1)
                assert failure.stderr and not raw.exists(); rejected += 1
        report = {'instruction_forms':43,'oracle_cases':sum(r['kind']=='exact-oracle' for r in records),
                  'managed_binaries':sum(r['kind']=='managed' for r in records),'guarded_calls':calls,
                  'llvm_dumper_roundtrips':roundtrips,'dumper_reassembly_batches':1,'manual_modes':manual_modes,'rejections':rejected,'records':records}
        report['exact_binaries'] = report['oracle_cases']*len(levels)
        report['compiled_binaries'] = report['exact_binaries']+report['managed_binaries']+manual_modes
        if args.out: Path(args.out).write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS: 43 RV64I/M arithmetic forms, {report["oracle_cases"]} LLVM cases, {calls} guarded calls, {rejected} rejections')


if __name__ == '__main__': main()
