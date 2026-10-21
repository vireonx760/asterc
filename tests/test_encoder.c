#include "encoder_internal.h"
#include "regalloc_internal.h"
#include "test_process.h"
#include <stdio.h>

static AstTargetConfig target;
static int failures, dump;
static unsigned cases;
static void require(int ok, const char *what) {
  if (!ok) {
    fprintf(stderr, "encoder: %s\n", what);
    ++failures;
  }
}
static AstPhysicalOperand reg(const char *name, unsigned bits,
                              AstMachineRegBank bank) {
  AstPhysicalOperand o = {0};
  MachinePhysRegView view;
  o.kind = AST_PHYSICAL_OPERAND_REGISTER;
  o.bank = bank;
  o.bit_width = bits;
  (void)snprintf(o.register_name, sizeof(o.register_name), "%s", name);
  if (!machine_physreg_view(&target, name, &view)) {
    require(0, name);
    return o;
  }
  o.register_unit = view.alias_unit;
  return o;
}
static AstPhysicalOperand mem(const char *name, unsigned bits, int64_t disp) {
  AstPhysicalOperand o = reg(name, bits, AST_MACHINE_BANK_GPR);
  o.kind = AST_PHYSICAL_OPERAND_MEMORY;
  o.displacement = disp;
  return o;
}
static AstPhysicalOperand imm(uint64_t bits, unsigned width) {
  AstPhysicalOperand o = {0};
  o.kind = AST_PHYSICAL_OPERAND_IMMEDIATE;
  o.bit_width = width;
  o.immediate_bits = bits;
  return o;
}
static AstPhysicalOperand block(unsigned id) {
  AstPhysicalOperand o = {0};
  o.kind = AST_PHYSICAL_OPERAND_BLOCK;
  o.block = id;
  return o;
}
static AstPhysicalOperand symbol(void) {
  AstPhysicalOperand o = {0};
  o.kind = AST_PHYSICAL_OPERAND_SYMBOL;
  o.symbol.text = "external";
  o.symbol.span.length = 8u;
  return o;
}
static AstPhysicalInstructionInfo inst(AstMachineOpcode op, unsigned defs,
                                       const AstPhysicalOperand *o,
                                       unsigned n) {
  AstPhysicalInstructionInfo i = {0};
  i.opcode = op;
  i.result_count = defs;
  i.operand_count = n;
  if (n)
    memcpy(i.operands, o, n * sizeof(*o));
  return i;
}
static void report(const char *assembly, const AstEncoderOutput *out) {
  if (!dump)
    return;
  printf("%s\t%s\t", ast_target_arch_name(target.arch), assembly);
  for (size_t n = 0; n < out->size; ++n)
    printf("%02x", out->bytes[n]);
  putchar('\n');
}
static void one(AstMachineOpcode op, unsigned defs, const char *assembly,
                const AstPhysicalOperand *o, unsigned n) {
  AstPhysicalInstructionInfo i = inst(op, defs, o, n);
  AstPhysicalBlockInfo bb = {0, 1, &i};
  AstEncoderOutput out;
  ++cases;
  int ok = encoder_blocks(target, &bb, 1u, 0u, &out);
  if (!ok)
    fprintf(stderr, "opcode %u (%s): %s\n", (unsigned)op, assembly,
            out.message);
  require(ok && out.size && !out.fixup_count, assembly);
  if (ok)
    report(assembly, &out);
  ast_encoder_output_destroy(&out);
}
#define G(n, w) reg(n, w, AST_MACHINE_BANK_GPR)
#define F(n, w) reg(n, w, AST_MACHINE_BANK_FP)
#define V(n, w) reg(n, w, AST_MACHINE_BANK_VECTOR)
#define ONE(op, d, asmtext, ...)                                               \
  do {                                                                         \
    const AstPhysicalOperand operands[] = {__VA_ARGS__};                       \
    one(AST_MACHINE_##op, d, asmtext, operands,                                \
        (unsigned)(sizeof(operands) / sizeof(operands[0])));                   \
  } while (0)
#define ZERO(op, asmtext) one(AST_MACHINE_##op, 0u, asmtext, NULL, 0u)

static void x86_cases(void) {
  const uint64_t compact_values[] = {0u,
                                     UINT32_MAX,
                                     UINT64_C(0x100000000),
                                     (uint64_t)INT32_MIN,
                                     UINT64_MAX,
                                     (uint64_t)INT32_MIN - 1u};
  for (unsigned r = 0; r < 2u; ++r)
    for (unsigned v = 0; v < 6u; ++v) {
      uint64_t value = compact_values[v];
      char assembly[96];
      const char *wide = r ? "r9" : "rax", *narrow = r ? "r9d" : "eax";
      if (value <= UINT32_MAX)
        snprintf(assembly, sizeof(assembly), "mov %s, 0x%llx", narrow,
                 (unsigned long long)value);
      else if (value == (uint64_t)(int64_t)(int32_t)(uint32_t)value)
        snprintf(assembly, sizeof(assembly), "mov %s, %lld", wide,
                 (long long)(int64_t)value);
      else
        snprintf(assembly, sizeof(assembly), "movabs %s, 0x%llx", wide,
                 (unsigned long long)value);
      ONE(X86_MOV_IMM, 1, assembly, G(wide, 64), imm(value, 64));
    }
  ONE(X86_FMOV_BITS, 1, "movd xmm9, r15d", F("xmm9", 32), G("r15d", 32));
  ONE(X86_FMOV_BITS, 1, "movq r9, xmm15", G("r9", 64), F("xmm15", 64));
  ONE(X86_SINT_TO_FP, 1, "cvtsi2sd xmm9, r15", F("xmm9", 64), G("r15", 64));
  ONE(X86_SINT_TO_FP, 1, "cvtsi2ss xmm9, r15d", F("xmm9", 32), G("r15d", 32));
  ONE(X86_FP_TO_SINT, 1, "cvttsd2si r9, xmm15", G("r9", 64), F("xmm15", 64));
  ONE(X86_FP_TO_SINT, 1, "cvttss2si r9d, xmm15", G("r9d", 32), F("xmm15", 32));
  ONE(X86_FP_CONVERT, 1, "cvtss2sd xmm9, xmm15", F("xmm9", 64), F("xmm15", 32));
  ONE(X86_FP_CONVERT, 1, "cvtsd2ss xmm9, xmm15", F("xmm9", 32), F("xmm15", 64));
  ONE(X86_UINT64_TO_FP, 2,
      "test r9, r9; js .Lhigh$; cvtsi2sd xmm15, r9; jmp .Ldone$; .Lhigh$: mov "
      "r11, r9; shr r11, 1; jnc .Leven$; or r11, 1; .Leven$: cvtsi2sd xmm15, "
      "r11; addsd xmm15, xmm15; .Ldone$:",
      F("xmm15", 64), G("r11", 64), G("r9", 64));
  ONE(X86_UINT64_TO_FP, 2,
      "test r9, r9; js .Lhigh$; cvtsi2ss xmm15, r9; jmp .Ldone$; .Lhigh$: mov "
      "r11, r9; shr r11, 1; jnc .Leven$; or r11, 1; .Leven$: cvtsi2ss xmm15, "
      "r11; addss xmm15, xmm15; .Ldone$:",
      F("xmm15", 32), G("r11", 64), G("r9", 64));
  ONE(X86_FP_TO_UINT64, 3,
      "movabs r11, 0x43e0000000000000; movq xmm9, r11; ucomisd xmm15, xmm9; jb "
      ".Llow$; subsd xmm9, xmm15; cvttsd2si r9, xmm9; neg r9; bts r9, 63; jmp "
      ".Ldone$; .Llow$: cvttsd2si r9, xmm15; .Ldone$:",
      G("r9", 64), G("r11", 64), F("xmm9", 64), F("xmm15", 64));
  ONE(X86_FP_TO_UINT64, 3,
      "mov r11d, 0x5f000000; movd xmm9, r11d; ucomiss xmm15, xmm9; jb .Llow$; "
      "subss xmm9, xmm15; cvttss2si r9, xmm9; neg r9; bts r9, 63; jmp .Ldone$; "
      ".Llow$: cvttss2si r9, xmm15; .Ldone$:",
      G("r9", 64), G("r11", 64), F("xmm9", 32), F("xmm15", 32));
  ZERO(X86_NOP, "nop");
  ZERO(X86_RET, "ret");
  ZERO(X86_TRAP, "ud2");
  ZERO(X86_FENCE, "mfence");
  const char *names[][4] = {{"al", "ax", "eax", "rax"},
                            {"spl", "sp", "esp", "rsp"},
                            {"bpl", "bp", "ebp", "rbp"},
                            {"r9b", "r9w", "r9d", "r9"},
                            {"r15b", "r15w", "r15d", "r15"}};
  const char *src[] = {"r8b", "r8w", "r8d", "r8"};
  const unsigned widths[] = {8, 16, 32, 64};
  const char *sizes[] = {"byte", "word", "dword", "qword"};
  const char *bases[] = {"rax", "rsp", "rbp", "r12", "r13"};
  const int disps[] = {0, 127, 128, -128, -129, 2147483647};
  char s[256];
  for (unsigned w = 0; w < 4; ++w)
    for (unsigned n = 0; n < 5; ++n) {
      snprintf(s, sizeof(s), "mov %s, %s", names[n][w], src[w]);
      ONE(X86_MOV, 1, s, G(names[n][w], widths[w]), G(src[w], widths[w]));
      snprintf(s, sizeof(s), "mov %s, 4660", names[n][w == 3 ? 2 : w]);
      if (!w)
        snprintf(s, sizeof(s), "mov %s, 52", names[n][w]);
      ONE(X86_MOV_IMM, 1, s, G(names[n][w], widths[w]),
          imm(w ? 4660u : 52u, widths[w]));
      for (unsigned d = 0; d < 6; ++d) {
        snprintf(s, sizeof(s), "mov %s, %s ptr [%s%+d]", names[n][w], sizes[w],
                 bases[n], disps[d]);
        ONE(X86_LOAD, 1, s, G(names[n][w], widths[w]),
            mem(bases[n], widths[w], disps[d]));
        snprintf(s, sizeof(s), "mov %s ptr [%s%+d], %s", sizes[w], bases[n],
                 disps[d], names[n][w]);
        ONE(X86_STORE, 0, s, G(names[n][w], widths[w]),
            mem(bases[n], widths[w], disps[d]));
      }
    }
  ONE(X86_LEA, 1, "lea r9, [r12-129]", G("r9", 64), mem("r12", 64, -129));
  ONE(X86_ADD, 1, "add r9, r15", G("r9", 64), G("r9", 64), G("r15", 64));
  ONE(X86_ADD, 1, "add r9, -128", G("r9", 64), G("r9", 64),
      imm((uint64_t)-128, 64));
  ONE(X86_SUB, 1, "sub r9, 128", G("r9", 64), G("r9", 64), imm(128, 64));
  ONE(X86_AND, 1, "and r9w, -1", G("r9w", 16), G("r9w", 16), imm(65535, 16));
  ONE(X86_OR, 1, "or r9d, r15d", G("r9d", 32), G("r9d", 32), G("r15d", 32));
  ONE(X86_XOR, 1, "xor r9b, spl", G("r9b", 8), G("r9b", 8), G("spl", 8));
  ONE(X86_IN, 1, "in al, dx", G("al", 8), G("dx", 16));
  ONE(X86_IN, 1, "in ax, dx", G("ax", 16), G("dx", 16));
  ONE(X86_IN, 1, "in eax, dx", G("eax", 32), G("dx", 16));
  ONE(X86_IN, 1, "in al, 255", G("al", 8), imm(255, 8));
  ONE(X86_IN, 1, "in ax, 128", G("ax", 16), imm(128, 8));
  ONE(X86_IN, 1, "in eax, 0", G("eax", 32), imm(0, 8));
  ONE(X86_OUT, 0, "out dx, al", G("al", 8), G("dx", 16));
  ONE(X86_OUT, 0, "out dx, ax", G("ax", 16), G("dx", 16));
  ONE(X86_OUT, 0, "out dx, eax", G("eax", 32), G("dx", 16));
  ONE(X86_OUT, 0, "out 233, al", G("al", 8), imm(233, 8));
  ONE(X86_OUT, 0, "out 128, ax", G("ax", 16), imm(128, 8));
  ONE(X86_OUT, 0, "out 0, eax", G("eax", 32), imm(0, 8));
  ONE(X86_LIDT, 0, "lidt [r12]", G("r12", 64));
  ONE(X86_LGDT, 0, "lgdt [rbp]", G("rbp", 64));
  ONE(X86_MUL, 1, "imul r9, r15", G("r9", 64), G("r9", 64), G("r15", 64));
  ONE(X86_MUL, 1, "imul r9, r9, 7", G("r9", 64), G("r9", 64), imm(7, 64));
  ONE(X86_MUL, 1, "imul r9d, r9d, 4660", G("r9d", 32), G("r9d", 32),
      imm(4660, 32));
  ONE(X86_NEG, 1, "neg r9w", G("r9w", 16), G("r9w", 16));
  ONE(X86_NOT, 1, "not spl", G("spl", 8), G("spl", 8));
  ONE(X86_SHL, 1, "shl r9, cl", G("r9", 64), G("r9", 64), G("cl", 8));
  ONE(X86_SHL, 1, "shl r9, 5", G("r9", 64), G("r9", 64), imm(5, 64));
  ONE(X86_ASHR, 1, "sar r9b, cl", G("r9b", 8), G("r9b", 8), G("cl", 8));
  ONE(X86_ASHR, 1, "sar r9w, 3", G("r9w", 16), G("r9w", 16), imm(3, 16));
  ONE(X86_LSHR, 1, "shr r9d, cl", G("r9d", 32), G("r9d", 32), G("cl", 8));
  ONE(X86_LSHR, 1, "shr r9d, 7", G("r9d", 32), G("r9d", 32), imm(7, 32));
  ONE(X86_CDQ, 1, "cdq", G("edx", 32), G("eax", 32));
  ONE(X86_CQO, 1, "cqo", G("rdx", 64), G("rax", 64));
  ONE(X86_SDIV, 2, "idiv r9", G("rax", 64), G("rdx", 64), G("rax", 64),
      G("rdx", 64), G("r9", 64));
  ONE(X86_UDIV, 2, "div r9d", G("eax", 32), G("edx", 32), G("eax", 32),
      G("edx", 32), G("r9d", 32));
  ONE(X86_MOVSX, 1, "movsx r9, spl", G("r9", 64), G("spl", 8));
  ONE(X86_MOVSX, 1, "movsxd r9, r15d", G("r9", 64), G("r15d", 32));
  ONE(X86_MOVZX, 1, "movzx r9d, r15w", G("r9d", 32), G("r15w", 16));
  ONE(X86_MOVZX, 1, "mov r9d, r15d", G("r9", 64), G("r15d", 32));
  ONE(X86_ICMP_EQ, 1, "cmp r9, r15; sete r8b", G("r8b", 1), G("r9", 64),
      G("r15", 64));
  ONE(X86_ICMP_LT, 1, "cmp r9d, r15d; setb spl", G("spl", 1), G("r9d", 32),
      G("r15d", 32));
  ONE(X86_ICMP_LT, 1, "cmp r9, 25; setb spl", G("spl", 1), G("r9", 64),
      imm(25, 64));
  ONE(X86_FCMP_EQ, 1,
      "ucomisd xmm9, xmm15; setnp r8b; jp .Ldone$; sete r8b; .Ldone$:",
      G("r8b", 1), F("xmm9", 64), F("xmm15", 64));
  ONE(X86_FCMP_NE, 1,
      "ucomiss xmm9, xmm15; setp spl; jp .Ldone$; setne spl; .Ldone$:",
      G("spl", 1), F("xmm9", 32), F("xmm15", 32));
  ONE(X86_FCMP_LT, 1,
      "ucomisd xmm9, xmm15; setnp al; jp .Ldone$; setb al; .Ldone$:",
      G("al", 1), F("xmm9", 64), F("xmm15", 64));
  ONE(X86_FCMP_LE, 1,
      "ucomiss xmm9, xmm15; setnp r15b; jp .Ldone$; setbe r15b; .Ldone$:",
      G("r15b", 1), F("xmm9", 32), F("xmm15", 32));
  ONE(X86_FCMP_GT, 1, "ucomisd xmm9, xmm15; seta r8b", G("r8b", 1),
      F("xmm9", 64), F("xmm15", 64));
  ONE(X86_FCMP_GE, 1, "ucomiss xmm9, xmm15; setae r8b", G("r8b", 1),
      F("xmm9", 32), F("xmm15", 32));
  ONE(X86_SELECT, 1, "test r8b, r8b; mov r9, r8; cmovnz r9, r15", G("r9", 64),
      G("r8b", 1), G("r15", 64), G("r8", 64));
  ONE(X86_SELECT, 1, "test al, al; cmovz rax, r8", G("rax", 64), G("al", 1),
      G("rax", 64), G("r8", 64));
  ONE(X86_SELECT, 1, "test al, al; cmovnz eax, r8d", G("eax", 32), G("al", 1),
      G("r8d", 32), G("eax", 32));
  ONE(X86_SELECT, 1, "test r8b, r8b; mov ax, r9w; cmovnz ax, r10w", G("ax", 16),
      G("r8b", 1), G("r10w", 16), G("r9w", 16));
  ONE(X86_SELECT, 1,
      "cmp byte ptr [r12+8], 0; jz .Lelse$; movss xmm9, xmm15; jmp .Ldone$; "
      ".Lelse$: movss xmm9, xmm8; .Ldone$:",
      F("xmm9", 32), mem("r12", 1, 8), F("xmm15", 32), F("xmm8", 32));
  ONE(X86_MOV, 1, "movss xmm9, xmm15", F("xmm9", 32), F("xmm15", 32));
  ONE(X86_MOV, 1, "movsd xmm9, xmm15", F("xmm9", 64), F("xmm15", 64));
  ONE(X86_MOV, 1, "movdqu xmm9, xmm15", V("xmm9", 128), V("xmm15", 128));
  ONE(X86_LOAD, 1, "movsd xmm9, qword ptr [r12+128]", F("xmm9", 64),
      mem("r12", 64, 128));
  ONE(X86_STORE, 0, "movdqu xmmword ptr [r13-128], xmm15", V("xmm15", 128),
      mem("r13", 128, -128));
  ONE(X86_FADD, 1, "addss xmm9, xmm15", F("xmm9", 32), F("xmm9", 32),
      F("xmm15", 32));
  ONE(X86_FSUB, 1, "subsd xmm9, xmm15", F("xmm9", 64), F("xmm9", 64),
      F("xmm15", 64));
  ONE(X86_FMUL, 1, "mulss xmm9, xmm15", F("xmm9", 32), F("xmm9", 32),
      F("xmm15", 32));
  ONE(X86_FDIV, 1, "divsd xmm9, xmm15", F("xmm9", 64), F("xmm9", 64),
      F("xmm15", 64));
  ONE(X86_POPCNT, 1, "popcnt r9, r15", G("r9", 64), G("r15", 64));
  ONE(X86_CALL_INDIRECT, 0, "call r9", G("r9", 64));
}
static void a64_cases(void) {
  ONE(A64_STORE, 0, "str q31, [sp, #32]", F("q31", 128), mem("sp", 128, 32));
  ONE(A64_LOAD, 1, "ldr q31, [sp, #32]", F("q31", 128), mem("sp", 128, 32));
  ZERO(A64_RET, "ret");
  ZERO(A64_TRAP, "brk #0");
  ZERO(A64_FENCE, "dmb ish");
  char s[256], a[8], b[8], c[8];
  for (unsigned w = 32; w <= 64; w *= 2u) {
    snprintf(a, sizeof(a), "%c9", w == 64 ? 'x' : 'w');
    snprintf(b, sizeof(b), "%c15", w == 64 ? 'x' : 'w');
    snprintf(c, sizeof(c), "%c30", w == 64 ? 'x' : 'w');
    AstMachineOpcode ops[] = {
        AST_MACHINE_A64_ADD,  AST_MACHINE_A64_SUB,  AST_MACHINE_A64_MUL,
        AST_MACHINE_A64_AND,  AST_MACHINE_A64_OR,   AST_MACHINE_A64_XOR,
        AST_MACHINE_A64_SHL,  AST_MACHINE_A64_LSHR, AST_MACHINE_A64_ASHR,
        AST_MACHINE_A64_SDIV, AST_MACHINE_A64_UDIV};
    const char *mn[] = {"add", "sub", "mul", "and",  "orr", "eor",
                        "lsl", "lsr", "asr", "sdiv", "udiv"};
    for (unsigned j = 0; j < sizeof(ops) / sizeof(ops[0]); ++j) {
      snprintf(s, sizeof(s), "%s %s, %s, %s", mn[j], a, b, c);
      AstPhysicalOperand o[] = {G(a, w), G(b, w), G(c, w)};
      one(ops[j], 1, s, o, 3);
    }
    snprintf(s, sizeof(s), "mov %s, %s", a, b);
    ONE(A64_MOV, 1, s, G(a, w), G(b, w));
    snprintf(s, sizeof(s), "neg %s, %s", a, b);
    ONE(A64_NEG, 1, s, G(a, w), G(b, w));
    snprintf(s, sizeof(s), "mvn %s, %s", a, b);
    ONE(A64_NOT, 1, s, G(a, w), G(b, w));
    snprintf(s, sizeof(s), "msub %s, %s, %s, %s", a, b, c, a);
    ONE(A64_MSUB, 1, s, G(a, w), G(b, w), G(c, w), G(a, w));
    snprintf(s, sizeof(s), "cmp %s, %s", a, b);
    ONE(A64_CMP, 0, s, G(a, w), G(b, w));
    snprintf(s, sizeof(s), "cmp %s, #0", a);
    ONE(A64_CMP_ZERO, 0, s, G(a, w));
    snprintf(s, sizeof(s), "sbfm %s, %s, #0, #7", a, b);
    ONE(A64_SBFM, 1, s, G(a, w), G(b, w), imm(0, 32), imm(7, 32));
    snprintf(s, sizeof(s), "ubfm %s, %s, #0, #15", a, b);
    ONE(A64_UBFM, 1, s, G(a, w), G(b, w), imm(0, 32), imm(15, 32));
    for (unsigned sh = 0; sh < w; sh += 16) {
      snprintf(s, sizeof(s), "movz %s, #65535, lsl #%u", a, sh);
      ONE(A64_MOVZ, 1, s, G(a, w), imm(65535, 16), imm(sh, 32));
      snprintf(s, sizeof(s), "movk %s, #32768, lsl #%u", a, sh);
      ONE(A64_MOVK, 1, s, G(a, w), imm(32768, 16), imm(sh, 32));
    }
    for (unsigned cc = 0; cc < 14; ++cc) {
      static const char *conditions[] = {"eq", "ne", "hs", "lo", "mi",
                                         "pl", "vs", "vc", "hi", "ls",
                                         "ge", "lt", "gt", "le"};
      snprintf(s, sizeof(s), "csel %s, %s, %s, %s", a, b, c, conditions[cc]);
      ONE(A64_CSEL, 1, s, G(a, w), G(b, w), G(c, w), imm(cc, 32));
      if (w == 32) {
        snprintf(s, sizeof(s), "cset %s, %s", a, conditions[cc]);
        ONE(A64_CSET, 1, s, G(a, w), imm(cc, 32));
      }
    }
  }
  ONE(A64_SUB, 1, "sub sp, sp, #4096", G("sp", 64), G("sp", 64), imm(4096, 64));
  ONE(A64_ADD, 1, "add sp, sp, #4080", G("sp", 64), G("sp", 64), imm(4080, 64));
  ONE(A64_LEA, 1, "sub x9, sp, #128", G("x9", 64), mem("sp", 64, -128));
  const unsigned widths[] = {8, 16, 32, 64, 32, 64, 128};
  const char *names[] = {"w9", "w9", "w9", "x9", "s9", "d9", "q9"};
  const char *mn_load[] = {"ldrb", "ldrh", "ldr", "ldr", "ldr", "ldr", "ldr"};
  const char *mn_store[] = {"strb", "strh", "str", "str", "str", "str", "str"};
  const char *mn_uload[] = {"ldurb", "ldurh", "ldur", "ldur",
                            "ldur",  "ldur",  "ldur"};
  const char *mn_ustore[] = {"sturb", "sturh", "stur", "stur",
                             "stur",  "stur",  "stur"};
  for (unsigned j = 0; j < 7; ++j)
    for (unsigned k = 0; k < 4; ++k) {
      int d = k == 0   ? 0
              : k == 1 ? (int)(4095u * (widths[j] / 8u))
              : k == 2 ? -256
                       : 255;
      /* AArch64 byte/halfword loads and stores still name a W register;
         the memory operand carries the 8/16-bit access width. */
      AstPhysicalOperand r = j < 4   ? G(names[j], widths[j] <= 32u ? 32u : 64u)
                             : j < 6 ? F(names[j], widths[j])
                                     : V(names[j], widths[j]);
      int scaled = d >= 0 && d % (int)(widths[j] / 8u) == 0;
      snprintf(s, sizeof(s), "%s %s, [sp, #%d]",
               scaled ? mn_load[j] : mn_uload[j], names[j], d);
      ONE(A64_LOAD, 1, s, r, mem("sp", widths[j], d));
      snprintf(s, sizeof(s), "%s %s, [sp, #%d]",
               scaled ? mn_store[j] : mn_ustore[j], names[j], d);
      ONE(A64_STORE, 0, s, r, mem("sp", widths[j], d));
    }
  for (unsigned w = 32; w <= 64; w *= 2u) {
    snprintf(a, sizeof(a), "%c9", w == 32 ? 's' : 'd');
    snprintf(b, sizeof(b), "%c15", w == 32 ? 's' : 'd');
    snprintf(c, sizeof(c), "%c30", w == 32 ? 's' : 'd');
    AstMachineOpcode ops[] = {AST_MACHINE_A64_FADD, AST_MACHINE_A64_FSUB,
                              AST_MACHINE_A64_FMUL, AST_MACHINE_A64_FDIV};
    const char *mn[] = {"fadd", "fsub", "fmul", "fdiv"};
    for (unsigned j = 0; j < 4; ++j) {
      snprintf(s, sizeof(s), "%s %s, %s, %s", mn[j], a, b, c);
      AstPhysicalOperand o[] = {F(a, w), F(b, w), F(c, w)};
      one(ops[j], 1, s, o, 3);
    }
    snprintf(s, sizeof(s), "fmov %s, %s", a, b);
    ONE(A64_MOV, 1, s, F(a, w), F(b, w));
    snprintf(s, sizeof(s), "fneg %s, %s", a, b);
    ONE(A64_FNEG, 1, s, F(a, w), F(b, w));
    snprintf(s, sizeof(s), "fcmp %s, %s", a, b);
    ONE(A64_FCMP, 0, s, F(a, w), F(b, w));
    snprintf(s, sizeof(s), "fcsel %s, %s, %s, ne", a, b, c);
    ONE(A64_FCSEL, 1, s, F(a, w), F(b, w), F(c, w), imm(1, 32));
    snprintf(c, sizeof(c), "%c30", w == 32 ? 'w' : 'x');
    snprintf(s, sizeof(s), "fmov %s, %s", a, c);
    ONE(A64_FMOV, 1, s, F(a, w), G(c, w));
    snprintf(s, sizeof(s), "fmov %s, %s", c, a);
    ONE(A64_FMOV, 1, s, G(c, w), F(a, w));
    for (unsigned iw = 32; iw <= 64; iw *= 2u) {
      snprintf(c, sizeof(c), "%c30", iw == 32 ? 'w' : 'x');
      snprintf(s, sizeof(s), "scvtf %s, %s", a, c);
      ONE(A64_SCVTF, 1, s, F(a, w), G(c, iw));
      snprintf(s, sizeof(s), "ucvtf %s, %s", a, c);
      ONE(A64_UCVTF, 1, s, F(a, w), G(c, iw));
      snprintf(s, sizeof(s), "fcvtzs %s, %s", c, a);
      ONE(A64_FCVTZS, 1, s, G(c, iw), F(a, w));
      snprintf(s, sizeof(s), "fcvtzu %s, %s", c, a);
      ONE(A64_FCVTZU, 1, s, G(c, iw), F(a, w));
    }
  }
  ONE(A64_FCVT, 1, "fcvt d9, s15", F("d9", 64), F("s15", 32));
  ONE(A64_FCVT, 1, "fcvt s9, d15", F("s9", 32), F("d15", 64));
  ONE(A64_MOV, 1, "mov v9.16b, v15.16b", V("v9", 128), V("v15", 128));
  ONE(A64_VEC_AND, 1, "and v9.8b, v15.8b, v30.8b", V("d9", 64), V("d15", 64),
      V("d30", 64));
  ONE(A64_VEC_OR, 1, "orr v9.16b, v15.16b, v30.16b", V("v9", 128),
      V("v15", 128), V("v30", 128));
  ONE(A64_VEC_XOR, 1, "eor v9.16b, v15.16b, v30.16b", V("v9", 128),
      V("v15", 128), V("v30", 128));
  ONE(A64_CALL_INDIRECT, 0, "blr x9", G("x9", 64));
}
static void rv_cases(void) {
  ZERO(RISCV_RET, "ret");
  ZERO(RISCV_EBREAK, "ebreak");
  ZERO(RISCV_FENCE, "fence rw, rw");
  ONE(RISCV_MV, 1, "mv x9, x31", G("x9", 64), G("x31", 64));
  ONE(RISCV_NEG, 1, "neg x9, x31", G("x9", 64), G("x31", 64));
  ONE(RISCV_NOT, 1, "not x9, x31", G("x9", 64), G("x31", 64));
  AstMachineOpcode ops[] = {
      AST_MACHINE_RISCV_ADD,   AST_MACHINE_RISCV_ADDW,  AST_MACHINE_RISCV_SUB,
      AST_MACHINE_RISCV_SUBW,  AST_MACHINE_RISCV_MUL,   AST_MACHINE_RISCV_MULW,
      AST_MACHINE_RISCV_DIV,   AST_MACHINE_RISCV_DIVW,  AST_MACHINE_RISCV_DIVU,
      AST_MACHINE_RISCV_DIVUW, AST_MACHINE_RISCV_REM,   AST_MACHINE_RISCV_REMW,
      AST_MACHINE_RISCV_REMU,  AST_MACHINE_RISCV_REMUW, AST_MACHINE_RISCV_AND,
      AST_MACHINE_RISCV_OR,    AST_MACHINE_RISCV_XOR,   AST_MACHINE_RISCV_SLL,
      AST_MACHINE_RISCV_SLLW,  AST_MACHINE_RISCV_SRL,   AST_MACHINE_RISCV_SRLW,
      AST_MACHINE_RISCV_SRA,   AST_MACHINE_RISCV_SRAW,  AST_MACHINE_RISCV_SLT,
      AST_MACHINE_RISCV_SLTU};
  const char *mn[] = {"add",  "addw", "sub",   "subw", "mul",  "mulw", "div",
                      "divw", "divu", "divuw", "rem",  "remw", "remu", "remuw",
                      "and",  "or",   "xor",   "sll",  "sllw", "srl",  "srlw",
                      "sra",  "sraw", "slt",   "sltu"};
  char s[256];
  for (unsigned j = 0; j < sizeof(ops) / sizeof(ops[0]); ++j) {
    snprintf(s, sizeof(s), "%s x9, x15, x31", mn[j]);
    AstPhysicalOperand o[] = {G("x9", 64), G("x15", 64), G("x31", 64)};
    one(ops[j], 1, s, o, 3);
  }
  const int vals[] = {-2048, -1, 0, 2047};
  for (unsigned j = 0; j < 4; ++j) {
    snprintf(s, sizeof(s), "addi x9, x15, %d", vals[j]);
    ONE(RISCV_ADDI, 1, s, G("x9", 64), G("x15", 64),
        imm((uint64_t)vals[j], 64));
    snprintf(s, sizeof(s), "addiw x9, x15, %d", vals[j]);
    ONE(RISCV_ADDIW, 1, s, G("x9", 64), G("x15", 64),
        imm((uint64_t)vals[j], 64));
    snprintf(s, sizeof(s), "xori x9, x15, %d", vals[j]);
    ONE(RISCV_XORI, 1, s, G("x9", 64), G("x15", 64),
        imm((uint64_t)vals[j], 64));
    snprintf(s, sizeof(s), "sltiu x9, x15, %d", vals[j]);
    ONE(RISCV_SLTIU, 1, s, G("x9", 64), G("x15", 64),
        imm((uint64_t)vals[j], 64));
  }
  ONE(RISCV_SLLI, 1, "slli x9, x15, 63", G("x9", 64), G("x15", 64),
      imm(63, 64));
  ONE(RISCV_SRLI, 1, "srli x9, x15, 32", G("x9", 64), G("x15", 64),
      imm(32, 64));
  ONE(RISCV_SRAI, 1, "srai x9, x15, 1", G("x9", 64), G("x15", 64), imm(1, 64));
  AstMachineOpcode loads[] = {
      AST_MACHINE_RISCV_LB,  AST_MACHINE_RISCV_LBU, AST_MACHINE_RISCV_LH,
      AST_MACHINE_RISCV_LHU, AST_MACHINE_RISCV_LW,  AST_MACHINE_RISCV_LWU,
      AST_MACHINE_RISCV_LD,  AST_MACHINE_RISCV_FLW, AST_MACHINE_RISCV_FLD};
  const char *loadmn[] = {"lb",  "lbu", "lh",  "lhu", "lw",
                          "lwu", "ld",  "flw", "fld"};
  const unsigned w[] = {8, 8, 16, 16, 32, 32, 64, 32, 64};
  for (unsigned j = 0; j < 9; ++j)
    for (unsigned k = 0; k < 4; ++k) {
      const char *r = j < 7 ? "x9" : "f9";
      snprintf(s, sizeof(s), "%s %s, %d(x31)", loadmn[j], r, vals[k]);
      AstPhysicalOperand o[] = {j < 7 ? G(r, 64) : F(r, w[j]),
                                mem("x31", w[j], vals[k])};
      one(loads[j], 1, s, o, 2);
    }
  ONE(RISCV_SB, 0, "sb x9, -2048(sp)", G("x9", 64), mem("sp", 8, -2048));
  ONE(RISCV_SH, 0, "sh x9, 2047(sp)", G("x9", 64), mem("sp", 16, 2047));
  ONE(RISCV_SW, 0, "sw x9, -1(sp)", G("x9", 64), mem("sp", 32, -1));
  ONE(RISCV_SD, 0, "sd x9, 0(sp)", G("x9", 64), mem("sp", 64, 0));
  ONE(RISCV_FSW, 0, "fsw f9, 2047(sp)", F("f9", 32), mem("sp", 32, 2047));
  ONE(RISCV_FSD, 0, "fsd f9, -2048(sp)", F("f9", 64), mem("sp", 64, -2048));
  for (unsigned j = 0; j < 2; ++j) {
    unsigned bits = j ? 64 : 32;
    const char *fmt = j ? "d" : "s";
    AstMachineOpcode fops[] = {
        j ? AST_MACHINE_RISCV_FADD_D : AST_MACHINE_RISCV_FADD_S,
        j ? AST_MACHINE_RISCV_FSUB_D : AST_MACHINE_RISCV_FSUB_S,
        j ? AST_MACHINE_RISCV_FMUL_D : AST_MACHINE_RISCV_FMUL_S,
        j ? AST_MACHINE_RISCV_FDIV_D : AST_MACHINE_RISCV_FDIV_S};
    const char *fmn[] = {"fadd", "fsub", "fmul", "fdiv"};
    for (unsigned k = 0; k < 4; ++k) {
      snprintf(s, sizeof(s), "%s.%s f9, f15, f31, rne", fmn[k], fmt);
      AstPhysicalOperand o[] = {F("f9", bits), F("f15", bits), F("f31", bits)};
      one(fops[k], 1, s, o, 3);
    }
    snprintf(s, sizeof(s), "fmv.%s f9, f31", fmt);
    AstPhysicalOperand move[] = {F("f9", bits), F("f31", bits)};
    one(j ? AST_MACHINE_RISCV_FMV_D : AST_MACHINE_RISCV_FMV_S, 1, s, move, 2);
    snprintf(s, sizeof(s), "fneg.%s f9, f31", fmt);
    one(j ? AST_MACHINE_RISCV_FNEG_D : AST_MACHINE_RISCV_FNEG_S, 1, s, move, 2);
    const char *cmn[] = {"feq", "flt", "fle"};
    for (unsigned k = 0; k < 3; ++k) {
      snprintf(s, sizeof(s), "%s.%s x9, f15, f31", cmn[k], fmt);
      AstPhysicalOperand o[] = {G("x9", 64), F("f15", bits), F("f31", bits)};
      one((AstMachineOpcode)(AST_MACHINE_RISCV_FEQ_S + k * 2u + j), 1, s, o, 3);
    }
    for (unsigned k = 0; k < 4; ++k) {
      const char *imn[] = {"w", "wu", "l", "lu"};
      unsigned iw = k < 2 ? 32 : 64;
      snprintf(s, sizeof(s), "fcvt.%s.%s f9, x31, rne", fmt, imn[k]);
      AstPhysicalOperand of[] = {F("f9", bits), G("x31", iw), imm(0, 32)};
      one((AstMachineOpcode)(AST_MACHINE_RISCV_FCVT_S_W + j * 8u + k * 2u), 1,
          s, of, 3);
      snprintf(s, sizeof(s), "fcvt.%s.%s x9, f31, rtz", imn[k], fmt);
      AstPhysicalOperand oi[] = {G("x9", iw), F("f31", bits), imm(1, 32)};
      one((AstMachineOpcode)(AST_MACHINE_RISCV_FCVT_S_W + j * 8u + k * 2u + 1u),
          1, s, oi, 3);
    }
  }
  ONE(RISCV_FMV_W_X, 1, "fmv.w.x f9, x31", F("f9", 32), G("x31", 64));
  ONE(RISCV_FMV_X_W, 1, "fmv.x.w x9, f31", G("x9", 64), F("f31", 32));
  ONE(RISCV_FMV_D_X, 1, "fmv.d.x f9, x31", F("f9", 64), G("x31", 64));
  ONE(RISCV_FMV_X_D, 1, "fmv.x.d x9, f31", G("x9", 64), F("f31", 64));
  ONE(RISCV_FCVT_S_D, 1, "fcvt.s.d f9, f31, rne", F("f9", 32), F("f31", 64),
      imm(0, 32));
  ONE(RISCV_FCVT_D_S, 1, "fcvt.d.s f9, f31", F("f9", 64), F("f31", 32),
      imm(0, 32));
  ONE(RISCV_CALL_INDIRECT, 0, "jalr ra, x9, 0", G("x9", 64));
}

static uint32_t read32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
         ((uint32_t)p[3] << 24u);
}
static int resolve_test(void *user, AstName name, uint64_t *address) {
  if (!user || name.span.length != 8u || memcmp(name.text, "external", 8u))
    return 0;
  *address = *(const uint64_t *)user;
  return 1;
}
static int resolve_qualified_test(void *user, AstName module, AstName name,
                                  uint64_t *address) {
  if (!user || module.span.length != 7u || memcmp(module.text, "dep.mod", 7u) ||
      name.span.length != 8u || memcmp(name.text, "external", 8u))
    return 0;
  *address = *(const uint64_t *)user;
  return 1;
}
static int64_t sign_extend(uint32_t value, unsigned bits) {
  uint64_t sign = UINT64_C(1) << (bits - 1u), mask = (sign << 1u) - 1u;
  return (value & sign) ? -(int64_t)((~(uint64_t)value & mask) + 1u)
                        : (int64_t)(value & mask);
}
static void fixups(void) {
  AstMachineOpcode call =
      target.arch == AST_TARGET_ARCH_X86_64    ? AST_MACHINE_X86_CALL
      : target.arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_CALL
                                               : AST_MACHINE_RISCV_CALL;
  AstMachineOpcode address_op =
      target.arch == AST_TARGET_ARCH_X86_64    ? AST_MACHINE_X86_LEA
      : target.arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_LEA
                                               : AST_MACHINE_RISCV_LA;
  const char *destination = target.arch == AST_TARGET_ARCH_X86_64 ? "r9" : "x9";
  const int64_t offsets[] = {-2097148, -4096, -2048, -4,   0,
                             4,        2044,  2048,  4096, 2097148};
  const uint64_t base_address = UINT64_C(0x100000ffc);
  for (unsigned mode = 0; mode < 2; ++mode)
    for (unsigned n = 0; n < sizeof(offsets) / sizeof(offsets[0]); ++n) {
      AstPhysicalOperand operands[] = {G(destination, 64), symbol()};
      AstPhysicalInstructionInfo i = mode ? inst(address_op, 1, operands, 2)
                                          : inst(call, 0, operands + 1, 1);
      AstPhysicalBlockInfo bb = {0, 1, &i};
      AstEncoderOutput out;
      require(encoder_blocks(target, &bb, 1, 0, &out) && out.fixup_count == 1,
              "symbol field encodes");
      if (!out.fixup_count) {
        ast_encoder_output_destroy(&out);
        continue;
      }
      unsigned char saved[16] = {0};
      require(out.size <= sizeof(saved), "fixup fixture size");
      if (out.size > sizeof(saved)) {
        ast_encoder_output_destroy(&out);
        continue;
      }
      memcpy(saved, out.bytes, out.size);
      require(
          !ast_encoder_apply_fixups(&out, base_address, resolve_test, NULL) &&
              out.diagnostic == AST_ENCODER_DIAG_UNRESOLVED_SYMBOL &&
              out.fixup_count == 1 && !memcmp(saved, out.bytes, out.size),
          "unresolved fixup leaves bytes intact");
      uint64_t address = base_address + (uint64_t)offsets[n];
      require(ast_encoder_apply_fixups(&out, base_address, resolve_test,
                                       &address) &&
                  !out.fixups && !out.fixup_count,
              "resolve and consume fixup");
      uint64_t decoded = 0;
      if (target.arch == AST_TARGET_ARCH_X86_64)
        decoded = base_address + out.size +
                  (uint64_t)sign_extend(read32(out.bytes + out.size - 4u), 32);
      else if (target.arch == AST_TARGET_ARCH_AARCH64) {
        uint32_t hi = read32(out.bytes);
        if (!mode)
          decoded =
              base_address + (uint64_t)(sign_extend(hi & 0x03ffffffu, 26) * 4);
        else {
          uint32_t immediate =
                       ((hi >> 29u) & 3u) | (((hi >> 5u) & 0x7ffffu) << 2u),
                   lo = read32(out.bytes + 4u);
          decoded = (base_address & ~UINT64_C(4095)) +
                    (uint64_t)(sign_extend(immediate, 21) * 4096) +
                    ((lo >> 10u) & 4095u);
        }
      } else {
        uint32_t hi = read32(out.bytes), lo = read32(out.bytes + 4u);
        decoded = base_address + (uint64_t)sign_extend(hi & 0xfffff000u, 32) +
                  (uint64_t)sign_extend(lo >> 20u, 12);
      }
      require(decoded == address, "decoded relocation reaches requested "
                                  "address across page boundaries");
      memcpy(saved, out.bytes, out.size);
      require(ast_encoder_apply_fixups(&out, base_address, NULL, NULL) &&
                  !memcmp(saved, out.bytes, out.size),
              "consumed fixups are idempotent");
      ast_encoder_output_destroy(&out);
    }
  AstPhysicalOperand operand = symbol();
  AstPhysicalInstructionInfo i = inst(call, 0, &operand, 1);
  AstPhysicalBlockInfo bb = {0, 1, &i};
  AstEncoderOutput out;
  require(encoder_blocks(target, &bb, 1, 0, &out), "range fixture");
  unsigned char saved[16] = {0};
  memcpy(saved, out.bytes, out.size);
  uint64_t too_far = UINT64_C(0x10000000000);
  require(
      !ast_encoder_apply_fixups(&out, base_address, resolve_test, &too_far) &&
          out.diagnostic == AST_ENCODER_DIAG_RELOCATION_RANGE &&
          out.fixup_count == 1 && !memcmp(saved, out.bytes, out.size),
      "out-of-range fixup is transactional");
  size_t original_offset = out.fixups[0].offset;
  out.fixups[0].offset = out.size;
  require(
      !ast_encoder_apply_fixups(&out, base_address, resolve_test, &too_far) &&
          out.diagnostic == AST_ENCODER_DIAG_INVALID_FIXUP,
      "field beyond buffer rejected");
  out.fixups[0].offset = original_offset;
  out.fixups[0].addend = INT64_MIN;
  uint64_t small_address = 1;
  require(!ast_encoder_apply_fixups(&out, base_address, resolve_test,
                                    &small_address) &&
              out.diagnostic == AST_ENCODER_DIAG_RELOCATION_RANGE,
          "negative addend underflow rejected");
  out.fixups[0].addend = 0;
  AstEncoderFixup *two =
      (AstEncoderFixup *)realloc(out.fixups, 2u * sizeof(*two));
  require(two != NULL, "overlap fixture allocation");
  if (two) {
    out.fixups = two;
    out.fixups[1] = out.fixups[0];
    out.fixup_count = 2;
    uint64_t nearby = base_address + 64u;
    require(
        !ast_encoder_apply_fixups(&out, base_address, resolve_test, &nearby) &&
            out.diagnostic == AST_ENCODER_DIAG_INVALID_FIXUP &&
            !memcmp(saved, out.bytes, out.size),
        "overlapping fixups rejected before writing");
  }
  ast_encoder_output_destroy(&out);

  /* r5 regression: module-qualified symbols must survive physical encoding
     and reach the qualified resolver without changing the legacy resolver API.
   */
  AstPhysicalOperand qualified = symbol();
  qualified.module.text = "dep.mod";
  qualified.module.span.length = 7u;
  qualified.module.hash = 1u;
  AstPhysicalInstructionInfo qi = inst(call, 0, &qualified, 1);
  AstPhysicalBlockInfo qbb = {0, 1, &qi};
  AstEncoderOutput qout;
  require(encoder_blocks(target, &qbb, 1, 0, &qout) && qout.fixup_count == 1,
          "qualified symbol field encodes");
  if (qout.fixup_count == 1) {
    require(qout.fixups[0].module.text == qualified.module.text &&
                qout.fixups[0].module.span.length ==
                    qualified.module.span.length,
            "qualified fixup preserves module");
    uint64_t qaddress = base_address + 64u;
    require(ast_encoder_apply_fixups_qualified(
                &qout, base_address, resolve_qualified_test, &qaddress),
            "qualified resolver applies relocation");
  }
  ast_encoder_output_destroy(&qout);
}
static void constants(void) {
  uint64_t values[] = {0,
                       1,
                       UINT64_MAX,
                       2047,
                       2048,
                       (uint64_t)-2048,
                       (uint64_t)-2049,
                       UINT64_C(0x7fffffff),
                       UINT64_C(0x80000000),
                       UINT64_C(0xffffffff80000000),
                       INT64_MAX,
                       UINT64_C(0x8000000000000000),
                       UINT64_C(0x0123456789abcdef)};
  uint64_t random = UINT64_C(0xfedcba9876543210);
  for (unsigned n = 0; n < 4096; ++n) {
    random ^= random << 13u;
    random ^= random >> 7u;
    random ^= random << 17u;
    uint64_t value =
        n < sizeof(values) / sizeof(values[0]) ? values[n] : random;
    AstPhysicalOperand o[] = {G("x9", 64), imm(value, 64)};
    AstPhysicalInstructionInfo i = inst(AST_MACHINE_RISCV_LI, 1, o, 2);
    AstPhysicalBlockInfo bb = {0, 1, &i};
    AstEncoderOutput out;
    require(encoder_blocks(target, &bb, 1, 0, &out), "RV64 constant encodes");
    uint64_t regs[32] = {0};
    int valid = out.size > 0 && out.size <= 44 && !(out.size % 4u);
    for (size_t p = 0; p < out.size && valid; p += 4u) {
      uint32_t w = read32(out.bytes + p), op = w & 127u, rd = (w >> 7u) & 31u,
               rs = (w >> 15u) & 31u, funct = (w >> 12u) & 7u;
      int64_t k = (int64_t)(w >> 20u);
      if (k >= 2048)
        k -= 4096;
      if (rd != 9u || (rs != 0u && rs != 9u && op != 0x37u)) {
        valid = 0;
        break;
      }
      if (op == 0x37u)
        regs[rd] = (uint64_t)(int64_t)(int32_t)(w & 0xfffff000u);
      else if (op == 0x13u && funct == 0u)
        regs[rd] = regs[rs] + (uint64_t)k;
      else if (op == 0x1bu && funct == 0u)
        regs[rd] =
            (uint64_t)(int64_t)(int32_t)(uint32_t)(regs[rs] + (uint64_t)k);
      else if (op == 0x13u && funct == 1u && (w >> 26u) == 0u)
        regs[rd] = regs[rs] << ((w >> 20u) & 63u);
      else
        valid = 0;
    }
    require(valid && regs[9] == value, "RV64 arbitrary constant execution");
    ast_encoder_output_destroy(&out);
  }
}
static void branches_and_failures(void) {
  AstMachineOpcode ret =
      target.arch == AST_TARGET_ARCH_X86_64    ? AST_MACHINE_X86_RET
      : target.arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_RET
                                               : AST_MACHINE_RISCV_RET;
  AstMachineOpcode jmp =
      target.arch == AST_TARGET_ARCH_X86_64    ? AST_MACHINE_X86_BR
      : target.arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_BR
                                               : AST_MACHINE_RISCV_J;
  AstPhysicalInstructionInfo ins[3];
  AstPhysicalOperand b0 = block(0), b2 = block(2);
  ins[0] = inst(jmp, 0, &b2, 1);
  ins[1] = inst(jmp, 0, &b0, 1);
  ins[2] = inst(ret, 0, NULL, 0);
  AstPhysicalBlockInfo bb[] = {
      {0, 1, &ins[0]}, {1, 1, &ins[1]}, {2, 1, &ins[2]}};
  AstEncoderOutput out;
  require(encoder_blocks(target, bb, 3, 1, &out),
          "nonzero entry and block fixups");
  /* The encoder now folds branches whose targets are the immediately next
     laid-out blocks on all three 64-bit backends.  LLVM MC is the byte
     oracle, so describe the canonical folded stream rather than the old
     unfused source spelling. */
  const char *asmtext = "ret";
  report(asmtext, &out);
  ast_encoder_output_destroy(&out);
  ins[0] = inst(ret, 0, NULL, 0);
  ins[1] = inst(jmp, 0, &b0, 1);
  bb[2].instruction_count = 0;
  require(encoder_blocks(target, bb, 2, 0, &out), "backward branch");
  asmtext = target.arch == AST_TARGET_ARCH_X86_64 ? ".Lback$: ret; jmp .Lback$"
            : target.arch == AST_TARGET_ARCH_AARCH64
                ? ".Lback$: ret; b .Lback$"
                : ".Lback$: ret; j .Lback$";
  report(asmtext, &out);
  ast_encoder_output_destroy(&out);
  ins[1].operands[0] = block(3);
  require(!encoder_blocks(target, bb, 2, 0, &out) && !out.bytes &&
              !out.fixups && !out.size,
          "invalid branch is transactional");
  ast_encoder_output_destroy(&out);
  ins[1].operands[0] = block(2);
  require(!encoder_blocks(target, bb, 3, 0, &out) && !out.size,
          "empty branch target rejected");
  ast_encoder_output_destroy(&out);
  ins[0].opcode = AST_MACHINE_G_COPY;
  require(!encoder_blocks(target, bb, 1, 0, &out) && !out.size,
          "unlowered instruction rejected");
  ast_encoder_output_destroy(&out);
  require(!encoder_blocks(target, bb, 1, 1, &out), "invalid entry rejected");
  ast_encoder_output_destroy(&out);
  AstMachineOpcode call =
      target.arch == AST_TARGET_ARCH_X86_64    ? AST_MACHINE_X86_CALL
      : target.arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_CALL
                                               : AST_MACHINE_RISCV_CALL;
  AstPhysicalOperand sym = symbol();
  ins[0] = inst(call, 0, &sym, 1);
  require(encoder_blocks(target, bb, 1, 0, &out) && out.fixup_count == 1 &&
              out.fixups[0].symbol.text == sym.symbol.text,
          "one call relocation, no measuring-pass duplicates");
  if (out.fixup_count)
    require(out.fixups[0].offset + out.fixups[0].width == out.size,
            "fixup field bounds");
  ast_encoder_output_destroy(&out);
  require(!ast_encoder_function(NULL, 0, &out) &&
              out.diagnostic == AST_ENCODER_DIAG_INVALID_PHYSICAL,
          "null public input");
  ast_encoder_output_destroy(&out);
}
static void x86_compact_branches(void) {
  uint8_t padding[128];
  memset(padding, 0x90, sizeof(padding));
  AstPhysicalInstructionInfo fill = inst(AST_MACHINE_X86_RAW_BYTES, 0, NULL, 0);
  fill.raw_bytes = padding;
  AstPhysicalInstructionInfo ret = inst(AST_MACHINE_X86_RET, 0, NULL, 0);
  for (unsigned conditional = 0; conditional < 2u; ++conditional)
    for (unsigned backward = 0; backward < 2u; ++backward)
      for (unsigned long_form = 0; long_form < 2u; ++long_form) {
        unsigned pad =
            backward ? 126u - conditional * 2u + long_form : 127u + long_form;
        fill.raw_byte_count = pad;
        AstPhysicalOperand args[] = {G("al", 8), block(backward ? 0u : 2u),
                                     block(backward ? 2u : 1u)};
        AstPhysicalInstructionInfo jump =
            conditional ? inst(AST_MACHINE_X86_COND_BR, 0, args, 3)
                        : inst(AST_MACHINE_X86_BR, 0, &args[1], 1);
        AstPhysicalBlockInfo bb[] = {{0, 1, backward ? &fill : &jump},
                                     {1, 1, backward ? &jump : &fill},
                                     {2, 1, &ret}};
        AstEncoderOutput out;
        char assembly[160];
        ++cases;
        int ok = encoder_blocks(target, bb, 3, 0, &out);
        size_t offset = (backward ? pad : 0u) + (conditional ? 2u : 0u);
        require(ok &&
                    out.size == pad + 1u + (conditional ? 2u : 0u) +
                                    (long_form ? (conditional ? 6u : 5u) : 2u),
                "rel8 boundary length");
        if (ok)
          require(out.bytes[offset] == (long_form
                                            ? (conditional ? 0x0fu : 0xe9u)
                                            : (conditional ? 0x75u : 0xebu)),
                  "rel8 boundary opcode");
        if (backward)
          snprintf(assembly, sizeof(assembly),
                   ".Lstart$: .fill %u,1,0x90; %s%s .Lstart$; ret", pad,
                   conditional ? "test al, al; " : "",
                   conditional ? "jnz" : "jmp");
        else
          snprintf(assembly, sizeof(assembly),
                   "%s%s .Lend$; .fill %u,1,0x90; .Lend$: ret",
                   conditional ? "test al, al; " : "",
                   conditional ? "jnz" : "jmp", pad);
        if (ok)
          report(assembly, &out);
        ast_encoder_output_destroy(&out);
      }
  AstPhysicalOperand end = block(3);
  AstPhysicalInstructionInfo jumps[] = {inst(AST_MACHINE_X86_BR, 0, &end, 1),
                                        inst(AST_MACHINE_X86_BR, 0, &end, 1)};
  fill.raw_byte_count = 124;
  AstPhysicalBlockInfo cascade[] = {
      {0, 1, &jumps[0]}, {1, 1, &jumps[1]}, {2, 1, &fill}, {3, 1, &ret}};
  AstEncoderOutput out;
  ++cases;
  require(encoder_blocks(target, cascade, 4, 0, &out) && out.size == 129u &&
              out.bytes[0] == 0xebu && out.bytes[2] == 0xebu,
          "cascading branch relaxation");
  if (out.size)
    report("jmp .Lend$; jmp .Lend$; .fill 124,1,0x90; .Lend$: ret", &out);
  ast_encoder_output_destroy(&out);
  AstPhysicalOperand target_block = block(2), external = symbol();
  AstPhysicalInstructionInfo jump =
                                 inst(AST_MACHINE_X86_BR, 0, &target_block, 1),
                             call = inst(AST_MACHINE_X86_CALL, 0, &external, 1);
  AstPhysicalBlockInfo reloc[] = {{0, 1, &jump}, {1, 1, &call}, {2, 1, &ret}};
  require(encoder_blocks(target, reloc, 3, 0, &out) && out.size == 8u &&
              out.fixup_count == 1u && out.fixups[0].offset == 3u,
          "call relocation follows relaxed layout exactly once");
  ast_encoder_output_destroy(&out);
}
#include "atomic_encoder_cases.inc"
static void trace_layout_cases(void) {
  AstPhysicalOperand dest = G("rax", 64), value = imm(42u, 64u),
                     edge = block(2u);
  AstPhysicalInstructionInfo first = inst(AST_MACHINE_X86_BR, 0, &edge, 1);
  AstPhysicalInstructionInfo dead = inst(AST_MACHINE_X86_RET, 0, NULL, 0);
  AstPhysicalOperand args[] = {dest, value};
  AstPhysicalInstructionInfo body[] = {
      inst(AST_MACHINE_X86_MOV_IMM, 1, args, 2), dead};
  AstPhysicalBlockInfo bb[] = {{0, 1, &first}, {1, 1, &dead}, {2, 2, body}};
  AstEncoderOutput out, again;
  require(encoder_blocks_ordered(target, bb, 3, 0, &out) && out.size == 6u &&
              out.bytes[0] == 0xb8u && out.bytes[5] == 0xc3u,
          "trace follows successor and omits unreachable block");
  require(encoder_blocks_ordered(target, bb, 3, 0, &again) &&
              again.size == out.size &&
              !memcmp(again.bytes, out.bytes, out.size),
          "deterministic trace layout");
  ast_encoder_output_destroy(&out);
  ast_encoder_output_destroy(&again);
  first.operands[0].block = 0u;
  bb[0].instructions = body;
  bb[0].instruction_count = 2u;
  bb[2].instructions = &first;
  bb[2].instruction_count = 1u;
  require(encoder_blocks_ordered(target, bb, 3, 2, &out) && out.size == 6u &&
              out.bytes[0] == 0xb8u,
          "nonzero entry remains first in trace");
  ast_encoder_output_destroy(&out);
}
static void portable_trace_layout_cases(void) {
  int a64 = target.arch == AST_TARGET_ARCH_AARCH64;
  AstPhysicalOperand edge = block(2u);
  AstPhysicalInstructionInfo first =
      inst(a64 ? AST_MACHINE_A64_BR : AST_MACHINE_RISCV_J, 0, &edge, 1);
  AstPhysicalInstructionInfo ret =
      inst(a64 ? AST_MACHINE_A64_RET : AST_MACHINE_RISCV_RET, 0, NULL, 0);
  AstPhysicalOperand args[] = {reg(a64 ? "x0" : "a0", 64, AST_MACHINE_BANK_GPR),
                               imm(42u, 64u), imm(0u, 8u)};
  AstPhysicalInstructionInfo body[] = {
      inst(a64 ? AST_MACHINE_A64_MOVZ : AST_MACHINE_RISCV_LI, 1, args,
           a64 ? 3u : 2u),
      ret};
  AstPhysicalBlockInfo bb[] = {{0, 1, &first}, {1, 1, &ret}, {2, 2, body}};
  AstEncoderOutput out;
  require(encoder_blocks_ordered(target, bb, 3, 0, &out) && out.size == 8u,
          "A64/RV trace follows successor and omits unreachable block");
  ast_encoder_output_destroy(&out);
}
static void rv_far_branch_layout(void) {
  enum { N = 1250 };
  AstPhysicalInstructionInfo *fill = calloc(N + 1u, sizeof(*fill));
  require(fill != NULL, "RV far branch fixture allocation");
  if (!fill)
    return;
  AstPhysicalOperand args[] = {reg("a0", 64, AST_MACHINE_BANK_GPR),
                               reg("a0", 64, AST_MACHINE_BANK_GPR),
                               imm(1u, 64u)};
  for (unsigned j = 0; j < N; ++j)
    fill[j] = inst(AST_MACHINE_RISCV_ADDI, 1, args, 3);
  AstPhysicalInstructionInfo ret = inst(AST_MACHINE_RISCV_RET, 0, NULL, 0);
  fill[N] = ret;
  AstPhysicalOperand branch_args[] = {reg("a0", 64, AST_MACHINE_BANK_GPR),
                                      reg("zero", 64, AST_MACHINE_BANK_GPR),
                                      block(2u)};
  AstPhysicalOperand next = block(1u);
  AstPhysicalInstructionInfo entry[] = {
      inst(AST_MACHINE_RISCV_BNE, 0, branch_args, 3),
      inst(AST_MACHINE_RISCV_J, 0, &next, 1)};
  AstPhysicalBlockInfo bb[] = {{0, 2, entry}, {1, N + 1u, fill}, {2, 1, &ret}};
  AstEncoderOutput out;
  int ok = encoder_blocks(target, bb, 3, 0, &out);
  require(ok && out.size == N * 4u + 16u,
          "RV far branch uses stable eight-byte expansion");
  if (ok) {
    uint32_t jump_word = read32(out.bytes + 4u);
    uint32_t raw = ((jump_word >> 31u) << 20u) |
                   (((jump_word >> 21u) & 1023u) << 1u) |
                   (((jump_word >> 20u) & 1u) << 11u) | (jump_word & 0xff000u);
    require((jump_word & 127u) == 0x6fu &&
                sign_extend(raw, 21) + 4 == (int64_t)out.size - 4,
            "RV final JAL offset uses final expanded layout");
    report("beq a0, zero, .Lskip$; jal zero, .Lend$; .Lskip$: .rept 1250; addi "
           "a0, a0, 1; .endr; ret; .Lend$: ret",
           &out);
  }
  ast_encoder_output_destroy(&out);
  free(fill);
}
int main(int argc, char **argv) {
  ast_test_process_setup();
  dump = argc == 2 && !strcmp(argv[1], "--corpus");
  for (unsigned a = AST_TARGET_ARCH_X86_64; a <= AST_TARGET_ARCH_RISCV64; ++a) {
    target = ast_target_config_default((AstTargetArch)a);
    target.features |=
        a == AST_TARGET_ARCH_X86_64 ? AST_TARGET_FEATURE_X86_POPCNT : 0;
    if (a == AST_TARGET_ARCH_RISCV64)
      target.features |= AST_TARGET_FEATURE_RV_M | AST_TARGET_FEATURE_RV_F |
                         AST_TARGET_FEATURE_RV_D;
    atomic_cases();
    if (a == AST_TARGET_ARCH_X86_64) {
      x86_cases();
      x86_compact_branches();
      trace_layout_cases();
    } else if (a == AST_TARGET_ARCH_AARCH64) {
      a64_cases();
      portable_trace_layout_cases();
    } else {
      rv_cases();
      constants();
      portable_trace_layout_cases();
      rv_far_branch_layout();
    }
    branches_and_failures();
    fixups();
  }
  if (!dump)
    printf("%u encoding forms, 4096 RV64 constants; %d failures\n", cases,
           failures);
  return failures ? 1 : 0;
}
