#include "../../isa/system_contract.h"
#include "physical_internal.h"

static int integer_text(AstHirText text, uint64_t *out) {
  uint64_t value = 0u;
  uint32_t base = 10u, start = 0u, digits = 0u;
  if (!text.data || !text.length)
    return 0;
  if (text.length > 2u && text.data[0] == '0') {
    char p = (char)tolower((unsigned char)text.data[1]);
    if (p == 'x') {
      base = 16u;
      start = 2u;
    } else if (p == 'b') {
      base = 2u;
      start = 2u;
    } else if (p == 'o') {
      base = 8u;
      start = 2u;
    }
  }
  for (uint32_t j = start; j < text.length; ++j) {
    unsigned char c = (unsigned char)text.data[j];
    if (c == '_')
      continue;
    uint32_t d = c >= '0' && c <= '9'   ? (uint32_t)(c - '0')
                 : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a') + 10u
                 : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A') + 10u
                                        : UINT32_MAX;
    if (d >= base || value > (UINT64_MAX - d) / base)
      return 0;
    value = value * base + d;
    ++digits;
  }
  *out = value;
  return digits != 0u;
}
static int integer_expr(const AstHirNativeExpr *expr, uint64_t *out,
                        uint32_t depth) {
  AstHirNativeExprInfo e;
  if (depth > 16u || !expr || !ast_hir_native_expr_info(expr, &e))
    return 0;
  if (e.kind == AST_HIR_NATIVE_EXPR_CONSTANT && !e.constant.hi &&
      !e.constant.is_negative) {
    *out = e.constant.lo;
    return 1;
  }
  if (e.kind == AST_HIR_NATIVE_EXPR_LITERAL && e.token_kind == AST_TOK_INTEGER)
    return integer_text(e.literal, out);
  if (e.kind == AST_HIR_NATIVE_EXPR_UNARY && e.token_kind == AST_TOK_PLUS &&
      e.child_count == 1u)
    return integer_expr(ast_hir_native_expr_child(expr, 0u), out, depth + 1u);
  return 0;
}
static int immediate(const AstHirNativeStmtInfo *s, const char *role,
                     AstPhysicalOperand *out) {
  const AstHirNativeOperandInfo *o = system_role(s, role);
  uint64_t v;
  if (!o || o->is_memory || !integer_expr(o->value, &v, 0u))
    return 0;
  *out = physical_immediate(v, 64u);
  return 1;
}
static int system_register(const AstTargetConfig *target,
                           const AstHirNativeStmtInfo *s, const char *role,
                           AstPhysicalOperand *out) {
  const AstHirNativeOperandInfo *o = system_role(s, role);
  AstHirNativeExprInfo e;
  if (!o || o->is_memory)
    return 0;
  if (immediate(s, role, out))
    return 1;
  if (!ast_hir_native_expr_info(o->value, &e) ||
      e.kind != AST_HIR_NATIVE_EXPR_NAME ||
      e.binding_kind != AST_HIR_NATIVE_BINDING_TARGET)
    return 0;
#define AST_SYSTEM_REGISTER(architecture, spelling, code)                      \
  if (target->arch == AST_TARGET_ARCH_##architecture &&                        \
      system_name(e.name, spelling)) {                                         \
    *out = physical_immediate(code, 64u);                                      \
    return 1;                                                                  \
  }
#include "asterasm/detail/system_registers.inc"
#undef AST_SYSTEM_REGISTER
  return 0;
}
static int reg(const AstTargetConfig *target, const NativeBindings *bindings,
               const AstHirNativeStmtInfo *s, const char *role, uint32_t width,
               int writing, AstPhysicalOperand *out) {
  const AstHirNativeOperandInfo *o = system_role(s, role);
  AstHirNativeExprInfo e;
  MachinePhysRegView v;
  char name[AST_MACHINE_REGISTER_NAME_CAP];
  if (!o || o->is_memory || !ast_hir_native_expr_info(o->value, &e) ||
      e.kind != AST_HIR_NATIVE_EXPR_NAME)
    return 0;
  if (bindings && e.binding_kind == AST_HIR_NATIVE_BINDING_INPUT) {
    if (writing || e.binding_index >= bindings->input_count)
      return 0;
    *out = bindings->inputs[e.binding_index];
  } else if (bindings && e.binding_kind == AST_HIR_NATIVE_BINDING_OUTPUT) {
    if (e.binding_index >= bindings->output_count ||
        (!writing && !bindings->defined[e.binding_index]))
      return 0;
    *out = bindings->outputs[e.binding_index];
  } else
    goto hardware;
  if (out->bank != AST_MACHINE_BANK_GPR ||
      !machine_physreg_name(target, out->bank, out->register_unit % 64u, width,
                            out->register_name))
    return 0;
  out->bit_width = width;
  return 1;
hardware:
  if (e.binding_kind != AST_HIR_NATIVE_BINDING_TARGET || !e.name.text ||
      !e.name.span.length || e.name.span.length >= sizeof(name))
    return 0;
  for (uint32_t j = 0; j < e.name.span.length; ++j)
    name[j] = (char)tolower((unsigned char)e.name.text[j]);
  name[e.name.span.length] = 0;
  if (!machine_physreg_view(target, name, &v) || v.width != width ||
      (v.file != MACHINE_PHYSREG_FILE_GPR &&
       !(target->arch == AST_TARGET_ARCH_AARCH64 && !strcmp(name, "xzr"))) ||
      v.index >= (target->arch == AST_TARGET_ARCH_X86_64 ? 16u : 32u) ||
      (writing && target->arch == AST_TARGET_ARCH_AARCH64 &&
       (v.index == 18u || v.index == 30u)) ||
      (writing && target->arch == AST_TARGET_ARCH_RISCV64 && v.index >= 1u &&
       v.index <= 4u) ||
      (writing && target->arch == AST_TARGET_ARCH_X86_64 && v.index == 4u))
    return 0;
  memset(out, 0, sizeof(*out));
  out->kind = AST_PHYSICAL_OPERAND_REGISTER;
  out->bank = v.file == MACHINE_PHYSREG_FILE_SPECIAL ? AST_MACHINE_BANK_SPECIAL
                                                     : AST_MACHINE_BANK_GPR;
  out->bit_width = width;
  out->register_unit = v.alias_unit;
  memcpy(out->register_name, name, e.name.span.length + 1u);
  return 1;
}
/* Reconstruct a canonical instruction directly from immutable native HIR.
   No allocator state, implicit scratch or opaque bytes participate. */
static int parse(const AstTargetConfig *target, const AstHirNativeStmtInfo *s,
                 const NativeBindings *bindings,
                 AstPhysicalInstructionInfo *i) {
  const SystemForm *f = system_source_form(target->arch, s);
  if (!f || s->kind != AST_HIR_NATIVE_INSTRUCTION || s->attribute_count ||
      (f->feature & ~target->features))
    return 0;
  memset(i, 0, sizeof(*i));
  i->opcode = system_opcode(target->arch);
  AstPhysicalOperand *o = i->operands;
  uint32_t source_count = 0u;
  switch (f->shape) {
  case SYSTEM_SHAPE_FIXED:
  case SYSTEM_SHAPE_EXCEPTION_RETURN:
    break;
  case SYSTEM_SHAPE_IMPLICIT:
    for (uint32_t pass = 0; pass < 2u; ++pass) {
      static const char *const names[] = {"eax", "ecx", "edx", "ebx"};
      uint32_t mask = pass ? f->uses : f->definitions;
      for (uint32_t r = 0; r < 4u; ++r)
        if (mask & (1u << r)) {
          AstPhysicalOperand *p = &o[i->operand_count++];
          p->kind = AST_PHYSICAL_OPERAND_REGISTER;
          p->bank = AST_MACHINE_BANK_GPR;
          p->bit_width = 32u;
          p->register_unit = 64u + r;
          memcpy(p->register_name, names[r], 4u);
          if (!pass)
            ++i->result_count;
        }
    }
    break;
  case SYSTEM_SHAPE_X86_READ_SPECIAL:
  case SYSTEM_SHAPE_X86_WRITE_SPECIAL: {
    int reading = f->shape == SYSTEM_SHAPE_X86_READ_SPECIAL;
    char bank;
    uint32_t number;
    source_count = 2u;
    i->operand_count = 2u;
    i->result_count = reading ? 1u : 0u;
    if (!reg(target, bindings, s, reading ? "dst" : "src", 64u, reading,
             &o[0]) ||
        !system_special_name(system_role(s, reading ? "src" : "dst"), &bank,
                             &number))
      return 0;
    o[1] = physical_immediate(number, 64u);
    break;
  }
  case SYSTEM_SHAPE_X86_ADDRESS:
    source_count = 1u;
    i->operand_count = 1u;
    if (!reg(target, bindings, s, "addr", 64u, 0, &o[0]))
      return 0;
    break;
  case SYSTEM_SHAPE_X86_SELECTOR_READ:
  case SYSTEM_SHAPE_X86_SELECTOR_WRITE: {
    int reading = f->shape == SYSTEM_SHAPE_X86_SELECTOR_READ;
    source_count = 1u;
    i->operand_count = 1u;
    i->result_count = reading ? 1u : 0u;
    if (!reg(target, bindings, s, reading ? "dst" : "src", 16u, reading, &o[0]))
      return 0;
    break;
  }
  case SYSTEM_SHAPE_A64_READ_SYSREG:
  case SYSTEM_SHAPE_A64_WRITE_SYSREG: {
    int reading = f->shape == SYSTEM_SHAPE_A64_READ_SYSREG;
    source_count = 2u;
    i->operand_count = 2u;
    i->result_count = reading ? 1u : 0u;
    if (!reg(target, bindings, s, reading ? "dst" : "src", 64u, reading,
             &o[0]) ||
        !system_register(target, s, "sys", &o[1]))
      return 0;
    break;
  }
  case SYSTEM_SHAPE_A64_SYS:
  case SYSTEM_SHAPE_A64_SYSL: {
    int reading = f->shape == SYSTEM_SHAPE_A64_SYSL;
    source_count = 5u;
    i->operand_count = 5u;
    i->result_count = reading ? 1u : 0u;
    if (!reg(target, bindings, s, reading ? "dst" : "src", 64u, reading,
             &o[0]) ||
        !immediate(s, "op1", &o[1]) || !immediate(s, "crn", &o[2]) ||
        !immediate(s, "crm", &o[3]) || !immediate(s, "op2", &o[4]))
      return 0;
    break;
  }
  case SYSTEM_SHAPE_A64_BARRIER:
    source_count = 1u;
    i->operand_count = 1u;
    if (!immediate(s, "option", &o[0]))
      return 0;
    break;
  case SYSTEM_SHAPE_A64_EXCEPTION:
    source_count = 1u;
    i->operand_count = 1u;
    if (!immediate(s, "imm", &o[0]))
      return 0;
    break;
  case SYSTEM_SHAPE_A64_PSTATE:
    source_count = 2u;
    i->operand_count = 1u;
    if (!immediate(s, "imm", &o[0]))
      return 0;
    break;
  case SYSTEM_SHAPE_RV_CSR_REG:
  case SYSTEM_SHAPE_RV_CSR_IMM:
    source_count = 3u;
    i->operand_count = 3u;
    i->result_count = 1u;
    if (!reg(target, bindings, s, "dst", 64u, 1, &o[0]) ||
        !system_register(target, s, "csr", &o[1]) ||
        (f->shape == SYSTEM_SHAPE_RV_CSR_REG
             ? !reg(target, bindings, s, "src", 64u, 0, &o[2])
             : !immediate(s, "imm", &o[2])))
      return 0;
    break;
  case SYSTEM_SHAPE_RV_FENCE:
    source_count = 2u;
    i->operand_count = 2u;
    if (!immediate(s, "pred", &o[0]) || !immediate(s, "succ", &o[1]))
      return 0;
    break;
  case SYSTEM_SHAPE_RV_FENCE_REG:
    source_count = 2u;
    i->operand_count = 2u;
    if (!reg(target, bindings, s, "addr", 64u, 0, &o[0]) ||
        !reg(target, bindings, s,
             f == &system_forms[SYSTEM_RV_HFENCE_GVMA] ||
                     f == &system_forms[SYSTEM_RV_HINVAL_GVMA]
                 ? "vmid"
                 : "asid",
             64u, 0, &o[1]))
      return 0;
    break;
  }
  if (source_count != s->operand_count)
    return 0;
  o[i->operand_count++] = physical_immediate((uint64_t)(f - system_forms), 16u);
  return system_physical_contract(i);
}
int physical_native_system(PhysicalContext *c, const AstHirNativeStmtInfo *s,
                           int emit) {
  AstPhysicalInstructionInfo i;
  if (!parse(&c->program->target, s, NULL, &i))
    return 0;
  if (!emit)
    return 1;
  if (!physical_emit(c, i.opcode, i.result_count, i.operands, i.operand_count))
    return 0;
  PhysicalBlock *b = &c->fn->blocks[c->block];
  b->instructions[b->count - 1u].span = s->span;
  return 1;
}
int physical_native_system_info(const AstTargetConfig *target,
                                const AstHirNativeStmtInfo *s,
                                AstPhysicalInstructionInfo *out) {
  return parse(target, s, NULL, out);
}
int physical_native_system_bound_info(const AstTargetConfig *target,
                                      const AstHirNativeStmtInfo *s,
                                      const NativeBindings *bindings,
                                      AstPhysicalInstructionInfo *out) {
  return parse(target, s, bindings, out);
}
int physical_verify_system_source(const AstRegAllocProgram *p,
                                  const AstHirNativeStmtInfo *s,
                                  const AstPhysicalInstructionInfo *actual) {
  AstPhysicalInstructionInfo expected;
  if (!parse(&p->target, s, NULL, &expected) ||
      expected.opcode != actual->opcode ||
      expected.result_count != actual->result_count ||
      expected.operand_count != actual->operand_count ||
      !system_physical_contract(actual))
    return 0;
  for (uint32_t j = 0; j < expected.operand_count; ++j) {
    const AstPhysicalOperand *a = &expected.operands[j],
                             *b = &actual->operands[j];
    if (a->kind != b->kind || a->bank != b->bank ||
        a->bit_width != b->bit_width || a->register_unit != b->register_unit ||
        a->immediate_bits != b->immediate_bits ||
        memcmp(a->register_name, b->register_name, sizeof(a->register_name)))
      return 0;
  }
  return 1;
}
