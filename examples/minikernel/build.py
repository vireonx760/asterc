#!/usr/bin/env python3
"""Build an AsterASM-only BIOS image and optionally exercise its UI in QEMU."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import queue
import threading
import subprocess
import struct
import time

ROOT = Path(__file__).resolve().parent
SECTOR = 512
LOAD_SECTORS = 112
KERNEL_BASE = 0x11000
KERNEL_OFFSET = KERNEL_BASE - 0x10000
KERNEL_CAPACITY = LOAD_SECTORS * SECTOR - KERNEL_OFFSET


def run(command):
    command = [str(x) for x in command]
    print('+', subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True)


def compile_unit(compiler, source, target, base, output, opt='O2'):
    run([compiler, 'build', '-'+opt, '--target', target, '--abi',
         'cdecl' if target == 'x86_32' else 'sysv', '--emit', 'bin',
         '--base', hex(base), '--no-red-zone', '-I', ROOT, '-o', output, ROOT/source])
    return output.read_bytes()


def build_boot(compiler, out):
    """Return sector zero and the protected-mode/paging prefix at LBA 1."""
    code = compile_unit(compiler, 'bootsector.aa', 'x86_64', 0x7c00, out/'boot-real.bin')
    tables = compile_unit(compiler, 'boot/tables.aa', 'x86_64', 0x7d80, out/'boot-tables.bin')
    if len(code) > 0x180 or len(tables) != 128 or tables[-2:] != b'\x55\xaa':
        raise RuntimeError('BIOS sector code/data layout exceeds its reserved region')
    if struct.unpack_from('<HI', tables, 32) != (31, 0x7d80):
        raise RuntimeError('GDTR contract mismatch')
    if struct.unpack_from('<BBHHHQ', tables, 40) != (16, 0, LOAD_SECTORS, 0, 0x1000, 1):
        raise RuntimeError('BIOS disk packet contract mismatch')
    boot = code + bytes(0x180-len(code)) + tables
    protected = compile_unit(compiler, 'boot/protected.aa', 'x86_64', 0x10000, out/'boot-protected.bin')
    paging = compile_unit(compiler, 'boot/paging.aa', 'x86_32', 0x10200, out/'boot-paging.bin')
    if len(protected) > 512 or len(paging) > KERNEL_OFFSET-512:
        raise RuntimeError('protected-mode units exceed their fixed load regions')
    prefix = protected + bytes(512-len(protected)) + paging
    prefix += bytes(KERNEL_OFFSET-len(prefix))
    (out/'boot.bin').write_bytes(boot)
    return boot, prefix


def qemu_command(qemu, image, display=False):
    return [qemu, '-accel', 'tcg', '-machine', 'pc', '-cpu', 'qemu64', '-m', '32M',
            '-drive', 'format=raw,file='+str(image).replace(',', ',,'),
            '-nic', 'none', '-no-reboot', '-no-shutdown',
            *([] if display else ['-display', 'none'])]


def smoke_qemu(qemu, image, timeout):
    serial = image.with_suffix('.serial.log')
    serial.write_bytes(b'')
    command = qemu_command(qemu, image) + ['-serial', 'file:'+str(serial), '-qmp', 'stdio']
    proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    start = time.monotonic(); deadline = start+timeout
    replies = queue.Queue()
    def reader():
        for line in proc.stdout: replies.put(line)
        replies.put(b'')
    thread = threading.Thread(target=reader, daemon=True); thread.start()
    observed = []
    try:
        def receive():
            remaining = deadline-time.monotonic()
            if remaining <= 0: raise RuntimeError('QMP timed out')
            try: line = replies.get(timeout=remaining)
            except queue.Empty: raise RuntimeError('QMP timed out') from None
            if not line: raise RuntimeError('QEMU closed QMP: '+proc.stderr.read().decode(errors='replace'))
            return json.loads(line)
        receive()
        def qmp(execute, arguments=None):
            payload = {'execute': execute}
            if arguments: payload['arguments'] = arguments
            proc.stdin.write(json.dumps(payload).encode()+b'\n'); proc.stdin.flush()
            while True:
                reply = receive()
                if 'error' in reply: raise RuntimeError(reply)
                if 'return' in reply: return reply['return']
        qmp('qmp_capabilities')
        def wait_marker(marker, count=1):
            while time.monotonic() < deadline:
                data = serial.read_bytes()
                if data.count(marker.encode()+b'\n') >= count:
                    observed.append(marker); return
                if proc.poll() is not None: break
                time.sleep(0.02)
            raise RuntimeError(f'missing {marker} #{count}: '+serial.read_text(errors='replace'))
        def key(name):
            qmp('human-monitor-command', {'command-line': 'sendkey '+name})
            time.sleep(0.15)  # Let the key-release event reach the PS/2 controller.
        wait_marker('ASTER'); wait_marker('MENU')
        key('1'); wait_marker('SNAKE'); key('q'); wait_marker('MENU', 2)
        key('2'); wait_marker('TETRIS'); key('q'); wait_marker('MENU', 3)
        key('esc'); wait_marker('HALT')
        qmp('quit')
        proc.wait(timeout=3)
    finally:
        if proc.poll() is None: proc.kill(); proc.wait(timeout=3)
        thread.join(timeout=1)
        proc.stdin.close(); proc.stdout.close(); proc.stderr.close()
    result = {'status': 'PASS', 'markers': observed, 'seconds': round(time.monotonic()-start,3),
              'serial': serial.read_text(), 'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest()}
    print('QEMU PASS: boot, menu, Snake, return, Tetris, return, halt', flush=True)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True, help='asterc 1.2.1 executable')
    p.add_argument('--out-dir', type=Path, default=ROOT/'build')
    p.add_argument('--opt', choices=['O0','O2','Oz'], default='O2')
    action = p.add_mutually_exclusive_group()
    action.add_argument('--run', action='store_true')
    action.add_argument('--smoke', action='store_true', help='headless QEMU boot and keyboard/UI exercise')
    p.add_argument('--serial-only', action='store_true')
    p.add_argument('--smoke-timeout', type=float, default=20)
    p.add_argument('--qemu', default='qemu-system-x86_64')
    a = p.parse_args()
    if a.smoke_timeout <= 0: p.error('--smoke-timeout must be positive')
    if a.serial_only and not a.run: p.error('--serial-only requires --run')
    compiler = shutil.which(a.compiler) or str(Path(a.compiler).resolve())
    out = a.out_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
    boot, prefix = build_boot(compiler, out)
    kernel = compile_unit(compiler, 'kernel.aa', 'x86_64', KERNEL_BASE, out/'kernel.bin', a.opt)
    if len(kernel) > KERNEL_CAPACITY: raise RuntimeError(f'kernel exceeds {KERNEL_CAPACITY} bytes')
    image = out/'aster-minikernel.img'
    image.write_bytes(boot+prefix+kernel+bytes(KERNEL_CAPACITY-len(kernel)))
    sources = {x.relative_to(ROOT).as_posix(): hashlib.sha256(x.read_bytes()).hexdigest()
               for x in sorted(ROOT.rglob('*.aa'))}
    result = {'status': 'BUILT', 'opt': a.opt, 'kernel_bytes': len(kernel),
              'kernel_capacity': KERNEL_CAPACITY, 'source_sha256': sources}
    print(f'image: {image}; kernel: {len(kernel)}/{KERNEL_CAPACITY} bytes', flush=True)
    if a.run or a.smoke:
        qemu = shutil.which(a.qemu)
        if not qemu: raise RuntimeError('qemu-system-x86_64 not found; supply --qemu')
        if a.smoke: result.update(smoke_qemu(qemu, image, a.smoke_timeout))
        else: run(qemu_command(qemu, image, display=not a.serial_only)+['-serial','stdio','-monitor','none'])
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
