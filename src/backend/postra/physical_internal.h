#ifndef ASTERASM_PHYSICAL_INTERNAL_H
#define ASTERASM_PHYSICAL_INTERNAL_H
#include "../../abi/abi_lookup.h"
#include "../regalloc/allocation_internal.h"

typedef struct PhysicalCopy {
  AstPhysicalOperand dst, src;
} PhysicalCopy;
typedef struct PhysicalContext {
  AstRegAllocProgram *program;
  AllocationFunction *fn;
  const MachineFunction *machine;
  uint32_t function;
  uint32_t block;
  uint32_t instruction;
  uint32_t *locals;
  uint32_t *call_sites;
} PhysicalContext;

typedef struct NativeBindings {
  const AstPhysicalOperand *inputs, *outputs;
  const uint8_t *defined;
  uint32_t input_count, output_count;
} NativeBindings;

AstPhysicalOperand physical_register(PhysicalContext *c, AstMachineRegBank bank,
                                     uint32_t index, uint32_t bits);
AstPhysicalOperand physical_named_register(PhysicalContext *c, const char *name,
                                           uint32_t bits);
AstPhysicalOperand physical_frame(uint32_t object, uint32_t bits,
                                  int64_t offset);
AstPhysicalOperand physical_entry(uint32_t bits, int64_t offset);
AstPhysicalOperand physical_stack(PhysicalContext *c, uint32_t bits,
                                  int64_t offset);
AstPhysicalOperand physical_home(PhysicalContext *c, uint32_t vreg);
AstPhysicalOperand physical_home_at_slot(PhysicalContext *c, uint32_t vreg,
                                         uint32_t slot);
AstPhysicalOperand physical_immediate(uint64_t bits, uint32_t width);
int physical_emit(PhysicalContext *c, AstMachineOpcode op, uint32_t defs,
                  const AstPhysicalOperand *args, uint32_t count);
int physical_move(PhysicalContext *c, AstPhysicalOperand dst,
                  AstPhysicalOperand src);
int physical_run_peephole(PhysicalContext *c);
int physical_rv_select(PhysicalContext *c, AstPhysicalOperand dst,
                       AstPhysicalOperand condition, AstPhysicalOperand yes,
                       AstPhysicalOperand no, int *handled);
int physical_parallel_copy(PhysicalContext *c, PhysicalCopy *copies,
                           uint32_t count);
int physical_same_location(const AstPhysicalOperand *a,
                           const AstPhysicalOperand *b);
uint32_t physical_temp_object(PhysicalContext *c, uint64_t bytes,
                              AstFrameObjectKind kind);
int physical_abi_entry(PhysicalContext *c);
int physical_abi_call(PhysicalContext *c, const MachineInstruction *mi,
                      uint32_t call);
int physical_abi_return(PhysicalContext *c, const MachineInstruction *mi);
int physical_resolve_frames(PhysicalContext *c);
int physical_emit_epilogue(PhysicalContext *c);
int physical_emit_prologue(PhysicalContext *c);
int physical_atomic_source(const AstRegAllocProgram *p, uint32_t f,
                           const AstPhysicalInstructionInfo *i);
int physical_atomic_count(const AstRegAllocProgram *p, uint32_t f);
int physical_atomic_instruction(PhysicalContext *c,
                                const MachineInstruction *mi);
int physical_native_instruction(PhysicalContext *c,
                                const MachineInstruction *mi);
int physical_native_other_info(const AstTargetConfig *target,
                               const AstHirNativeStmtInfo *s,
                               const NativeBindings *bindings,
                               AstPhysicalInstructionInfo *out);
int physical_native_scalar_info(const AstTargetConfig *target,
                                const AstHirNativeStmtInfo *s,
                                const NativeBindings *bindings,
                                AstPhysicalInstructionInfo *out);
int physical_native_a64_scalar_info(const AstTargetConfig *target,
                                    const AstHirNativeStmtInfo *s,
                                    const NativeBindings *bindings,
                                    AstPhysicalInstructionInfo *out);
struct A64DataForm;
const struct A64DataForm *
physical_native_a64_data_form(const AstTargetConfig *target,
                              const AstHirNativeStmtInfo *s);
int physical_native_a64_data_info(const AstTargetConfig *target,
                                  const AstHirNativeStmtInfo *s,
                                  const NativeBindings *bindings,
                                  AstPhysicalInstructionInfo *out);
int physical_native_memory_info(const AstTargetConfig *target,
                                const AstHirNativeStmtInfo *s,
                                const NativeBindings *bindings, int manual,
                                AstPhysicalInstructionInfo *out);
int physical_native_indirect_info(const AstTargetConfig *target,
                                  const AstHirNativeStmtInfo *s,
                                  AstPhysicalInstructionInfo *out);
int physical_native_packet_equal(const AstPhysicalInstructionInfo *a,
                                 const AstPhysicalInstructionInfo *b);
int physical_verify_native_x86_region(const AstRegAllocProgram *p,
                                      uint32_t function, uint32_t instruction,
                                      const PhysicalBlock *block,
                                      uint32_t begin, uint32_t start,
                                      uint32_t end, uint32_t tail, int manual);
int physical_native_system_bound_info(const AstTargetConfig *target,
                                      const AstHirNativeStmtInfo *s,
                                      const NativeBindings *bindings,
                                      AstPhysicalInstructionInfo *out);
int physical_verify_native_other_region(const AstRegAllocProgram *p,
                                        uint32_t function, uint32_t instruction,
                                        const PhysicalBlock *block,
                                        uint32_t begin, uint32_t start,
                                        uint32_t end, uint32_t tail);
int physical_verify_native_transfers(PhysicalContext *c,
                                     const AstMirInstructionInfo *source,
                                     const NativeBindings *bindings,
                                     const PhysicalBlock *block, uint32_t begin,
                                     uint32_t start, uint32_t end,
                                     uint32_t tail);
int physical_native_function_policy(PhysicalContext *c, int *handled);
int physical_verify_native(const AstRegAllocProgram *p, uint32_t function);
int physical_verify_native_manual_other(const AstRegAllocProgram *p,
                                        uint32_t function);
int physical_native_system(PhysicalContext *c, const AstHirNativeStmtInfo *s,
                           int emit);
int physical_native_system_info(const AstTargetConfig *target,
                                const AstHirNativeStmtInfo *s,
                                AstPhysicalInstructionInfo *out);
int physical_verify_system_source(const AstRegAllocProgram *p,
                                  const AstHirNativeStmtInfo *s,
                                  const AstPhysicalInstructionInfo *actual);
int physical_native_vector_info(const AstTargetConfig *target,
                                const AstHirNativeStmtInfo *s, int manual,
                                AstPhysicalInstructionInfo *out);
int physical_verify_vector_source(const AstRegAllocProgram *p,
                                  const AstHirNativeStmtInfo *s, int manual,
                                  const AstPhysicalInstructionInfo *actual);
int physical_native_vector_register(const AstTargetConfig *target,
                                    const AstHirNativeExpr *expr,
                                    AstPhysicalOperand *out);
int physical_native_vector_integer(const AstHirNativeExpr *expr, int64_t *out);
int physical_native_integer_bits(const AstHirNativeExpr *expr, uint64_t *out);
int physical_native_scalable_info(const AstTargetConfig *target,
                                  const AstHirNativeStmtInfo *s,
                                  AstPhysicalInstructionInfo *out);
int physical_native_vector_other_info(const AstTargetConfig *target,
                                      const AstHirNativeStmtInfo *s,
                                      AstPhysicalInstructionInfo *out);
#endif
