#!/usr/bin/env python3
"""RV64 JALR bytes, ABI call/tail-call contracts and low target-bit clearing."""
import argparse
import json
from pathlib import Path
import tempfile

from native_processor_qemu import command, other_kernel
from native_scalar_contracts import assemble, MASK

OFFSETS = (-2048,-1,0,1,2047)


def body(call,base,offset):
    jump = 'jalr {dst:'+('ra' if call else 'zero')+',base:'+base+',offset:'+str(offset)+'};'
    return ('addi {dst:sp,lhs:sp,rhs:-16}; sd {src:ra,offset:0}; '+jump+
            ' ld {dst:ra,offset:0}; addi {dst:sp,lhs:sp,rhs:16}; ret;') if call else jump


def assembly(call,base,offset):
    jump = f'jalr {"ra" if call else "zero"},{offset}({base})'
    return f'addi sp,sp,-16\nsd ra,0(sp)\n{jump}\nld ra,0(sp)\naddi sp,sp,16\nret' if call else jump


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',required=True); parser.add_argument('--dumper'); parser.add_argument('--clang',required=True)
    parser.add_argument('--out'); parser.add_argument('--python-path'); parser.add_argument('--qemu',action='store_true'); parser.add_argument('--smoke',action='store_true')
    args = parser.parse_args(); records = []; calls = rejected = roundtrips = 0
    modes = ('native',) if args.smoke else ('native','exact','unsafe exact'); levels = ('0',) if args.smoke else ('0','2','z')
    with tempfile.TemporaryDirectory(prefix='aster-native-indirect-') as temporary:
        work = Path(temporary); source = work/'case.aa'; raw = work/'case.bin'
        def build(call,base,offset,mode='unsafe exact',level='0'):
            source.write_text('module indirect.contract; export fn run() @naked {'+mode+' riscv64 {'+body(call,base,offset)+'}}')
            command([args.compiler,'build','--target','riscv64','-O'+level,'-j','1','-o',raw,source]); return raw.read_bytes()
        if args.qemu:
            for mode in modes:
                for level in levels:
                    binary = bytearray(); entries = []
                    for call in (False,True):
                        for offset in OFFSETS:
                            code = build(call,'a1',offset,mode,level); assert len(code) == (24 if call else 4)
                            entries.append((call,offset,len(binary))); binary.extend(code)
                    library = work/'functions.bin'; library.write_bytes(binary)
                    runner = ['mv s11,sp']+[f'li s{j},{0x123450+j}' for j in range(11)]
                    for call,offset,position in entries:
                        for value in (0,1,MASK,1 << 63,0xabcdef0187654321):
                            # Form an odd target deliberately: JALR must clear bit zero.
                            runner += [f'li a0,{value}','la a1,indirect_callee',f'li t0,{1-offset}','add a1,a1,t0',
                                       f'la t0,aster_entry+{position}','jalr ra,0(t0)',f'li t0,{(value+31)&MASK}',
                                       'bne a0,t0,failed','bne sp,s11,failed']
                            for j in range(11): runner += [f'li t0,{0x123450+j}',f'bne s{j},t0,failed']
                            calls += 1
                    runner += ['j indirect_checks_done','.balign 4','indirect_callee:','addi a0,a0,31','ret','indirect_checks_done:']
                    other_kernel('riscv64','\n'.join(runner)+'\n',library,work,args.clang)
                    records.append({'mode':mode,'level':level,'binaries':len(entries),'guarded_calls':len(entries)*5})
                    print('PASS: JALR QEMU',mode,'O'+level,flush=True)
        else:
            if not args.dumper: parser.error('--dumper required for byte/contracts qualification')
            bases = ('a1',) if args.smoke else tuple('x'+str(j) for j in range(32))
            cases = [(call,base,offset) for call in (False,True) for base in bases for offset in OFFSETS]
            expected = assemble(args.clang,'riscv64','\n'.join(assembly(*case) for case in cases),work)
            position = 0; binary = bytearray()
            for call,base,offset in cases:
                width = 24 if call else 4
                for level in levels:
                    code = build(call,base,offset,level=level)
                    assert code == expected[position:position+width],(call,base,offset,level,code.hex())
                    records.append({'call':call,'base':base,'offset':offset,'level':level})
                position += width; binary.extend(code)
            raw.write_bytes(binary)
            dump = json.loads(command([args.dumper,'--target','riscv64','--strict','--format','json',raw]).stdout)
            rebuilt = assemble(args.clang,'riscv64','\n'.join(i['assembly'] for i in dump['instructions']),work)
            assert rebuilt == binary == expected; roundtrips = len(cases)
            for mode in modes:
                for call in (False,True):
                    code = build(call,'a1',0,mode)
                    assert code == assemble(args.clang,'riscv64',assembly(call,'a1',0),work)
                    records.append({'call':call,'mode':mode})
                invalid = ['jalr {dst:t0,base:a1,offset:0};','jalr {dst:zero,base:a1,offset:2048};',
                           'jalr {dst:zero,base:a1,offset:-2049};','jalr {dst:zero,base:f0,offset:0};',
                           'jalr {dst:ra,base:a1,offset:0}; ret;',
                           'addi {dst:sp,lhs:sp,rhs:-16}; jalr {dst:zero,base:a1,offset:0};',
                           'jalr {dst:zero,base:a1};']
                for text in invalid:
                    source.write_text('module indirect.bad; export fn run() @naked {'+mode+' riscv64 {'+text+'}}')
                    raw.unlink(missing_ok=True)
                    failure = command([args.compiler,'build','--target','riscv64','-j','1','-o',raw,source],expected=1)
                    assert failure.stderr and not raw.exists(); rejected += 1
    report = {'qemu_runs':len(records) if args.qemu else 0,'compiled_binaries':sum(r['binaries'] for r in records) if args.qemu else len(records),
              'guarded_calls':calls,'rejections':rejected,'llvm_dumper_roundtrips':roundtrips,'records':records}
    if args.out: Path(args.out).write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS: JALR {"QEMU" if args.qemu else "contracts"}: {report["compiled_binaries"]} binaries, {calls} guarded calls, {rejected} rejections')


if __name__ == '__main__': main()
