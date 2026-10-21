"""Link generated atomic code with C and execute real concurrent threads."""
from pathlib import Path
import argparse,os,subprocess,tempfile,shutil
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',required=True);p.add_argument('--clang',required=True)
a=p.parse_args();root=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='aster-atomic-native-') as temp:
    obj=Path(temp)/'atomic.o';exe=Path(temp)/('test.exe' if os.name=='nt' else 'test')
    def run(cmd):
        r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=90)
        if r.returncode:raise RuntimeError(f'{cmd}\n{r.stdout}{r.stderr}')
        return r.stdout
    abi,fmt=('win64','coff') if os.name=='nt' else ('sysv','elf')
    for limit in [0,2]:
        run([a.compiler,'build','--target','x86_64','--abi',abi,'--emit','obj','--format',fmt,'--reg-limit',str(limit),'-j','1','-o',obj,root/'compiler_e2e/atomic_threads.aa'])
        flags=['--target=x86_64-w64-windows-gnu'] if os.name=='nt' and shutil.which('gcc') else ([] if os.name=='nt' else ['-pthread'])
        run([a.clang,*flags,'-std=c17','-O2',root/'atomic_threads.c',obj,'-o',exe])
        print(f'reg-limit={limit}: '+run([exe]),end='')
