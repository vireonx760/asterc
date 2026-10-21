#ifndef ASTERASM_MACHINE_H
#define ASTERASM_MACHINE_H

#include "asterasm/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstMachineProgram AstMachineProgram;

typedef uint32_t AstMachineVReg;
typedef uint32_t AstMachineInstId;
typedef uint32_t AstMachineBlockId;

#define AST_MACHINE_INVALID_ID UINT32_MAX
#define AST_MACHINE_REGISTER_NAME_CAP AST_ABI_REGISTER_NAME_CAP

typedef enum AstMachineStage {
  AST_MACHINE_STAGE_INVALID = 0,
  AST_MACHINE_STAGE_GENERIC,
  AST_MACHINE_STAGE_REGBANK,
  AST_MACHINE_STAGE_SELECTED
} AstMachineStage;

typedef enum AstMachineRegBank {
  AST_MACHINE_BANK_INVALID = 0,
  AST_MACHINE_BANK_GPR,
  AST_MACHINE_BANK_FP,
  AST_MACHINE_BANK_VECTOR,
  AST_MACHINE_BANK_MASK,
  AST_MACHINE_BANK_SPECIAL
} AstMachineRegBank;

typedef enum AstMachineOperandKind {
  AST_MACHINE_OPERAND_INVALID = 0,
  AST_MACHINE_OPERAND_VREG,
  AST_MACHINE_OPERAND_IMMEDIATE,
  AST_MACHINE_OPERAND_BLOCK,
  AST_MACHINE_OPERAND_STACK_SLOT,
  AST_MACHINE_OPERAND_SYMBOL
} AstMachineOperandKind;

typedef enum AstMachineOpcode {
  AST_MACHINE_OP_INVALID = 0,

  /* Generic Machine IR. These must not survive instruction selection. */
  AST_MACHINE_G_CONSTANT,
  AST_MACHINE_G_UNDEF,
  AST_MACHINE_G_COPY,
  AST_MACHINE_G_STACK_ADDR,
  AST_MACHINE_G_LOAD,
  AST_MACHINE_G_STORE,
  AST_MACHINE_G_GLOBAL_ADDR,
  AST_MACHINE_G_FUNCTION_ADDR,
  AST_MACHINE_G_GENERIC_CONST,
  AST_MACHINE_G_UNARY,
  AST_MACHINE_G_BINARY,
  AST_MACHINE_G_CAST,
  AST_MACHINE_G_SELECT,
  AST_MACHINE_G_PACK,
  AST_MACHINE_G_CALL_DIRECT,
  AST_MACHINE_G_CALL_INDIRECT,
  AST_MACHINE_G_INDEX,
  AST_MACHINE_G_MEMBER,
  AST_MACHINE_G_ADDRESS_OF,
  AST_MACHINE_G_ATOMIC,
  AST_MACHINE_G_FENCE,
  AST_MACHINE_G_INTRINSIC,
  AST_MACHINE_G_BIND,
  AST_MACHINE_G_NATIVE,
  AST_MACHINE_G_NATIVE_RESULT,
  AST_MACHINE_G_BR,
  AST_MACHINE_G_COND_BR,
  AST_MACHINE_G_RET,
  AST_MACHINE_G_UNREACHABLE,

  /* x86-64 selected instructions and target pseudos. */
  AST_MACHINE_X86_UNDEF,
  AST_MACHINE_X86_MOV,
  AST_MACHINE_X86_MOV_IMM,
  AST_MACHINE_X86_LEA,
  AST_MACHINE_X86_LOAD,
  AST_MACHINE_X86_STORE,
  AST_MACHINE_X86_NEG,
  AST_MACHINE_X86_NOT,
  AST_MACHINE_X86_ADD,
  AST_MACHINE_X86_SUB,
  AST_MACHINE_X86_MUL,
  AST_MACHINE_X86_UMUL_WIDE, /* unsigned RDX:RAX <- RAX * r/m64 */
  AST_MACHINE_X86_SDIV,
  AST_MACHINE_X86_UDIV,
  AST_MACHINE_X86_SREM,
  AST_MACHINE_X86_UREM,
  AST_MACHINE_X86_AND,
  AST_MACHINE_X86_OR,
  AST_MACHINE_X86_XOR,
  AST_MACHINE_X86_SHL,
  AST_MACHINE_X86_ASHR,
  AST_MACHINE_X86_LSHR,
  AST_MACHINE_X86_ICMP_EQ,
  AST_MACHINE_X86_ICMP_NE,
  AST_MACHINE_X86_ICMP_LT,
  AST_MACHINE_X86_ICMP_LE,
  AST_MACHINE_X86_ICMP_GT,
  AST_MACHINE_X86_ICMP_GE,
  AST_MACHINE_X86_FADD,
  AST_MACHINE_X86_FSUB,
  AST_MACHINE_X86_FMUL,
  AST_MACHINE_X86_FDIV,
  AST_MACHINE_X86_FREM,
  AST_MACHINE_X86_FCMP_EQ,
  AST_MACHINE_X86_FCMP_NE,
  AST_MACHINE_X86_FCMP_LT,
  AST_MACHINE_X86_FCMP_LE,
  AST_MACHINE_X86_FCMP_GT,
  AST_MACHINE_X86_FCMP_GE,
  AST_MACHINE_X86_VEC_ADD,
  AST_MACHINE_X86_VEC_SUB,
  AST_MACHINE_X86_VEC_MUL,
  AST_MACHINE_X86_VEC_AND,
  AST_MACHINE_X86_VEC_OR,
  AST_MACHINE_X86_VEC_XOR,
  AST_MACHINE_X86_VEC_CMP,
  AST_MACHINE_X86_CAST,
  AST_MACHINE_X86_SELECT,
  AST_MACHINE_X86_CALL,
  AST_MACHINE_X86_CALL_INDIRECT,
  AST_MACHINE_X86_INDEX,
  AST_MACHINE_X86_MEMBER,
  AST_MACHINE_X86_ATOMIC,
  AST_MACHINE_X86_FENCE,
  AST_MACHINE_X86_INTRINSIC,
  AST_MACHINE_X86_BIND,
  AST_MACHINE_X86_NATIVE,
  AST_MACHINE_X86_NATIVE_RESULT,
  AST_MACHINE_X86_BR,
  AST_MACHINE_X86_COND_BR,
  AST_MACHINE_X86_RET,
  AST_MACHINE_X86_TRAP,
  /* Physical, implicit high-half sign extension before signed division. */
  AST_MACHINE_X86_CDQ,
  AST_MACHINE_X86_CQO,
  AST_MACHINE_X86_MOVSX,
  AST_MACHINE_X86_MOVZX,
  AST_MACHINE_X86_NOP,
  AST_MACHINE_X86_POPCNT,
  AST_MACHINE_X86_BSF,
  AST_MACHINE_X86_BSR,
  AST_MACHINE_X86_BSWAP,
  AST_MACHINE_X86_ROL,
  AST_MACHINE_X86_ROR,
  /* Scalar conversions. Unsigned 64-bit compound forms explicitly declare
     their scratch registers as additional result operands; no hidden clobbers.
   */
  AST_MACHINE_X86_FMOV_BITS,
  AST_MACHINE_X86_SINT_TO_FP,
  AST_MACHINE_X86_FP_TO_SINT,
  AST_MACHINE_X86_FP_CONVERT,
  AST_MACHINE_X86_UINT64_TO_FP,
  AST_MACHINE_X86_FP_TO_UINT64,
  AST_MACHINE_X86_CLI,
  AST_MACHINE_X86_STI,
  AST_MACHINE_X86_HLT,
  AST_MACHINE_X86_CLD,
  /* Fully constrained atomic sequence; every scratch is an explicit definition.
   */
  AST_MACHINE_X86_LOCKED_ATOMIC,
  /* Port I/O has explicit accumulator/port operands; descriptor instructions
     take the 64-bit address of their ten-byte memory operand. */
  AST_MACHINE_X86_IN,
  AST_MACHINE_X86_OUT,
  AST_MACHINE_X86_LIDT,
  AST_MACHINE_X86_LGDT,
  /* Explicit manual/native stack and control-flow forms. */
  AST_MACHINE_X86_PUSH,
  AST_MACHINE_X86_POP,
  AST_MACHINE_X86_JMP,
  AST_MACHINE_X86_JMP_INDIRECT,
  /* Opaque bytes are only materialized by unsafe exact manual regions. */
  AST_MACHINE_X86_RAW_BYTES,
  /* Late Physical-IR compare+branch fusion. Operand 0 stores the original
     ICMP opcode, followed by lhs, rhs, true block, false block. */
  AST_MACHINE_X86_SYSTEM,
  AST_MACHINE_X86_VECTOR,
  AST_MACHINE_X86_ICMP_BR,

  /* Keep each architecture in a separate range as instruction sets grow. */
  AST_MACHINE_A64_BEGIN = 1023,
#define AST_MACHINE_OPCODE(name) AST_MACHINE_##name,
#include "asterasm/detail/aarch64_opcodes.inc"
#undef AST_MACHINE_OPCODE
  AST_MACHINE_RISCV_BEGIN = 2047,
#define AST_MACHINE_OPCODE(name) AST_MACHINE_##name,
#include "asterasm/detail/riscv64_opcodes.inc"
#undef AST_MACHINE_OPCODE
} AstMachineOpcode;

typedef enum AstMachineVRegFlags {
  AST_MACHINE_VREG_NONE = 0,
  AST_MACHINE_VREG_FUNCTION_ARGUMENT = 1u << 0,
  AST_MACHINE_VREG_FIXED_REGISTER = 1u << 1,
  AST_MACHINE_VREG_SPLIT_PART = 1u << 2,
  AST_MACHINE_VREG_ABI_DEFERRED = 1u << 3
} AstMachineVRegFlags;

typedef enum AstMachineOperandFlags {
  AST_MACHINE_OPERAND_NONE = 0,
  AST_MACHINE_OPERAND_FIXED_REGISTER = 1u << 0
} AstMachineOperandFlags;

typedef struct AstMachineOperand {
  AstMachineOperandKind kind;
  uint32_t flags;
  char fixed_register[AST_MACHINE_REGISTER_NAME_CAP];
  union {
    AstMachineVReg vreg;
    int64_t immediate;
    AstMachineBlockId block;
    AstMirStackSlotId stack_slot;
    AstName symbol;
  } as;
  /* For symbol operands, preserve the defining module from MIR so later
     relocation stages can distinguish same-named symbols in different
     modules. Zero means intentionally unqualified (for example a native
     external symbol). */
  AstName module;
} AstMachineOperand;

typedef struct AstMachineVRegInfo {
  AstMachineVReg id;
  AstMirVReg source_vreg;
  const AstSemaType *type;
  AstMachineRegBank bank;
  AstTargetRegClass reg_class;
  uint32_t bit_width;
  uint32_t part_index;
  uint32_t part_count;
  uint32_t flags;
  char fixed_register[AST_MACHINE_REGISTER_NAME_CAP];
} AstMachineVRegInfo;

typedef struct AstMachineInstructionInfo {
  AstMachineInstId id;
  AstMachineOpcode opcode;
  AstMirOp source_op;
  AstMirInstId source_instruction;
  AstTokenKind token_op;
  AstHirIntrinsicKind intrinsic;
  AstMemoryOrder memory_order;
  AstMemoryOrder failure_memory_order;
  AstSpan span;
  uint32_t effects;
  uint32_t flags;
  AstMachineVReg result_first;
  uint32_t result_count;
  const AstMachineOperand *operands;
  uint32_t operand_count;
} AstMachineInstructionInfo;

typedef struct AstMachineBlockInfo {
  AstMachineBlockId id;
  const AstMachineInstId *instructions;
  uint32_t instruction_count;
  uint8_t reachable;
  /* Flattened legalized block parameters. Edge arguments follow each BLOCK
     operand in a branch's operand stream, in this same part order. */
  const AstMachineVReg *arguments;
  uint32_t argument_count;
  uint32_t loop_depth;
} AstMachineBlockInfo;

typedef struct AstMachineFunctionInfo {
  AstName module_name;
  AstName name;
  uint32_t function_index;
  AstMachineBlockId entry_block;
  uint32_t block_count;
  uint32_t instruction_count;
  uint32_t vreg_count;
} AstMachineFunctionInfo;

typedef struct AstMachineStats {
  size_t function_count;
  size_t block_count;
  size_t instruction_count;
  size_t vreg_count;
  size_t fixed_register_count;
  size_t fixed_operand_count;
  size_t split_value_count;
  size_t deferred_abi_value_count;
} AstMachineStats;

typedef struct AstMachineConfig {
  uint8_t verify_sources;
  uint8_t verify_after_each_stage;
  uint8_t enable_peephole;
} AstMachineConfig;

ASTER_API AstMachineConfig ast_machine_config_default(void);
/*
 * Builds generic Machine IR from a frozen ABI program. The returned program
 * owns all Machine IR storage but borrows `abi` and its Target/MIR dependency
 * chain; those source objects must outlive the AstMachineProgram. Query
 * pointers/views are borrowed from the AstMachineProgram and become invalid
 * when it is destroyed.
 */
ASTER_API AstMachineProgram *ast_machine_build(const AstAbiProgram *abi,
                                               const AstMachineConfig *config);
ASTER_API void ast_machine_destroy(AstMachineProgram *program);
/* Stage transitions mutate the program in place. Reacquire borrowed query views
 * after a transition. */
ASTER_API int ast_machine_assign_regbanks(AstMachineProgram *program);
ASTER_API int ast_machine_select_instructions(AstMachineProgram *program);
ASTER_API int ast_machine_verify(const AstMachineProgram *program);
ASTER_API AstMachineStage ast_machine_stage(const AstMachineProgram *program);
ASTER_API AstMachineStats ast_machine_stats(const AstMachineProgram *program);
ASTER_API const AstAbiProgram *
ast_machine_source_abi(const AstMachineProgram *program);
ASTER_API size_t ast_machine_function_count(const AstMachineProgram *program);
ASTER_API int ast_machine_function_info(const AstMachineProgram *program,
                                        size_t function_index,
                                        AstMachineFunctionInfo *out);
ASTER_API int ast_machine_function_block(const AstMachineProgram *program,
                                         size_t function_index,
                                         AstMachineBlockId block,
                                         AstMachineBlockInfo *out);
ASTER_API int ast_machine_function_instruction(const AstMachineProgram *program,
                                               size_t function_index,
                                               AstMachineInstId inst,
                                               AstMachineInstructionInfo *out);
ASTER_API int ast_machine_function_vreg(const AstMachineProgram *program,
                                        size_t function_index,
                                        AstMachineVReg vreg,
                                        AstMachineVRegInfo *out);
ASTER_API const char *ast_machine_opcode_name(AstMachineOpcode opcode);
ASTER_API const char *ast_machine_regbank_name(AstMachineRegBank bank);
ASTER_API void ast_machine_dump_program(
    const AstMachineProgram *program,
    void (*write_fn)(void *user, const char *text, size_t length), void *user);

#ifdef __cplusplus
}
#endif

#endif
