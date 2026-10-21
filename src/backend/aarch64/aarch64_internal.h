#ifndef ASTERASM_AARCH64_INTERNAL_H
#define ASTERASM_AARCH64_INTERNAL_H
#include "../postra/physical_internal.h"
AstPhysicalOperand a64_stack(PhysicalContext *, uint32_t, int64_t);
AstPhysicalOperand a64_sp(PhysicalContext *);
int a64_move(PhysicalContext *, AstPhysicalOperand, AstPhysicalOperand);
int a64_parallel_copy(PhysicalContext *, PhysicalCopy *, uint32_t);
int a64_abi_entry(PhysicalContext *);
int a64_abi_call(PhysicalContext *, const MachineInstruction *, uint32_t);
int a64_abi_return(PhysicalContext *, const MachineInstruction *);
int a64_emit_epilogue(PhysicalContext *);
int a64_emit_prologue(PhysicalContext *);
int a64_resolve_frames(PhysicalContext *);
int a64_frame_layout(AstRegAllocProgram *, uint32_t);
int a64_materialize_integer(PhysicalContext *, AstPhysicalOperand, uint64_t);
/* Only the target-neutral operand constructors/emitter are shared. */
#define physical_stack a64_stack
#define physical_move a64_move
#define physical_parallel_copy a64_parallel_copy
#define physical_abi_entry a64_abi_entry
#define physical_abi_call a64_abi_call
#define physical_abi_return a64_abi_return
#define physical_emit_epilogue a64_emit_epilogue
#define physical_emit_prologue a64_emit_prologue
#define physical_resolve_frames a64_resolve_frames
#define frame_layout a64_frame_layout
#endif
