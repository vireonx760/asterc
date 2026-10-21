#!/usr/bin/env python3
"""RV64 branch bytes, independent signed/unsigned decisions and CFG contracts."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import tempfile

from native_scalar_contracts import assemble, signed, MASK
from native_binding_execution import scalar_call
from native_processor_qemu import command

NAMES = ('beq','bne','blt','bge','bltu','bgeu')


def decision(name,a,b):
    return {'beq':a == b,'bne':a != b,'blt':signed(a) < signed(b),
            'bge':signed(a) >= signed(b),'bltu':a < b,'bgeu':a >= b}[name]


def body(name,lhs='a0',rhs='a1'):
    return (f'{name} {{lhs:{lhs},rhs:{rhs},target:taken}}; '
            'addi {dst:a0,lhs:zero,rhs:11}; jal {dst:zero,target:done}; '
            'label {name:taken}; addi {dst:a0,lhs:zero,rhs:29}; label {name:done}; ret;')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',required=True); parser.add_argument('--dumper',required=True)
    parser.add_argument('--clang',required=True); parser.add_argument('--out'); parser.add_argument('--smoke',action='store_true')
    parser.add_argument('--python-path')
    args = parser.parse_args(); records = []; calls = rejected = roundtrips = 0
    modes = ('native',) if args.smoke else ('native','exact','unsafe exact')
    levels = ('0',) if args.smoke else ('0','2','z')
    operands = [(0,0),(1,1),(MASK,0),(0,MASK),(1 << 63,MASK),(MASK,1 << 63),
                ((1 << 63)-1,1 << 63),(1 << 63,(1 << 63)-1)]
    rng = random.Random(0xb2a6c4)
    operands += [(rng.getrandbits(64),rng.getrandbits(64)) for _ in range(8)]
    with tempfile.TemporaryDirectory(prefix='aster-native-branch-') as temporary:
        work = Path(temporary); source = work/'case.aa'; raw = work/'case.bin'
        for name in NAMES:
            for lhs,rhs in (('a0','a1'),('zero','a1'),('a0','a0')):
                asm = f'{name} {lhs},{rhs},1f\naddi a0,zero,11\njal zero,2f\n1: addi a0,zero,29\n2: ret'
                expected = assemble(args.clang,'riscv64',asm,work)
                assert len(expected) == 20
                for mode in modes:
                    for level in levels:
                        for frame in ('@naked','@frame(validate)'):
                            source.write_text('module branch.contract; export fn run() '+frame+' {'+mode+' riscv64 {'+body(name,lhs,rhs)+'}}')
                            command([args.compiler,'build','--target','riscv64','-O'+level,'-j','1','-o',raw,source])
                            code = raw.read_bytes(); assert code == expected,(name,lhs,rhs,mode,frame,level,code.hex(),expected.hex())
                            for a,b in operands:
                                x = 0 if lhs == 'zero' else a; y = a if rhs == 'a0' else b
                                result = scalar_call('riscv64',code,(a,b,0))
                                assert result == (29 if decision(name,x,y) else 11),(name,a,b,result)
                                calls += 1
                            records.append({'name':name,'lhs':lhs,'rhs':rhs,'mode':mode,'level':level,'frame':frame,
                                            'bytes':len(code),'sha256':hashlib.sha256(code).hexdigest()})
                dump = json.loads(command([args.dumper,'--target','riscv64','--strict','--format','json',raw]).stdout)
                rebuilt = assemble(args.clang,'riscv64','\n'.join(i['assembly'] for i in dump['instructions']),work)
                assert rebuilt == expected; roundtrips += 1
            for mode in modes:
                invalid = [body(name).replace('target:taken','target:absent'),
                           body(name).replace('label {name:taken};','label {name:taken}; label {name:taken};'),
                           body(name).replace('rhs:a1','rhs:sp',1),
                           body(name).replace('rhs:a1','rhs:1',1),
                           body(name).replace('target:taken','target:taken,extra:a0',1),
                           body(name).replace('addi {dst:a0,lhs:zero,rhs:11};','addi {dst:sp,lhs:sp,rhs:-16};'),
                           body(name).replace('lhs:a0','lhs:input',1)]
                for text in invalid:
                    source.write_text('module branch.invalid; export fn run() @naked {'+mode+' riscv64 {'+text+'}}')
                    raw.unlink(missing_ok=True)
                    failure = command([args.compiler,'build','--target','riscv64','-j','1','-o',raw,source],expected=1)
                    assert failure.stderr and not raw.exists(); rejected += 1
            print('PASS: branch',name,flush=True)
    report = {'instruction_forms':6,'compiled_binaries':len(records),'guarded_calls':calls,
              'llvm_dumper_roundtrips':roundtrips,'rejections':rejected,'records':records}
    if args.out: Path(args.out).write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS: six RV64 branches, {len(records)} binaries, {calls} guarded calls, {rejected} invalid CFG/contracts')


if __name__ == '__main__': main()
