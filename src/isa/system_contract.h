#ifndef ASTERASM_SYSTEM_CONTRACT_H
#define ASTERASM_SYSTEM_CONTRACT_H

#include "asterasm/regalloc.h"
#include <ctype.h>
#include <string.h>

/* Each admitted form encodes exactly one instruction. The final immediate
   operand identifies its schema; all register definitions precede uses. */
typedef enum SystemShape {
  SYSTEM_SHAPE_FIXED,
  SYSTEM_SHAPE_IMPLICIT,
  SYSTEM_SHAPE_EXCEPTION_RETURN,
  SYSTEM_SHAPE_X86_READ_SPECIAL,
  SYSTEM_SHAPE_X86_WRITE_SPECIAL,
  SYSTEM_SHAPE_X86_ADDRESS,
  SYSTEM_SHAPE_X86_SELECTOR_READ,
  SYSTEM_SHAPE_X86_SELECTOR_WRITE,
  SYSTEM_SHAPE_A64_READ_SYSREG,
  SYSTEM_SHAPE_A64_WRITE_SYSREG,
  SYSTEM_SHAPE_A64_SYS,
  SYSTEM_SHAPE_A64_SYSL,
  SYSTEM_SHAPE_A64_BARRIER,
  SYSTEM_SHAPE_A64_EXCEPTION,
  SYSTEM_SHAPE_A64_PSTATE,
  SYSTEM_SHAPE_RV_CSR_REG,
  SYSTEM_SHAPE_RV_CSR_IMM,
  SYSTEM_SHAPE_RV_FENCE,
  SYSTEM_SHAPE_RV_FENCE_REG
} SystemShape;
typedef enum SystemId {
#define AST_SYSTEM_FORM(id, name, arch, shape, bits, feature, defs, uses,      \
                        privileged)                                            \
  SYSTEM_##id,
#include "asterasm/detail/system_instructions.inc"
#undef AST_SYSTEM_FORM
  SYSTEM_COUNT
} SystemId;
typedef struct SystemForm {
  const char *name;
  AstTargetArch arch;
  SystemShape shape;
  uint32_t encoding;
  uint64_t feature;
  uint8_t definitions, uses, privileged;
} SystemForm;
static const SystemForm system_forms[SYSTEM_COUNT] = {
#define AST_SYSTEM_FORM(id, name, arch, shape, bits, feature, defs, uses,      \
                        privileged)                                            \
  {name,                                                                       \
   AST_TARGET_ARCH_##arch,                                                     \
   SYSTEM_SHAPE_##shape,                                                       \
   bits,                                                                       \
   feature,                                                                    \
   defs,                                                                       \
   uses,                                                                       \
   privileged},
#include "asterasm/detail/system_instructions.inc"
#undef AST_SYSTEM_FORM
};

static inline int system_name(AstName a, const char *s) {
  size_t n = strlen(s);
  if (!a.text || a.span.length != n)
    return 0;
  for (size_t j = 0; j < n; ++j)
    if (tolower((unsigned char)a.text[j]) != (unsigned char)s[j])
      return 0;
  return 1;
}
static inline const AstHirNativeOperandInfo *
system_role(const AstHirNativeStmtInfo *s, const char *name) {
  const AstHirNativeOperandInfo *found = NULL;
  for (uint32_t j = 0; j < s->operand_count; ++j)
    if (system_name(s->operands[j].name, name)) {
      if (found)
        return NULL;
      found = &s->operands[j];
    }
  return found;
}
static inline int system_special_name(const AstHirNativeOperandInfo *o,
                                      char *bank, uint32_t *reg) {
  AstHirNativeExprInfo e;
  if (!o || o->is_memory || !ast_hir_native_expr_info(o->value, &e) ||
      e.kind != AST_HIR_NATIVE_EXPR_NAME ||
      e.binding_kind != AST_HIR_NATIVE_BINDING_TARGET || !e.name.text ||
      e.name.span.length < 3u || e.name.span.length > 4u)
    return 0;
  char b = (char)tolower((unsigned char)e.name.text[0]);
  if ((b != 'c' && b != 'd') || tolower((unsigned char)e.name.text[1]) != 'r')
    return 0;
  uint32_t v = 0u;
  for (uint32_t j = 2u; j < e.name.span.length; ++j) {
    if (e.name.text[j] < '0' || e.name.text[j] > '9')
      return 0;
    v = v * 10u + (uint32_t)(e.name.text[j] - '0');
  }
  *bank = b;
  *reg = v;
  return 1;
}
static inline const SystemForm *
system_source_form(AstTargetArch arch, const AstHirNativeStmtInfo *s) {
  if (arch == AST_TARGET_ARCH_AARCH64 && system_name(s->mnemonic, "msr")) {
    const AstHirNativeOperandInfo *pstate = system_role(s, "pstate");
    AstHirNativeExprInfo e;
    if (pstate) {
      if (pstate->is_memory || !ast_hir_native_expr_info(pstate->value, &e) ||
          e.kind != AST_HIR_NATIVE_EXPR_NAME ||
          e.binding_kind != AST_HIR_NATIVE_BINDING_TARGET)
        return NULL;
      if (system_name(e.name, "daifset"))
        return &system_forms[SYSTEM_A64_DAIFSET];
      if (system_name(e.name, "daifclr"))
        return &system_forms[SYSTEM_A64_DAIFCLR];
      if (system_name(e.name, "spsel"))
        return &system_forms[SYSTEM_A64_SPSEL];
      return NULL;
    }
  }
  if (arch == AST_TARGET_ARCH_X86_64 && system_name(s->mnemonic, "mov")) {
    char bank;
    uint32_t reg;
    if (system_special_name(system_role(s, "src"), &bank, &reg))
      return &system_forms[bank == 'c' ? SYSTEM_X86_READ_CR
                                       : SYSTEM_X86_READ_DR];
    if (system_special_name(system_role(s, "dst"), &bank, &reg))
      return &system_forms[bank == 'c' ? SYSTEM_X86_WRITE_CR
                                       : SYSTEM_X86_WRITE_DR];
    return NULL;
  }
  for (uint32_t j = 0; j < SYSTEM_COUNT; ++j)
    if (system_forms[j].arch == arch &&
        system_name(s->mnemonic, system_forms[j].name))
      return &system_forms[j];
  return NULL;
}
static inline AstMachineOpcode system_opcode(AstTargetArch arch) {
  return arch == AST_TARGET_ARCH_X86_64    ? AST_MACHINE_X86_SYSTEM
         : arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_SYSTEM
                                           : AST_MACHINE_RISCV_SYSTEM;
}
static inline const SystemForm *
system_physical_form(const AstPhysicalInstructionInfo *i) {
  if (!i || !i->operand_count || i->operand_count > AST_PHYSICAL_MAX_OPERANDS)
    return NULL;
  const AstPhysicalOperand *tag = &i->operands[i->operand_count - 1u];
  if (tag->kind != AST_PHYSICAL_OPERAND_IMMEDIATE || tag->bit_width != 16u ||
      tag->immediate_bits >= SYSTEM_COUNT)
    return NULL;
  const SystemForm *f = &system_forms[tag->immediate_bits];
  return i->opcode == system_opcode(f->arch) ? f : NULL;
}
static inline int system_exception_return(const AstPhysicalInstructionInfo *i) {
  const SystemForm *f = system_physical_form(i);
  return f && f->shape == SYSTEM_SHAPE_EXCEPTION_RETURN;
}
static inline int system_gpr(const AstPhysicalOperand *o, uint32_t bits,
                             uint32_t count) {
  return o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         o->bank == AST_MACHINE_BANK_GPR && o->bit_width == bits &&
         o->register_unit >= 64u && o->register_unit < 64u + count;
}
static inline int system_imm(const AstPhysicalOperand *o, uint64_t maximum) {
  return o->kind == AST_PHYSICAL_OPERAND_IMMEDIATE && o->bit_width == 64u &&
         o->immediate_bits <= maximum;
}
static inline int system_a64_register(const AstPhysicalOperand *o) {
  return system_gpr(o, 64u, 31u) ||
         (o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
          o->bank == AST_MACHINE_BANK_SPECIAL && o->bit_width == 64u &&
          o->register_unit == 321u && strcmp(o->register_name, "xzr") == 0);
}
static inline uint32_t system_register_index(const AstPhysicalOperand *o) {
  return o->bank == AST_MACHINE_BANK_SPECIAL ? 31u : o->register_unit - 64u;
}
static inline int system_control_number(const SystemForm *f, uint32_t reg) {
  if (f == &system_forms[SYSTEM_X86_READ_CR] ||
      f == &system_forms[SYSTEM_X86_WRITE_CR])
    return reg == 0u || reg == 2u || reg == 3u || reg == 4u || reg == 8u;
  return reg <= 3u || reg == 6u ||
         reg == 7u; /* DR4/5 aliases are deliberately excluded. */
}
static inline int
system_physical_contract(const AstPhysicalInstructionInfo *i) {
  const SystemForm *f = system_physical_form(i);
  if (!f || i->raw_bytes || i->raw_byte_count)
    return 0;
  const AstPhysicalOperand *o = i->operands;
  uint32_t n = i->operand_count - 1u, d = i->result_count;
  switch (f->shape) {
  case SYSTEM_SHAPE_FIXED:
  case SYSTEM_SHAPE_EXCEPTION_RETURN:
    return !n && !d;
  case SYSTEM_SHAPE_IMPLICIT: {
    uint32_t count = 0u, defs = 0u;
    for (uint32_t pass = 0; pass < 2u; ++pass) {
      uint32_t mask = pass ? f->uses : f->definitions;
      for (uint32_t r = 0; r < 4u; ++r)
        if (mask & (1u << r)) {
          if (count >= n || !system_gpr(&o[count], 32u, 16u) ||
              o[count].register_unit != 64u + r)
            return 0;
          ++count;
          if (!pass)
            ++defs;
        }
    }
    return n == count && d == defs;
  }
  case SYSTEM_SHAPE_X86_READ_SPECIAL:
  case SYSTEM_SHAPE_X86_WRITE_SPECIAL:
    return n == 2u &&
           d == (f->shape == SYSTEM_SHAPE_X86_READ_SPECIAL ? 1u : 0u) &&
           system_gpr(&o[0], 64u, 16u) && system_imm(&o[1], 15u) &&
           system_control_number(f, (uint32_t)o[1].immediate_bits);
  case SYSTEM_SHAPE_X86_ADDRESS:
    return n == 1u && !d && system_gpr(&o[0], 64u, 16u);
  case SYSTEM_SHAPE_X86_SELECTOR_READ:
  case SYSTEM_SHAPE_X86_SELECTOR_WRITE:
    return n == 1u &&
           d == (f->shape == SYSTEM_SHAPE_X86_SELECTOR_READ ? 1u : 0u) &&
           system_gpr(&o[0], 16u, 16u);
  case SYSTEM_SHAPE_A64_READ_SYSREG:
  case SYSTEM_SHAPE_A64_WRITE_SYSREG:
    return n == 2u &&
           d == (f->shape == SYSTEM_SHAPE_A64_READ_SYSREG ? 1u : 0u) &&
           system_a64_register(&o[0]) && system_imm(&o[1], 65535u) &&
           o[1].immediate_bits >= 32768u;
  case SYSTEM_SHAPE_A64_SYS:
  case SYSTEM_SHAPE_A64_SYSL:
    return n == 5u && d == (f->shape == SYSTEM_SHAPE_A64_SYSL ? 1u : 0u) &&
           system_a64_register(&o[0]) && system_imm(&o[1], 7u) &&
           system_imm(&o[2], 15u) && system_imm(&o[3], 15u) &&
           system_imm(&o[4], 7u);
  case SYSTEM_SHAPE_A64_BARRIER:
    return n == 1u && !d && system_imm(&o[0], 15u) &&
           (f == &system_forms[SYSTEM_A64_ISB]
                ? o[0].immediate_bits == 15u
                : (o[0].immediate_bits & 3u) != 0u);
  case SYSTEM_SHAPE_A64_EXCEPTION:
    return n == 1u && !d && system_imm(&o[0], 65535u);
  case SYSTEM_SHAPE_A64_PSTATE:
    return n == 1u && !d &&
           system_imm(&o[0], f == &system_forms[SYSTEM_A64_SPSEL] ? 1u : 15u);
  case SYSTEM_SHAPE_RV_CSR_REG:
  case SYSTEM_SHAPE_RV_CSR_IMM: {
    if (n != 3u || d != 1u || !system_gpr(&o[0], 64u, 32u) ||
        !system_imm(&o[1], 4095u) ||
        (f->shape == SYSTEM_SHAPE_RV_CSR_REG ? !system_gpr(&o[2], 64u, 32u)
                                             : !system_imm(&o[2], 31u)))
      return 0;
    uint32_t source = f->shape == SYSTEM_SHAPE_RV_CSR_REG
                          ? o[2].register_unit - 64u
                          : (uint32_t)o[2].immediate_bits;
    int writing = (f->encoding & 0x3000u) == 0x1000u || source != 0u;
    return !writing || (o[1].immediate_bits & 0xc00u) != 0xc00u;
  }
  case SYSTEM_SHAPE_RV_FENCE:
    return n == 2u && !d && system_imm(&o[0], 15u) && system_imm(&o[1], 15u);
  case SYSTEM_SHAPE_RV_FENCE_REG:
    return n == 2u && !d && system_gpr(&o[0], 64u, 32u) &&
           system_gpr(&o[1], 64u, 32u);
  }
  return 0;
}
/* Fixed-width ISA words are shared as a contract; reference assemblers and
   emulator state checks are independent test oracles. */
static inline uint32_t system_word(const SystemForm *f,
                                   const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  uint32_t r = o[0].kind == AST_PHYSICAL_OPERAND_REGISTER
                   ? system_register_index(&o[0])
                   : 0u;
  uint32_t w = f->encoding;
  switch (f->shape) {
  case SYSTEM_SHAPE_A64_READ_SYSREG:
  case SYSTEM_SHAPE_A64_WRITE_SYSREG:
    return w | ((uint32_t)o[1].immediate_bits << 5u) | r;
  case SYSTEM_SHAPE_A64_SYS:
  case SYSTEM_SHAPE_A64_SYSL:
    return w | r | ((uint32_t)o[1].immediate_bits << 16u) |
           ((uint32_t)o[2].immediate_bits << 12u) |
           ((uint32_t)o[3].immediate_bits << 8u) |
           ((uint32_t)o[4].immediate_bits << 5u);
  case SYSTEM_SHAPE_A64_BARRIER:
  case SYSTEM_SHAPE_A64_PSTATE:
    return w | ((uint32_t)o[0].immediate_bits << 8u);
  case SYSTEM_SHAPE_A64_EXCEPTION:
    return w | ((uint32_t)o[0].immediate_bits << 5u);
  case SYSTEM_SHAPE_RV_CSR_REG:
  case SYSTEM_SHAPE_RV_CSR_IMM:
    return w | (r << 7u) | ((uint32_t)o[1].immediate_bits << 20u) |
           ((f->shape == SYSTEM_SHAPE_RV_CSR_REG
                 ? o[2].register_unit - 64u
                 : (uint32_t)o[2].immediate_bits)
            << 15u);
  case SYSTEM_SHAPE_RV_FENCE:
    return w | ((uint32_t)o[0].immediate_bits << 24u) |
           ((uint32_t)o[1].immediate_bits << 20u);
  case SYSTEM_SHAPE_RV_FENCE_REG:
    return w | (r << 15u) | ((o[1].register_unit - 64u) << 20u);
  default:
    return w;
  }
}
#endif
