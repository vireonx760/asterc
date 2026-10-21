#!/usr/bin/env python3
"""Optional reports observe the selected artifact without affecting its bytes."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(argv, expected=0):
    p=subprocess.run([str(x) for x in argv],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
    assert p.returncode==expected,(argv,p.returncode,p.stdout,p.stderr)
    return p


def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--compiler',required=True)
    p.add_argument('--c-elf-fallback',action='store_true'); args=p.parse_args()
    count=0
    with tempfile.TemporaryDirectory(prefix='aster-opt-report-') as directory:
        root=Path(directory); src=root/'unit.aa'; report=root/'optimization.json'; raw=root/'unit.bin'
        code='module tooling.report; fn inc(x:u64)->u64 @inline {return x+1;} export fn run(inputs:u64)->u64 {return inc(inputs);}'
        src.write_text(code,encoding='utf-8')
        for target in ('x86_16','x86_32','x86_64','aarch64','riscv64'):
            for level in ('0','2','z'):
                object_supported=target in (('aarch64','riscv64') if args.c_elf_fallback else ('x86_64','aarch64','riscv64'))
                for emit in (('bin','obj') if object_supported else ('bin',)):
                    cmd=[args.compiler,'build','--target',target,'--emit',emit,'--format','elf','-O'+level,'-j','1','-o',raw,src]
                    run(cmd); expected=raw.read_bytes()
                    result=run(cmd+['--opt-report',report,'--stats'])
                    assert raw.read_bytes()==expected,(target,level,emit,'report changes code')
                    data=json.loads(report.read_text(encoding='utf-8'))
                    assert data['schema']==1 and data['target']==target,data
                    assert data['artifact_bytes']==len(expected) and data['text_bytes']>0,data
                    assert data['optimization_requested']=={'0':'none','2':'speed','z':'size'}[level],data
                    assert all(isinstance(v,(int,float)) and v>=0 for v in data['timing_seconds'].values()),data
                    assert data['backend_candidates']>=1,data
                    assert all(isinstance(v,(int,float)) and v>=0
                               for v in data['backend_timing_seconds'].values()),data
                    assert 'timing-seconds:' in result.stderr,result.stderr
                    count+=1
        if args.c_elf_fallback:
            previous=report.read_bytes()
            failed=run([args.compiler,'build','--target','x86_64','--emit','obj','--format','elf','--opt-report',report,'-o',raw,src],1)
            assert 'Standalone C ELF64 fallback' in failed.stderr,failed.stderr
            assert report.read_bytes()==previous
        run([args.compiler,'--opt-report'],2)
        run([args.compiler,'parse','--opt-report',report,src],2)
        run([args.compiler,'build','-o',raw,'--opt-report',raw,src],2)
        run([args.compiler,'build','--opt-report',src,src],2)
        alias=root/'source-alias.aa'; os.link(src,alias)
        run([args.compiler,'build','--opt-report',alias,src],2)
        assert src.read_text(encoding='utf-8')==code
        # A failed compilation must not replace a previous successful report.
        previous=report.read_bytes(); src.write_text('module broken;fn f( {',encoding='utf-8')
        run([args.compiler,'build','--opt-report',report,'-o',raw,src],1)
        assert report.read_bytes()==previous
    print(f'PASS: {count} optimization report/artifact comparisons and path/error contracts')


if __name__=='__main__':main()
