#include "../../isa/vector_contract.h"
#include "physical_internal.h"

static int reg(const AstTargetConfig *target, const AstHirNativeStmtInfo *s,
               const char *role, AstPhysicalOperand *out) {
  const AstHirNativeOperandInfo *o = system_role(s, role);
  return o && !o->is_memory &&
         physical_native_vector_register(target, o->value, out);
}
static int imm(const AstHirNativeStmtInfo *s, const char *role,
               AstPhysicalOperand *out, int optional, int64_t fallback) {
  const AstHirNativeOperandInfo *o = system_role(s, role);
  int64_t value;
  if (!o) {
    if (!optional)
      return 0;
    value = fallback;
  } else if (o->is_memory || !physical_native_vector_integer(o->value, &value))
    return 0;
  *out = physical_immediate((uint64_t)value, 64u);
  return 1;
}
static int mask(const AstHirNativeStmtInfo *s, AstPhysicalOperand *out) {
  if (!imm(s, "vm", out, 1, 1) || out->immediate_bits > 1u)
    return 0;
  if (!out->immediate_bits) {
    memset(out, 0, sizeof(*out));
    out->kind = AST_PHYSICAL_OPERAND_REGISTER;
    out->bank = AST_MACHINE_BANK_VECTOR;
    out->register_unit = 192u;
    memcpy(out->register_name, "v0", 3u);
  }
  return 1;
}
int physical_native_vector_other_info(const AstTargetConfig *target,
                                      const AstHirNativeStmtInfo *s,
                                      AstPhysicalInstructionInfo *out) {
  const VectorForm *f = vector_statement_form(target->arch, s);
  if (!f || s->attribute_count || s->kind != AST_HIR_NATIVE_INSTRUCTION)
    return 0;
  memset(out, 0, sizeof(*out));
  out->opcode = vector_opcode(target->arch);
  AstPhysicalOperand *o = out->operands;
  uint32_t count = 0u;
  out->result_count = 1u;
  if (target->arch == AST_TARGET_ARCH_AARCH64) {
    int load = f->shape == VECTOR_A64_LOAD,
        store = f->shape == VECTOR_A64_STORE;
    if (!reg(target, s, store ? "src" : "dst", &o[0]))
      return 0;
    if (load || store) {
      AstPhysicalOperand offset;
      if (!reg(target, s, "addr", &o[1])) {
        const AstHirNativeOperandInfo *address = system_role(s, "addr");
        AstHirNativeExprInfo e;
        if (!address || address->is_memory ||
            !ast_hir_native_expr_info(address->value, &e) ||
            e.kind != AST_HIR_NATIVE_EXPR_NAME || !system_name(e.name, "sp"))
          return 0;
        memset(&o[1], 0, sizeof(o[1]));
        o[1].kind = AST_PHYSICAL_OPERAND_REGISTER;
        o[1].bank = AST_MACHINE_BANK_SPECIAL;
        o[1].register_unit = 320u;
        o[1].bit_width = 64u;
        memcpy(o[1].register_name, "sp", 3u);
      }
      if ((o[1].bank != AST_MACHINE_BANK_GPR && o[1].register_unit != 320u) ||
          o[1].bit_width != 64u || !imm(s, "offset", &offset, 1, 0))
        return 0;
      count = 2u + (uint32_t)(system_role(s, "offset") != NULL);
      o[1].kind = AST_PHYSICAL_OPERAND_MEMORY;
      o[1].bit_width = o[0].bit_width;
      o[1].displacement = (int64_t)offset.immediate_bits;
      out->operand_count = 2u;
      if (store)
        out->result_count = 0u;
    } else {
      int logic = f->shape == VECTOR_A64_LOGIC;
      if (!reg(target, s, "lhs", &o[1]) || !reg(target, s, "rhs", &o[2]) ||
          !imm(s, "bits", &o[3], logic, 8))
        return 0;
      count = 3u + (uint32_t)(system_role(s, "bits") != NULL);
      out->operand_count = 4u;
    }
  } else if (target->arch == AST_TARGET_ARCH_RISCV64) {
    if (f->shape == VECTOR_RV_CONFIG || f->shape == VECTOR_RV_ICONFIG ||
        f->shape == VECTOR_RV_RCONFIG) {
      if (!reg(target, s, "dst", &o[0]) ||
          (f->shape == VECTOR_RV_ICONFIG ? !imm(s, "avl", &o[1], 0, 0)
                                         : !reg(target, s, "avl", &o[1])) ||
          (f->shape == VECTOR_RV_RCONFIG ? !reg(target, s, "vtype", &o[2])
                                         : !imm(s, "vtype", &o[2], 0, 0)) ||
          (o[0].register_unit >= 65u && o[0].register_unit <= 68u))
        return 0;
      count = out->operand_count = 3u;
    } else if (f->shape == VECTOR_RV_LOAD || f->shape == VECTOR_RV_STORE) {
      int load = f->shape == VECTOR_RV_LOAD;
      if (!reg(target, s, load ? "dst" : "src", &o[0]) ||
          !reg(target, s, "addr", &o[1]) || !mask(s, &o[2]))
        return 0;
      count = 2u + (uint32_t)(system_role(s, "vm") != NULL);
      out->operand_count = 3u;
      if (load)
        o[out->operand_count++] = o[0];
      else
        out->result_count = 0u;
    } else {
      if (!reg(target, s, "dst", &o[0]) || !reg(target, s, "lhs", &o[1]) ||
          ((f->shape == VECTOR_RV_VI || f->shape == VECTOR_RV_UIMM)
               ? !imm(s, "rhs", &o[2], 0, 0)
               : !reg(target, s, "rhs", &o[2])) ||
          !mask(s, &o[3]))
        return 0;
      o[4] = o[0];
      out->operand_count = 5u;
      count = 3u + (uint32_t)(system_role(s, "vm") != NULL);
    }
  } else
    return 0;
  if (count != s->operand_count)
    return 0;
  o[out->operand_count++] =
      physical_immediate((uint64_t)(f - vector_forms), 16u);
  return vector_physical_contract(out) && vector_features(target, out);
}
