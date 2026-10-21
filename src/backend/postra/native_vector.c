#include "../../isa/vector_contract.h"
#include "physical_internal.h"

/* Source reconstruction deliberately accepts architectural register names.
   Typed bindings enter and leave through the existing native MOV bridge. */
static int reg(const AstTargetConfig *target, const AstHirNativeExpr *expr,
               AstPhysicalOperand *out) {
  AstHirNativeExprInfo e;
  MachinePhysRegView v;
  char name[AST_MACHINE_REGISTER_NAME_CAP];
  if (!expr || !ast_hir_native_expr_info(expr, &e) ||
      e.kind != AST_HIR_NATIVE_EXPR_NAME ||
      e.binding_kind != AST_HIR_NATIVE_BINDING_TARGET || !e.name.text ||
      !e.name.span.length || e.name.span.length >= sizeof(name))
    return 0;
  for (uint32_t j = 0; j < e.name.span.length; ++j)
    name[j] = (char)tolower((unsigned char)e.name.text[j]);
  name[e.name.span.length] = 0;
  if (!machine_physreg_view(target, name, &v) || v.special || v.index >= 32u ||
      (v.file != MACHINE_PHYSREG_FILE_GPR &&
       v.file != MACHINE_PHYSREG_FILE_FP_VECTOR &&
       v.file != MACHINE_PHYSREG_FILE_VECTOR &&
       v.file != MACHINE_PHYSREG_FILE_MASK_PREDICATE))
    return 0;
  memset(out, 0, sizeof(*out));
  out->kind = AST_PHYSICAL_OPERAND_REGISTER;
  out->bank = v.file == MACHINE_PHYSREG_FILE_GPR ? AST_MACHINE_BANK_GPR
              : v.file == MACHINE_PHYSREG_FILE_MASK_PREDICATE
                  ? AST_MACHINE_BANK_MASK
                  : AST_MACHINE_BANK_VECTOR;
  out->bit_width = v.width;
  out->register_unit = v.alias_unit;
  memcpy(out->register_name, name, e.name.span.length + 1u);
  return 1;
}
static int integer(const AstHirNativeExpr *expr, int64_t *value,
                   uint32_t depth) {
  AstHirNativeExprInfo e;
  if (depth > 16u || !expr || !ast_hir_native_expr_info(expr, &e))
    return 0;
  if (e.kind == AST_HIR_NATIVE_EXPR_CONSTANT && !e.constant.hi &&
      e.constant.lo <= INT64_MAX) {
    *value = e.constant.is_negative ? -(int64_t)e.constant.lo
                                    : (int64_t)e.constant.lo;
    return 1;
  }
  if (e.kind == AST_HIR_NATIVE_EXPR_UNARY && e.child_count == 1u &&
      (e.token_kind == AST_TOK_PLUS || e.token_kind == AST_TOK_MINUS)) {
    if (!integer(ast_hir_native_expr_child(expr, 0u), value, depth + 1u))
      return 0;
    if (e.token_kind == AST_TOK_MINUS) {
      if (*value == INT64_MIN)
        return 0;
      *value = -*value;
    }
    return 1;
  }
  if (e.kind != AST_HIR_NATIVE_EXPR_LITERAL ||
      e.token_kind != AST_TOK_INTEGER || !e.literal.length)
    return 0;
  uint64_t n = 0u;
  uint32_t base = 10u, start = 0u, digits = 0u;
  if (e.literal.length > 2u && e.literal.data[0] == '0') {
    int p = tolower((unsigned char)e.literal.data[1]);
    if (p == 'x') {
      base = 16u;
      start = 2u;
    }
    if (p == 'b') {
      base = 2u;
      start = 2u;
    }
    if (p == 'o') {
      base = 8u;
      start = 2u;
    }
  }
  for (uint32_t j = start; j < e.literal.length; ++j) {
    unsigned char c = (unsigned char)e.literal.data[j];
    if (c == '_')
      continue;
    uint32_t d = c >= '0' && c <= '9' ? (uint32_t)(c - '0')
                                      : (uint32_t)tolower(c) - 'a' + 10u;
    if (d >= base || n > ((uint64_t)INT64_MAX - d) / base)
      return 0;
    n = n * base + d;
    ++digits;
  }
  *value = (int64_t)n;
  return digits != 0u;
}
int physical_native_vector_register(const AstTargetConfig *target,
                                    const AstHirNativeExpr *expr,
                                    AstPhysicalOperand *out) {
  return reg(target, expr, out);
}
int physical_native_vector_integer(const AstHirNativeExpr *expr, int64_t *out) {
  return integer(expr, out, 0u);
}
typedef struct VectorAddress {
  AstPhysicalOperand base, index;
  int64_t displacement;
  uint8_t scale;
} VectorAddress;
static int address(const AstTargetConfig *target, const AstHirNativeExpr *expr,
                   int sign, VectorAddress *a, uint32_t depth) {
  AstHirNativeExprInfo e;
  if (depth > 32u || !expr || !ast_hir_native_expr_info(expr, &e))
    return 0;
  if (e.kind == AST_HIR_NATIVE_EXPR_BINARY && e.child_count == 2u &&
      (e.token_kind == AST_TOK_PLUS || e.token_kind == AST_TOK_MINUS))
    return address(target, ast_hir_native_expr_child(expr, 0u), sign, a,
                   depth + 1u) &&
           address(target, ast_hir_native_expr_child(expr, 1u),
                   e.token_kind == AST_TOK_MINUS ? -sign : sign, a, depth + 1u);
  if (e.kind == AST_HIR_NATIVE_EXPR_BINARY && e.child_count == 2u &&
      e.token_kind == AST_TOK_STAR) {
    AstPhysicalOperand r;
    int64_t scale;
    const AstHirNativeExpr *x = ast_hir_native_expr_child(expr, 0u),
                           *y = ast_hir_native_expr_child(expr, 1u);
    if (sign < 0 || a->index.kind)
      return 0;
    if (!(reg(target, x, &r) && integer(y, &scale, 0u)) &&
        !(reg(target, y, &r) && integer(x, &scale, 0u)))
      return 0;
    if (r.bank != AST_MACHINE_BANK_GPR || r.bit_width != 64u ||
        (scale != 1 && scale != 2 && scale != 4 && scale != 8))
      return 0;
    a->index = r;
    a->scale = (uint8_t)scale;
    return 1;
  }
  int64_t n;
  if (integer(expr, &n, 0u)) {
    if (sign < 0) {
      if (n == INT64_MIN)
        return 0;
      n = -n;
    }
    if (n < INT32_MIN || n > INT32_MAX || a->displacement + n < INT32_MIN ||
        a->displacement + n > INT32_MAX)
      return 0;
    a->displacement += n;
    return 1;
  }
  AstPhysicalOperand r;
  if (sign < 0 || !reg(target, expr, &r) || r.bank != AST_MACHINE_BANK_GPR ||
      r.bit_width != 64u)
    return 0;
  if (!a->base.kind)
    a->base = r;
  else if (!a->index.kind) {
    a->index = r;
    a->scale = 1u;
  } else
    return 0;
  return 1;
}
static int operand(const AstTargetConfig *target,
                   const AstHirNativeOperandInfo *o, int manual,
                   AstPhysicalOperand *out) {
  if (!o)
    return 0;
  if (!o->is_memory)
    return reg(target, o->value, out);
  VectorAddress a = {0};
  uint64_t size = ast_sema_type_size(o->memory_type);
  if ((size != 16u && size != 32u && size != 64u) ||
      !address(target, o->memory_address, 1, &a, 0u) || !a.base.kind ||
      (!manual && a.base.register_unit == 68u))
    return 0;
  *out = a.base;
  out->kind = AST_PHYSICAL_OPERAND_MEMORY;
  out->bit_width = (uint32_t)size * 8u;
  out->displacement = a.displacement;
  if (a.index.kind) {
    out->scale = a.scale;
    out->index_register_unit = a.index.register_unit;
    memcpy(out->index_register_name, a.index.register_name,
           sizeof(out->index_register_name));
  }
  return vector_memory(out, out->bit_width);
}
int physical_native_vector_info(const AstTargetConfig *target,
                                const AstHirNativeStmtInfo *s, int manual,
                                AstPhysicalInstructionInfo *out) {
  const VectorForm *f = vector_statement_form(target->arch, s);
  if (!f || s->kind != AST_HIR_NATIVE_INSTRUCTION || s->attribute_count)
    return 0;
  if (target->arch != AST_TARGET_ARCH_X86_64) {
    if (!(manual || target->arch == AST_TARGET_ARCH_AARCH64) ||
        !physical_native_vector_other_info(target, s, out))
      return 0;
    return manual || out->operand_count < 2u ||
           out->operands[1].kind != AST_PHYSICAL_OPERAND_MEMORY ||
           out->operands[1].register_unit != 320u;
  }
  memset(out, 0, sizeof(*out));
  out->opcode = AST_MACHINE_X86_VECTOR;
  AstPhysicalOperand *o = out->operands;
  uint32_t count = 2u;
  if (!operand(target, system_role(s, "dst"), manual, &o[0]))
    return 0;
  if (f->shape == VECTOR_X86_VBINARY || f->shape == VECTOR_X86_EBINARY) {
    count = 3u;
    if (!operand(target, system_role(s, "lhs"), manual, &o[1]) ||
        !operand(target, system_role(s, "rhs"), manual, &o[2]))
      return 0;
  } else {
    if (!operand(target, system_role(s, "src"), manual, &o[1]))
      return 0;
    if (f->shape == VECTOR_X86_BINARY) {
      o[2] = o[1];
      o[1] = o[0];
    }
  }
  const AstHirNativeOperandInfo *mask = system_role(s, "mask"),
                                *zero = system_role(s, "zero");
  if (count + (uint32_t)(mask != NULL) + (uint32_t)(zero != NULL) !=
          s->operand_count ||
      (!vector_evex(f) && (mask || zero)))
    return 0;
  out->result_count = 1u;
  if (o[0].kind == AST_PHYSICAL_OPERAND_MEMORY &&
      (f->shape == VECTOR_X86_MOVE || f->shape == VECTOR_X86_VMOVE ||
       f->shape == VECTOR_X86_EMOVE)) {
    AstPhysicalOperand swap = o[0];
    o[0] = o[1];
    o[1] = swap;
    out->result_count = 0u;
  }
  out->operand_count =
      (f->shape == VECTOR_X86_BINARY || f->shape == VECTOR_X86_VBINARY ||
       f->shape == VECTOR_X86_EBINARY)
          ? 3u
          : 2u;
  if (vector_evex(f)) {
    AstPhysicalOperand k = physical_immediate(0u, 64u);
    int64_t z = 0;
    if (mask &&
        (!operand(target, mask, manual, &k) || k.bank != AST_MACHINE_BANK_MASK))
      return 0;
    if (mask && k.register_unit == 256u)
      k = physical_immediate(0u, 64u);
    if (zero &&
        (zero->is_memory || !integer(zero->value, &z, 0u) || z < 0 || z > 1))
      return 0;
    o[out->operand_count++] = k;
    o[out->operand_count++] = physical_immediate((uint64_t)z, 64u);
    if (k.kind == AST_PHYSICAL_OPERAND_REGISTER && !z && out->result_count)
      o[out->operand_count++] = o[0];
  }
  o[out->operand_count++] =
      physical_immediate((uint64_t)(f - vector_forms), 16u);
  return vector_physical_contract(out) && vector_features(target, out);
}
int physical_verify_vector_source(const AstRegAllocProgram *p,
                                  const AstHirNativeStmtInfo *s, int manual,
                                  const AstPhysicalInstructionInfo *actual) {
  AstPhysicalInstructionInfo expected;
  if (!physical_native_vector_info(&p->target, s, manual, &expected) ||
      expected.opcode != actual->opcode ||
      expected.result_count != actual->result_count ||
      expected.operand_count != actual->operand_count ||
      !vector_physical_contract(actual))
    return 0;
  if (p->target.arch == AST_TARGET_ARCH_RISCV64 &&
      p->abi != AST_ABI_RISCV_LP64 && p->abi != AST_ABI_RISCV_LP64F &&
      p->abi != AST_ABI_RISCV_LP64D)
    return 0;
  if (manual && p->target.arch == AST_TARGET_ARCH_AARCH64 &&
      p->abi == AST_ABI_AAPCS64_SVE)
    for (uint32_t d = 0; d < actual->result_count; ++d)
      if (actual->operands[d].bank == AST_MACHINE_BANK_VECTOR &&
          actual->operands[d].register_unit >= 136u &&
          actual->operands[d].register_unit <
              (p->abi == AST_ABI_AAPCS64_SVE ? 152u : 144u))
        return 0;
  for (uint32_t j = 0; j < expected.operand_count; ++j) {
    const AstPhysicalOperand *a = &expected.operands[j],
                             *b = &actual->operands[j];
    if (a->kind != b->kind || a->bank != b->bank ||
        a->bit_width != b->bit_width || a->register_unit != b->register_unit ||
        a->immediate_bits != b->immediate_bits ||
        a->displacement != b->displacement || a->scale != b->scale ||
        a->index_register_unit != b->index_register_unit ||
        memcmp(a->register_name, b->register_name, sizeof(a->register_name)) ||
        memcmp(a->index_register_name, b->index_register_name,
               sizeof(a->index_register_name)))
      return 0;
  }
  return 1;
}
