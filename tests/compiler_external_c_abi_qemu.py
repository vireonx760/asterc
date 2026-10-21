#!/usr/bin/env python3
"""Actual AsterASM ET_REL -> independent C17 objects -> static ELF -> QEMU.

Checks both directions of the C ABI, nested/multisite calls, 9 stack/register
arguments, narrow return extension, seeded nonvolatile registers and SP
alignment. No BIN .incbin substitute is used for the AsterASM objects.
Negative ELF runners deliberately corrupt ABI state; their nonzero outcomes
must be observed. All programs must execute, not merely link successfully.
"""
import argparse
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile

TYPES = [('u8', 'unsigned char', 8, False), ('i8', 'signed char', 8, True),
         ('u16', 'unsigned short', 16, False), ('i16', 'short', 16, True),
         ('u32', 'unsigned int', 32, False), ('i32', 'int', 32, True),
         ('u64', 'unsigned long long', 64, False), ('i64', 'long long', 64, True)]


def checked(command, env=None, success=True):
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=45)
    if (result.returncode == 0) != success:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))
    return result


def probe_assembly(arm):
    if arm:
        registers = ['x' + str(i) for i in range(19, 30)]
        lines = ['.text', '.globl abi_probe', '.type abi_probe,%function', 'abi_probe:',
                 'sub sp,sp,#128']
        for i, reg in enumerate(registers + ['x30']):
            lines.append(f'str {reg},[sp,#{i*8}]')
        lines += ['str x3,[sp,#96]', 'mov x16,#0x7eed', 'str x16,[sp,#104]',
                  'mov x16,x0', 'mov x0,x1', 'mov x1,x2']
        for i, reg in enumerate(registers):
            lines.append(f'mov {reg},#{0x500+i}')
        lines += ['blr x16', 'ldr x16,[sp,#96]', 'str x0,[x16]',
                  'ldr x16,[sp,#104]', 'mov x17,#0x7eed', 'cmp x16,x17', 'b.ne 1f']
        for i, reg in enumerate(registers):
            lines += [f'cmp {reg},#{0x500+i}', 'b.ne 1f']
        lines += ['mov x0,#0', 'b 2f', '1:', 'mov x0,#1', '2:']
        for i, reg in enumerate(registers + ['x30']):
            lines.append(f'ldr {reg},[sp,#{i*8}]')
        lines += ['add sp,sp,#128', 'ret', '.size abi_probe,.-abi_probe']
        lines += ['.globl bad_saved', 'bad_saved:', 'mov x19,#0', 'ret',
                  '.globl bad_stack', 'bad_stack:', 'str xzr,[sp,#104]', 'ret',
                  '.globl entry_sp_alignment', 'entry_sp_alignment:',
                  'mov x0,sp', 'and x0,x0,#15', 'ret']
        lines += ['.globl c_clobber', 'c_clobber:', 'add x0,x0,x1']
        lines += [f'mov x{i},#0' for i in range(1,18)]
        lines += ['ret', '.globl bad_alignment', 'bad_alignment:',
                  'sub sp,sp,#32', 'str x30,[sp]', 'sub sp,sp,#8',
                  'bl entry_sp_alignment', 'add sp,sp,#8', 'ldr x30,[sp]',
                  'add sp,sp,#32', 'ret']
    else:
        registers = ['s' + str(i) for i in range(12)]
        lines = ['.text', '.option norelax', '.globl abi_probe', '.type abi_probe,@function',
                 'abi_probe:', 'addi sp,sp,-128']
        for i, reg in enumerate(registers + ['ra']):
            lines.append(f'sd {reg},{i*8}(sp)')
        lines += ['sd a3,104(sp)', 'li t1,0x7eed', 'sd t1,112(sp)',
                  'mv t0,a0', 'mv a0,a1', 'mv a1,a2']
        for i, reg in enumerate(registers):
            lines.append(f'li {reg},{0x500+i}')
        lines += ['jalr ra,t0,0', 'ld t0,104(sp)', 'sd a0,0(t0)',
                  'ld t0,112(sp)', 'li t1,0x7eed', 'bne t0,t1,1f']
        for i, reg in enumerate(registers):
            lines += [f'li t0,{0x500+i}', f'bne {reg},t0,1f']
        lines += ['li a0,0', 'j 2f', '1:', 'li a0,1', '2:']
        for i, reg in enumerate(registers + ['ra']):
            lines.append(f'ld {reg},{i*8}(sp)')
        lines += ['addi sp,sp,128', 'ret', '.size abi_probe,.-abi_probe']
        lines += ['.globl bad_saved', 'bad_saved:', 'li s2,0', 'ret',
                  '.globl bad_stack', 'bad_stack:', 'sd zero,112(sp)', 'ret',
                  '.globl entry_sp_alignment', 'entry_sp_alignment:',
                  'andi a0,sp,15', 'ret']
        lines += ['.globl c_clobber', 'c_clobber:', 'add a0,a0,a1']
        lines += [f'li {reg},0' for reg in ['t'+str(i) for i in range(7)]+['a'+str(i) for i in range(1,8)]]
        lines += ['ret', '.globl bad_alignment', 'bad_alignment:',
                  'addi sp,sp,-32', 'sd ra,0(sp)', 'addi sp,sp,-8',
                  'call entry_sp_alignment', 'addi sp,sp,8', 'ld ra,0(sp)',
                  'addi sp,sp,32', 'ret']
    return '\n'.join(lines) + '\n'


def sources():
    leaf = ['module qual.provider;',
            'section ".data" @writable { export global cell:u64=41; export global pointer:ptr<u64> = &cell; }',
            'section ".bss" @writable { export global cleared:u64; }',
            'export fn memory_roundtrip(p:ptr<u64>,v:u64)->u64 @abi(c) { store.u64 [p]=v; return load.u64 [p]+1; }',
            'export fn leaf(x:u64,y:u64)->u64 @abi(c) { return (x/7)+(y%3); }',
            'export fn many(a:u64,b:u64,c:u64,d:u64,e:u64,f:u64,g:u64,h:u64,i:u64)->u64 @abi(c) { return a+b+c+d+e+f+g+h+i; }',
            'type Pair=struct @layout(c) { a:u64; b:u64; };',
            'type Triple=struct @layout(c) { a:u64; b:u64; c:u64; };',
            'type Mixed=struct @layout(c) { a:u64; b:f64; };',
            'fn pair_value(v:Pair)->Pair { return v; }',
            'fn triple_value(v:Triple)->Triple { return v; }',
            'fn mixed_value(v:Mixed)->Mixed { return v; }',
            'export fn pair_roundtrip(v:Pair)->Pair @abi(c) { return pair_value(v); }',
            'export fn triple_roundtrip(v:Triple)->Triple @abi(c) { return triple_value(v); }',
            'export fn mixed_roundtrip(v:Mixed)->Mixed @abi(c) { return mixed_value(v); }']
    for typ, _, _, sign in TYPES:
        for op, suffix in (('/', 'div'), ('%', 'rem')):
            divisor = '-7' if sign else '7'
            leaf.append(f'export fn scalar_{typ}_{suffix}(x:{typ},y:{typ})->{typ} @abi(c) {{ return x {op} y; }}')
            leaf.append(f'export fn literal_{typ}_{suffix}(x:{typ},y:{typ})->{typ} @abi(c) {{ return x {op} {divisor}; }}')
    bridge = '''module qual.bridge;
extern fn c_mix(x:u64,y:u64)->u64;
extern fn c_clobber(x:u64,y:u64)->u64;
extern fn entry_sp_alignment()->u64;
export fn bridge(x:u64,y:u64)->u64 @abi(c) {
    let saved:u64=x+y;
    let first:u64=c_mix(x,y);
    let second:u64=c_mix(y,x);
    let third:u64=c_clobber(x,y);
    return first+second+(saved/7)+(y%3)+third;
}
export fn align_bridge()->u64 @abi(c) { return entry_sp_alignment(); }
'''
    return '\n'.join(leaf) + '\n', bridge


def c_runner(negative):
    lines = ['typedef unsigned long long U;', 'typedef long long I;',
             'extern U leaf(U,U);', 'extern U bridge(U,U);', 'extern U align_bridge(void);',
             'extern U many(U,U,U,U,U,U,U,U,U);',
             'extern U cell, cleared;', 'extern U *pointer;', 'extern U memory_roundtrip(U*,U);',
             'extern U abi_probe(U(*)(U,U),U,U,U*);',
             'extern U bad_saved(U,U);', 'extern U bad_stack(U,U);', 'extern U bad_alignment(void);',
             'typedef struct { U a,b; } Pair;', 'typedef struct { U a,b,c; } Triple;',
             'typedef struct { U a; double b; } Mixed;',
             'extern Pair pair_roundtrip(Pair);', 'extern Triple triple_roundtrip(Triple);',
             'extern Mixed mixed_roundtrip(Mixed);',
             'static volatile U calls;', 'static volatile U sink;',
             '__attribute__((noinline)) U c_mix(U x,U y) { calls++; return leaf(x,y)+x*3+y; }']
    checks = []
    rng = random.Random(0x6a3ac17)
    call_count = 0
    for typ, ctype, bits, signed in TYPES:
        lo = -(1 << (bits-1)) if signed else 0
        hi = (1 << (bits-int(signed))) - 1
        values = [lo, lo+1, -7, -1, 0, 1, 7, hi-1, hi]
        values = sorted(set(x for x in values if lo <= x <= hi))
        values += [rng.randint(lo,hi) for _ in range(16)]
        for dynamic in (True, False):
            for op, suffix in (('/', 'div'), ('%', 'rem')):
                name = ('scalar_' if dynamic else 'literal_') + typ + '_' + suffix
                lines.append(f'extern {ctype} {name}({ctype},{ctype});')
                for x in values:
                    y = rng.choice([-7,-3,-1,1,3,7] if signed else [1,3,7,hi]) if dynamic else (-7 if signed else 7)
                    if signed and x == lo and y == -1:
                        y = 3
                    q = abs(x)//abs(y)
                    if signed and (x < 0) != (y < 0):
                        q = -q
                    wanted = q if op=='/' else x-q*y
                    def literal(value):
                        return f'({ctype})0x{value & ((1<<bits)-1):x}ULL'
                    # A widening cast exposes a bad narrow C ABI return, not
                    # merely a low-width register mask in the Python harness.
                    wide = 'I' if signed else 'U'
                    checks.append(f'if (({wide}){name}({literal(x)},{literal(y)}) != ({wide}){literal(wanted)}) return 10;')
                    call_count += 1
    lines += ['int run_test(void) {', 'U got=0;', 'if (align_bridge()!=0) return 11;']
    if negative:
        if negative == 'alignment':
            lines += ['got=bad_alignment();', 'sink=got;', 'return got==8 ? 77 : 0;', '}']
            return '\n'.join(lines)+'\n', 1
        symbol = 'bad_saved' if negative == 'saved' else 'bad_stack'
        lines += [f'if (abi_probe({symbol},17,9,&got)==0) return 0;', 'sink=got;', 'return 77;', '}']
        return '\n'.join(lines)+'\n', 1
    lines += ['if (cell!=41 || pointer!=&cell || cleared!=0) return 16;',
              'if (memory_roundtrip(pointer,80)!=81 || cell!=80) return 17;',
              'if (memory_roundtrip(&cleared,99)!=100 || cleared!=99) return 18;',
              'Pair pair=pair_roundtrip((Pair){17,0xffffffffffffffffULL});',
              'if (pair.a!=17 || pair.b!=0xffffffffffffffffULL) return 19;',
              'Triple triple=triple_roundtrip((Triple){17,29,0x8000000000000000ULL});',
              'if (triple.a!=17 || triple.b!=29 || triple.c!=0x8000000000000000ULL) return 20;',
              'Mixed mixed=mixed_roundtrip((Mixed){37,1.25}); union { double d; U u; } bits;',
              'bits.d=mixed.b; if (mixed.a!=37 || bits.u!=0x3ff4000000000000ULL) return 21;']
    lines += checks
    # Python integer arithmetic provides the expectations. The independent
    # C caller stays freestanding RV64I and needs no __udivdi3/__umoddi3.
    rows = []
    for x in range(0, 193, 7):
        y = 241-x
        leaf = x//7+y%3
        want = (leaf+x*3+y)+(y//7+x%3+y*3+x)+(x+y)//7+y%3+x+y
        rows.append('{'+','.join(str(v)+'ULL' for v in (x,y,want,leaf))+'}')
    lines += ['if (many(1,2,3,4,5,6,7,8,9)!=45) return 12;',
              'static const struct {U x,y,want,leaf;} v[] = {'+','.join(rows)+'};',
              'for (unsigned n=0; n<sizeof(v)/sizeof(v[0]); n++) {',
              'U x=v[n].x, y=v[n].y, want=v[n].want;',
              'U before=calls;', 'if (abi_probe(bridge,x,y,&got)!=0) return 13;',
              'sink=got;', 'if (got!=want || calls!=before+2) return 14;',
              'if (abi_probe(leaf,x,y,&got)!=0 || got!=v[n].leaf) return 15;', '}',
              'return 0;', '}']
    return '\n'.join(lines)+'\n', call_count + 6 + 1 + 1 + 28*2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--opt-level',choices=['0','2','z'],default='0')
    parser.add_argument('--clang', default='clang')
    parser.add_argument('--out')
    parser.add_argument('--quick', action='store_true')
    args = parser.parse_args()
    for tool in (args.clang, 'ld.lld', 'readelf', 'qemu-aarch64', 'qemu-riscv64'):
        if not shutil.which(tool):
            raise RuntimeError('Mandatory external C ABI runtime tool missing: '+tool)
    records = []
    with tempfile.TemporaryDirectory(prefix='aster-external-c-') as temp:
        root = Path(temp)
        for arch, m in [('aarch64',False),('riscv64',True),('riscv64',False)]:
            arm = arch=='aarch64'
            triple = 'aarch64-linux-gnu' if arm else 'riscv64-linux-gnu'
            flags = ['--target='+triple, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror',
                     '-ffreestanding', '-fno-builtin', '-fno-stack-protector']
            if not arm:
                flags += ['-march=rv64im' if m else '-march=rv64i', '-mabi=lp64']
            entry = root/'entry.S'
            start = ('bl run_test\nmov x8,#93\nsvc #0' if arm else 'call run_test\nli a7,93\necall')
            entry.write_text('.text\n.globl _start\n_start:\n'+start+'\n'+probe_assembly(arm))
            for policy in ('hot','size'):
                for limit in ([2] if args.quick else [0,2]):
                    env = os.environ.copy(); env['ASTERASM_DIV_POLICY']=policy
                    objects=[]
                    for label,source_text in zip(('provider','bridge'),sources()):
                        source, obj = root/(label+'.aa'), root/(label+'.o')
                        source.write_text(source_text)
                        command=[args.compiler,'build','-O'+args.opt_level,'--target',arch,'--emit','obj','--format','elf',
                                 '--no-red-zone','--reg-limit',str(limit),'-j','1','-o',str(obj),str(source)]
                        if m:
                            command += ['--feature','m']
                        checked(command,env)
                        objects.append(str(obj))
                    relocation=checked(['readelf','-r','-sW',objects[1]]).stdout
                    expected='R_AARCH64_CALL26' if arm else 'R_RISCV_CALL_PLT'
                    if expected not in relocation or 'UND c_mix' not in relocation:
                        raise AssertionError('Missing actual AsterASM call relocation')
                    for negative in (None,'saved','stack','alignment'):
                        runner=root/'runner.c'; exe=root/'runner.elf'
                        text,calls=c_runner(negative);runner.write_text(text)
                        link=[args.clang,*flags,'-nostdlib','-static','-fuse-ld=lld',
                              '-Wl,-e,_start','-Wl,--build-id=none','-o',str(exe),str(entry),str(runner),*objects]
                        checked(link)
                        qemu_cmd=['qemu-'+arch]
                        if not arm and not m:qemu_cmd+=['-cpu','rv64,m=false']
                        result=subprocess.run(qemu_cmd+[str(exe)],capture_output=True,text=True,timeout=30)
                        wanted=77 if negative else 0
                        if result.returncode != wanted:
                            raise AssertionError((arch,m,policy,limit,negative,result.returncode,result.stderr))
                        records.append({'target':arch,'m':m,'policy':policy,'reg_limit':limit,
                                        'negative':negative,'exit_code':result.returncode,'checks':calls})
                    # Undefined providers must fail linking, never become zero.
                    runner.write_text(c_runner(None)[0])
                    checked([args.clang,*flags,'-nostdlib','-static','-fuse-ld=lld','-o',str(root/'undefined.elf'),
                             str(entry),str(runner),objects[1]],success=False)
    report={'clang':checked([args.clang,'--version']).stdout.splitlines()[0],
            'qemu':{a:checked(['qemu-'+a,'--version']).stdout.splitlines()[0] for a in ('aarch64','riscv64')},
            'records':records}
    if args.out:
        Path(args.out).write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS: {len(records)} actual QEMU ELF executions, '
          f'{sum(r["checks"] for r in records if not r["negative"])} C/ABI checks, '
          f'{sum(bool(r["negative"]) for r in records)} detected ABI corruptions')


if __name__ == '__main__':
    main()
