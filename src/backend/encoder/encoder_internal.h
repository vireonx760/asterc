#ifndef ASTERASM_ENCODER_INTERNAL_H
#define ASTERASM_ENCODER_INTERNAL_H
#include "asterasm/encoder.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct EncoderLocalBranch {
  size_t offset;
  uint32_t target;
  uint8_t size;
} EncoderLocalBranch;

typedef struct EncoderBuilder {
  AstEncoderOutput *out;
  AstTargetConfig target;
  size_t capacity, fixup_capacity;
  const size_t *block_offsets;
  size_t block_count;
  uint32_t instruction;
  uint32_t current_block;
  uint32_t next_block;
  EncoderLocalBranch *local_branches;
  uint8_t *short_branches;
  size_t branch_count, branch_capacity;
} EncoderBuilder;

int encoder_fail(EncoderBuilder *b, AstEncoderDiagCode code,
                 const char *message);
int encoder_le(EncoderBuilder *b, uint64_t bits, unsigned bytes);
int encoder_fixup(EncoderBuilder *b, size_t offset, uint8_t width,
                  AstEncoderFixupKind kind, AstName module, AstName symbol);
int encoder_delta(EncoderBuilder *b, uint32_t block, size_t pc, int64_t *delta);
int encoder_reg(const EncoderBuilder *b, const AstPhysicalOperand *o,
                AstTargetRegisterInfo *r);
int encoder_shape(const AstPhysicalInstructionInfo *i, uint32_t defs,
                  uint32_t count);
int encoder_same(const AstPhysicalOperand *a, const AstPhysicalOperand *b);
int encoder_x86_instruction(EncoderBuilder *b,
                            const AstPhysicalInstructionInfo *i);
int encoder_a64_instruction(EncoderBuilder *b,
                            const AstPhysicalInstructionInfo *i);
int encoder_rv_instruction(EncoderBuilder *b,
                           const AstPhysicalInstructionInfo *i);
/* Internal view entry point used by byte-level tests. Blocks are indexed by ID.
 */
int encoder_blocks(AstTargetConfig target, const AstPhysicalBlockInfo *blocks,
                   size_t count, uint32_t entry, AstEncoderOutput *out);
int encoder_blocks_ordered(AstTargetConfig target,
                           const AstPhysicalBlockInfo *blocks, size_t count,
                           uint32_t entry, AstEncoderOutput *out);
#endif
