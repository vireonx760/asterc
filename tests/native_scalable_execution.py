#!/usr/bin/env python3
"""Execute SVE/SVE2/SME in bare-metal QEMU, including independent VL/SVL.

Scalar checks validate every stored lane, inactive memory and ABI canaries.
The guest enables architectural access explicitly; no host ISA is required.
"""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile

from native_processor_contracts import run
from native_processor_qemu import other_kernel
from native_scalable_contracts import wrapper


def cases():
    return [
        ('predicated-memory', False, '''
            whilelo {dst:p0,lhs:xzr,rhs:x2,bits:32};
            ld1w {dst:z0,pred:p0,addr:x0,offset:0,bits:32};
            dup {dst:z1,rhs:7,bits:32}; movprfx {dst:z2,src:z0};
            add {dst:z2,pred:p0,rhs:z1,bits:32};
            st1w {src:z2,pred:p0,addr:x1,offset:0,bits:32};
            cntp {dst:x0,pred:p0,pn:p0,bits:32};''', lambda i: 107+13*i),
        ('widening', False, '''
            ptrue {dst:p0,bits:32}; dup {dst:z0,rhs:-3,bits:16};
            dup {dst:z1,rhs:11,bits:16};
            saddlb {dst:z2,lhs:z0,rhs:z1,bits:32,input_bits:16};
            st1w {src:z2,pred:p0,addr:x1,offset:0,bits:32};
            cntp {dst:x0,pred:p0,pn:p0,bits:32};''', lambda i: 8),
        ('matrix-fp32', True, '''
            ptrue {dst:p0,bits:32}; zero {imm:255};
            fdup {dst:z0,rhs:2.0,bits:32}; fdup {dst:z1,rhs:3.0,bits:32};
            fmopa {dst:za0,rows:p0,cols:p0,lhs:z0,rhs:z1,bits:32};
            movz {dst:w12,imm:0,shift:0};
            mova {dst:z2,pred:p0,zan:za0h,index:w12,imm:0,bits:32};
            st1w {src:z2,pred:p0,addr:x1,offset:0,bits:32};
            cntp {dst:x0,pred:p0,pn:p0,bits:32};''', lambda i: 0x40c00000),
        ('matrix-int8', True, '''
            ptrue {dst:p0,bits:8}; ptrue {dst:p1,bits:32}; zero {imm:255};
            dup {dst:z0,rhs:-3,bits:8}; dup {dst:z1,rhs:7,bits:8};
            smopa {dst:za0,rows:p0,cols:p0,lhs:z0,rhs:z1,bits:32,input_bits:8};
            movz {dst:w12,imm:0,shift:0};
            mova {dst:z2,pred:p1,zan:za0h,index:w12,imm:0,bits:32};
            st1w {src:z2,pred:p1,addr:x1,offset:0,bits:32};
            cntp {dst:x0,pred:p1,pn:p1,bits:32};''', lambda i: (-84) & 0xffffffff),
        ('streaming-clamp-select', True, '''
            ptrue {dst:p0,bits:32}; movz {dst:w12,imm:0,shift:0};
            psel {dst:p1,lhs:p0,rhs:p0,index:w12,imm:0,bits:32};
            dup {dst:z0,rhs:-3,bits:32}; dup {dst:z1,rhs:7,bits:32};
            dup {dst:z2,rhs:-100,bits:32};
            sclamp {dst:z2,lhs:z0,rhs:z1,bits:32};
            st1w {src:z2,pred:p1,addr:x1,offset:0,bits:32};
            cntp {dst:x0,pred:p1,pn:p1,bits:32};''', lambda i: (-3) & 0xffffffff),
    ]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True)
    p.add_argument('--clang', required=True)
    p.add_argument('--out')
    args = p.parse_args()
    records = []
    with tempfile.TemporaryDirectory(prefix='aster-scalable-qemu-') as directory:
        root = Path(directory); src = root/'unit.aa'; raw = root/'unit.bin'
        for vl, svl in ((128, 256), (256, 128), (512, 512), (2048, 256)):
            for name, matrix, code, reference in cases():
                lanes = (svl if matrix else vl)//32
                for count in ((0, 1, lanes-1, lanes) if name == 'predicated-memory' else (lanes,)):
                    for level in ('0', '2', 'z'):
                        mode = {'0':'native', '2':'exact', 'z':'unsafe exact'}[level]
                        if matrix:
                            prefix, _, suffix, _ = wrapper()
                        else: prefix, suffix = '', 'ret;'
                        src.write_text('module scalable.kernel; export fn run() @naked {'+mode+' aarch64 {'+prefix+code+suffix+'}}', encoding='utf-8')
                        run([args.compiler, 'build', '--target', 'aarch64', '--feature', 'sve2', '--feature', 'sme', '-O'+level, '-j', '1', '-o', raw, src])
                        enabled = '''.arch armv9-a+sve2+sme
ldr x9,=0x3330000
msr cpacr_el1,x9
isb
'''+f'mov x9,#{vl//128-1}\nmsr zcr_el1,x9\nmov x9,#{svl//128-1}\nmsr smcr_el1,x9\nisb\n'
                        canaries = ''.join(f'mov x9,#{0xabc+j}\nfmov d{8+j},x9\n' for j in range(8))
                        checks = f'cmp x0,#{count}\nb.ne failed\n'
                        checks += ''.join(f'fmov x9,d{8+j}\ncmp x9,#{0xabc+j}\nb.ne failed\n' for j in range(8))
                        checks += 'mrs x9,svcr\ncbnz x9,failed\n'
                        for i in range(64):
                            value = reference(i) if i < count else 0xdecafbad
                            checks += f'ldr w9,[x1,#{i*4}]\nldr w10,={value}\ncmp w9,w10\nb.ne failed\n'
                        data = '\nb .\n.balign 16\ninput:\n.word '+','.join(str(100+13*i) for i in range(64))+'\noutput:\n.fill 64,4,0xdecafbad\n'
                        # Skip embedded fixture data before the harness exit.
                        startup = enabled+canaries+f'adr x0,input\nadr x1,output\nmov x2,#{count}\nbl aster_entry\n'+checks+'\nb after_data\n'+data+'\nafter_data:\n'
                        image, result = other_kernel('aarch64', startup, raw, root, args.clang, a64_cpu='max')
                        records.append(dict(case=name, vl=vl, svl=svl, count=count, level=level, mode=mode,
                                            exit_code=result.returncode, image_sha256=hashlib.sha256(image.read_bytes()).hexdigest()))
                        print(f'PASS {name} VL={vl} SVL={svl} count={count} O{level}', flush=True)
    if args.out: Path(args.out).write_text(json.dumps(dict(records=records), indent=2)+'\n', encoding='utf-8')
    print(f'PASS: {len(records)} scalable QEMU executions')


if __name__ == '__main__': main()
