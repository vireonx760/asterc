#include "../isa/aarch64_data_contract.h"
#include "../isa/aarch64_scalable.h"
#include "../isa/aarch64_scalar_contract.h"
#include "../isa/riscv64_branch_contract.h"
#include "../isa/riscv64_memory_contract.h"
#include "../isa/riscv64_scalar_contract.h"
#include "../isa/system_contract.h"
#include "../isa/vector_contract.h"
#include "target_internal.h"

static const char *digit_suffix(const char *s, const char *prefix) {
  while (*prefix) {
    if (*s != *prefix)
      return NULL;
    ++s;
    ++prefix;
  }
  return isdigit((unsigned char)*s) ? s : NULL;
}

static int parse_suffix(const char *s, const char *prefix, unsigned max,
                        unsigned *out) {
  const char *digits = digit_suffix(s, prefix);
  if (!digits)
    return 0;
  char *end = NULL;
  unsigned long v = strtoul(digits, &end, 10);
  if (!end || *end || v > max)
    return 0;
  *out = (unsigned)v;
  return 1;
}

static int x86_named_gpr(const char *name, AstTargetRegisterInfo *out) {
  /* Architectural encoding numbers: RAX=0, RCX=1, RDX=2, RBX=3,
     RSP=4, RBP=5, RSI=6, RDI=7. Keep this order stable for ISel/encoder. */
  static const char *r64[] = {"rax", "rcx", "rdx", "rbx",
                              "rsp", "rbp", "rsi", "rdi"};
  static const char *r32[] = {"eax", "ecx", "edx", "ebx",
                              "esp", "ebp", "esi", "edi"};
  static const char *r16[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
  static const char *r8[] = {"al",  "cl",  "dl",  "bl",
                             "spl", "bpl", "sil", "dil"};
  static const char *hi8[] = {"ah", "ch", "dh", "bh"};
  for (unsigned i = 0; i < 8u; i++) {
    if (target_ascii_ieq(name, r64[i])) {
      out->valid = 1;
      out->reg_class = AST_TARGET_REGCLASS_GPR64;
      out->width = 64;
      out->index = i;
      return 1;
    }
    if (target_ascii_ieq(name, r32[i])) {
      out->valid = 1;
      out->reg_class = AST_TARGET_REGCLASS_GPR32;
      out->width = 32;
      out->index = i;
      return 1;
    }
    if (target_ascii_ieq(name, r16[i])) {
      out->valid = 1;
      out->reg_class = AST_TARGET_REGCLASS_GPR16;
      out->width = 16;
      out->index = i;
      return 1;
    }
    if (target_ascii_ieq(name, r8[i])) {
      out->valid = 1;
      out->reg_class = AST_TARGET_REGCLASS_GPR8;
      out->width = 8;
      out->index = i;
      if (i >= 4u)
        out->extended = 1;
      return 1;
    }
  }
  for (unsigned i = 0; i < 4u; i++)
    if (target_ascii_ieq(name, hi8[i])) {
      out->valid = 1;
      out->reg_class = AST_TARGET_REGCLASS_GPR8;
      out->width = 8;
      out->index = i;
      out->high8 = 1;
      return 1;
    }
  return 0;
}

static int parse_x86(const AstTargetConfig *c, const char *name,
                     AstTargetRegisterInfo *out) {
  if (x86_named_gpr(name, out))
    return 1;
  unsigned n = 0;
  if ((name[0] == 'r' || name[0] == 'R') && isdigit((unsigned char)name[1])) {
    char *end = NULL;
    unsigned long v = strtoul(name + 1, &end, 10);
    if (v >= 8u && v <= 15u) {
      if (*end == 0) {
        out->valid = 1;
        out->reg_class = AST_TARGET_REGCLASS_GPR64;
        out->width = 64;
        out->index = (uint32_t)v;
        out->extended = 1;
        return 1;
      }
      if ((end[0] == 'd' || end[0] == 'D') && end[1] == 0) {
        out->valid = 1;
        out->reg_class = AST_TARGET_REGCLASS_GPR32;
        out->width = 32;
        out->index = (uint32_t)v;
        out->extended = 1;
        return 1;
      }
      if ((end[0] == 'w' || end[0] == 'W') && end[1] == 0) {
        out->valid = 1;
        out->reg_class = AST_TARGET_REGCLASS_GPR16;
        out->width = 16;
        out->index = (uint32_t)v;
        out->extended = 1;
        return 1;
      }
      if ((end[0] == 'b' || end[0] == 'B') && end[1] == 0) {
        out->valid = 1;
        out->reg_class = AST_TARGET_REGCLASS_GPR8;
        out->width = 8;
        out->index = (uint32_t)v;
        out->extended = 1;
        return 1;
      }
    }
  }
  if (parse_suffix(name, "xmm", 31u, &n) ||
      parse_suffix(name, "XMM", 31u, &n)) {
    if (n >= 16u && !(c->features & AST_TARGET_FEATURE_X86_AVX512F))
      return 0;
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_VEC128;
    out->width = 128;
    out->index = n;
    out->extended = n >= 8u;
    return 1;
  }
  if (parse_suffix(name, "ymm", 31u, &n) ||
      parse_suffix(name, "YMM", 31u, &n)) {
    if (!(c->features & AST_TARGET_FEATURE_X86_AVX) ||
        (n >= 16u && !(c->features & AST_TARGET_FEATURE_X86_AVX512F)))
      return 0;
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_VEC256;
    out->width = 256;
    out->index = n;
    out->extended = n >= 8u;
    return 1;
  }
  if (parse_suffix(name, "zmm", 31u, &n) ||
      parse_suffix(name, "ZMM", 31u, &n)) {
    if (!(c->features & AST_TARGET_FEATURE_X86_AVX512F))
      return 0;
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_VEC512;
    out->width = 512;
    out->index = n;
    out->extended = n >= 8u;
    return 1;
  }
  if (parse_suffix(name, "k", 7u, &n) || parse_suffix(name, "K", 7u, &n)) {
    if (!(c->features & AST_TARGET_FEATURE_X86_AVX512F))
      return 0;
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_MASK;
    out->width = 64;
    out->index = n;
    return 1;
  }
  if (target_ascii_ieq(name, "rip") || target_ascii_ieq(name, "eip") ||
      target_ascii_ieq(name, "rflags") || target_ascii_ieq(name, "eflags")) {
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_SPECIAL;
    out->width =
        target_ascii_ieq(name, "rip") || target_ascii_ieq(name, "rflags") ? 64u
                                                                          : 32u;
    out->special = 1;
    return 1;
  }
  return 0;
}

static int parse_a64(const AstTargetConfig *c, const char *name,
                     AstTargetRegisterInfo *out) {
  unsigned n = 0;
  if (parse_suffix(name, "x", 31u, &n)) {
    if (n == 31u)
      return 0;
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_GPR64;
    out->width = 64;
    out->index = n;
    return 1;
  }
  if (parse_suffix(name, "w", 31u, &n)) {
    if (n == 31u)
      return 0;
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_GPR32;
    out->width = 32;
    out->index = n;
    return 1;
  }
  if (target_ascii_ieq(name, "sp") || target_ascii_ieq(name, "xzr")) {
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_SPECIAL;
    out->width = 64;
    out->special = 1;
    out->index = 31u;
    return 1;
  }
  if (target_ascii_ieq(name, "wsp") || target_ascii_ieq(name, "wzr")) {
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_SPECIAL;
    out->width = 32;
    out->special = 1;
    out->index = 31u;
    return 1;
  }
  struct P {
    const char *p;
    uint32_t w;
  } aliases[] = {{"b", 8},  {"h", 16},  {"s", 32},
                 {"d", 64}, {"q", 128}, {"v", 128}};
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
    if (parse_suffix(name, aliases[i].p, 31u, &n)) {
      out->valid = 1;
      out->reg_class = aliases[i].w <= 64u ? AST_TARGET_REGCLASS_VEC64
                                           : AST_TARGET_REGCLASS_VEC128;
      out->width = aliases[i].w;
      out->index = n;
      return 1;
    }
  if (parse_suffix(name, "z", 31u, &n)) {
    if (!(c->features &
          (AST_TARGET_FEATURE_A64_SVE | AST_TARGET_FEATURE_A64_SME)))
      return 0;
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_SCALABLE_VECTOR;
    out->index = n;
    return 1;
  }
  if (parse_suffix(name, "p", 15u, &n)) {
    if (!(c->features &
          (AST_TARGET_FEATURE_A64_SVE | AST_TARGET_FEATURE_A64_SME)))
      return 0;
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_PREDICATE;
    out->index = n;
    return 1;
  }
  return 0;
}

static int riscv_gpr_alias(const char *name, unsigned *idx) {
  static const char *alias[32] = {
      "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
      "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
      "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
  for (unsigned i = 0; i < 32u; i++)
    if (target_ascii_ieq(name, alias[i])) {
      *idx = i;
      return 1;
    }
  if (target_ascii_ieq(name, "fp")) {
    *idx = 8u;
    return 1;
  }
  return 0;
}

static int riscv_fpr_alias(const char *name, unsigned *idx) {
  const char *prefixes[] = {"ft", "fs", "fa"};
  for (unsigned p = 0; p < 3u; p++) {
    unsigned v;
    if (parse_suffix(name, prefixes[p], 11u, &v)) {
      if (p == 0 && v <= 7u) {
        *idx = (unsigned)v;
        return 1;
      }
      if (p == 0 && v >= 8u && v <= 11u) {
        *idx = (unsigned)(20u + v);
        return 1;
      }
      if (p == 1 && v <= 1u) {
        *idx = (unsigned)(8u + v);
        return 1;
      }
      if (p == 1 && v >= 2u && v <= 11u) {
        *idx = (unsigned)(16u + v);
        return 1;
      }
      if (p == 2 && v <= 7u) {
        *idx = (unsigned)(10u + v);
        return 1;
      }
    }
  }
  return 0;
}

static int parse_rv(const AstTargetConfig *c, const char *name,
                    AstTargetRegisterInfo *out) {
  unsigned n = 0;
  if (parse_suffix(name, "x", 31u, &n) || riscv_gpr_alias(name, &n)) {
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_GPR64;
    out->width = 64;
    out->index = n;
    return 1;
  }
  if (parse_suffix(name, "f", 31u, &n) || riscv_fpr_alias(name, &n)) {
    if (!(c->features & (AST_TARGET_FEATURE_RV_F | AST_TARGET_FEATURE_RV_D)))
      return 0;
    out->valid = 1;
    out->reg_class = (c->features & AST_TARGET_FEATURE_RV_D)
                         ? AST_TARGET_REGCLASS_FP64
                         : AST_TARGET_REGCLASS_FP32;
    out->width = (c->features & AST_TARGET_FEATURE_RV_D) ? 64u : 32u;
    out->index = n;
    return 1;
  }
  if (parse_suffix(name, "v", 31u, &n)) {
    if (!(c->features & AST_TARGET_FEATURE_RV_V))
      return 0;
    out->valid = 1;
    out->reg_class = AST_TARGET_REGCLASS_SCALABLE_VECTOR;
    out->index = n;
    return 1;
  }
  return 0;
}

int ast_target_parse_register(const AstTargetConfig *config, const char *name,
                              AstTargetRegisterInfo *out) {
  if (!config || !name || !out)
    return 0;
  memset(out, 0, sizeof(*out));
  AstTargetConfig c = *config;
  c.features = ast_target_feature_closure(c.arch, c.features);
  if (c.arch == AST_TARGET_ARCH_X86_64)
    return parse_x86(&c, name, out);
  if (c.arch == AST_TARGET_ARCH_AARCH64)
    return parse_a64(&c, name, out);
  if (c.arch == AST_TARGET_ARCH_RISCV64)
    return parse_rv(&c, name, out);
  return 0;
}

int ast_target_x86_registers_compatible(const char *const *registers,
                                        size_t count, int rex_required) {
  AstTargetConfig c = ast_target_config_default(AST_TARGET_ARCH_X86_64);
  c.features |= AST_TARGET_FEATURE_X86_AVX512F;
  int high8 = 0, rex = rex_required != 0;
  for (size_t i = 0; i < count; i++) {
    AstTargetRegisterInfo r;
    if (!ast_target_parse_register(&c, registers[i], &r) || r.special)
      return 0;
    high8 |= r.high8 != 0;
    rex |= r.extended != 0;
  }
  return !(high8 && rex);
}

static int native_arch_matches(AstTargetArch arch, AstName name) {
  if (arch == AST_TARGET_ARCH_X86_64)
    return target_name_eq(name, "x86_64") || target_name_eq(name, "x86-64") ||
           target_name_eq(name, "amd64");
  if (arch == AST_TARGET_ARCH_AARCH64)
    return target_name_eq(name, "aarch64") || target_name_eq(name, "arm64");
  if (arch == AST_TARGET_ARCH_RISCV64)
    return target_name_eq(name, "riscv64") || target_name_eq(name, "rv64");
  return 0;
}

static int name_in(AstName n, const char *const *xs, size_t count) {
  for (size_t i = 0; i < count; i++)
    if (target_name_eq(n, xs[i]))
      return 1;
  return 0;
}

static int pure_register_mnemonic(AstTargetArch arch, AstName n) {
  if (arch == AST_TARGET_ARCH_AARCH64 &&
      (target_name_eq(n, "cmp") || target_name_eq(n, "cmn")))
    return 0;
  if (rv_scalar_source(arch, n))
    return 1;
  static const char *x86[] = {"mov", "add", "sub",  "and",  "or",
                              "xor", "cmp", "test", "shl",  "shr",
                              "sar", "rol", "ror",  "imul", "lea"};
  static const char *a64[] = {"add", "sub", "and", "orr", "eor",  "cmp", "cmn",
                              "lsl", "lsr", "asr", "mul", "madd", "msub"};
  static const char *rv[] = {"add", "sub",  "and", "or",   "xor", "sll",
                             "srl", "sra",  "slt", "sltu", "mul", "mulh",
                             "div", "divu", "rem", "remu", "lui", "auipc"};
  if (arch == AST_TARGET_ARCH_X86_64)
    return name_in(n, x86, sizeof(x86) / sizeof(x86[0]));
  if (arch == AST_TARGET_ARCH_AARCH64)
    return name_in(n, a64, sizeof(a64) / sizeof(a64[0]));
  if (arch == AST_TARGET_ARCH_RISCV64)
    return name_in(n, rv, sizeof(rv) / sizeof(rv[0]));
  return 0;
}

static int exact_pseudo_mnemonic(AstTargetArch arch, AstName n) {
  /* RET has a single architecturally fixed JALR x0,x1,0 encoding in
     our exact RV64 schema; other assembler aliases remain forbidden. */
  static const char *rv[] = {"mv",   "nop",  "li", "la",
                             "call", "tail", "j",  "jr"};
  static const char *a64[] = {"mov"};
  if (arch == AST_TARGET_ARCH_RISCV64)
    return name_in(n, rv, sizeof(rv) / sizeof(rv[0]));
  if (arch == AST_TARGET_ARCH_AARCH64)
    return name_in(n, a64, sizeof(a64) / sizeof(a64[0]));
  return 0;
}

static int native_name_to_buf(AstName n, char *buf, size_t cap) {
  if (!buf || cap < 2u || !n.text)
    return 0;
  size_t len = (size_t)n.span.length;
  if (len >= cap)
    return 0;
  for (size_t i = 0u; i < len; ++i)
    buf[i] = (char)tolower((unsigned char)n.text[i]);
  buf[len] = '\0';
  return 1;
}

static uint64_t native_all_register_features(AstTargetArch arch) {
  if (arch == AST_TARGET_ARCH_X86_64)
    return AST_TARGET_FEATURE_X86_SSE2 | AST_TARGET_FEATURE_X86_SSE41 |
           AST_TARGET_FEATURE_X86_AVX | AST_TARGET_FEATURE_X86_AVX2 |
           AST_TARGET_FEATURE_X86_AVX512F | AST_TARGET_FEATURE_X86_AVX512BW |
           AST_TARGET_FEATURE_X86_AVX512DQ | AST_TARGET_FEATURE_X86_F16C;
  if (arch == AST_TARGET_ARCH_AARCH64)
    return AST_TARGET_FEATURE_A64_FP16 | AST_TARGET_FEATURE_A64_BF16 |
           AST_TARGET_FEATURE_A64_SVE | AST_TARGET_FEATURE_A64_SVE2 |
           AST_TARGET_FEATURE_A64_SME;
  if (arch == AST_TARGET_ARCH_RISCV64)
    return AST_TARGET_FEATURE_RV_M | AST_TARGET_FEATURE_RV_A |
           AST_TARGET_FEATURE_RV_F | AST_TARGET_FEATURE_RV_D |
           AST_TARGET_FEATURE_RV_ZBB | AST_TARGET_FEATURE_RV_ZBA |
           AST_TARGET_FEATURE_RV_ZBC | AST_TARGET_FEATURE_RV_ZBS |
           AST_TARGET_FEATURE_RV_ZFH | AST_TARGET_FEATURE_RV_V;
  return 0u;
}

static uint64_t native_register_requirement(AstTargetArch arch,
                                            const AstTargetRegisterInfo *r) {
  if (!r)
    return 0u;
  if (arch == AST_TARGET_ARCH_X86_64) {
    if (r->reg_class == AST_TARGET_REGCLASS_VEC512 ||
        r->reg_class == AST_TARGET_REGCLASS_MASK ||
        ((r->reg_class == AST_TARGET_REGCLASS_VEC128 ||
          r->reg_class == AST_TARGET_REGCLASS_VEC256) &&
         r->index >= 16u))
      return AST_TARGET_FEATURE_X86_AVX512F;
    if (r->reg_class == AST_TARGET_REGCLASS_VEC256)
      return AST_TARGET_FEATURE_X86_AVX;
  } else if (arch == AST_TARGET_ARCH_AARCH64) {
    if (r->reg_class == AST_TARGET_REGCLASS_SCALABLE_VECTOR ||
        r->reg_class == AST_TARGET_REGCLASS_PREDICATE)
      return AST_TARGET_FEATURE_A64_SVE;
  } else if (arch == AST_TARGET_ARCH_RISCV64) {
    if (r->reg_class == AST_TARGET_REGCLASS_FP32 ||
        r->reg_class == AST_TARGET_REGCLASS_FP64)
      return AST_TARGET_FEATURE_RV_F;
    if (r->reg_class == AST_TARGET_REGCLASS_SCALABLE_VECTOR)
      return AST_TARGET_FEATURE_RV_V;
  }
  return 0u;
}

static int native_looks_like_register(AstTargetArch arch, const char *name) {
  if (!name || !*name)
    return 0;
  if (arch == AST_TARGET_ARCH_X86_64) {
    if ((name[0] == 'r' && isdigit((unsigned char)name[1])) ||
        (digit_suffix(name, "xmm") || digit_suffix(name, "ymm") ||
         digit_suffix(name, "zmm")) ||
        (name[0] == 'k' && isdigit((unsigned char)name[1])))
      return 1;
  } else if (arch == AST_TARGET_ARCH_AARCH64) {
    if (strchr("xwvqdshbzp", name[0]) && isdigit((unsigned char)name[1]))
      return 1;
  } else if (arch == AST_TARGET_ARCH_RISCV64) {
    if ((strchr("xfv", name[0]) && isdigit((unsigned char)name[1])) ||
        (digit_suffix(name, "fa") || digit_suffix(name, "fs") ||
         digit_suffix(name, "ft")))
      return 1;
  }
  return 0;
}

uint64_t target_native_instruction_features(AstTargetArch arch,
                                            AstName mnemonic) {
  const RvScalarForm *scalar = rv_scalar_source(arch, mnemonic);
  if (scalar)
    return scalar->feature;
  const VectorForm *vector = vector_source_form(arch, mnemonic);
  if (vector)
    return vector->feature;
  for (uint32_t j = 0; j < SYSTEM_COUNT; ++j)
    if (system_forms[j].arch == arch &&
        system_name(mnemonic, system_forms[j].name))
      return system_forms[j].feature;
  char name[64];
  if (arch == AST_TARGET_ARCH_X86_64 &&
      native_name_to_buf(mnemonic, name, sizeof(name)) &&
      strcmp(name, "popcnt") == 0)
    return AST_TARGET_FEATURE_X86_POPCNT;
  return 0u;
}

static int native_exact_mnemonic_known(AstTargetArch arch, const char *m) {
  AstName name = {0};
  name.text = m;
  name.span.length = (uint32_t)strlen(m);
  if (a64_scalar_source(arch, name))
    return 1;
  if (a64_data_source(arch, name))
    return 1;
  if (arch == AST_TARGET_ARCH_AARCH64 &&
      a64_scalable_find(name) < a64_scalable_form_count)
    return 1;
  if (arch == AST_TARGET_ARCH_RISCV64 &&
      rv_native_branch_source(name) != AST_MACHINE_OP_INVALID)
    return 1;
  if (rv_memory_source(arch, name))
    return 1;
  if (arch == AST_TARGET_ARCH_RISCV64)
    for (uint32_t j = 0; j < RV_SCALAR_COUNT; ++j)
      if (!strcmp(m, rv_scalar_forms[j].name))
        return 1;
  for (uint32_t j = 0; j < VECTOR_COUNT; ++j)
    if (vector_forms[j].arch == arch && strcmp(m, vector_forms[j].name) == 0)
      return 1;
  for (uint32_t j = 0; j < SYSTEM_COUNT; ++j)
    if (system_forms[j].arch == arch && strcmp(m, system_forms[j].name) == 0)
      return 1;
  static const char *x86[] = {
      "mov", "lea",  "add", "sub", "and",     "or",   "xor",    "cmp", "test",
      "shl", "shr",  "sar", "rol", "ror",     "imul", "idiv",   "div", "push",
      "pop", "call", "jmp", "ret", "syscall", "nop",  "popcnt", "cli", "sti",
      "hlt", "cld",  "in",  "out", "lidt",    "lgdt"};
  static const char *a64[] = {
      "label", "b",   "cbz", "cbnz", "add", "sub", "and",  "orr",  "eor", "cmp",
      "cmn",   "ldr", "str", "ldp",  "stp", "adr", "adrp", "bl",   "br",  "blr",
      "ret",   "svc", "nop", "lsl",  "lsr", "asr", "mul",  "madd", "msub"};
  static const char *rv[] = {
      "label", "beq",      "bne",     "add",   "addi",   "sub",  "mul",  "div",
      "divu",  "rem",      "remu",    "and",   "andi",   "or",   "ori",  "xor",
      "xori",  "sll",      "slli",    "srl",   "srli",   "sra",  "srai", "slt",
      "sltu",  "lui",      "auipc",   "ld",    "sd",     "lw",   "sw",   "jal",
      "jalr",  "ret",      "fence",   "ecall", "ebreak", "lr.w", "lr.d", "sc.w",
      "sc.d",  "amoadd.w", "amoadd.d"};
  const char *const *list = NULL;
  size_t count = 0u;
  if (arch == AST_TARGET_ARCH_X86_64) {
    list = x86;
    count = sizeof(x86) / sizeof(x86[0]);
  } else if (arch == AST_TARGET_ARCH_AARCH64) {
    list = a64;
    count = sizeof(a64) / sizeof(a64[0]);
  } else if (arch == AST_TARGET_ARCH_RISCV64) {
    list = rv;
    count = sizeof(rv) / sizeof(rv[0]);
  }
  for (size_t i = 0u; i < count; ++i)
    if (strcmp(m, list[i]) == 0)
      return 1;
  return 0;
}

static int native_bound_register_move(AstTargetArch arch,
                                      const AstHirNativeStmtInfo *s) {
  if (!(arch == AST_TARGET_ARCH_AARCH64 ? system_name(s->mnemonic, "mov")
                                        : arch == AST_TARGET_ARCH_RISCV64 &&
                                              system_name(s->mnemonic, "mv")) ||
      s->operand_count != 2u)
    return 0;
  const AstHirNativeOperandInfo *operands[] = {system_role(s, "dst"),
                                               system_role(s, "src")};
  int binding = 0;
  for (uint32_t j = 0; j < 2u; ++j) {
    AstHirNativeExprInfo e;
    if (!operands[j] || operands[j]->is_memory ||
        !ast_hir_native_expr_info(operands[j]->value, &e) ||
        e.kind != AST_HIR_NATIVE_EXPR_NAME)
      return 0;
    if (e.binding_kind == AST_HIR_NATIVE_BINDING_INPUT ||
        e.binding_kind == AST_HIR_NATIVE_BINDING_OUTPUT)
      binding = 1;
    else if (e.binding_kind != AST_HIR_NATIVE_BINDING_TARGET)
      return 0;
  }
  /* Bound register moves always select one fixed ORR/ADDI/vector-copy
     encoding. Immediate materialization is deliberately excluded. */
  return binding;
}

static int native_expr_scan(AstTargetProgram *program, size_t fi,
                            AstMirInstId instruction,
                            const AstHirNativeExpr *expr, int value_operand,
                            uint64_t *required_features, int *has_high8,
                            int *needs_rex) {
  if (!expr)
    return 1;
  AstHirNativeExprInfo ei;
  if (!ast_hir_native_expr_info(expr, &ei))
    return 0;
  int ok = 1;
  if (ei.kind == AST_HIR_NATIVE_EXPR_NAME &&
      ei.binding_kind == AST_HIR_NATIVE_BINDING_TARGET && ei.name.text) {
    char name[64];
    if (!native_name_to_buf(ei.name, name, sizeof(name))) {
      target_diag(program, AST_TARGET_DIAG_INVALID_NATIVE_REGISTER,
                  AST_DIAG_ERROR, ei.span, (uint32_t)fi, instruction,
                  "native target name is too long to validate");
      ok = 0;
    } else {
      AstTargetRegisterInfo reg;
      if (ast_target_parse_register(&program->config, name, &reg)) {
        if (program->config.arch == AST_TARGET_ARCH_X86_64 && value_operand &&
            reg.special &&
            (target_ascii_ieq(name, "rip") || target_ascii_ieq(name, "eip") ||
             target_ascii_ieq(name, "rflags") ||
             target_ascii_ieq(name, "eflags"))) {
          target_diag(
              program, AST_TARGET_DIAG_INVALID_NATIVE_REGISTER, AST_DIAG_ERROR,
              ei.span, (uint32_t)fi, instruction,
              "x86 register '%s' is not valid as a generic value operand",
              name);
          ok = 0;
        }
        if (reg.high8)
          *has_high8 = 1;
        if (reg.extended)
          *needs_rex = 1;
      } else {
        AstTargetConfig all = program->config;
        all.features = native_all_register_features(all.arch);
        if (ast_target_parse_register(&all, name, &reg)) {
          uint64_t req = native_register_requirement(all.arch, &reg);
          if (required_features)
            *required_features |= req;
          target_diag(
              program, AST_TARGET_DIAG_REQUIRED_FEATURE, AST_DIAG_ERROR,
              ei.span, (uint32_t)fi, instruction,
              "native register '%s' requires an unavailable target feature",
              name);
          if (reg.high8)
            *has_high8 = 1;
          if (reg.extended)
            *needs_rex = 1;
          ok = 0;
        } else if (native_looks_like_register(program->config.arch, name)) {
          target_diag(program, AST_TARGET_DIAG_INVALID_NATIVE_REGISTER,
                      AST_DIAG_ERROR, ei.span, (uint32_t)fi, instruction,
                      "invalid native register '%s' for %s", name,
                      ast_target_arch_name(program->config.arch));
          ok = 0;
        }
      }
    }
  }
  for (uint32_t i = 0u; i < ei.child_count; ++i) {
    const AstHirNativeExpr *child = ast_hir_native_expr_child(expr, i);
    if (!native_expr_scan(program, fi, instruction, child, value_operand,
                          required_features, has_high8, needs_rex))
      ok = 0;
  }
  return ok;
}

int target_validate_native(AstTargetProgram *program, size_t fi,
                           const AstMirFunction *function,
                           const AstMirInstructionInfo *inst,
                           AstTargetInstructionInfo *out) {
  const AstHirFunction *hf = ast_mir_function_source_hir(function);
  if (!hf)
    return 0;
  const AstHirNativeRegion *region =
      ast_hir_function_native_region(hf, inst->native_region);
  AstHirNativeRegionInfo ri;
  if (!region || !ast_hir_native_region_info(region, &ri))
    return 0;
  if (!native_arch_matches(program->config.arch, ri.arch)) {
    out->action = AST_LEGALIZE_UNSUPPORTED;
    target_diag(program, AST_TARGET_DIAG_NATIVE_ARCH_MISMATCH, AST_DIAG_ERROR,
                inst->span, (uint32_t)fi, inst->id,
                "native region architecture does not match selected target %s",
                ast_target_arch_name(program->config.arch));
    return 0;
  }
  int pure = 1;
  for (uint32_t si = 0; si < ri.statement_count; si++) {
    AstHirNativeStmtInfo s;
    if (!ast_hir_native_region_statement(region, si, &s)) {
      pure = 0;
      continue;
    }
    if (s.kind == AST_HIR_NATIVE_BYTES) {
      pure = 0;
      continue;
    }
    char mnemonic[64];
    int have_mnemonic =
        native_name_to_buf(s.mnemonic, mnemonic, sizeof(mnemonic));
    uint64_t instruction_features =
        target_native_instruction_features(program->config.arch, s.mnemonic);
    out->required_features |= instruction_features;
    if (instruction_features & ~program->config.features) {
      out->action = AST_LEGALIZE_UNSUPPORTED;
      target_diag(
          program, AST_TARGET_DIAG_REQUIRED_FEATURE, AST_DIAG_ERROR, s.span,
          (uint32_t)fi, inst->id,
          "native instruction '%s' requires an unavailable target feature",
          have_mnemonic ? mnemonic : "?");
    }
    if ((ri.flags & AST_HIR_NATIVE_EXACT) &&
        program->config.exact_native_fail_closed) {
      int bound_move = native_bound_register_move(program->config.arch, &s);
      if (!bound_move &&
          exact_pseudo_mnemonic(program->config.arch, s.mnemonic)) {
        out->action = AST_LEGALIZE_UNSUPPORTED;
        target_diag(program, AST_TARGET_DIAG_INVALID_NATIVE_FORM,
                    AST_DIAG_ERROR, s.span, (uint32_t)fi, inst->id,
                    "exact native region uses a pseudo/alias instruction form "
                    "that is not encoding-stable");
      } else if (!bound_move &&
                 (!have_mnemonic || !native_exact_mnemonic_known(
                                        program->config.arch, mnemonic))) {
        out->action = AST_LEGALIZE_UNSUPPORTED;
        target_diag(program, AST_TARGET_DIAG_INVALID_NATIVE_FORM,
                    AST_DIAG_ERROR, s.span, (uint32_t)fi, inst->id,
                    "exact native instruction is not in the v0.7 "
                    "encoding-stable schema");
      }
    }
    if (a64_scalable_statement(program->config.arch, &s) ||
        !pure_register_mnemonic(program->config.arch, s.mnemonic))
      pure = 0;
    int has_high8 = 0, needs_rex = 0;
    uint64_t register_features = 0u;
    for (uint32_t oi = 0; oi < s.operand_count; oi++) {
      const AstHirNativeOperandInfo *op = &s.operands[oi];
      const AstHirNativeExpr *expr =
          op->is_memory ? op->memory_address : op->value;
      if (op->is_memory)
        pure = 0;
      if (!native_expr_scan(program, fi, inst->id, expr, !op->is_memory,
                            &register_features, &has_high8, &needs_rex))
        out->action = AST_LEGALIZE_UNSUPPORTED;
    }
    out->required_features |= register_features;
    if (program->config.arch == AST_TARGET_ARCH_X86_64 && has_high8 &&
        needs_rex) {
      out->action = AST_LEGALIZE_UNSUPPORTED;
      target_diag(program, AST_TARGET_DIAG_INVALID_NATIVE_FORM, AST_DIAG_ERROR,
                  s.span, (uint32_t)fi, inst->id,
                  "x86 high-8 register cannot be encoded in a form that "
                  "requires REX/extended register encoding");
    }
  }
  if (pure && (inst->flags & AST_MIR_INST_CONSERVATIVE_EFFECTS)) {
    out->narrowed_effects =
        inst->flags &
        ~(uint32_t)(AST_MIR_INST_READS_MEMORY | AST_MIR_INST_WRITES_MEMORY |
                    AST_MIR_INST_CONSERVATIVE_EFFECTS);
    out->native_effects_refined = 1u;
  }
  return out->action != AST_LEGALIZE_UNSUPPORTED;
}
