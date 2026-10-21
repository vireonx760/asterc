#ifndef ASTERASM_ALLOCATION_INTERNAL_H
#define ASTERASM_ALLOCATION_INTERNAL_H
#include "asterasm/regalloc.h"
#include "regalloc_internal.h"

typedef struct PhysicalBlock {
  AstPhysicalInstructionInfo *instructions;
  uint32_t count;
  uint32_t capacity;
} PhysicalBlock;

/* A virtual register can have one independently allocated home per disjoint
   liveness segment.  CFG edges materialize transitions between these homes. */
typedef struct RegAllocLocationPiece {
  uint32_t begin;
  uint32_t end;
  AstRegAllocLocation location;
} RegAllocLocationPiece;

/* Final per-physical-register live ranges.  These are built once after
   allocation and queried by post-RA scratch borrowing, so lowering can test
   whether (for example) r11 is actually live without rescanning every vreg
   for every temporary. */
typedef struct RegAllocUnitRange {
  uint32_t begin;
  uint32_t end;
} RegAllocUnitRange;

typedef struct AllocationFunction {
  MachineLiveness liveness;
  AstArena allocation_storage;
  AstRegAllocLocation *locations;
  RegAllocLocationPiece **location_pieces;
  RegAllocLocationPiece *location_piece_storage;
  uint32_t *location_piece_counts;
  uint32_t *copy_sources;
  uint32_t *affinity_sources;
  uint32_t *alternate_affinity_sources;
  uint32_t *abi_preferred_units;
  uint32_t *binding_units;
  uint32_t binding_count;
  struct NativeLowerState *native_state;
  AstFrameObjectInfo *objects;
  uint32_t object_count;
  uint32_t object_capacity;
  AstFrameInfo frame;
  PhysicalBlock *blocks;
  uint32_t block_count;
  uint32_t block_capacity;
  uint32_t *callee_save_objects; /* Indexed by alias unit. */
  uint32_t *emergency_objects;
  uint32_t *work_objects;
  uint32_t *register_object_storage;
  uint32_t copy_temp;
  uint32_t abi_temp;
  uint32_t sret_home;
  uint32_t *call_slots;
  uint32_t call_count;
  RegAllocUnitRange *unit_range_storage;
  uint32_t *unit_range_offsets; /* MACHINE_REGISTER_UNITS + 1 entries. */
} AllocationFunction;

struct AstRegAllocProgram {
  const AstMachineProgram *machine;
  AstTargetConfig target;
  AstAbiKind abi;
  AstRegAllocConfig config;
  struct RegAllocRegisterSet *register_sets;
  AllocationFunction *functions;
  size_t function_count;
  AstRegAllocStats stats;
  AstRegAllocDiagnostic diagnostic;
  char diagnostic_storage[512];
  uint8_t allocation_verified;
  uint8_t finalized;
};

#define MACHINE_REGISTER_UNITS 384u

typedef int (*RegAllocFunctionFn)(AstRegAllocProgram *program,
                                  uint32_t function, void *context);
int regalloc_parallel_functions(AstRegAllocProgram *program,
                                RegAllocFunctionFn lower, void *context);
AstRegAllocProgram *regalloc_prepare(const AstMachineProgram *machine,
                                     const AstRegAllocConfig *config);
int regalloc_allocate_function(AstRegAllocProgram *program, uint32_t function);
int regalloc_finalize_function(AstRegAllocProgram *program, uint32_t function);
int physical_verify_one_function(const AstRegAllocProgram *program,
                                 uint32_t function);
void regalloc_release_function(AllocationFunction *function);

int regalloc_fail(AstRegAllocProgram *p, AstRegAllocDiagCode code, uint32_t fn,
                  uint32_t inst, const char *message);
const AstRegAllocLocation *regalloc_location_at(const AllocationFunction *fn,
                                                uint32_t vreg, uint32_t slot);
uint32_t regalloc_location_piece_count(const AllocationFunction *fn,
                                       uint32_t vreg);
const AstRegAllocLocation *regalloc_location_piece(const AllocationFunction *fn,
                                                   uint32_t vreg,
                                                   uint32_t piece);
int regalloc_unit_live_at(const AllocationFunction *fn, uint32_t unit,
                          uint32_t slot);
int regalloc_verify_function(const AstRegAllocProgram *p, uint32_t function);
int regalloc_verify_bind_instruction(const AstRegAllocProgram *p,
                                     uint32_t function,
                                     const MachineInstruction *mi);
uint32_t frame_add_object(AllocationFunction *fn, AstFrameObjectKind kind,
                          uint64_t size, uint32_t align);
int frame_init_register_objects(AllocationFunction *fn);
int frame_layout(AstRegAllocProgram *p, uint32_t function);
uint32_t frame_required_alignment(const AstRegAllocProgram *p,
                                  uint32_t function);
int physical_lower_function(AstRegAllocProgram *p, uint32_t function);
int physical_lower_aarch64(AstRegAllocProgram *p, uint32_t function);
int physical_lower_riscv64(AstRegAllocProgram *p, uint32_t function);
int physical_verify_aarch64(const AstRegAllocProgram *p, uint32_t function);
int physical_verify_riscv64(const AstRegAllocProgram *p, uint32_t function);
void physical_release_function(AllocationFunction *fn);
void physical_native_release(AllocationFunction *fn);
int physical_verify_function(const AstRegAllocProgram *p, uint32_t function);

#endif
