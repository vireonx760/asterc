#ifndef ASTERASM_REGALLOC_H
#define ASTERASM_REGALLOC_H

#include "asterasm/machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstRegAllocProgram AstRegAllocProgram;

typedef enum AstRegAllocLocationKind {
  AST_REGALLOC_LOCATION_NONE = 0,
  AST_REGALLOC_LOCATION_REGISTER,
  AST_REGALLOC_LOCATION_SPILL,
  AST_REGALLOC_LOCATION_REMATERIALIZE
} AstRegAllocLocationKind;

typedef struct AstRegAllocLocation {
  AstRegAllocLocationKind kind;
  AstMachineRegBank bank;
  uint32_t bit_width;
  uint32_t register_unit;
  char register_name[AST_MACHINE_REGISTER_NAME_CAP];
  uint32_t frame_object;
  uint64_t immediate_bits;
} AstRegAllocLocation;

typedef enum AstRegAllocDiagCode {
  AST_REGALLOC_DIAG_NONE = 8000,
  AST_REGALLOC_DIAG_INVALID_MACHINE,
  AST_REGALLOC_DIAG_UNSUPPORTED_TARGET,
  AST_REGALLOC_DIAG_UNSUPPORTED_VALUE,
  AST_REGALLOC_DIAG_INVALID_LIVENESS,
  AST_REGALLOC_DIAG_IMPOSSIBLE_CONSTRAINT,
  AST_REGALLOC_DIAG_UNSUPPORTED_FRAME,
  AST_REGALLOC_DIAG_UNSUPPORTED_INSTRUCTION,
  AST_REGALLOC_DIAG_OUT_OF_MEMORY,
  AST_REGALLOC_DIAG_INVALID_PHYSICAL
} AstRegAllocDiagCode;

typedef struct AstRegAllocDiagnostic {
  AstRegAllocDiagCode code;
  uint32_t function_index;
  AstMachineInstId instruction;
  AstSpan span;
  const char *message;
} AstRegAllocDiagnostic;

typedef struct AstRegAllocConfig {
  uint8_t verify_machine;
  uint8_t verify_allocation;
  uint8_t enable_coalescing;
  uint8_t enable_rematerialization;
  uint8_t enable_red_zone;
  /* Zero means all target registers. A smaller limit is useful for stress
   * tests. */
  uint32_t register_limit;
} AstRegAllocConfig;

typedef struct AstRegAllocStats {
  size_t function_count;
  size_t assigned_registers;
  size_t spilled_values;
  size_t rematerialized_values;
  size_t coalesced_copies;
  size_t live_segments;
  size_t frame_bytes;
  size_t physical_instructions;
  size_t edge_blocks;
  size_t error_count;
} AstRegAllocStats;

typedef enum AstFrameObjectKind {
  AST_FRAME_OBJECT_LOCAL = 0,
  AST_FRAME_OBJECT_SPILL,
  AST_FRAME_OBJECT_CALLEE_SAVE,
  AST_FRAME_OBJECT_COPY_TEMP,
  AST_FRAME_OBJECT_ABI_TEMP,
  AST_FRAME_OBJECT_EMERGENCY
} AstFrameObjectKind;

typedef struct AstFrameObjectInfo {
  uint32_t id;
  AstFrameObjectKind kind;
  uint64_t size;
  uint32_t alignment;
  int64_t offset; /* Relative to final, post-prologue SP. */
} AstFrameObjectInfo;

typedef struct AstFrameInfo {
  uint64_t stack_adjustment; /* Fixed body allocation; excludes dynamic
                                alignment padding. */
  uint64_t outgoing_size;
  uint32_t object_count;
  uint32_t alignment;
  uint8_t uses_red_zone;
  uint8_t has_calls;
  uint8_t needs_stack_probe;
  uint8_t
      realigns_stack; /* RBP preserves the incoming stack base on SysV x86. */
} AstFrameInfo;

#define AST_PHYSICAL_MAX_OPERANDS 12u
typedef enum AstPhysicalOperandKind {
  AST_PHYSICAL_OPERAND_INVALID = 0,
  AST_PHYSICAL_OPERAND_REGISTER,
  AST_PHYSICAL_OPERAND_MEMORY,
  AST_PHYSICAL_OPERAND_IMMEDIATE,
  AST_PHYSICAL_OPERAND_BLOCK,
  AST_PHYSICAL_OPERAND_SYMBOL
} AstPhysicalOperandKind;

typedef struct AstPhysicalOperand {
  AstPhysicalOperandKind kind;
  AstMachineRegBank bank;
  uint32_t bit_width;
  uint32_t register_unit;
  char register_name[AST_MACHINE_REGISTER_NAME_CAP];
  /* MEMORY: base register above + optional index*scale + displacement.
     scale==0 means no index; otherwise it is one of 1,2,4,8.  Abstract
     frame operands keep scale==0 and are resolved before encoding. */
  uint32_t index_register_unit;
  char index_register_name[AST_MACHINE_REGISTER_NAME_CAP];
  uint8_t scale;
  int64_t displacement;
  uint64_t immediate_bits;
  AstMachineBlockId block;
  AstName symbol;
  /* Defining module for symbol operands. Preserved through register
     allocation/physical lowering for qualified relocation resolution. */
  AstName module;
} AstPhysicalOperand;

typedef struct AstPhysicalInstructionInfo {
  AstMachineOpcode opcode;
  AstMachineInstId source_instruction;
  AstSpan span;
  uint32_t result_count; /* Leading operands are explicit definitions. */
  uint32_t operand_count;
  AstPhysicalOperand operands[AST_PHYSICAL_MAX_OPERANDS];
  AstMemoryOrder memory_order;
  AstMemoryOrder failure_memory_order;
  uint8_t is_signed;
  /* Set only on a physical memory transfer whose selected target form has a
   * verified non-temporal encoding. */
  uint8_t nontemporal;
  /* Borrowed from the HIR arena; valid for AST_MACHINE_X86_RAW_BYTES only. */
  const uint8_t *raw_bytes;
  uint32_t raw_byte_count;
} AstPhysicalInstructionInfo;

typedef struct AstPhysicalBlockInfo {
  AstMachineBlockId id;
  uint32_t instruction_count;
  const AstPhysicalInstructionInfo *instructions;
} AstPhysicalBlockInfo;

ASTER_API AstRegAllocConfig ast_regalloc_config_default(void);
/* Borrows the selected Machine program and its dependency chain. A non-null
   result may contain diagnostics; check stats.error_count / verify before use.
 */
ASTER_API AstRegAllocProgram *
ast_regalloc_build(const AstMachineProgram *machine,
                   const AstRegAllocConfig *config);
ASTER_API void ast_regalloc_destroy(AstRegAllocProgram *program);
ASTER_API int ast_regalloc_verify(const AstRegAllocProgram *program);
ASTER_API AstRegAllocStats
ast_regalloc_stats(const AstRegAllocProgram *program);
ASTER_API const AstRegAllocDiagnostic *
ast_regalloc_diagnostics(const AstRegAllocProgram *program, size_t *count);
ASTER_API int ast_regalloc_location(const AstRegAllocProgram *program,
                                    size_t function, AstMachineVReg vreg,
                                    AstRegAllocLocation *out);
/* Materialize physical instructions, ABI transfers and frame addresses.
   The source Machine program is unchanged; query views must be reacquired. */
ASTER_API int ast_regalloc_finalize(AstRegAllocProgram *program);
ASTER_API int ast_physical_verify(const AstRegAllocProgram *program);
ASTER_API int ast_regalloc_frame(const AstRegAllocProgram *program,
                                 size_t function, AstFrameInfo *out);
ASTER_API int ast_regalloc_frame_object(const AstRegAllocProgram *program,
                                        size_t function, uint32_t object,
                                        AstFrameObjectInfo *out);
ASTER_API size_t ast_physical_block_count(const AstRegAllocProgram *program,
                                          size_t function);
ASTER_API int ast_physical_block(const AstRegAllocProgram *program,
                                 size_t function, AstMachineBlockId block,
                                 AstPhysicalBlockInfo *out);
ASTER_API void ast_regalloc_dump(const AstRegAllocProgram *program,
                                 void (*write_fn)(void *user, const char *text,
                                                  size_t size),
                                 void *user);

#ifdef __cplusplus
}
#endif
#endif
