#ifndef ASTERASM_MACHINE_INTERNAL_H
#define ASTERASM_MACHINE_INTERNAL_H

#include "../../core/parallel.h"
#include "asterasm/machine.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MachineInstruction {
  AstMachineInstructionInfo info;
  uint32_t operand_offset;
  /* A dead instruction keeps its source identity and slot so the Machine
     IR verifier and debug/source maps remain one-to-one.  Target lowering
     simply emits no physical instruction for it. */
  uint8_t dead;
} MachineInstruction;

typedef struct MachineBlock {
  AstMachineBlockInfo info;
  AstMachineInstId *instructions;
  AstMachineVReg *arguments;
} MachineBlock;

typedef struct MachineFunction {
  AstMachineFunctionInfo info;
  MachineBlock *blocks;
  MachineInstruction *instructions;
  AstMachineVRegInfo *vregs;
  AstMachineOperand *operands;
  uint32_t operand_count;
  uint32_t operand_capacity;
  uint32_t *source_first;
  uint32_t *source_parts;
  uint32_t source_vreg_count;
  /* SSA copy propagation aliases are private to the Machine IR pipeline.
     Public query views continue to expose the original vreg identities. */
  AstMachineVReg *vreg_alias;
} MachineFunction;

struct AstMachineProgram {
  const AstAbiProgram *abi;
  const AstTargetProgram *target;
  const AstMirProgram *mir;
  AstMachineConfig config;
  AstMachineStage stage;
  MachineFunction *functions;
  size_t function_count;
  AstMachineStats stats;
};

int machine_build_function(AstMachineProgram *p, uint32_t function_index);
int machine_assign_regbanks_impl(AstMachineProgram *p);
int machine_select_x86_64(AstMachineProgram *p);
int machine_select_aarch64(AstMachineProgram *p);
int machine_select_riscv64(AstMachineProgram *p);
AstMachineOpcode machine_generic_opcode(AstMirOp op);
AstMachineOpcode machine_x86_opcode(const MachineFunction *fn,
                                    const MachineInstruction *mi);
AstMachineOpcode machine_a64_opcode(const MachineFunction *fn,
                                    const MachineInstruction *mi);
AstMachineOpcode machine_rv_opcode(const MachineFunction *fn,
                                   const MachineInstruction *mi);
int machine_opcode_is_selected(AstTargetArch arch, AstMachineOpcode op);
int machine_opcode_terminator_kind(AstMachineOpcode op);
AstMachineRegBank machine_bank_for_class(AstTargetRegClass rc);
int machine_register_bank(const AstTargetConfig *tc, const char *name,
                          AstMachineRegBank *out);
int machine_register_compatible(const AstTargetConfig *tc, const char *name,
                                AstMachineRegBank expected);
int machine_opcode_is_generic(AstMachineOpcode op);
void machine_refresh_operand_views(MachineFunction *fn);
int machine_cfg_validate(const MachineFunction *fn);
int machine_run_peephole(AstMachineProgram *p);
int machine_instruction_removable(const MachineFunction *fn,
                                  const MachineInstruction *mi);
AstMachineVReg machine_canonical_vreg(const MachineFunction *fn,
                                      AstMachineVReg vreg);

#endif
