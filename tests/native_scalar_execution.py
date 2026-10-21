#!/usr/bin/env python3
"""QEMU executes RV64I/M arithmetic, zero destinations and six branch decisions."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile

from native_processor_qemu import command, other_kernel
from native_scalar_contracts import NAMES, IMM, SHIFT, WORD_SHIFT, UPPER, MASK, native, reference, signed
from native_branch_contracts import NAMES as BRANCHES, body as branch_body, decision


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',required=True); parser.add_argument('--clang',required=True)
    parser.add_argument('--out'); parser.add_argument('--smoke',action='store_true')
    parser.add_argument('--python-path')
    args = parser.parse_args(); records = []; calls = branch_calls = 0
    modes = ('native',) if args.smoke else ('native','exact','unsafe exact')
    levels = ('0',) if args.smoke else ('0','2','z')
    operands = [(0,0),(1,1),(MASK,0),(1 << 63,MASK),((1 << 63)-1,1 << 63),
                (1 << 63,0),(0xabcdef0187654321,0xfedcba9876543210),(MASK,65),
                (0x80000000,0xffffffff),(0xffffffff,0x80000000)]
    with tempfile.TemporaryDirectory(prefix='aster-scalar-qemu-') as temporary:
        work = Path(temporary); source = work/'case.aa'; raw = work/'case.bin'; library = work/'functions.bin'
        for mode in modes:
            for level in levels:
                binary = bytearray(); entries = []
                for name in NAMES:
                    immediate = -17 if name in IMM else 31 if name in SHIFT+WORD_SHIFT else 0x80001 if name in UPPER else None
                    for sink in (False,True):
                        body = native(name,'zero' if sink else 'a0','a1','a2' if immediate is None else str(immediate))
                        source.write_text('module scalar.execute; export fn run() @naked {'+mode+' riscv64 {'+body+'ret;}}')
                        command([args.compiler,'build','--target','riscv64','--feature','m','-O'+level,'-j','1','-o',raw,source])
                        code = raw.read_bytes(); assert len(code) == 8,(name,mode,level,len(code))
                        entries.append((name,sink,immediate,len(binary)))
                        binary.extend(code)
                library.write_bytes(binary)
                runner = ['mv s11,sp'] + [f'li s{j},{0x123450+j}' for j in range(11)]
                for name,sink,immediate,offset in entries:
                    for a,b in operands:
                        canary = 0xfedcba9876543210
                        runner += [f'li a0,{canary}',f'li a1,{a}',f'li a2,{b}',f'la t0,aster_entry+{offset}','jalr ra,0(t0)']
                        if sink: runner += [f'li t0,{canary}']
                        elif name == 'auipc':
                            runner += [f'la t0,aster_entry+{offset}',f'li t1,{signed(immediate << 12,32)}','add t0,t0,t1']
                        else: runner += [f'li t0,{reference(name,a,b if immediate is None else immediate,0)}']
                        runner += ['bne a0,t0,failed','bne sp,s11,failed']
                        for j in range(11): runner += [f'li t0,{0x123450+j}',f'bne s{j},t0,failed']
                        calls += 1
                for name in BRANCHES:
                    source.write_text('module branch.execute; export fn run() @naked {'+mode+' riscv64 {'+branch_body(name)+'}}')
                    command([args.compiler,'build','--target','riscv64','-O'+level,'-j','1','-o',raw,source])
                    code = raw.read_bytes(); assert len(code) == 20
                    offset = len(binary); binary.extend(code)
                    for a,b in operands:
                        expected = 29 if decision(name,a,b) else 11
                        runner += [f'li a0,{a}',f'li a1,{b}',f'la t0,aster_entry+{offset}','jalr ra,0(t0)',
                                   f'li t0,{expected}','bne a0,t0,failed','bne sp,s11,failed']
                        for j in range(11): runner += [f'li t0,{0x123450+j}',f'bne s{j},t0,failed']
                        branch_calls += 1
                library.write_bytes(binary)
                elf,_ = other_kernel('riscv64','\n'.join(runner)+'\n',library,work,args.clang)
                records.append({'mode':mode,'level':level,'binaries':len(entries)+len(BRANCHES),'calls':(len(entries)+len(BRANCHES))*len(operands),
                                'sha256':hashlib.sha256(binary).hexdigest(),'kernel_sha256':hashlib.sha256(elf.read_bytes()).hexdigest()})
                print('PASS: RV64I/M QEMU',mode,'O'+level,flush=True)
    report = {'qemu_runs':len(records),'guarded_calls':calls+branch_calls,'arithmetic_calls':calls,'branch_calls':branch_calls,
              'compiled_binaries':sum(r['binaries'] for r in records),'arithmetic_forms':43,'branch_forms':6,'records':records}
    if args.out: Path(args.out).write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS: {len(records)} QEMU boots, {calls} arithmetic and {branch_calls} branch calls; all 43 arithmetic opcodes and x0 sinks')


if __name__ == '__main__': main()
