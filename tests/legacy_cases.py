"""Shared deterministic legacy x86 semantic shapes."""
TYPES = ['u8','i8','u16','i16','u32','i32','u64','i64']
ARITH = ['+','-','*','&','|','^']
CMP = ['==','!=','<','<=','>','>=']
SHIFT = ['<<','>>']
DIV = ['/','%']


def source_case(index: int) -> str:
    if index < 48:
        ty, op = TYPES[index // len(ARITH)], ARITH[index % len(ARITH)]
        return f'export fn run(a:{ty}, b:{ty})->{ty} {{ return a {op} b; }}'
    index -= 48
    if index < 48:
        ty, op = TYPES[index // len(CMP)], CMP[index % len(CMP)]
        return f'export fn run(a:{ty}, b:{ty})->bool {{ return a {op} b; }}'
    index -= 48
    if index < 16:
        ty, op = TYPES[index // len(SHIFT)], SHIFT[index % len(SHIFT)]
        return f'export fn run(a:{ty}, b:{ty})->{ty} {{ return a {op} b; }}'
    index -= 16
    if index < 16:
        ty, op = TYPES[index // len(DIV)], DIV[index % len(DIV)]
        return f'export fn run(a:{ty}, b:{ty})->{ty} {{ return a {op} b; }}'
    index -= 16
    special = [
        'export fn run()->u64 { return leaf(); } fn leaf()->u64 { return 42; }',
        'export fn run(n:u32)->u32 { let i:u32=0; let s:u32=0; loop { if i==n { break; } s=s+i; i=i+1; } return s; }',
        'export fn run(n:u32)->u32 { let a:u32=1; let b:u32=2; let i:u32=0; loop { if i==n { break; } let t:u32=a; a=b; b=t; i=i+1; } return a*10+b; }',
        'section ".data" @writable { global counter:u32=7; global p:ptr<u32> = &counter; } export fn run(x:u32)->u32 { unsafe { store.u32 [p]=x; return counter+cast<u32>(sizeof(usize)); } }',
        'section ".rodata" @readonly { const a:[u8;60]={1,2,3}; } export fn run()->u32 { unsafe { let p:ptr<const u8> = int_to_ptr(ptr_to_int(&a)+59); return zext<u32>(load.u8 [p]); } }',
        'export fn run()->u16 { return cast<u16>(256*16-1); }',
        'export fn run(c:bool,a:u32,b:u32)->u32 { return c ? a : b; }',
        'export fn run(a:bool)->bool { return !a; }',
        'export fn run()->u64 { return bitcast<u64>(scale(2.5)); } fn scale(x:f64)->f64 { return x*4.0+0.5; }',
        'export fn run()->usize { unsafe { return ptr_to_int(&leaf); } } fn leaf()->u32 { return 7; }',
        'type Triple=struct { a:u32; b:u32; c:u32; }; export fn run(v:Triple)->Triple { return leaf(v); } fn leaf(v:Triple)->Triple { return v; }',
    ]
    if not 0 <= index < len(special):
        raise SystemExit(f'invalid semantic case {index}')
    return special[index]
