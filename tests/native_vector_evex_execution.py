#!/usr/bin/env python3
"""AVX-512 executes in Bochs; masks, upper ZMM aliases and native SSA survive."""
import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile

from native_processor_qemu import command, x86_kernel
from native_vector_execution import ENABLE

# Bochs uses POSIX image locks. Stage Windows-created images onto the Linux
# filesystem instead of depending on DrvFS locking semantics or NTFS handles.
STAGED_RUN = '''import pathlib, shutil, subprocess, sys, tempfile
with tempfile.TemporaryDirectory(prefix="aster-evex-") as temporary:
    root = pathlib.Path(temporary)
    shutil.copyfile(sys.argv[2], root / "disk.img")
    config = pathlib.Path(sys.argv[3]).read_text().replace("@WORK@", temporary)
    (root / "bochsrc").write_text(config)
    shutil.copyfile(sys.argv[4], root / "debugger.rc")
    result = subprocess.run([sys.argv[1], "-q", "-debugger", "-f", str(root / "bochsrc"), "-rc", str(root / "debugger.rc")],
                            capture_output=True, text=True, timeout=45)
    print(result.stdout); print(result.stderr)
    if (root / "bochs.log").exists():
        shutil.copyfile(root / "bochs.log", pathlib.Path(sys.argv[3]).with_name("bochs.log"))
        if result.returncode: print((root / "bochs.log").read_text()[-6000:])
    sys.exit(result.returncode)
'''


def bochs_run(args, root, disk):
    def guest_path(path):
        absolute = Path(path).resolve().as_posix()
        return '/mnt/' + absolute[0].lower() + absolute[2:] if args.wsl_distro else absolute
    bios = str(PurePosixPath(args.bochs).parent.parent / 'share/bochs') if args.wsl_distro else str(Path(args.bochs).resolve().parent.parent / 'share/bochs')
    # Full CHS geometry is required by the Bochs ATA model, unlike QEMU's raw
    # disk loader. The shared AsterASM BIOS startup reads 112 sectors with INT 13h.
    with disk.open('ab') as stream: stream.write(bytes(max(0, 1008 * 512 - disk.stat().st_size)))
    config = root / 'bochsrc'; script = root / 'debugger.rc'; log = root / 'bochs.log'
    work = '@WORK@' if args.wsl_distro else guest_path(root)
    config.write_text(f'''megs: 32
romimage: file="{bios}/BIOS-bochs-latest"
vgaromimage: file="{bios}/VGABIOS-lgpl-latest.bin"
display_library: nogui
cpu: model=corei7_skylake_x, count=1, ips=1000000, reset_on_triple_fault=0
clock: sync=none, time0=946684800
ata0-master: type=disk, path="{work}/disk.img", mode=flat, cylinders=1, heads=16, spt=63
boot: disk
log: {work}/bochs.log
panic: action=fatal
error: action=report
info: action=report
debug: action=ignore
mouse: enabled=0
''')
    nm = str(Path(args.clang).with_name('llvm-nm' + ('.exe' if os.name == 'nt' else '')))
    symbols = command([nm, '-n', root / 'start.elf']).stdout.splitlines()
    stop = next(int(line.split()[0], 16) for line in symbols if line.split()[-1:] == ['finished'])
    script.write_text(f'lb 0x{stop:x}\nc\nr\nq\n', newline='\n')
    argv = [args.bochs, '-q', '-debugger', '-f', guest_path(config), '-rc', guest_path(script)]
    if args.wsl_distro:
        argv = ['wsl.exe', '-d', args.wsl_distro, '--', 'python3', '-c', STAGED_RUN,
                args.bochs, guest_path(disk), guest_path(config), guest_path(script)]
    argv[0] = shutil.which(argv[0]) or argv[0]
    result = subprocess.run(argv, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
    output = result.stdout + result.stderr
    (root / 'bochs-debugger.log').write_text(output)
    values = re.findall(r'\brax:\s*(?:0x)?([0-9a-fA-F_]+)', output)
    assert result.returncode == 0 and values and int(values[-1].replace('_', ''), 16) == 0x10, (argv, result.returncode, output[-6000:], log.read_text(errors='replace')[-4000:] if log.exists() else '')
    return hashlib.sha256(output.encode()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True); p.add_argument('--clang', required=True)
    p.add_argument('--bochs', required=True); p.add_argument('--wsl-distro'); p.add_argument('--out'); p.add_argument('--smoke', action='store_true'); p.add_argument('--work')
    args = p.parse_args(); records = []
    if args.work: Path(args.work).mkdir(parents=True, exist_ok=True)
    with (nullcontext(args.work) if args.work else tempfile.TemporaryDirectory(prefix='aster-evex-cpu-')) as temporary:
        root = Path(temporary); source = root / 'case.aa'; raw = root / 'case.bin'
        for bits, encoding in ((128, 'vex'), (128, 'evex'), (256, 'evex'), (512, 'evex')):
            reg = 'xmm2' if encoding == 'vex' else {128: 'xmm31', 256: 'ymm31', 512: 'zmm31'}[bits]; lanes = bits // 32
            full_reg = 'zmm2' if encoding == 'vex' else 'zmm31'
            transfer = 'vmovdqu' if encoding == 'vex' else 'vmovdqu32'
            for style in (('unmasked',) if encoding == 'vex' else ('unmasked', 'merge', 'zero')):
                extra = '' if style == 'unmasked' else ',mask:k1,zero:' + ('1' if style == 'zero' else '0')
                body = f'mov {{dst:rax,src:p}}; {transfer} {{dst:{reg},src:mem<vec<{lanes},u32>>[rax]}}; ' + \
                    f'vpaddd {{dst:{reg},lhs:{reg},rhs:{reg}{extra}}}; vpaddd {{dst:zmm0,lhs:{full_reg},rhs:{full_reg}}}; ' + \
                    f'vpaddd {{dst:zmm15,lhs:{full_reg},rhs:{full_reg}}}; ' + \
                    f'{transfer} {{dst:mem<vec<{lanes},u32>>[rax],src:{reg}}}; mov {{dst:out,src:x}};'
                for mode in ('native', 'exact', 'unsafe exact'):
                    source.write_text('module vector.evex_cpu; export fn run(p:ptr<u32>,seed:u64,x:vec<4,u32>)->u64 @abi(c) {'
                                      'let live:u64=seed*17+9; let out:vec<4,u32>; ' + mode +
                                      ' x86_64 inputs(p,x) outputs(out) { ' + body + ' } return live+zext<u64>(out[0]); }')
                    for abi in ('sysv', 'win64'):
                        arg, seed = ('rdi', 'esi') if abi == 'sysv' else ('rcx', 'edx')
                        oracle = ENABLE.replace('mov eax,7', 'mov eax,0xe7') + f'''
vmovdqu64 zmm0,zmmword ptr [rip+canary0]
vmovdqu64 {full_reg},zmmword ptr [rip+canary31]
mov eax,0x5555
kmovw k1,eax
''' + f'lea {arg},[rip+values]\nmov {seed},11\n' + \
                        ('movdqu xmm0,xmmword ptr [rip+input_vector]\n' if abi == 'sysv' else 'lea r8,[rip+input_vector]\n') + \
                        'call aster_entry\ncmp rax,238\njne failed\n' + f'''
vpxord zmm1,{full_reg},zmmword ptr [rip+canary31]
vptestmd k2,zmm1,zmm1
kortestw k2,k2
jnz failed
vpxord zmm1,zmm0,zmmword ptr [rip+canary0]
vptestmd k2,zmm1,zmm1
kortestw k2,k2
jnz failed
'''
                        for index in range(16):
                            expected = index + 1
                            if index < lanes:
                                if style == 'unmasked' or index % 2 == 0: expected *= 2
                                elif style == 'zero': expected = 0
                            oracle += f'cmp dword ptr [rip+values+{index * 4}],{expected}\njne failed\n'
                        oracle += 'jmp checked\n.balign 64\ncanary0: .long ' + ','.join(str(42 + i if i < 4 else 0x10203040 + i * 17) for i in range(16)) + \
                            '\ncanary31: .long ' + ','.join(str(0xdead0000 + i * 31) for i in range(16)) + \
                            '\ninput_vector: .long 42,43,44,45\nvalues: .long ' + ','.join(str(i + 1) for i in range(16)) + '\nchecked:\n'
                        for level in ('0', '2', 'z'):
                            command([args.compiler, 'build', '--target', 'x86_64', '--abi', abi, '--feature', 'avx512f', '--feature', 'avx2',
                                     '--feature', 'avx512vl', '-O' + level, '--no-red-zone', '--reg-limit', '2', '-j', '1', '-o', raw, source])
                            disk, _ = x86_kernel(oracle, raw, root, args.clang, abi, args.compiler, run_emulator=False)
                            transcript = bochs_run(args, root, disk)
                            records.append({'bits': bits, 'encoding': encoding, 'style': style, 'mode': mode, 'abi': abi, 'level': level,
                                            'sha256': hashlib.sha256(raw.read_bytes()).hexdigest(), 'transcript_sha256': transcript})
                            if args.smoke: break
                        if args.smoke: break
                    if args.smoke: break
                print('PASS:', encoding.upper(), bits, style, flush=True)
                if args.smoke: break
            if args.smoke: break
    if args.out: Path(args.out).write_text(json.dumps({'bochs_runs': len(records), 'records': records}, indent=2) + '\n')
    print(f'PASS: {len(records)} Bochs AVX-512 runs' + (' (smoke subset)' if args.smoke else
          '; masks, zero/merge, complete ZMM aliases, VEX MAX_VL, ABI and live SSA'))


if __name__ == '__main__': main()
