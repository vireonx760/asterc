#!/usr/bin/env python3
"""Check explicit inline attributes, refusal diagnostics and global opt-out."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from compiler_division_unicorn import PROFILES, execute, normalize


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler',required=True)
    p.add_argument('--python-path',default='')
    args=p.parse_args()
    if args.python_path:
        import sys
        sys.path.insert(0,args.python_path)
    positive='module qual.policy; export fn run(x:u64,y:u64)->u64 @abi(c) { return forced(x,y)+kept(y); } '
    positive+='fn forced(x:u64,y:u64)->u64 @inline { return ((x^7)+y)^31; } '
    positive+='fn kept(x:u64)->u64 @noinline { return x+3; }'
    checks=0
    with tempfile.TemporaryDirectory(prefix='aster-inline-policy-') as temp:
        root=Path(temp);source=root/'unit.aa';binary=root/'unit.bin'
        for profile,(target,abi,with_m) in enumerate(PROFILES):
            for level,disabled in (('0',False),('2',False),('z',False),('2',True)):
                source.write_text(positive)
                command=[args.compiler,'build','-O'+level,'--target',target,'--abi',abi,'--no-red-zone',
                    '-j','1','--stats','--dump-mir','-o',str(binary),str(source)]
                if with_m:command+=['--feature','m']
                if disabled:command+=['--no-inline']
                result=subprocess.run(command,capture_output=True,text=True,timeout=45)
                assert not result.returncode,(profile,level,disabled,result.stderr)
                forced=bool(re.search(r'forced=[1-9][0-9]* selected=explicit-policy',result.stderr))
                assert forced==(level!='0' and not disabled),(profile,level,disabled,result.stderr)
                run_body=result.stdout.split('\nfn ',2)[1] if '\nfn ' in result.stdout else result.stdout
                assert re.search(r'g\.call_direct[^\n]*kept',run_body),('noinline call lost',result.stdout)
                code=binary.read_bytes()
                for x,y in ((0,0),(7,3),((1<<64)-1,1)):
                    got,_=execute(code,profile,x,y,64,False)
                    assert got==normalize((((x^7)+y)^31)+y+3,64,False),(profile,level,x,y,got)
                    checks+=1
        negatives=[
            ('fn helper(x:u64)->u64 @inline @noinline { return x; }','sema:invalid_attribute'),
            ('fn helper(x:u64)->u64 @inline(1) { return x; }','sema:invalid_attribute'),
            ('fn helper(x:u64)->u64 @noinline(1) { return x; }','sema:invalid_attribute'),
            ('fn helper(x:u64)->u64 @inline { if x==0 { return 1; } return helper(x-1); }','cannot satisfy @inline'),
            ('fn helper(x:u64)->u64 @inline { let v:u64=x; '+
             ' '.join(f'v=(v^{i+1})+x;' for i in range(70))+' return v; }','cannot satisfy @inline'),
        ]
        for helper,want in negatives:
            source.write_text('module qual.policy; export fn run(x:u64)->u64 @abi(c) { return helper(x); } '+helper)
            result=subprocess.run([args.compiler,'build','-O2','--target','x86_64','--abi','sysv',
                '-j','1','-o',str(binary),str(source)],capture_output=True,text=True,timeout=45)
            assert result.returncode and want in result.stderr,(helper,result.stderr)
            checks+=1
    print(f'PASS: {checks} explicit inline policy CPU/diagnostic checks')


if __name__=='__main__':main()
