#ifndef ASTERASM_RISCV64_BRANCH_CONTRACT_H
#define ASTERASM_RISCV64_BRANCH_CONTRACT_H
#include "system_contract.h"

static inline int rv_native_branch_funct3(AstMachineOpcode op) {
  switch (op) {
  case AST_MACHINE_RISCV_NATIVE_BEQ:
    return 0;
  case AST_MACHINE_RISCV_NATIVE_BNE:
    return 1;
  case AST_MACHINE_RISCV_NATIVE_BLT:
    return 4;
  case AST_MACHINE_RISCV_NATIVE_BGE:
    return 5;
  case AST_MACHINE_RISCV_NATIVE_BLTU:
    return 6;
  case AST_MACHINE_RISCV_NATIVE_BGEU:
    return 7;
  default:
    return -1;
  }
}
static inline AstMachineOpcode rv_native_branch_source(AstName name) {
  if (system_name(name, "beq"))
    return AST_MACHINE_RISCV_NATIVE_BEQ;
  if (system_name(name, "bne"))
    return AST_MACHINE_RISCV_NATIVE_BNE;
  if (system_name(name, "blt"))
    return AST_MACHINE_RISCV_NATIVE_BLT;
  if (system_name(name, "bge"))
    return AST_MACHINE_RISCV_NATIVE_BGE;
  if (system_name(name, "bltu"))
    return AST_MACHINE_RISCV_NATIVE_BLTU;
  if (system_name(name, "bgeu"))
    return AST_MACHINE_RISCV_NATIVE_BGEU;
  return AST_MACHINE_OP_INVALID;
}
#endif
