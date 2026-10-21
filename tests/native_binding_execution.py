#!/usr/bin/env python3
"""Execute typed AArch64/RV64 native bridges, including privileged and NEON paths."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile

_bootstrap = argparse.ArgumentParser(add_help=False)
_bootstrap.add_argument('--python-path')
_known, _ = _bootstrap.parse_known_args()
if _known.python_path: sys.path.insert(0,_known.python_path)
import unicorn as u
from unicorn import arm64_const as arm, riscv_const as rv
from native_processor_qemu import command, other_kernel
from native_processor_contracts import assemble

MASK = (1 << 64) - 1


def scalar_call(arch, code, arguments, stack_bytes=0x100000):
    is_arm = arch == 'aarch64'
    cpu = u.Uc(u.UC_ARCH_ARM64 if is_arm else u.UC_ARCH_RISCV, u.UC_MODE_ARM if is_arm else u.UC_MODE_RISCV64)
    cpu.mem_map(0x100000, 0x100000); cpu.mem_map(0x400000, stack_bytes)
    cpu.mem_write(0x100000, code)
    sp = arm.UC_ARM64_REG_SP if is_arm else rv.UC_RISCV_REG_SP
    link = arm.UC_ARM64_REG_LR if is_arm else rv.UC_RISCV_REG_RA
    args = [arm.UC_ARM64_REG_X0, arm.UC_ARM64_REG_X1, arm.UC_ARM64_REG_X2] if is_arm else [rv.UC_RISCV_REG_A0, rv.UC_RISCV_REG_A1, rv.UC_RISCV_REG_A2]
    preserved = [getattr(arm, 'UC_ARM64_REG_X' + str(n)) for n in range(19, 30)] if is_arm else [getattr(rv, 'UC_RISCV_REG_S' + str(n)) for n in range(12)]
    stack_top = 0x400000 + stack_bytes - 0x1000
    cpu.reg_write(sp, stack_top); cpu.reg_write(link, 0x1f0000)
    for r, value in zip(args, arguments): cpu.reg_write(r, value & MASK)
    for n, r in enumerate(preserved): cpu.reg_write(r, 0xabcdef1234000000 + n)
    cpu.emu_start(0x100000, 0x1f0000, timeout=1000000, count=100000)
    pc = arm.UC_ARM64_REG_PC if is_arm else rv.UC_RISCV_REG_PC
    assert cpu.reg_read(pc) == 0x1f0000 and cpu.reg_read(sp) == stack_top
    for n, r in enumerate(preserved): assert cpu.reg_read(r) == 0xabcdef1234000000 + n, (arch, r)
    return cpu.reg_read(args[0])


def signed(value, bits): return value - (1 << bits) if value & (1 << (bits - 1)) else value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True); parser.add_argument('--clang', required=True)
    parser.add_argument('--proof', required=True); parser.add_argument('--dumper', required=True)
    parser.add_argument('--python-path')
    parser.add_argument('--out'); parser.add_argument('--smoke', action='store_true')
    args = parser.parse_args(); records = []; calls = qemu_runs = rejections = roundtrips = 0
    modes = ('native', 'exact') if args.smoke else ('native', 'exact', 'unsafe exact')
    levels = ('0',) if args.smoke else ('0', '2', 'z')
    pressures = (2,) if args.smoke else (0, 2)
    promotions = (False,) if args.smoke else (False, True)
    types = ('u8', 'u64') if args.smoke else ('u8', 'i8', 'u16', 'i16', 'u32', 'i32', 'u64', 'i64')
    with tempfile.TemporaryDirectory(prefix='aster-native-bindings-') as temporary:
        root = Path(temporary); source = root / 'case.aa'; raw = root / 'case.bin'

        def build(arch, text, mode, level, pressure, promote, feature=None):
            source.write_text(text)
            argv = [args.compiler, 'build', '--target', arch, '-O' + level, '--reg-limit', str(pressure), '-j', '1', '-o', raw, source]
            if not promote: argv += ['--no-promote']
            if feature: argv += ['--feature', feature]
            command(argv)
            code = raw.read_bytes()
            records.append({'arch': arch, 'mode': mode, 'level': level, 'reg_limit': pressure, 'promote': promote,
                            'bytes': len(code), 'sha256': hashlib.sha256(code).hexdigest()})
            return code

        for base in ('x0','x1','sp'):
            for offset in (-(1 << 31),-33554433,-16777216,-65537,-4096,-1,0,1,4095,4096,65537,16777216,33554433,(1 << 31)-1):
                command([args.proof,'--emit-address',str(offset),base,raw]); code = raw.read_bytes()
                a,b = 0xfedcba9876543210,0x123456789abcdef0
                start = a if base == 'x0' else b if base == 'x1' else 0x4ff000
                assert scalar_call('aarch64',code,(a,b,0)) == (start+offset) & MASK,(base,offset)
                calls += 1
                if base != 'x0': assert len(code) <= 16,(base,offset,len(code))
                payload = json.loads(command([args.dumper,'--target','aarch64','--strict','--format','json',raw]).stdout)
                rebuilt = assemble(args.clang,'aarch64','\n'.join(i['assembly'] for i in payload['instructions']),root)
                assert rebuilt == code,('address roundtrip',base,offset)
                roundtrips += 1
                records.append({'arch': 'aarch64', 'kind': 'address-boundary', 'offset': offset, 'base': base,
                                'bytes': len(code), 'sha256': hashlib.sha256(code).hexdigest()})

        for arch in ('aarch64', 'riscv64'):
            for padding in (65536,33554432):
                command([args.proof,'--emit-frame',arch,raw,str(padding)])
                code = raw.read_bytes()
                for a,b,c in [(11,13,17),(MASK,0x123456789abcdef0,0xfedcba9876543210)]:
                    assert scalar_call(arch,code,(a,b,c),stack_bytes=0x4000000) == (a*17+b+c+a+b) & MASK
                    calls += 1
                payload = json.loads(command([args.dumper,'--target',arch,'--strict','--format','json',raw]).stdout)
                assert payload['unknown'] == 0
                rebuilt = assemble(args.clang,arch,'\n'.join(i['assembly'] for i in payload['instructions']),root)
                assert rebuilt == code,(arch,padding,'large frame dumper mismatch')
                roundtrips += 1
                records.append({'arch': arch, 'kind': 'large-frame', 'padding': padding, 'bytes': len(code), 'sha256': hashlib.sha256(code).hexdigest()})
            for typ in types:
                bits = int(typ[1:]); mask = (1 << bits) - 1; is_signed = typ[0] == 'i'
                operands = [(0, 0, 0), (1, 2, 13), (mask, 1, MASK), (1 << (bits - 1), mask, 17),
                            (0xfedcba9876543210 & mask, 0x123456789abcdef0 & mask, 123)]
                for mode in modes:
                    move, hard = ('mov', 'x19') if arch == 'aarch64' else ('mv', 's2')
                    text = ('module native.bindings; export fn run(a:' + typ + ',b:' + typ + ',c:u64)->u64 {'
                            'let live:u64=cast<u64>(a)*17+cast<u64>(b)+c; let out:' + typ + '; ' + mode + ' ' + arch +
                            ' inputs(a,b) outputs(out) {' + move + ' {dst:' + hard + ',src:a}; add {dst:out,lhs:' + hard + ',rhs:b};'
                            '} return live+cast<u64>(out);}')
                    for level in levels:
                        for pressure in pressures:
                            for promote in promotions:
                                code = build(arch,text,mode,level,pressure,promote)
                                records[-1]['kind'] = 'scalar-' + typ
                                for a, b, c in operands:
                                    av, bv = (signed(a,bits),signed(b,bits)) if is_signed else (a,b)
                                    out = (a+b) & mask
                                    expected = (av*17+bv+c+(signed(out,bits) if is_signed else out)) & MASK
                                    # RV psABI extends narrow scalars to 32, then sign-extends to XLEN.
                                    abi_a = signed(a,32) if arch == 'riscv64' and bits == 32 else av
                                    abi_b = signed(b,32) if arch == 'riscv64' and bits == 32 else bv
                                    actual = scalar_call(arch,code,(abi_a,abi_b,c))
                                    assert actual == expected, (arch,typ,mode,level,pressure,promote,a,b,actual,expected)
                                    calls += 1
                print('PASS: scalar bindings', arch, typ, flush=True)

            for mode in modes:
                first, second = ('x17','x16') if arch == 'aarch64' else ('t5','t6')
                xor = 'eor' if arch == 'aarch64' else 'xor'
                text = ('module native.copy_cycle; export fn run(a:u64,b:u64,c:u64)->u64 {'
                        'bind a to ' + arch + '.' + first + '; bind b to ' + arch + '.' + second + ';'
                        'let live:u64=a*17+b+c; let p:u64; let q:u64; ' + mode + ' ' + arch +
                        ' inputs(a,b) outputs(p,q) {add {dst:p,lhs:a,rhs:b}; ' + xor +
                        ' {dst:q,lhs:a,rhs:b};} return live+p+q;}')
                for level in levels:
                    for pressure in pressures:
                        for promote in promotions:
                            code = build(arch,text,mode,level,pressure,promote)
                            records[-1]['kind'] = 'copy-cycle-multiple-results'
                            for a,b,c in [(1,2,13),(0xfedcba9876543210,0x123456789abcdef0,MASK)]:
                                actual = scalar_call(arch,code,(a,b,c))
                                expected = (a*17+b+c+a+b+(a^b)) & MASK
                                assert actual == expected,(arch,mode,level,pressure,promote,actual,expected)
                                calls += 1

            for mode in modes:
                body = ('msr {sys:tpidr_el1,src:a}; mrs {dst:out,sys:tpidr_el1};' if arch == 'aarch64' else
                        'csrrw {dst:zero,csr:mscratch,src:a}; csrrs {dst:out,csr:mscratch,src:zero};')
                text = ('module native.system_binding; export fn run(a:u64,b:u64)->u64 {'
                        'let live:u64=a*17+b; let out:u64; ' + mode + ' ' + arch +
                        ' inputs(a,b) outputs(out) {' + body + '} return live+out;}')
                for level in levels:
                    for pressure in pressures:
                        build(arch,text,mode,level,pressure,False,'zicsr' if arch == 'riscv64' else None)
                        records[-1]['kind'] = 'privileged-register-roundtrip'
                        oracle = ('mov x0,#11\nmov x1,#13\nbl aster_entry\ncmp x0,#211\nb.ne failed\n'
                                  'mrs x10,tpidr_el1\ncmp x10,#11\nb.ne failed\n' if arch == 'aarch64' else
                                  'li a0,11\nli a1,13\ncall aster_entry\nli t0,211\nbne a0,t0,failed\n'
                                  'csrr t0,mscratch\nli t1,11\nbne t0,t1,failed\n')
                        other_kernel(arch,oracle,raw,root,args.clang); qemu_runs += 1
            print('PASS: privileged bindings', arch, flush=True)

            bad = ['add {dst:a,lhs:a,rhs:b};', 'add {dst:out,lhs:out,rhs:b};',
                   'mov {dst:out,src:65536};' if arch == 'aarch64' else 'mv {dst:out,src:2048};',
                   'eret;' if arch == 'aarch64' else 'mret;',
                   'svc {imm:0};' if arch == 'aarch64' else 'ecall;',
                   'add {dst:sp,lhs:sp,rhs:16};' if arch == 'aarch64' else 'addi {dst:sp,lhs:sp,rhs:16};',
                   'msr {pstate:spsel,imm:0};' if arch == 'aarch64' else 'add {dst:tp,lhs:a,rhs:b};',
                   'msr {sys:0xc210,src:a};' if arch == 'aarch64' else 'add {dst:ra,lhs:a,rhs:b};']
            for mode in modes:
                for body in bad:
                    source.write_text('module native.rejected; export fn run(a:u64,b:u64)->u64 {let out:u64; ' + mode +
                                      ' ' + arch + ' inputs(a,b) outputs(out) {' + body +
                                      'add {dst:out,lhs:a,rhs:b};} return out;}')
                    raw.unlink(missing_ok=True)
                    argv = [args.compiler,'build','--target',arch,'-j','1','-o',raw,source]
                    if arch == 'riscv64': argv += ['--feature','zicsr']
                    result = command(argv,expected=1)
                    assert result.stderr and not raw.exists(),(arch,mode,body,result.stderr)
                    rejections += 1

        left = [0xffffffff,0x80000000,0x12345678,0x33445566]; right = [1,2,0xabcdef01,0xfedcba98]
        expected = [((a+b)+(a^b)) & 0xffffffff for a,b in zip(left,right)]
        data = ''.join('.balign 16\n' + name + ': .word ' + ','.join(map(str,values)) + '\n'
                       for name,values in [('left',left),('right',right),('expected',expected)])
        oracle = ('mov x9,#0x300000\nmsr cpacr_el1,x9\nisb\nadr x9,left\nldr q0,[x9]\nadr x9,right\nldr q1,[x9]\n'
                  'movi v8.16b,#0x5a\nbl aster_entry\nadr x9,expected\nldr q1,[x9]\neor v0.16b,v0.16b,v1.16b\n'
                  'umaxv b0,v0.16b\nfmov w9,s0\ncbnz w9,failed\nfmov x9,d8\nmov x10,#0x5a5a\n'
                  'movk x10,#0x5a5a,lsl #16\nmovk x10,#0x5a5a,lsl #32\nmovk x10,#0x5a5a,lsl #48\n'
                  'cmp x9,x10\nb.ne failed\nb checked\n' + data + 'checked:\n')
        for mode in modes:
            text = ('module native.neon_binding; export fn run(a:vec<4,u32>,b:vec<4,u32>)->vec<4,u32> {'
                    'let live:vec<4,u32>=a+b; bind live to aarch64.q8; let out:vec<4,u32>; ' + mode +
                    ' aarch64 inputs(a,b) outputs(out) { mov {dst:q0,src:a}; mov {dst:q1,src:b};'
                    'eor {dst:v8,lhs:v0,rhs:v1}; mov {dst:out,src:q8}; } return live+out;}')
            for level in levels:
                for pressure in pressures:
                    build('aarch64',text,mode,level,pressure,False)
                    records[-1]['kind'] = 'neon-full-live-alias'
                    other_kernel('aarch64',oracle,raw,root,args.clang); qemu_runs += 1
        report = {'binaries': len(records), 'scalar_calls': calls, 'qemu_runs': qemu_runs, 'rejections': rejections,
                  'llvm_dumper_roundtrips': roundtrips,
                  'unicorn': u.__version__, 'records': records}
        if args.out: Path(args.out).write_text(json.dumps(report,indent=2) + '\n')
    print(f'PASS: {len(records)} typed native binaries, {calls} guarded scalar calls, {qemu_runs} privileged/NEON QEMU runs, {rejections} rejections')


if __name__ == '__main__': main()
