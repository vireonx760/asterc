#ifndef ASTERASM_MIR_H
#define ASTERASM_MIR_H

#include "asterasm/hir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstMirProgram AstMirProgram;
typedef struct AstMirFunction AstMirFunction;

typedef uint32_t AstMirVReg;
typedef uint32_t AstMirBlockId;
typedef uint32_t AstMirInstId;
typedef uint32_t AstMirStackSlotId;
typedef uint32_t AstMirMemoryId;

#define AST_MIR_INVALID_ID UINT32_MAX
#define AST_MIR_UNKNOWN_SIZE UINT64_MAX

/* Generic MIR deliberately precedes target legalization and instruction
 * selection. */
typedef enum AstMirOp {
  AST_MIR_OP_INVALID = 0,
  AST_MIR_OP_CONSTANT,
  AST_MIR_OP_UNDEF,
  AST_MIR_OP_COPY,
  AST_MIR_OP_STACK_ADDR,
  AST_MIR_OP_STACK_LOAD,
  AST_MIR_OP_STACK_STORE,
  AST_MIR_OP_GLOBAL_ADDR,
  AST_MIR_OP_GLOBAL_LOAD,
  AST_MIR_OP_GLOBAL_STORE,
  AST_MIR_OP_FUNCTION_ADDR,
  AST_MIR_OP_GENERIC_CONST,
  AST_MIR_OP_UNARY,
  AST_MIR_OP_BINARY,
  AST_MIR_OP_CAST,
  AST_MIR_OP_SELECT,
  /* Synthetic aggregate construction used after portable intrinsic
   * expansion. Operands are laid out in source aggregate field order. */
  AST_MIR_OP_PACK,
  AST_MIR_OP_CALL_DIRECT,
  AST_MIR_OP_CALL_INDIRECT,
  AST_MIR_OP_INDEX,
  AST_MIR_OP_MEMBER,
  AST_MIR_OP_ADDRESS_OF,
  AST_MIR_OP_LOAD,
  AST_MIR_OP_STORE,
  AST_MIR_OP_ATOMIC,
  AST_MIR_OP_FENCE,
  AST_MIR_OP_INTRINSIC,
  AST_MIR_OP_BIND,
  AST_MIR_OP_NATIVE,
  AST_MIR_OP_NATIVE_RESULT
} AstMirOp;

typedef enum AstMirTermKind {
  AST_MIR_TERM_NONE = 0,
  AST_MIR_TERM_BR,
  AST_MIR_TERM_COND_BR,
  AST_MIR_TERM_RETURN,
  AST_MIR_TERM_UNREACHABLE
} AstMirTermKind;

typedef enum AstMirVRegKind {
  AST_MIR_VREG_INVALID = 0,
  AST_MIR_VREG_BLOCK_ARGUMENT,
  AST_MIR_VREG_INSTRUCTION_RESULT
} AstMirVRegKind;

/* These are target-independent bank hints, not physical register classes. */
typedef enum AstMirRegBank {
  AST_MIR_BANK_INVALID = 0,
  AST_MIR_BANK_GENERAL,
  AST_MIR_BANK_FLOAT,
  AST_MIR_BANK_POINTER,
  AST_MIR_BANK_CAPABILITY,
  AST_MIR_BANK_VECTOR,
  AST_MIR_BANK_MASK,
  AST_MIR_BANK_AGGREGATE,
  AST_MIR_BANK_GENERIC
} AstMirRegBank;

typedef enum AstMirVRegFlags {
  AST_MIR_VREG_NONE = 0,
  AST_MIR_VREG_FUNCTION_ARGUMENT = 1u << 0,
  AST_MIR_VREG_PHI = 1u << 1,
  AST_MIR_VREG_UNDEFINED = 1u << 2
} AstMirVRegFlags;

typedef enum AstMirMemoryKind {
  AST_MIR_MEMORY_INVALID = 0,
  AST_MIR_MEMORY_ENTRY,
  AST_MIR_MEMORY_BLOCK_ARGUMENT,
  AST_MIR_MEMORY_INSTRUCTION_RESULT
} AstMirMemoryKind;

typedef enum AstMirStackSlotFlags {
  AST_MIR_STACK_SLOT_NONE = 0,
  AST_MIR_STACK_SLOT_MUTABLE = 1u << 0,
  AST_MIR_STACK_SLOT_PARAMETER = 1u << 1,
  AST_MIR_STACK_SLOT_ADDRESS_TAKEN = 1u << 2,
  AST_MIR_STACK_SLOT_DYNAMIC_SIZE = 1u << 3
} AstMirStackSlotFlags;

typedef enum AstMirInstructionFlags {
  AST_MIR_INST_NONE = 0,
  AST_MIR_INST_READS_MEMORY = 1u << 0,
  AST_MIR_INST_WRITES_MEMORY = 1u << 1,
  AST_MIR_INST_ATOMIC = 1u << 2,
  AST_MIR_INST_CALL = 1u << 3,
  AST_MIR_INST_MAY_TRAP = 1u << 4,
  AST_MIR_INST_VOLATILE = 1u << 5,
  AST_MIR_INST_UNSAFE = 1u << 6,
  AST_MIR_INST_CONSTRAINT = 1u << 7,
  /* Effects are a fail-closed approximation pending target legalization. */
  AST_MIR_INST_CONSERVATIVE_EFFECTS = 1u << 8,
  /* One-to-many lowering helper with no one-to-one HIR instruction origin. */
  AST_MIR_INST_SYNTHETIC = 1u << 9,
  AST_MIR_INST_NONTEMPORAL = 1u << 10
} AstMirInstructionFlags;

typedef enum AstMirDiagCode {
  AST_MIR_DIAG_NONE = 5000,
  AST_MIR_DIAG_HIR_REQUIRED,
  AST_MIR_DIAG_HIR_INVALID,
  AST_MIR_DIAG_UNSUPPORTED_HIR,
  AST_MIR_DIAG_INVALID_CFG,
  AST_MIR_DIAG_MISSING_TERMINATOR,
  AST_MIR_DIAG_INVALID_SUCCESSOR,
  AST_MIR_DIAG_BRANCH_ARGUMENT_MISMATCH,
  AST_MIR_DIAG_INVALID_VREG,
  AST_MIR_DIAG_INVALID_OPERAND,
  AST_MIR_DIAG_TYPE_MISMATCH,
  AST_MIR_DIAG_DOMINANCE,
  AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
  AST_MIR_DIAG_INVALID_STACK_SLOT,
  AST_MIR_DIAG_INVALID_EFFECTS,
  AST_MIR_DIAG_INVALID_LIVENESS,
  AST_MIR_DIAG_INTERNAL,
  AST_MIR_DIAG_TOO_MANY_DIAGNOSTICS,
  AST_MIR_DIAG_INLINE_POLICY
} AstMirDiagCode;

typedef struct AstMirDiagnostic {
  AstMirDiagCode code;
  AstDiagSeverity severity;
  AstSpan span;
  const char *path;
  uint32_t line;
  uint32_t column;
  const char *message;
} AstMirDiagnostic;

typedef struct AstMirConfig {
  size_t thread_count;
  size_t arena_block_size;
  uint32_t max_diagnostics;
  uint8_t verify_hir_before_build;
  uint8_t verify_after_build;
  uint8_t promote_locals;
} AstMirConfig;

typedef struct AstMirStats {
  size_t module_count;
  size_t function_count;
  size_t extern_function_count;
  size_t dispatch_function_count;
  size_t global_count;
  size_t block_count;
  size_t instruction_count;
  size_t vreg_count;
  size_t memory_value_count;
  size_t stack_slot_count;
  size_t promoted_local_count;
  size_t phi_count;
  size_t memory_phi_count;
  size_t terminator_count;
  size_t unreachable_block_count;
  size_t error_count;
  size_t warning_count;
  size_t arena_bytes_used;
  size_t arena_bytes_reserved;
} AstMirStats;

typedef struct AstMirVRegInfo {
  AstMirVRegKind kind;
  const AstSemaType *type;
  AstSpan span;
  AstMirBlockId block;
  uint32_t definition_index;
  AstMirRegBank bank;
  uint32_t bit_width;
  uint32_t flags;
  uint32_t use_count;
  uint64_t live_start;
  uint64_t live_end;
  uint64_t spill_weight;
} AstMirVRegInfo;

typedef struct AstMirMemoryInfo {
  AstMirMemoryKind kind;
  AstMirBlockId block;
  uint32_t definition_index;
  /* Block arguments list one incoming memory state per predecessor, in CFG
   * order. */
  const AstMirMemoryId *incoming;
  uint32_t incoming_count;
} AstMirMemoryInfo;

typedef struct AstMirStackSlotInfo {
  AstMirStackSlotId id;
  AstHirLocalId source_local;
  AstName name;
  const AstSemaType *type;
  AstSpan span;
  uint64_t size;
  uint32_t alignment;
  uint32_t flags;
} AstMirStackSlotInfo;

typedef struct AstMirInstructionInfo {
  AstMirInstId id;
  AstMirOp op;
  uint32_t
      inline_site; /* 0 = original; otherwise a one-based source-site ID. */
  AstMirVReg result;
  const AstSemaType *type;
  AstSpan span;
  uint32_t effects;
  uint32_t flags;
  AstTokenKind token_op;
  AstHirIntrinsicKind intrinsic;
  AstMemoryOrder memory_order;
  AstMemoryOrder failure_memory_order;
  AstName symbol;
  AstName module;
  AstHirLocalId source_local;
  AstMirStackSlotId stack_slot;
  uint32_t aux_index;
  uint32_t native_region;
  AstSemaConstInfo constant;
  const AstSemaType *call_signature;
  const AstMirVReg *operands;
  uint32_t operand_count;
  AstMirMemoryId memory_in;
  AstMirMemoryId memory_out;
  AstHirInstId source_hir_instruction;
} AstMirInstructionInfo;

typedef struct AstMirTerminatorInfo {
  AstMirTermKind kind;
  AstSpan span;
  AstMirVReg condition_or_value;
  AstMirBlockId target_true;
  AstMirBlockId target_false;
  const AstMirVReg *true_args;
  uint32_t true_arg_count;
  const AstMirVReg *false_args;
  uint32_t false_arg_count;
  AstMirMemoryId memory;
} AstMirTerminatorInfo;

typedef struct AstMirBlockInfo {
  AstMirBlockId id;
  AstSpan span;
  const AstMirVReg *arguments;
  /* Each valid entry records the HIR local promoted into this SSA block
   * argument. */
  const AstHirLocalId *argument_source_locals;
  uint32_t argument_count;
  const AstMirInstId *instructions;
  uint32_t instruction_count;
  AstMirTerminatorInfo terminator;
  AstMirMemoryId memory_argument;
  uint32_t predecessor_count;
  uint32_t successor_count;
  AstMirBlockId immediate_dominator;
  uint32_t reverse_postorder;
  uint32_t loop_depth;
  uint32_t live_in_count;
  uint32_t live_out_count;
  uint8_t reachable;
} AstMirBlockInfo;

typedef struct AstMirFunctionInfo {
  AstName module_name;
  AstName name;
  const AstSemaType *type;
  AstSpan span;
  uint32_t flags;
  AstMirBlockId entry_block;
  uint32_t block_count;
  uint32_t instruction_count;
  uint32_t vreg_count;
  uint32_t memory_value_count;
  uint32_t stack_slot_count;
  uint32_t promoted_local_count;
  uint32_t phi_count;
  uint32_t memory_phi_count;
  AstHirFramePolicy frame_policy;
  uint32_t stack_alignment;
  AstHirTextValue abi;
  AstHirTextValue link_name;
  AstHirTextValue target_constraint;
  AstHirTextValue requires_constraint;
} AstMirFunctionInfo;

ASTER_API AstMirConfig ast_mir_config_default(void);
ASTER_API AstMirProgram *ast_mir_build(const AstFrontend *frontend,
                                       AstHirProgram *hir,
                                       const AstMirConfig *config);
ASTER_API void ast_mir_destroy(AstMirProgram *program);
/* PRE enforces HIR-fidelity with no optimization certificates. POST accepts
 * only individually proved replacements and validates the rewritten SSA/CFG. */
typedef enum AstMirVerifyMode {
  AST_MIR_VERIFY_PRE_OPT = 0,
  AST_MIR_VERIFY_POST_OPT = 1
} AstMirVerifyMode;
ASTER_API int ast_mir_verify(AstMirProgram *program);
ASTER_API int ast_mir_verify_mode(AstMirProgram *program,
                                  AstMirVerifyMode mode);
/* Independent CFG/SSA/memory-chain verifier. Does not establish HIR
 * equivalence; use the origin/certificate verifier in addition for optimization
 * soundness. */
ASTER_API int ast_mir_verify_structural(AstMirProgram *program);
/* Experimental, explicit opt-in: verified scalar GVN and neutral identities.
 * The pre-optimization HIR fidelity invariant is checked before any rewrite.
 * Rewritten nodes retain independent equivalence certificates checked by MIR.
 */
typedef struct AstMirOptimizeStats {
  uint64_t examined;
  uint64_t rewritten;
  uint64_t cross_block_rewritten;
  uint64_t skipped_effectful;
  uint64_t verifier_runs;
  /* Appended for the experimental 1.2.0-dev API; existing fields retain order.
   */
  uint64_t constant_folded;
  uint64_t branch_folded; /* WIP7: literal-proved CFG edge canonicalization */
  uint64_t blocks_removed;
  uint64_t instructions_removed;
  uint64_t phis_removed;
  uint64_t edges_threaded;
  uint64_t rounds;
} AstMirOptimizeStats;
ASTER_API int ast_mir_optimize_local_gvn(AstMirProgram *program,
                                         AstMirOptimizeStats *stats);
ASTER_API int ast_mir_optimize_scalar(AstMirProgram *program,
                                      AstMirOptimizeStats *stats);
/* Experimental SCCP constant materialization: certified replay from original
 * SSA and executable edges; never rewrites CFG, effects, or block arguments. */
ASTER_API int ast_mir_optimize_sccp_constants(AstMirProgram *program,
                                              AstMirOptimizeStats *stats);
/* Experimental CFG transformation: replace only literal-proved COND_BR with
 * BR; rebuild CFG, memory SSA and liveness, and validate HIR-origin proofs.
 * Does NOT perform DCE, remove blocks, or rewrite PHI block arguments. */
ASTER_API int ast_mir_optimize_const_branches(AstMirProgram *program,
                                              AstMirOptimizeStats *stats);
/* Transactional structural SCCP/CFG/PHI/mark-sweep DCE. On failure the original
 * function table is restored. POST verification replays against an owned,
 * HIR-checked snapshot and independently verifies CFG/SSA/effects/liveness. */
ASTER_API int ast_mir_optimize(AstMirProgram *program,
                               AstMirOptimizeStats *stats);

typedef enum AstMirInlineMode {
  AST_MIR_INLINE_NONE = 0,
  AST_MIR_INLINE_BALANCED,
  AST_MIR_INLINE_SIZE
} AstMirInlineMode;

typedef struct AstMirInlineConfig {
  AstMirInlineMode mode;
  uint32_t max_sites;
  uint32_t max_depth;
  uint32_t max_callee_instructions;
  uint32_t max_function_instructions;
  uint32_t max_module_growth_percent;
  uint8_t closed_world; /* Definitions cannot be replaced by symbol
                           interposition. */
} AstMirInlineConfig;

typedef struct AstMirInlineStats {
  uint64_t considered, inlined, rejected_recursive, rejected_contract;
  uint64_t rejected_budget, rejected_cost, instructions_before,
      instructions_after;
  uint32_t max_depth;
  uint64_t forced; /* Successful @inline sites; explicit size-guard override. */
} AstMirInlineStats;

typedef struct AstMirInlineSiteInfo {
  AstName callee_module, callee_name;
  AstSpan call_span;
  uint32_t parent; /* One-based parent site, or 0 for an original caller. */
} AstMirInlineSiteInfo;

ASTER_API AstMirInlineConfig ast_mir_inline_config_default(void);
/* Transactional whole-module direct-call inlining. Preserves an owned source
 * snapshot, source-site chains, CFG/SSA/effects and deterministic growth
 * limits. Accepts original or scalar-optimized MIR; repeated inlining is
 * rejected. */
ASTER_API int ast_mir_inline(AstMirProgram *program,
                             const AstMirInlineConfig *config,
                             AstMirInlineStats *stats);
ASTER_API uint32_t
ast_mir_function_inline_site_count(const AstMirFunction *function);
ASTER_API int ast_mir_function_inline_site(const AstMirFunction *function,
                                           uint32_t site,
                                           AstMirInlineSiteInfo *out);
/* Opt-in, read-only SCCP executable-edge/value-lattice analysis.
 * UNKNOWN is the lattice bottom; CONSTANT joins to OVERDEFINED on conflict.
 * No CFG rewrite, DCE or speculative removal of MAY_TRAP is performed.
 * The caller owns result arrays and must call ast_mir_sccp_result_destroy().
 * An output result must be uninitialized/zeroed before analysis. */
typedef enum AstMirSccpKind {
  AST_MIR_SCCP_UNKNOWN = 0,
  AST_MIR_SCCP_CONSTANT = 1,
  AST_MIR_SCCP_OVERDEFINED = 2
} AstMirSccpKind;
typedef struct AstMirSccpValue {
  AstMirSccpKind kind;
  uint64_t value;
} AstMirSccpValue;
typedef struct AstMirSccpResult {
  AstMirSccpValue *values;
  uint8_t *executable_blocks;
  /* For each block b: true/first edge at 2*b, false/second at 2*b+1. */
  uint8_t *executable_edges;
  uint32_t value_count;
  uint32_t block_count;
  uint32_t executable_block_count;
  uint32_t executable_edge_count;
  uint32_t constant_value_count;
  uint32_t overdefined_value_count;
  uint32_t effectful_instructions;
  uint32_t iterations;
} AstMirSccpResult;
ASTER_API int ast_mir_sccp_analyze(AstMirProgram *program,
                                   size_t function_index,
                                   AstMirSccpResult *out);
ASTER_API void ast_mir_sccp_result_destroy(AstMirSccpResult *result);
ASTER_API AstMirStats ast_mir_stats(const AstMirProgram *program);
/* The caller retains ownership of HIR and must keep it alive for the MIR
 * lifetime. */
ASTER_API const AstHirProgram *ast_mir_source_hir(const AstMirProgram *program);
ASTER_API const AstMirDiagnostic *
ast_mir_diagnostics(const AstMirProgram *program, size_t *count_out);
ASTER_API const char *ast_mir_diag_code_name(AstMirDiagCode code);
ASTER_API const char *ast_mir_op_name(AstMirOp op);
ASTER_API const char *ast_mir_term_kind_name(AstMirTermKind kind);
ASTER_API const char *ast_mir_reg_bank_name(AstMirRegBank bank);

ASTER_API size_t ast_mir_function_count(const AstMirProgram *program);
ASTER_API const AstMirFunction *
ast_mir_function_at(const AstMirProgram *program, size_t index);
ASTER_API int ast_mir_find_function(const AstMirProgram *program,
                                    const char *module_name,
                                    const char *function_name,
                                    const AstMirFunction **out);
ASTER_API int ast_mir_function_info(const AstMirFunction *function,
                                    AstMirFunctionInfo *out);
ASTER_API int ast_mir_function_block(const AstMirFunction *function,
                                     AstMirBlockId block, AstMirBlockInfo *out);
ASTER_API int ast_mir_function_instruction(const AstMirFunction *function,
                                           AstMirInstId inst,
                                           AstMirInstructionInfo *out);
ASTER_API int ast_mir_function_vreg(const AstMirFunction *function,
                                    AstMirVReg vreg, AstMirVRegInfo *out);
ASTER_API int ast_mir_function_memory(const AstMirFunction *function,
                                      AstMirMemoryId memory,
                                      AstMirMemoryInfo *out);
ASTER_API int ast_mir_function_stack_slot(const AstMirFunction *function,
                                          AstMirStackSlotId slot,
                                          AstMirStackSlotInfo *out);
ASTER_API int ast_mir_function_predecessor(const AstMirFunction *function,
                                           AstMirBlockId block, uint32_t index,
                                           AstMirBlockId *out);
ASTER_API int ast_mir_function_successor(const AstMirFunction *function,
                                         AstMirBlockId block, uint32_t index,
                                         AstMirBlockId *out);
ASTER_API int ast_mir_function_is_live_in(const AstMirFunction *function,
                                          AstMirBlockId block, AstMirVReg vreg);
ASTER_API int ast_mir_function_is_live_out(const AstMirFunction *function,
                                           AstMirBlockId block,
                                           AstMirVReg vreg);
ASTER_API const AstHirFunction *
ast_mir_function_source_hir(const AstMirFunction *function);

ASTER_API void ast_mir_dump_program(
    const AstMirProgram *program,
    void (*write_fn)(void *user, const char *text, size_t length), void *user);
ASTER_API void ast_mir_dump_function(
    const AstMirFunction *function,
    void (*write_fn)(void *user, const char *text, size_t length), void *user);

#ifdef __cplusplus
}
#endif

#endif
