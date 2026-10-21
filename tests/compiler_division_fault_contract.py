#!/usr/bin/env python3
"""Linux/Windows x86_64 faults with process-isolated generated-code calls.

Important: this tests machine-level behavior, not a portable language contract.
No signal is intentionally raised in the parent test process.
"""
import argparse
import ctypes
import json
import os
import platform
import sys
import subprocess
import tempfile
from pathlib import Path
from compiler_narrow_isa_execution import TYPES
from native_jit import executable_code, native_x86_supported

CTYPES={(8,False):ctypes.c_uint8,(8,True):ctypes.c_int8,
        (16,False):ctypes.c_uint16,(16,True):ctypes.c_int16,
        (32,False):ctypes.c_uint32,(32,True):ctypes.c_int32,
        (64,False):ctypes.c_uint64,(64,True):ctypes.c_int64}


def child_main(path, bits, signed, x, y, op):
    # A fresh subprocess owns the potentially faulting machine code mapping;
    # SIGFPE is reported as the process exit signal to the parent.
    if os.name == 'nt':
        # Do not show a Windows Error Reporting dialog for an expected fault.
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
    else:
        import resource
        resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    code=Path(path).read_bytes()
    ct=CTYPES[bits,signed]
    with executable_code(code) as address:
        fn=ctypes.CFUNCTYPE(ct,ct,ct)(address)
        try:
            actual=fn(x,y)
        except OSError as error:
            # CPython's Windows FFI translates the actual hardware SEH into
            # OSError. Accept only its specific divide exception, not AV or
            # arbitrary child failures. The parent checks the structured tag.
            if os.name != 'nt' or str(error) not in ('exception: integer divide by zero',
                                                    'exception: integer overflow'):
                raise
            print(json.dumps({'seh': str(error).removeprefix('exception: ')}))
            return
        if signed and y == -1:
            lo=-(1<<(bits-1))
            if actual != (lo if op=='/' else 0):sys.exit(98)


def run():
    pa=argparse.ArgumentParser()
    pa.add_argument('--compiler')
    pa.add_argument('--child-bin')
    pa.add_argument('--child-bits',type=int)
    pa.add_argument('--child-signed',type=int)
    pa.add_argument('--child-x',type=int)
    pa.add_argument('--child-y',type=int)
    pa.add_argument('--child-op')
    pa.add_argument('--out')
    args=pa.parse_args()
    if args.child_bin:
        child_main(args.child_bin,args.child_bits,bool(args.child_signed),
                   args.child_x,args.child_y,args.child_op)
        return
    if not args.compiler:pa.error('--compiler is required for parent')
    native = native_x86_supported()
    count=0
    records=[]
    with tempfile.TemporaryDirectory(prefix='aster-division-fault-') as tmp:
        source=Path(tmp)/'div.aa'; output=Path(tmp)/'div.bin'
        for typ,bits,signed in TYPES:
            if not native:break
            ct=CTYPES[bits,signed]
            lo=-(1<<(bits-1)) if signed else 0
            for op in ('/','%'):
                source.write_text(f'module division.fault; export fn f(x:{typ}, y:{typ})->{typ} @abi(c) '
                                  f'{{ return x {op} y; }}\n')
                p=subprocess.run([args.compiler,'build','--target','x86_64','--emit','bin',
                   '--abi','win64' if os.name=='nt' else 'sysv',
                   '-j','1','-o',str(output),str(source)],capture_output=True,text=True,timeout=30)
                if p.returncode:raise AssertionError((typ,op,p.stderr))
                code=output.read_bytes()
                cases=[(1,0,True)]
                if signed:
                    # i8/i16 divide via widened machine IDIV and truncate.
                    # i32/i64 overflow raises #DE.
                    cases.append((lo,-1,bits in (32,64)))
                for x,y,expect_fault in cases:
                    result=subprocess.run([sys.executable,str(Path(__file__).resolve()),
                        '--child-bin',str(output),'--child-bits',str(bits),
                        '--child-signed',str(int(signed)),'--child-x',str(x),
                        '--child-y',str(y),'--child-op',op],capture_output=True,timeout=10)
                    if expect_fault:
                        if os.name=='nt':
                            status = 0xc0000094 if y==0 else 0xc0000095
                            observed = (result.returncode & 0xffffffff)==status
                            if result.returncode==0:
                                observed = json.loads(result.stdout)=={'seh':'integer divide by zero' if y==0 else 'integer overflow'}
                        else:observed = result.returncode==-8
                        if not observed:
                            raise AssertionError(('missing #DE',typ,op,x,y,result.returncode,result.stdout,result.stderr))
                    elif result.returncode!=0:
                        raise AssertionError(('unexpected fault/value',typ,op,x,y,
                                              result.returncode,result.stderr))
                    count+=1
                    records.append({'type':typ,'op':op,'x':x,'y':y,'expected_fault':expect_fault,
                                    'exit_code':result.returncode,'stdout':result.stdout.decode().strip()})
        # All widths and both operators must reject constant zero before
        # code emission. Remove stale output before every negative compile.
        for target in ('x86_64','aarch64','riscv64'):
            for typ,bits,signed in TYPES:
                for op in ('/','%'):
                    source.write_text(f'module division.fault; export fn f(x:{typ})->{typ} @abi(c) '
                                      f'{{ return x {op} 0; }}\n')
                    output.unlink(missing_ok=True)
                    p=subprocess.run([args.compiler,'build','--target',target,'--emit','bin','-j','1',
                             '-o',str(output),str(source)],capture_output=True,text=True,timeout=30)
                    if p.returncode==0 or output.exists() or 'division_by_zero' not in p.stderr:
                        raise AssertionError(('constant zero not diagnosed atomically',target,typ,op,p.stderr))
                    count+=1
                if signed:
                    lo=-(1<<(bits-1))
                    for op in ('/','%'):
                        source.write_text(f'module division.fault; const n:{typ}={lo}; const d:{typ}=-1; '
                                          f'export fn f()->{typ} @abi(c) {{ return n {op} d; }}\n')
                        output.unlink(missing_ok=True)
                        p=subprocess.run([args.compiler,'build','--target',target,'--emit','bin','-j','1',
                                 '-o',str(output),str(source)],capture_output=True,text=True,timeout=30)
                        if op=='/':
                            if p.returncode==0 or output.exists() or 'integer_division_overflow' not in p.stderr:
                                raise AssertionError(('constant MIN/-1 not diagnosed',target,typ,p.stderr))
                        elif p.returncode or not output.is_file():
                            raise AssertionError(('constant MIN%-1 should be zero',target,typ,p.stderr))
                        count+=1
    if args.out:
        Path(args.out).write_text(json.dumps({'native_supported':native,'platform':platform.system(),
                                             'cases':count,'native_records':records},indent=2)+'\n')
    print(f'PASS: {count} native faults/widened-narrow and cross-target constant fault cases; native={native}')

if __name__=='__main__':run()
