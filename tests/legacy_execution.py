"""Execute semantic x86_16/x86_32 output; check stack ABI and object formats."""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile
import struct

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--python-path', required=True)
p.add_argument('--object-writer', action='store_true')
args = p.parse_args()
sys.path.insert(0, args.python_path)
import unicorn as uc
from unicorn import x86_const as x

def execute(data, bits, arguments, result_bits, entry=None):
    base, stack, stop = (0x1000, 0xf000, 0xf800) if bits == 16 else (0x100000, 0x480000, 0x1f0000)
    cpu = uc.Uc(uc.UC_ARCH_X86, uc.UC_MODE_16 if bits == 16 else uc.UC_MODE_32)
    cpu.mem_map(0, 0x500000)
    cpu.mem_write(base, data)
    cpu.reg_write(x.UC_X86_REG_ESP, stack - 4)
    cpu.mem_write(stack - 4, struct.pack('<I', stop) + arguments)
    preserved = [x.UC_X86_REG_EBX, x.UC_X86_REG_ESI, x.UC_X86_REG_EDI, x.UC_X86_REG_EBP]
    for n, reg in enumerate(preserved): cpu.reg_write(reg, 0xabc000 + n * 0x101)
    try:
        cpu.emu_start(base if entry is None else entry, stop, timeout=2000000, count=1000000)
    except uc.UcError as error:
        raise AssertionError(f'x86_{bits}: {error}; PC={cpu.reg_read(x.UC_X86_REG_EIP):#x}') from error
    assert cpu.reg_read(x.UC_X86_REG_EIP) == stop, 'execution did not return'
    assert cpu.reg_read(x.UC_X86_REG_ESP) == stack, 'callee damaged stack'
    for n, reg in enumerate(preserved): assert cpu.reg_read(reg) == 0xabc000 + n * 0x101, ('callee-saved', reg)
    result = cpu.reg_read(x.UC_X86_REG_EAX) | (cpu.reg_read(x.UC_X86_REG_EDX) << 32)
    return result & ((1 << result_bits) - 1)

def parameter(value, bits):
    return (value & ((1 << bits) - 1)).to_bytes(max(4, bits // 8), 'little')

def trunc_div(a, b):
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q

def cases():
    yield 'logical_binary', (Path(__file__).parent/'compiler_e2e/logical_binary.aa').read_text().replace('module compiler.logical_binary;', ''), b'', 32, 42
    yield 'indirect_pointer', (Path(__file__).parent/'compiler_e2e/indirect_pointer.aa').read_text().replace('module compiler.indirect_pointer;', ''), b'', 32, 42
    yield 'macro_runtime', (Path(__file__).parent/'compiler_e2e/macro_runtime.aa').read_text().replace('module compiler.macro_runtime;', ''), b'', 32, 42
    yield 'comptime_layout', (Path(__file__).parent/'compiler_e2e/comptime_layout.aa').read_text().replace('module compiler.comptime_layout;', ''), b'', 32, 42
    yield 'derived_addresses', (Path(__file__).parent/'compiler_e2e/derived_addresses.aa').read_text().replace('module compiler.derived_addresses;', ''), b'', 64, 42
    yield 'call', 'export fn run()->u64 { return leaf(); } fn leaf()->u64 { return 42; }', b'', 64, 42
    yield 'cast', 'export fn run()->u16 { return cast<u16>(256*16-1); }', b'', 16, 4095
    yield 'loop', 'export fn run(n:u32)->u32 { let i:u32=0; let sum:u32=0; loop { if i==n { break; } sum=sum+i; i=i+1; } return sum; }', parameter(19,32), 32, 171
    yield 'phi_swap', 'export fn run(n:u32)->u32 { let a:u32=1; let b:u32=2; let i:u32=0; loop { if i==n { break; } let t:u32=a; a=b; b=t; i=i+1; } return a*10+b; }', parameter(3,32), 32, 21
    yield 'pointer', 'section ".data" @writable { global counter:u32=7; global p:ptr<u32> = &counter; } export fn run(x:u32)->u32 { unsafe { store.u32 [p]=x; return counter+sizeof(usize); } }', parameter(38,32), 32, 42
    yield 'partial', 'section ".rodata" @readonly { const a:[u8;60]={1,2,3}; } export fn run()->u32 { unsafe { let p:ptr<const u8> = int_to_ptr(ptr_to_int(&a)+59); return zext<u32>(load.u8 [p]); } }', b'', 32, 0
    yield 'float', 'export fn run()->u64 { return bitcast<u64>(scale(2.5)); } fn scale(x:f64)->f64 { return x*4.0+0.5; }', b'', 64, struct.unpack('<Q',struct.pack('<d',10.5))[0]
    for bits in [8,16,32,64]:
        for signed in [False, True]:
            ty = ('i' if signed else 'u')+str(bits)
            mask = (1 << bits)-1
            a = -17 if signed else (1 << (bits-1))+13
            b = 7
            values = [(a,b), (0,0), (-1 if signed else mask, 1), (b,a)]
            for op, fn in [('+',lambda a,b:a+b),('-',lambda a,b:a-b),('*',lambda a,b:a*b),('&',lambda a,b:a&b),('|',lambda a,b:a|b),('^',lambda a,b:a^b)]:
                yield f'{ty}_{ord(op)}', f'export fn run(a:{ty},b:{ty})->{ty} {{ return a {op} b; }}', parameter(a,bits)+parameter(b,bits), bits, fn(a,b)&mask
            for op, fn in [('==',lambda a,b:a==b),('!=',lambda a,b:a!=b),('<',lambda a,b:a<b),('<=',lambda a,b:a<=b),('>',lambda a,b:a>b),('>=',lambda a,b:a>=b)]:
                for a,b in values:
                    yield f'{ty}_cmp', f'export fn run(a:{ty},b:{ty})->bool {{ return a {op} b; }}', parameter(a,bits)+parameter(b,bits), 8, int(fn(a,b))
            a,b = (-17 if signed else mask-1),7
            quotient = trunc_div(a, b) if signed else a // b
            for op, result in [('/',quotient),('%',a-quotient*b),('<<',a<<b),('>>',a>>b)]:
                    yield f'{ty}_divshift',f'export fn run(a:{ty},b:{ty})->{ty} {{ return a {op} b; }}',parameter(a,bits)+parameter(b,bits),bits,result&mask

count = 0
with tempfile.TemporaryDirectory(prefix='aster-legacy-') as tmp:
    tmp = Path(tmp); source = tmp/'case.aa'; output = tmp/'case.bin'
    for bits in [32,16]:
        for promote in [True,False]:
            for name, code, parameters, width, expected in cases():
                source.write_text('module legacy.test;\n'+code+'\n')
                command = [str(args.compiler),'build','--target',f'x86_{bits}','--base','0x1000' if bits==16 else '0x100000','-j','1','-o',str(output),str(source)]
                if not promote: command += ['--no-promote']
                proc = subprocess.run(command,capture_output=True,text=True,timeout=30)
                assert proc.returncode == 0, (bits,promote,name,code,proc.stderr)
                got = execute(output.read_bytes(),bits,parameters,width)
                assert got == expected, (bits,promote,name,code,parameters.hex(),got,expected)
                count += 1
        source.write_text('module legacy.object; section ".data" @writable { global x:u32=42; global p:ptr<u32> = &x; } export fn run()->u32 { return x; }')
        for format, magic in [('elf',b'\x7fELF\x01'),('coff',b'\x4c\x01'),('macho',b'\xce\xfa\xed\xfe')]:
            output.unlink(missing_ok=True)
            proc = subprocess.run([str(args.compiler),'build','--target',f'x86_{bits}','--emit','obj','--format',format,'-j','1','-o',str(output),str(source)],capture_output=True,text=True,timeout=30)
            if args.object_writer:
                assert proc.returncode == 0, (format,proc.stderr)
                assert output.read_bytes().startswith(magic), format
            else:
                assert proc.returncode != 0 and not output.exists(), (format,proc.stderr)
                assert 'Standalone C ELF64 fallback' in proc.stderr, proc.stderr
print(f'{count} legacy CPU executions and 6 i386 object-format checks passed (writer={args.object_writer}).')
