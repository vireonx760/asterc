#ifndef ASTERASM_RISCV64_INTERNAL_H
#define ASTERASM_RISCV64_INTERNAL_H
#include "../postra/physical_internal.h"

typedef PhysicalContext RvContext;
typedef struct RvWork {
  uint8_t excluded[MACHINE_REGISTER_UNITS];
  AstPhysicalOperand saved[AST_PHYSICAL_MAX_OPERANDS];
  uint32_t count;
} RvWork;
AstPhysicalOperand rv_reg(RvContext *c, AstMachineRegBank bank, uint32_t index,
                          uint32_t bits);
AstPhysicalOperand rv_named(RvContext *c, const char *name, uint32_t bits);
AstPhysicalOperand rv_home(RvContext *c, uint32_t value);
AstPhysicalOperand rv_memory(AstPhysicalOperand base, uint32_t bits,
                             int64_t offset);
AstPhysicalOperand rv_stack(RvContext *c, uint32_t bits, int64_t offset);
AstPhysicalOperand rv_block(uint32_t block);
int rv_emit(RvContext *c, AstMachineOpcode op, uint32_t defs,
            const AstPhysicalOperand *args, uint32_t count);
int rv_move(RvContext *c, AstPhysicalOperand dst, AstPhysicalOperand src);
int rv_address(RvContext *c, AstPhysicalOperand dst,
               AstPhysicalOperand address);
int rv_normalize(RvContext *c, AstPhysicalOperand dst, AstPhysicalOperand src,
                 uint32_t bits, int sign);
uint32_t rv_temp(RvContext *c, uint64_t bytes, AstFrameObjectKind kind);
AstPhysicalOperand rv_borrow(RvContext *c, RvWork *w, AstMachineRegBank bank,
                             uint32_t bits);
AstPhysicalOperand rv_ensure(RvContext *c, RvWork *w, AstPhysicalOperand value,
                             AstMachineRegBank bank);
int rv_restore(RvContext *c, const RvWork *w);
int rv_entry(RvContext *c);
int rv_call(RvContext *c, const MachineInstruction *mi, uint32_t call);
int rv_return(RvContext *c, const MachineInstruction *mi);
int rv_epilogue(RvContext *c);
int rv_frame_finish(RvContext *c);
#endif
