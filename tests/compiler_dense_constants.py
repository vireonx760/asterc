#!/usr/bin/env python3
"""Constant ABI returns must not introduce a frame or memory snapshot on x86-64."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from compiler_division_unicorn import PROFILES, execute


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler',required=True)
    p.add_argument('--python-path',default='')
    args=p.parse_args()
    if args.python_path:
        import sys
        sys.path.insert(0,args.python_path)
    count=0; largest=0
    values=(0,42,(1<<32)-1,1<<32,(1<<64)-1,(1<<63)+17)
    with tempfile.TemporaryDirectory(prefix='aster-dense-constant-') as temp:
        root=Path(temp);source=root/'unit.aa';binary=root/'unit.bin'
        for profile,(target,abi,with_m) in enumerate(PROFILES):
            for value in values:
                source.write_text(f'module qual.constant; export fn run(x:u64,y:u64)->u64 @abi(c) {{ return {value}; }}')
                for level in ('0','2','z'):
                    command=[args.compiler,'build','-O'+level,'--target',target,'--abi',abi,'--no-red-zone',
                        '-j','1','-o',str(binary),str(source)]
                    if with_m:command+=['--feature','m']
                    result=subprocess.run(command,capture_output=True,text=True,timeout=45)
                    assert not result.returncode,(profile,value,level,result.stderr)
                    code=binary.read_bytes()
                    if target=='x86_64':
                        maximum=6 if value<1<<32 else 8 if value==(1<<64)-1 else 11
                        assert len(code)<=maximum,('constant return has an unnecessary frame',profile,value,level,len(code),maximum)
                        largest=max(largest,len(code))
                    for x,y in ((0,0),(17,29),((1<<64)-1,1)):
                        got,_=execute(code,profile,x,y,64,False)
                        assert got==value,(profile,value,level,got)
                        count+=1
    print(f'PASS: 90 constant-return binaries, {count} CPU calls; x86-64 <= {largest} bytes at O0/O2/Oz')


if __name__=='__main__':main()
