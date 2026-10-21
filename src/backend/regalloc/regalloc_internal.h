#ifndef ASTERASM_REGALLOC_INTERNAL_H
#define ASTERASM_REGALLOC_INTERNAL_H

#include "../common/machine_internal.h"
#include "asterasm/arena.h"
#include <stdint.h>

typedef enum MachinePhysRegFile {
  MACHINE_PHYSREG_FILE_INVALID = 0,
  MACHINE_PHYSREG_FILE_GPR,
  MACHINE_PHYSREG_FILE_FP_VECTOR,
  MACHINE_PHYSREG_FILE_VECTOR,
  MACHINE_PHYSREG_FILE_MASK_PREDICATE,
  MACHINE_PHYSREG_FILE_SPECIAL
} MachinePhysRegFile;

typedef struct MachinePhysRegView {
  MachinePhysRegFile file;
  AstMachineRegBank bank;
  uint32_t index;
  uint32_t width;
  uint32_t alias_unit;
  uint8_t reserved;
  uint8_t special;
} MachinePhysRegView;

int machine_physreg_view(const AstTargetConfig *config, const char *name,
                         MachinePhysRegView *out);
int machine_physreg_alias(const AstTargetConfig *config, const char *lhs,
                          const char *rhs);
uint32_t machine_physreg_preserved_bits(AstAbiKind abi,
                                        const MachinePhysRegView *view);
int machine_physreg_name(const AstTargetConfig *config, AstMachineRegBank bank,
                         uint32_t index, uint32_t bits,
                         char out[AST_MACHINE_REGISTER_NAME_CAP]);

typedef struct MachineBitSet {
  uint64_t *words;
  uint32_t word_count;
} MachineBitSet;

typedef struct MachineLivenessBlock {
  MachineBitSet use;
  MachineBitSet def;
  MachineBitSet live_in;
  MachineBitSet live_out;
} MachineLivenessBlock;

typedef struct MachineLiveSegment {
  uint32_t begin;
  uint32_t end; /* Half-open. Uses precede definitions at an instruction. */
} MachineLiveSegment;

typedef struct MachineUsePosition {
  uint32_t slot;
  uint32_t instruction;
  uint32_t operand;
  uint32_t weight;
  uint8_t fixed;
} MachineUsePosition;

typedef struct MachineLiveInterval {
  uint32_t first_slot;
  uint32_t last_slot;
  uint32_t use_count;
  uint32_t def_count;
  uint8_t valid;
  MachineLiveSegment *segments;
  uint32_t segment_count;
  uint32_t segment_capacity;
  MachineUsePosition *uses;
  uint32_t use_capacity;
  uint64_t weight;
} MachineLiveInterval;

typedef struct MachineLiveness {
  AstArena storage;
  uint32_t block_count;
  uint32_t vreg_count;
  MachineLivenessBlock *blocks;
  MachineLiveInterval *intervals;
  uint32_t *instruction_slots;
  uint32_t *block_begin;
  uint32_t *block_end;
  uint32_t slot_count;
  uint64_t
      *bitset_storage; /* Owns all block sets and the temporary live set. */
} MachineLiveness;

int machine_liveness_build(const MachineFunction *fn, MachineLiveness *out);
void machine_liveness_destroy(MachineLiveness *liveness);
int machine_liveness_test_bit(const MachineBitSet *set, uint32_t bit);
int machine_liveness_verify(const MachineFunction *fn,
                            const MachineLiveness *liveness);
int machine_intervals_overlap(const MachineLiveInterval *a,
                              const MachineLiveInterval *b);
int machine_interval_covers(const MachineLiveInterval *interval, uint32_t slot);
int machine_cfg_validate(const MachineFunction *fn);

#endif
