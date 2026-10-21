#!/usr/bin/env python3
"""Bound deep call graphs and duplicated source-site trees without host recursion."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

from compiler_division_unicorn import execute, normalize


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler',required=True)
    p.add_argument('--python-path',default='')
    args=p.parse_args()
    if args.python_path:
        import sys
        sys.path.insert(0,args.python_path)
    chain='module qual.deep; export fn run(x:u64,y:u64)->u64 @abi(c) { return node0(x)+y; }\n'
    chain+='\n'.join(f'fn node{i}(x:u64)->u64 {{ return '+(f'node{i+1}(x)+1;' if i<299 else 'x+1;')+' }' for i in range(300))
    diamond='module qual.diamond; export fn run(x:u64,y:u64)->u64 @abi(c) { return f7(x,y); }\n'
    diamond+='fn f0(x:u64,y:u64)->u64 { return (x^y)+1; }\n'
    diamond+='\n'.join(f'fn f{i}(x:u64,y:u64)->u64 {{ return f{i-1}(x,y)+f{i-1}(y,x); }}' for i in range(1,8))
    count=0; max_sites=0; max_depth=0
    with tempfile.TemporaryDirectory(prefix='aster-inline-limits-') as temp:
        root=Path(temp);source=root/'unit.aa';binary=root/'unit.bin'
        for name,text,oracle in (('deep',chain,lambda x,y:x+300+y),('diamond',diamond,lambda x,y:128*((x^y)+1))):
            source.write_text(text)
            for profile,target,abi in ((0,'x86_64','sysv'),(2,'aarch64','aapcs64')):
                for level in ('2','z'):
                    hashes=[]
                    for threads in ('1','4'):
                        command=[args.compiler,'build','-O'+level,'--target',target,'--abi',abi,'--no-red-zone',
                            '--stats','-j',threads,'-o',str(binary),str(source)]
                        result=subprocess.run(command,capture_output=True,text=True,timeout=120)
                        assert not result.returncode,(name,profile,level,result.stderr)
                        match=re.search(r'MIR inline: sites=(\d+) depth=(\d+)',result.stderr)
                        assert match,(name,result.stderr)
                        sites,depth=map(int,match.groups());max_sites=max(sites,max_sites);max_depth=max(depth,max_depth)
                        assert 0<sites<=256 and 0<depth<=8,(sites,depth)
                        code=binary.read_bytes();hashes.append(hashlib.sha256(code).digest())
                        for x,y in ((0,0),(7,3),((1<<64)-1,1)):
                            got,_=execute(code,profile,x,y,64,False)
                            assert got==normalize(oracle(x,y),64,False),(name,profile,level,x,y,got)
                            count+=1
                    assert hashes[0]==hashes[1],(name,'non-deterministic scheduling')
    print(f'PASS: 16 deep/DAG binaries, {count} CPU calls; sites <= {max_sites}, depth <= {max_depth}; deterministic 1/4 threads')


if __name__=='__main__':main()
