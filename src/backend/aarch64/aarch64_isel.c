#include "machine_internal.h"

static int type_is_float(const AstSemaType *t) {
  return t && ast_sema_type_kind(t) == AST_SEMA_TYPE_FLOAT;
}
static int type_is_vector(const AstSemaType *t) {
  if (!t)
    return 0;
  AstSemaTypeKind k = ast_sema_type_kind(t);
  return k == AST_SEMA_TYPE_VECTOR || k == AST_SEMA_TYPE_SVECTOR ||
         k == AST_SEMA_TYPE_PRED;
}
static const AstSemaType *result_type(const MachineFunction *fn,
                                      const MachineInstruction *mi) {
  if (mi->info.result_first == AST_MACHINE_INVALID_ID ||
      mi->info.result_first >= fn->info.vreg_count)
    return NULL;
  return fn->vregs[mi->info.result_first].type;
}
static const AstSemaType *operand_type(const MachineFunction *fn,
                                       const MachineInstruction *mi) {
  for (uint32_t i = 0; i < mi->info.operand_count; ++i) {
    const AstMachineOperand *o = &mi->info.operands[i];
    if (o->kind == AST_MACHINE_OPERAND_VREG && o->as.vreg < fn->info.vreg_count)
      return fn->vregs[o->as.vreg].type;
  }
  return NULL;
}
static AstMachineOpcode select_binary(const MachineFunction *fn,
                                      const MachineInstruction *mi) {
  const AstSemaType *t = operand_type(fn, mi);
  if (!t)
    t = result_type(fn, mi);
  int vec = type_is_vector(t), fp = type_is_float(t);
  if (vec) {
    switch (mi->info.token_op) {
    case AST_TOK_PLUS:
      return AST_MACHINE_A64_VEC_ADD;
    case AST_TOK_MINUS:
      return AST_MACHINE_A64_VEC_SUB;
    case AST_TOK_STAR:
      return AST_MACHINE_A64_VEC_MUL;
    case AST_TOK_AMP:
      return AST_MACHINE_A64_VEC_AND;
    case AST_TOK_PIPE:
      return AST_MACHINE_A64_VEC_OR;
    case AST_TOK_CARET:
      return AST_MACHINE_A64_VEC_XOR;
    case AST_TOK_EQ_EQ:
    case AST_TOK_BANG_EQ:
    case AST_TOK_LT:
    case AST_TOK_LT_EQ:
    case AST_TOK_GT:
    case AST_TOK_GT_EQ:
      return AST_MACHINE_A64_VEC_CMP;
    default:
      return AST_MACHINE_A64_INTRINSIC;
    }
  }
  if (fp) {
    switch (mi->info.token_op) {
    case AST_TOK_PLUS:
      return AST_MACHINE_A64_FADD;
    case AST_TOK_MINUS:
      return AST_MACHINE_A64_FSUB;
    case AST_TOK_STAR:
      return AST_MACHINE_A64_FMUL;
    case AST_TOK_SLASH:
      return AST_MACHINE_A64_FDIV;
    case AST_TOK_PERCENT:
      return AST_MACHINE_A64_FREM;
    case AST_TOK_EQ_EQ:
      return AST_MACHINE_A64_FCMP_EQ;
    case AST_TOK_BANG_EQ:
      return AST_MACHINE_A64_FCMP_NE;
    case AST_TOK_LT:
      return AST_MACHINE_A64_FCMP_LT;
    case AST_TOK_LT_EQ:
      return AST_MACHINE_A64_FCMP_LE;
    case AST_TOK_GT:
      return AST_MACHINE_A64_FCMP_GT;
    case AST_TOK_GT_EQ:
      return AST_MACHINE_A64_FCMP_GE;
    default:
      return AST_MACHINE_A64_INTRINSIC;
    }
  }
  int s = t ? ast_sema_type_is_signed(t) : 0;
  switch (mi->info.token_op) {
  case AST_TOK_PLUS:
    return AST_MACHINE_A64_ADD;
  case AST_TOK_MINUS:
    return AST_MACHINE_A64_SUB;
  case AST_TOK_STAR:
    return AST_MACHINE_A64_MUL;
  case AST_TOK_SLASH:
    return s ? AST_MACHINE_A64_SDIV : AST_MACHINE_A64_UDIV;
  case AST_TOK_PERCENT:
    return s ? AST_MACHINE_A64_SREM : AST_MACHINE_A64_UREM;
  case AST_TOK_AMP:
  case AST_TOK_AMP_AMP:
    return AST_MACHINE_A64_AND;
  case AST_TOK_PIPE:
  case AST_TOK_PIPE_PIPE:
    return AST_MACHINE_A64_OR;
  case AST_TOK_CARET:
    return AST_MACHINE_A64_XOR;
  case AST_TOK_SHIFT_LEFT:
    return AST_MACHINE_A64_SHL;
  case AST_TOK_SHIFT_RIGHT:
    return s ? AST_MACHINE_A64_ASHR : AST_MACHINE_A64_LSHR;
  case AST_TOK_EQ_EQ:
    return AST_MACHINE_A64_ICMP_EQ;
  case AST_TOK_BANG_EQ:
    return AST_MACHINE_A64_ICMP_NE;
  case AST_TOK_LT:
    return AST_MACHINE_A64_ICMP_LT;
  case AST_TOK_LT_EQ:
    return AST_MACHINE_A64_ICMP_LE;
  case AST_TOK_GT:
    return AST_MACHINE_A64_ICMP_GT;
  case AST_TOK_GT_EQ:
    return AST_MACHINE_A64_ICMP_GE;
  default:
    return AST_MACHINE_A64_INTRINSIC;
  }
}
AstMachineOpcode machine_a64_opcode(const MachineFunction *fn,
                                    const MachineInstruction *mi) {
  switch (mi->info.opcode) {
  case AST_MACHINE_G_CONSTANT:
    return AST_MACHINE_A64_MOV_IMM;
  case AST_MACHINE_G_UNDEF:
    return AST_MACHINE_A64_UNDEF;
  case AST_MACHINE_G_COPY:
    return AST_MACHINE_A64_MOV;
  case AST_MACHINE_G_STACK_ADDR:
  case AST_MACHINE_G_GLOBAL_ADDR:
  case AST_MACHINE_G_FUNCTION_ADDR:
  case AST_MACHINE_G_ADDRESS_OF:
    return AST_MACHINE_A64_LEA;
  case AST_MACHINE_G_LOAD:
    return AST_MACHINE_A64_LOAD;
  case AST_MACHINE_G_STORE:
    return AST_MACHINE_A64_STORE;
  case AST_MACHINE_G_GENERIC_CONST:
    return AST_MACHINE_A64_MOV_IMM;
  case AST_MACHINE_G_UNARY:
    if (mi->info.token_op == AST_TOK_MINUS)
      return AST_MACHINE_A64_NEG;
    if (mi->info.token_op == AST_TOK_TILDE || mi->info.token_op == AST_TOK_BANG)
      return AST_MACHINE_A64_NOT;
    return AST_MACHINE_A64_INTRINSIC;
  case AST_MACHINE_G_BINARY:
    return select_binary(fn, mi);
  case AST_MACHINE_G_CAST:
    return AST_MACHINE_A64_CAST;
  case AST_MACHINE_G_SELECT:
    return AST_MACHINE_A64_SELECT;
  case AST_MACHINE_G_PACK:
    return AST_MACHINE_A64_INTRINSIC;
  case AST_MACHINE_G_CALL_DIRECT:
    return AST_MACHINE_A64_CALL;
  case AST_MACHINE_G_CALL_INDIRECT:
    return AST_MACHINE_A64_CALL_INDIRECT;
  case AST_MACHINE_G_INDEX:
    return AST_MACHINE_A64_INDEX;
  case AST_MACHINE_G_MEMBER:
    return AST_MACHINE_A64_MEMBER;
  case AST_MACHINE_G_ATOMIC:
    return AST_MACHINE_A64_ATOMIC;
  case AST_MACHINE_G_FENCE:
    return AST_MACHINE_A64_FENCE;
  case AST_MACHINE_G_INTRINSIC:
    /* Modular integer arithmetic has the same low-bit machine
     * representation as the ordinary ALU operation. Overflow is
     * intentionally discarded by the .wrap contract. */
    if (mi->info.intrinsic == AST_HIR_INTRINSIC_ADD_WRAP)
      return AST_MACHINE_A64_ADD;
    if (mi->info.intrinsic == AST_HIR_INTRINSIC_SUB_WRAP)
      return AST_MACHINE_A64_SUB;
    if (mi->info.intrinsic == AST_HIR_INTRINSIC_MUL_WRAP)
      return AST_MACHINE_A64_MUL;
    /* All current targets use 64-bit pointer representations. The
     * address conversion preserves bits; unsafe remains in effects. */
    if (mi->info.intrinsic == AST_HIR_INTRINSIC_PTR_TO_INT ||
        mi->info.intrinsic == AST_HIR_INTRINSIC_INT_TO_PTR) {
      if (mi->info.result_count == 1u && mi->info.operand_count == 1u &&
          mi->info.operands[0].kind == AST_MACHINE_OPERAND_VREG &&
          fn->vregs[mi->info.result_first].bit_width == 64u &&
          fn->vregs[mi->info.operands[0].as.vreg].bit_width == 64u)
        return AST_MACHINE_A64_MOV;
      return AST_MACHINE_A64_CAST;
    }
    return mi->info.intrinsic == AST_HIR_INTRINSIC_SELECT
               ? AST_MACHINE_A64_SELECT
               : AST_MACHINE_A64_INTRINSIC;
  case AST_MACHINE_G_BIND:
    return AST_MACHINE_A64_BIND;
  case AST_MACHINE_G_NATIVE:
    return AST_MACHINE_A64_NATIVE;
  case AST_MACHINE_G_NATIVE_RESULT:
    return AST_MACHINE_A64_NATIVE_RESULT;
  case AST_MACHINE_G_BR:
    return AST_MACHINE_A64_BR;
  case AST_MACHINE_G_COND_BR:
    return AST_MACHINE_A64_COND_BR;
  case AST_MACHINE_G_RET:
    return AST_MACHINE_A64_RET;
  case AST_MACHINE_G_UNREACHABLE:
    return AST_MACHINE_A64_TRAP;
  default:
    return AST_MACHINE_OP_INVALID;
  }
}
int machine_select_aarch64(AstMachineProgram *p) {
  for (size_t fi = 0; fi < p->function_count; ++fi) {
    MachineFunction *fn = &p->functions[fi];
    for (uint32_t i = 0; i < fn->info.instruction_count; ++i) {
      AstMachineOpcode op = machine_a64_opcode(fn, &fn->instructions[i]);
      if (op == AST_MACHINE_OP_INVALID)
        return 0;
      fn->instructions[i].info.opcode = op;
    }
  }
  return 1;
}
