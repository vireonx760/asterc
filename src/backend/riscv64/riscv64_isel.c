#include "../common/machine_internal.h"

static const AstSemaType *rv_operand_type(const MachineFunction *f,
                                          const MachineInstruction *i) {
  for (uint32_t n = 0; n < i->info.operand_count; ++n)
    if (i->info.operands[n].kind == AST_MACHINE_OPERAND_VREG)
      return f->vregs[i->info.operands[n].as.vreg].type;
  return i->info.result_count ? f->vregs[i->info.result_first].type : NULL;
}
static AstMachineOpcode rv_binary(const MachineFunction *f,
                                  const MachineInstruction *i) {
  const AstSemaType *t = rv_operand_type(f, i);
  int fp = t && ast_sema_type_kind(t) == AST_SEMA_TYPE_FLOAT;
  int wide = t && ast_sema_type_bit_width(t) == 64u;
  int word = t && ast_sema_type_bit_width(t) == 32u;
  int sign = t && ast_sema_type_is_signed(t);
  if (fp)
    switch (i->info.token_op) {
    case AST_TOK_PLUS:
      return wide ? AST_MACHINE_RISCV_FADD_D : AST_MACHINE_RISCV_FADD_S;
    case AST_TOK_MINUS:
      return wide ? AST_MACHINE_RISCV_FSUB_D : AST_MACHINE_RISCV_FSUB_S;
    case AST_TOK_STAR:
      return wide ? AST_MACHINE_RISCV_FMUL_D : AST_MACHINE_RISCV_FMUL_S;
    case AST_TOK_SLASH:
      return wide ? AST_MACHINE_RISCV_FDIV_D : AST_MACHINE_RISCV_FDIV_S;
    case AST_TOK_EQ_EQ:
    case AST_TOK_BANG_EQ:
    case AST_TOK_LT:
    case AST_TOK_LT_EQ:
    case AST_TOK_GT:
    case AST_TOK_GT_EQ:
      return AST_MACHINE_RISCV_FCMP;
    default:
      return AST_MACHINE_RISCV_INTRINSIC;
    }
  switch (i->info.token_op) {
  case AST_TOK_PLUS:
    return word ? AST_MACHINE_RISCV_ADDW : AST_MACHINE_RISCV_ADD;
  case AST_TOK_MINUS:
    return word ? AST_MACHINE_RISCV_SUBW : AST_MACHINE_RISCV_SUB;
  case AST_TOK_STAR:
    return word ? AST_MACHINE_RISCV_MULW : AST_MACHINE_RISCV_MUL;
  case AST_TOK_SLASH:
    return sign ? (word ? AST_MACHINE_RISCV_DIVW : AST_MACHINE_RISCV_DIV)
                : (word ? AST_MACHINE_RISCV_DIVUW : AST_MACHINE_RISCV_DIVU);
  case AST_TOK_PERCENT:
    return sign ? (word ? AST_MACHINE_RISCV_REMW : AST_MACHINE_RISCV_REM)
                : (word ? AST_MACHINE_RISCV_REMUW : AST_MACHINE_RISCV_REMU);
  case AST_TOK_AMP:
  case AST_TOK_AMP_AMP:
    return AST_MACHINE_RISCV_AND;
  case AST_TOK_PIPE:
  case AST_TOK_PIPE_PIPE:
    return AST_MACHINE_RISCV_OR;
  case AST_TOK_CARET:
    return AST_MACHINE_RISCV_XOR;
  case AST_TOK_SHIFT_LEFT:
    return word ? AST_MACHINE_RISCV_SLLW : AST_MACHINE_RISCV_SLL;
  case AST_TOK_SHIFT_RIGHT:
    return sign ? (word ? AST_MACHINE_RISCV_SRAW : AST_MACHINE_RISCV_SRA)
                : (word ? AST_MACHINE_RISCV_SRLW : AST_MACHINE_RISCV_SRL);
  default:
    return AST_MACHINE_RISCV_CMP;
  }
}
AstMachineOpcode machine_rv_opcode(const MachineFunction *f,
                                   const MachineInstruction *i) {
  switch (i->info.opcode) {
  case AST_MACHINE_G_CONSTANT:
  case AST_MACHINE_G_GENERIC_CONST:
    return AST_MACHINE_RISCV_LI;
  case AST_MACHINE_G_UNDEF:
    return AST_MACHINE_RISCV_UNDEF;
  case AST_MACHINE_G_COPY:
    return AST_MACHINE_RISCV_MV;
  case AST_MACHINE_G_STACK_ADDR:
  case AST_MACHINE_G_GLOBAL_ADDR:
  case AST_MACHINE_G_FUNCTION_ADDR:
  case AST_MACHINE_G_ADDRESS_OF:
    return AST_MACHINE_RISCV_LA;
  case AST_MACHINE_G_LOAD:
    return AST_MACHINE_RISCV_LOAD;
  case AST_MACHINE_G_STORE:
    return AST_MACHINE_RISCV_STORE;
  case AST_MACHINE_G_UNARY: {
    const AstSemaType *t = rv_operand_type(f, i);
    if (i->info.token_op == AST_TOK_MINUS && t &&
        ast_sema_type_kind(t) == AST_SEMA_TYPE_FLOAT)
      return ast_sema_type_bit_width(t) == 64u ? AST_MACHINE_RISCV_FNEG_D
                                               : AST_MACHINE_RISCV_FNEG_S;
    return i->info.token_op == AST_TOK_MINUS ? AST_MACHINE_RISCV_NEG
                                             : AST_MACHINE_RISCV_NOT;
  }
  case AST_MACHINE_G_BINARY:
    return rv_binary(f, i);
  case AST_MACHINE_G_CAST:
    return AST_MACHINE_RISCV_CAST;
  case AST_MACHINE_G_SELECT:
    return AST_MACHINE_RISCV_SELECT;
  case AST_MACHINE_G_PACK:
    return AST_MACHINE_RISCV_INTRINSIC;
  case AST_MACHINE_G_CALL_DIRECT:
    return AST_MACHINE_RISCV_CALL;
  case AST_MACHINE_G_CALL_INDIRECT:
    return AST_MACHINE_RISCV_CALL_INDIRECT;
  case AST_MACHINE_G_INDEX:
    return AST_MACHINE_RISCV_INDEX;
  case AST_MACHINE_G_MEMBER:
    return AST_MACHINE_RISCV_MEMBER;
  case AST_MACHINE_G_ATOMIC:
    return AST_MACHINE_RISCV_ATOMIC;
  case AST_MACHINE_G_FENCE:
    return AST_MACHINE_RISCV_FENCE;
  case AST_MACHINE_G_INTRINSIC:
    /* Modular integer arithmetic has the same low-bit machine
     * representation as the ordinary ALU operation. Overflow is
     * intentionally discarded by the .wrap contract. */
    if (i->info.intrinsic == AST_HIR_INTRINSIC_ADD_WRAP)
      return AST_MACHINE_RISCV_ADD;
    if (i->info.intrinsic == AST_HIR_INTRINSIC_SUB_WRAP)
      return AST_MACHINE_RISCV_SUB;
    if (i->info.intrinsic == AST_HIR_INTRINSIC_MUL_WRAP)
      return AST_MACHINE_RISCV_MUL;
    /* All current targets use 64-bit pointer representations. The
     * address conversion preserves bits; unsafe remains in effects. */
    if (i->info.intrinsic == AST_HIR_INTRINSIC_PTR_TO_INT ||
        i->info.intrinsic == AST_HIR_INTRINSIC_INT_TO_PTR) {
      if (i->info.result_count == 1u && i->info.operand_count == 1u &&
          i->info.operands[0].kind == AST_MACHINE_OPERAND_VREG &&
          f->vregs[i->info.result_first].bit_width == 64u &&
          f->vregs[i->info.operands[0].as.vreg].bit_width == 64u)
        return AST_MACHINE_RISCV_MV;
      return AST_MACHINE_RISCV_CAST;
    }
    return i->info.intrinsic == AST_HIR_INTRINSIC_SELECT
               ? AST_MACHINE_RISCV_SELECT
               : AST_MACHINE_RISCV_INTRINSIC;
  case AST_MACHINE_G_BIND:
    return AST_MACHINE_RISCV_BIND;
  case AST_MACHINE_G_NATIVE:
    return AST_MACHINE_RISCV_NATIVE;
  case AST_MACHINE_G_NATIVE_RESULT:
    return AST_MACHINE_RISCV_NATIVE_RESULT;
  case AST_MACHINE_G_BR:
    return AST_MACHINE_RISCV_J;
  case AST_MACHINE_G_COND_BR:
    return AST_MACHINE_RISCV_COND_BR;
  case AST_MACHINE_G_RET:
    return AST_MACHINE_RISCV_RET;
  case AST_MACHINE_G_UNREACHABLE:
    return AST_MACHINE_RISCV_EBREAK;
  default:
    return AST_MACHINE_OP_INVALID;
  }
}
int machine_select_riscv64(AstMachineProgram *p) {
  for (size_t f = 0; f < p->function_count; ++f)
    for (uint32_t n = 0; n < p->functions[f].info.instruction_count; ++n) {
      MachineInstruction *i = &p->functions[f].instructions[n];
      AstMachineOpcode op = machine_rv_opcode(&p->functions[f], i);
      if (op == AST_MACHINE_OP_INVALID)
        return 0;
      i->info.opcode = op;
    }
  return 1;
}
