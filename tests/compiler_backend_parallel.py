"""Compare serial, parallel and retained backend artifacts, including objects."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', required=True)
p.add_argument('--object-writer', action='store_true')
args = p.parse_args()
profiles = [('x86_16','cdecl'),('x86_32','cdecl'),('x86_64','sysv'),
            ('x86_64','win64'),('aarch64','aapcs64'),('riscv64','lp64'),
            ('riscv64','lp64f'),('riscv64','lp64d')]
builds = pairs = 0
with tempfile.TemporaryDirectory(prefix='aster-backend-parallel-') as directory:
    root = Path(directory); source = root/'main.aa'; output = root/'output.bin'
    source.write_text('module backend.parallel;\n'
        'export fn run(x:u64)->u64 @abi(c) { return worker_0(x)+worker_63(x); }\n'+
        ''.join(f'export fn worker_{i}(x:u64)->u64 @abi(c) {{ '
                f'let a:u64=x*3+{i}; if a>17 {{return a/7;}} '
                'return select(x>10,a,x)^11; }\n' for i in range(64)),encoding='utf-8')
    for target, abi in profiles:
        base = [args.compiler,'build','--target',target,'--abi',abi,'-o',str(output),str(source)]
        if target == 'riscv64': base += ['--feature','m','--feature','f','--feature','d']
        if target == 'x86_16': base += ['--base','0x1000']
        for opt in ['-O0','-O2','-Oz']:
            modes = [('bin',None)]
            if args.object_writer:
                modes += [('obj','elf')]
                if target != 'riscv64': modes += [('obj','coff')]
                if target in ['x86_64','aarch64']: modes += [('obj','macho')]
            for emit, fmt in modes:
                reference = None
                for workers in ['1','2','8','0','8']:
                    command = base+[opt,'--emit',emit,'-j',workers,'--stats']
                    if fmt: command += ['--format',fmt]
                    proc = subprocess.run(command,capture_output=True,text=True,timeout=90)
                    assert proc.returncode == 0,(command,proc.stderr)
                    data = output.read_bytes(); builds += 1
                    if reference is None: reference = data
                    else:
                        assert data == reference,(target,abi,opt,emit,fmt,workers,'nondeterministic bytes')
                        pairs += 1
                    if workers == '1':
                        assert re.search(r'workers: available=\d+ peak=1 created=0 ',proc.stderr),proc.stderr
                if target == 'x86_64' and abi == 'sysv' and emit == 'bin':
                    proc = subprocess.run(base+[opt,'-j','8','--dump-regalloc'],
                                          capture_output=True,text=True,timeout=90)
                    assert proc.returncode == 0,(opt,proc.stderr)
                    assert output.read_bytes() == reference,(opt,'streaming/retained mismatch')
                    builds += 1; pairs += 1
    # More than one independent backend job fails. Completion order must not
    # select the diagnostic, and an allocation/native error must retain its
    # original text even when the encoder has not initialized an output yet.
    source.write_text('module backend.errors;\n'+''.join(
        f'export fn bad_{i}() @naked {{exact x86_64 {{'
        'mov {dst:ymm0,src:ymm1};ret;}}\n' for i in range(8)),encoding='utf-8')
    messages=[]
    for workers in ['1','8','0']:
        output.unlink(missing_ok=True)
        proc=subprocess.run([args.compiler,'build','--target','x86_64','--feature','avx',
                             '-j',workers,'-o',str(output),str(source)],
                            capture_output=True,text=True,timeout=90)
        assert proc.returncode==1 and not output.exists(),(workers,proc.stderr)
        assert 'backend function 0:' in proc.stderr and 'unsupported explicit instruction form' in proc.stderr,proc.stderr
        messages.append(proc.stderr)
    assert len(set(messages))==1,('nondeterministic backend diagnostics',messages)
print(f'PASS: {builds} backend builds, {pairs} deterministic comparisons; '
      '8 ISA/ABI profiles, O0/O2/Oz, 1/2/8/auto/repeated workers, retained/streamed graphs; '
      'three deterministic multiple-failure compilations.')
