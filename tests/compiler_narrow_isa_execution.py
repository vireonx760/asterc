#!/usr/bin/env python3
"""Independent, fail-closed ISA execution oracle for generated AArch64/RV64 BIN.

This decodes *emitted machine instructions*, not MIR, backend pseudo-opcodes or
compiler arithmetic algorithms. It is deliberately bounded and rejects unknown
instructions. It is NOT QEMU/actual target CPU or a complete ISA emulator.
"""
import argparse
import os
import random
import subprocess
import tempfile
from pathlib import Path

MASK = (1 << 64) - 1
TYPES = (('u8', 8, False), ('i8', 8, True), ('u16', 16, False),
         ('i16', 16, True), ('u32', 32, False), ('i32', 32, True),
         ('u64', 64, False), ('i64', 64, True))


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def signed_div(a, b):
    if b == 0:
        return -1
    if a == -(1 << 63) and b == -1:
        return a
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def a64_exec(code, arg, arg2=None, check_abi=False):
    assert len(code) > 0 and len(code) % 4 == 0
    regs = [0] * 32
    # Preserve AAPCS64 x19..x29 under non-zero caller register pressure.
    saved = {r: ((0xa640000000000000 ^ (r * 0x0102040810204081)) & MASK)
             for r in range(19, 30)} if check_abi else {}
    for r, value in saved.items(): regs[r] = value
    regs[0] = arg & MASK
    if arg2 is not None: regs[1] = arg2 & MASK
    sp = 0x100000
    memory = {}
    pc = 0
    for _ in range(5000):
        if pc < 0 or pc + 4 > len(code) or pc & 3:
            raise AssertionError(('A64 out of range', pc, len(code)))
        w = int.from_bytes(code[pc:pc + 4], 'little')
        rd, rn, rm = w & 31, (w >> 5) & 31, (w >> 16) & 31
        width = 64 if (w >> 31) else 32
        wm = (1 << width) - 1
        x = (0 if rn == 31 else regs[rn]) & wm
        y = (0 if rm == 31 else regs[rm]) & wm
        pc += 4
        if w == 0xd65f03c0:
            if sp != 0x100000:
                raise AssertionError('A64 SP restore')
            for r, expected in saved.items():
                if regs[r] != expected:
                    raise AssertionError(('AAPCS64 callee-saved clobber',r,regs[r],expected))
            return regs[0]
        # ADD/SUB immediate (SP uses register encoding 31)
        elif (w & 0x1f000000) == 0x11000000:
            imm = (w >> 10) & 0xfff
            if w & (1 << 22):
                imm <<= 12
            val = (sp if rn == 31 else regs[rn]) & wm
            result = (val - imm if w & 0x40000000 else val + imm) & wm
            if rd == 31:
                sp = result
                continue
        # STR/LDR unsigned immediate (SP permitted)
        elif (w & 0x3b000000) == 0x39000000:
            op_size = 1 << (w >> 30)
            addr = ((sp if rn == 31 else regs[rn]) + (((w >> 10) & 4095) * op_size)) & MASK
            if w & (1 << 22):
                result = 0
                for i in range(op_size):
                    result |= memory.get(addr+i, 0) << (8*i)
            else:
                val = regs[rd]
                for i in range(op_size):
                    memory[addr+i] = (val >> (8*i)) & 255
                continue
        # MOVZ/MOVN/MOVK 32-/64-bit
        elif (w & 0x1f800000) in (0x12800000,0x52800000,0x72800000):
            shift = ((w >> 21) & 3) * 16
            imm = ((w >> 5) & 65535) << shift
            op = (w >> 29) & 3
            if op == 2: result = imm
            elif op == 0: result = ~imm
            elif op == 3: result = (regs[rd] & ~(65535 << shift)) | imm
            else: raise AssertionError(('A64 MOV op',hex(w)))
        # Bitfield SBFM/UBFM (including sxtb/sxth/uxtb/uxth/asr/lsr)
        elif (w & 0x1f800000) in (0x13000000,0x53000000):
            immr = (w >> 16) & (63 if width == 64 else 31)
            imms = (w >> 10) & (63 if width == 64 else 31)
            if imms == width - 1:
                result = (sx(x,width) if (w & 0x7f800000)==0x13000000 else x) >> immr
            elif immr == 0:
                val = x & ((1 << (imms+1))-1)
                result = sx(val, imms+1) if (w & 0x7f800000)==0x13000000 else val
            else:
                raise AssertionError(('A64 bitfield unsupported',hex(w)))
        # SDIV/UDIV
        elif (w & 0x7fe0fc00) in (0x1ac00800,0x1ac00c00):
            signed = (w & 0x7fe0fc00) == 0x1ac00c00
            if y == 0: result = 0
            elif signed:
                aa, bb = sx(x,width), sx(y,width)
                q = (abs(aa)//abs(bb)) * (-1 if (aa<0)!=(bb<0) else 1)
                result = q
            else: result = x // y
        # (M)SUB signed/unsigned, 32- or 64-bit
        elif (w & 0x1fe08000) in (0x1b008000,0x1b000000):
            ra = (w >> 10) & 31
            val = 0 if ra == 31 else regs[ra] & wm
            is_sub = bool(w & (1 << 15))
            result = val - x*y if is_sub else val + x*y
        elif (w & 0x7fe0fc00) == 0x1bc07c00:  # UMULH
            result = (x*y) >> 64
        # Logical register ORR/EOR/AND/ORN
        elif (w & 0x1f000000) == 0x0a000000:
            shift = (w >> 22) & 3
            amount = (w >> 10) & 63
            if shift == 0: value = y << amount
            elif shift == 1: value = y >> amount
            elif shift == 2: value = sx(y,width) >> amount
            else: raise AssertionError(('A64 logical shift',hex(w)))
            if w & 0x00200000: value = ~value
            op = (w >> 29) & 3
            if op == 0: result = x & value
            elif op == 1: result = x | value
            elif op == 2: result = x ^ value
            else: result = x & value
        # ADD/SUB register
        elif (w & 0x1f200000) == 0x0b000000:
            shift = (w >> 22) & 3
            amount = (w >> 10) & 63
            if shift == 0: y <<= amount
            elif shift == 1: y >>= amount
            else: raise AssertionError(('A64 add shifted',hex(w)))
            result = x-y if w & (1<<30) else x+y
        else:
            raise AssertionError(('A64 unsupported opcode', hex(w), 'pc',pc-4))
        if rd != 31:
            regs[rd] = result & wm
    raise AssertionError(('A64 instruction budget exhausted',len(code)))


def rv64_exec(code, arg, with_m, arg2=None, check_abi=False):
    assert code and len(code) % 4 == 0
    regs = [0]*32
    # RISC-V psABI requires s0-s11 (x8/x9/x18..x27) preservation.
    saved = {r: ((0xb640000000000000 ^ (r * 0x0102040810204081)) & MASK)
             for r in (8, 9, *range(18, 28))} if check_abi else {}
    for r, value in saved.items(): regs[r] = value
    regs[10] = arg & MASK
    if arg2 is not None: regs[11] = arg2 & MASK
    regs[2] = 0x100000
    mem = {}
    pc = 0
    for _ in range(100000):
        if pc < 0 or pc+4 > len(code) or pc & 3:
            raise AssertionError(('RV64 PC out of range',pc,len(code)))
        w = int.from_bytes(code[pc:pc+4], 'little')
        opcode, rd, f3, rs1, rs2, f7 = w&127,(w>>7)&31,(w>>12)&7,(w>>15)&31,(w>>20)&31,w>>25
        a,b = regs[rs1],regs[rs2]
        nxt = pc+4
        if w == 0x00008067:
            if regs[2] != 0x100000:
                raise AssertionError('RV64 SP restore')
            for r, expected in saved.items():
                if regs[r] != expected:
                    raise AssertionError(('RV64 psABI callee-saved clobber',r,regs[r],expected))
            return regs[10]
        if opcode == 0x37: result = sx(w & 0xfffff000,32)   # LUI
        elif opcode == 0x17: result = pc + sx(w & 0xfffff000,32)  # AUIPC
        elif opcode in (0x13,0x1b):  # immediate integer/word
            imm = sx(w >> 20,12)
            shift = (w >> 20)&(31 if opcode==0x1b else 63)
            if f3 == 0: result = a+imm
            elif f3 == 7: result = a&imm
            elif f3 == 6: result = a|imm
            elif f3 == 4: result = a^imm
            elif f3 == 2: result = int(sx(a,64)<imm)
            elif f3 == 3: result = int(a < (imm&MASK))
            elif f3 == 1 and (w >> 26)==0: result = a << shift
            elif f3 == 5:
                arith = bool((w >> 30)&1)
                if (w >> 26) & 0xf not in (0,0x10 >> 2):
                    raise AssertionError(('RV shift enc',hex(w)))
                result = (sx(a,32 if opcode==0x1b else 64) >> shift) if arith else ((a & (0xffffffff if opcode==0x1b else MASK)) >> shift)
            else: raise AssertionError(('RV immediate unsupported',hex(w)))
            if opcode == 0x1b: result = sx(result,32)
        elif opcode in (0x33,0x3b):
            if f7 == 1:
                if not with_m: raise AssertionError(('RV64I emitted M opcode',hex(w)))
                bits = 32 if opcode == 0x3b else 64
                aa,bb = a&((1<<bits)-1),b&((1<<bits)-1)
                asa,bsb = sx(aa,bits),sx(bb,bits)
                if f3 == 0: result = aa*bb
                elif f3 == 1: result = (asa*bsb)>>64
                elif f3 == 2: result = (asa*bb)>>64
                elif f3 == 3: result = (aa*bb)>>64
                elif f3 in (4,5,6,7):
                    signed = f3 in (4,6)
                    aa,bb = (asa,bsb) if signed else (aa,bb)
                    if not bb: q = -1 if signed else (1<<bits)-1
                    elif signed and aa == -(1<<(bits-1)) and bb == -1: q=aa
                    elif signed: q = (abs(aa)//abs(bb))*(-1 if (aa<0)!=(bb<0) else 1)
                    else: q=aa//bb
                    result = q if f3 in (4,5) else (aa-q*bb if bb else aa)
                else: raise AssertionError(('RV64M unrecognized',hex(w)))
            elif f7 == 0:
                if f3 == 0: result = a+b
                elif f3 == 7: result = a&b
                elif f3 == 6: result = a|b
                elif f3 == 4: result = a^b
                elif f3 == 2: result = int(sx(a,64)<sx(b,64))
                elif f3 == 3: result = int(a<b)
                elif f3 == 1: result = a << (b & (31 if opcode==0x3b else 63))
                elif f3 == 5: result = a >> (b & (31 if opcode==0x3b else 63))
                else: raise AssertionError(('RV register',hex(w)))
            elif f7 == 0x20:
                if f3 == 0: result = a-b
                elif f3 == 5: result = sx(a,32 if opcode==0x3b else 64) >> (b & (31 if opcode==0x3b else 63))
                else: raise AssertionError(('RV subtraction',hex(w)))
            else: raise AssertionError(('RV register extension',hex(w)))
            if opcode == 0x3b: result = sx(result,32)
        elif opcode == 0x03:   # loads
            imm = sx(w >> 20,12)
            address = (a+imm)&MASK
            size = (1 << (f3&3)) if f3 in (0,1,2,3,4,5,6) else 0
            if size == 0: raise AssertionError(('RV load size',hex(w)))
            value = sum(mem.get(address+i,0)<<(8*i) for i in range(size))
            result = value if f3 >= 4 else sx(value,size*8)
        elif opcode == 0x23:   # stores
            imm = sx(((w>>25)<<5)|((w>>7)&31),12)
            address = (a+imm)&MASK
            size = 1 << f3
            if size not in (1,2,4,8): raise AssertionError(('RV store size',hex(w)))
            for i in range(size): mem[address+i] = (b>>(8*i)) & 255
            result = 0
        elif opcode == 0x63: # branches
            imm = sx((((w>>31)&1)<<12)|(((w>>7)&1)<<11)|(((w>>25)&63)<<5)|(((w>>8)&15)<<1),13)
            cond = {0:lambda:a==b,1:lambda:a!=b,4:lambda:sx(a,64)<sx(b,64),
                    5:lambda:sx(a,64)>=sx(b,64),6:lambda:a<b,7:lambda:a>=b}.get(f3)
            if cond is None: raise AssertionError(('RV branch',hex(w)))
            if cond(): nxt=pc+imm
            result=0
        elif opcode == 0x6f:  # JAL
            imm=sx((((w>>31)&1)<<20)|(((w>>12)&255)<<12)|(((w>>20)&1)<<11)|(((w>>21)&1023)<<1),21)
            result=pc+4
            nxt=pc+imm
        elif opcode == 0x67:  # JALR
            result=pc+4
            nxt=(a+sx(w>>20,12)) & ~1
        else: raise AssertionError(('RV64 unsupported opcode',hex(w),'pc',pc))
        if opcode != 0x23 and opcode != 0x63 and rd: regs[rd]=result&MASK
        regs[0]=0
        pc=nxt
    raise AssertionError(('RV64 instruction budget exhausted',len(code)))


def oracle(x,d,bits,signed,op):
    mask=(1<<bits)-1
    x &= mask
    if signed:
        x=sx(x,bits)
        a=abs(x)//abs(d)
        q=-a if (x<0)!=(d<0) else a
    else:q=x//d
    ret = (q if op=='/' else x-q*d) & mask
    return sx(ret,bits) if signed else ret


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--compiler',required=True)
    ap.add_argument('--quick',action='store_true')
    a=ap.parse_args()
    count=0
    configs=(('aarch64',False),('riscv64',True),('riscv64',False))
    rng=random.Random(0xA5249)
    with tempfile.TemporaryDirectory(prefix='aster-division-real-isa-') as td:
        src=Path(td)/'f.aa'; dest=Path(td)/'f.bin'
        for arch,m in configs:
            for typ,bits,signed in TYPES:
                low=-(1<<(bits-1)) if signed else 0
                high=(1<<(bits-1))-1 if signed else (1<<bits)-1
                divisors=([-7,-3,3,7,8] if signed else [3,7,8,31])
                if a.quick: divisors=divisors[:2]
                for d in divisors:
                    for op in ('/','%'):
                        if not low<=d<=high:continue
                        src.write_text(f'module division.isa; export fn f(x:{typ})->{typ} @abi(c) '
                                       f'{{ return x {op} {d}; }}\n',encoding='utf8')
                        cmd=[a.compiler,'build','--target',arch,'--emit','bin','-j','1',
                             '-o',str(dest),str(src)]
                        if m:cmd+=['--feature','m']
                        for policy in ('hot','size') if not a.quick else ('hot',):
                            env=os.environ.copy();env['ASTERASM_DIV_POLICY']=policy
                            p=subprocess.run(cmd,text=True,capture_output=True,timeout=30,env=env)
                            if p.returncode or not dest.is_file():
                                raise AssertionError(('compile failed',arch,m,typ,d,op,p.stderr))
                            code=dest.read_bytes()
                            if arch=='riscv64' and not m:
                                if any((w&127) in (0x33,0x3b) and (w>>25)==1
                                       for w in (int.from_bytes(code[i:i+4],'little') for i in range(0,len(code),4))):
                                    raise AssertionError('RV64I M instruction detected')
                            values=[low,low+1,-257,-128,-7,-1,0,1,2,7,127,257,high-1,high]
                            values=[v for v in values if low<=v<=high]
                            values += [rng.randint(low,high) for _ in range(5 if a.quick else 24)]
                            for v in values:
                                if signed and v==low and d==-1:continue
                                actual=a64_exec(code,v,check_abi=True) if arch=='aarch64' else rv64_exec(code,v,m,check_abi=True)
                                mask=(1<<bits)-1
                                actual=sx(actual,bits) if signed else actual&mask
                                expect=oracle(v,d,bits,signed,op)
                                if actual!=expect:
                                    raise AssertionError(('MISMATCH',arch,m,typ,v,d,op,policy,actual,expect,code.hex()))
                                count+=1
        print('PASS: independently decoded & executed',count,'AArch64 / RV64M / RV64I BIN calls')

if __name__=='__main__':main()
