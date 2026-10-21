#ifndef ASTERASM_MIR_INTERNAL_H
#define ASTERASM_MIR_INTERNAL_H

#include "../core/parallel.h"
#include "../hir/hir_internal.h"
#include "asterasm/mir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AST_MIR_DEFAULT_ARENA_BLOCK ((size_t)128u * 1024u)
#define AST_MIR_DEFAULT_MAX_DIAGNOSTICS 1024u
#define AST_MIR_MAX_SUCCESSORS 2u

typedef struct AstMirVRegData {
  AstMirVRegInfo info;
} AstMirVRegData;

typedef struct AstMirMemoryData {
  AstMirMemoryInfo info;
} AstMirMemoryData;

typedef struct AstMirInstruction {
  AstMirInstructionInfo info;
  AstMirVReg *operands;
} AstMirInstruction;

typedef struct AstMirTerminator {
  AstMirTerminatorInfo info;
  AstMirVReg *true_args;
  uint32_t true_arg_capacity;
  AstMirVReg *false_args;
  uint32_t false_arg_capacity;
} AstMirTerminator;

typedef struct AstMirBlock {
  AstMirBlockId id;
  AstSpan span;
  AstMirVReg *arguments;
  AstHirLocalId *argument_source_locals;
  uint32_t argument_count;
  uint32_t argument_capacity;
  uint32_t hir_argument_count;
  AstMirInstId *instructions;
  uint32_t instruction_count;
  uint32_t instruction_capacity;
  AstMirTerminator term;
  AstMirMemoryId memory_argument;
  AstMirBlockId *predecessors;
  uint32_t predecessor_count;
  uint32_t predecessor_capacity;
  AstMirBlockId successors[AST_MIR_MAX_SUCCESSORS];
  uint32_t successor_count;
  AstMirBlockId immediate_dominator;
  uint32_t reverse_postorder;
  uint32_t loop_depth;
  uint32_t live_in_count;
  uint32_t live_out_count;
  uint64_t position_begin;
  uint64_t position_end;
  uint8_t reachable;
} AstMirBlock;

typedef struct AstMirStackSlot {
  AstMirStackSlotInfo info;
} AstMirStackSlot;

/* Only exact, effect-free same-block CSE is currently admissible.
 * Certificates hold independent copies of ORIGINAL operands. */
/* Structural branch certificate.  The original MIR terminator is retained
 * independently of the rewritten control edge, so HIR provenance can still
 * be checked.  Only direct, HIR-owned boolean literals qualify in WIP7. */
typedef struct MirBranchCertificate {
  AstMirTerminator original;
  /* Immutable, independently owned value snapshots for both edge vectors. */
  AstMirVReg *saved_true;
  AstMirVReg *saved_false;
  uint8_t valid;
  uint8_t take_true;
} MirBranchCertificate;

typedef struct MirGvnCertificate {
  AstMirInstruction original;
  AstMirInstId witness;
  /* valid: 1 = GVN, 2 = identity, 3 = local fold, 4 = original-SSA SCCP proof.
   */
  uint8_t valid;
} MirGvnCertificate;

typedef struct MirOptimizationProof {
  /* Owned, PRE-OPT HIR-verified snapshot. Never aliases the emitted graph. */
  AstMirFunction *original;
} MirOptimizationProof;

typedef struct MirInlineProof {
  AstMirFunction **original;
  size_t function_count;
  AstMirInlineConfig config;
} MirInlineProof;

struct AstMirFunction {
  AstArena arena;
  const AstHirFunction *source_hir;
  AstName module_name;
  AstName name;
  const AstSemaType *type;
  AstSpan span;
  uint32_t flags;
  AstMirBlockId entry_block;
  AstHirFramePolicy frame_policy;
  uint32_t stack_alignment;
  AstHirTextValue abi;
  AstHirTextValue link_name;
  AstHirTextValue target_constraint;
  AstHirTextValue requires_constraint;

  AstMirBlock *blocks;
  uint32_t block_count;
  AstMirInstruction *instructions;
  MirGvnCertificate *gvn_certificates;
  MirBranchCertificate *branch_certificates;
  MirOptimizationProof *optimization_proof;
  AstMirInlineSiteInfo *inline_sites;
  uint32_t inline_site_count;
  uint32_t instruction_count;
  uint32_t instruction_capacity;
  AstMirVRegData *vregs;
  uint32_t vreg_count;
  uint32_t vreg_capacity;
  AstMirMemoryData *memory_values;
  uint32_t memory_value_count;
  uint32_t memory_value_capacity;
  AstMirStackSlot *stack_slots;
  uint32_t stack_slot_count;
  uint32_t stack_slot_capacity;
  uint32_t promoted_local_count;
  uint32_t phi_count;
  uint32_t memory_phi_count;

  uint64_t *live_in;
  uint64_t *live_out;
  uint32_t liveness_word_count;

  AstMirDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  uint8_t oom;
};

struct AstMirProgram {
  const AstFrontend *frontend;
  AstHirProgram *hir;
  AstMirConfig config;
  AstArena arena;
  AstMirFunction **functions;
  MirInlineProof *inline_proof;
  size_t function_count;
  AstMirDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  size_t build_diagnostic_count;
  AstMirStats stats;
  uint8_t oom;
};

typedef struct MirLowerContext {
  AstMirProgram *program;
  AstMirFunction *function;
  const AstHirFunction *hir;
  AstMirVReg *hir_values;
  uint32_t *hir_value_use_count;
  uint32_t *hir_value_direct_call_count;
  AstMirStackSlotId *local_slots;
  uint32_t *local_promoted_index;
  uint8_t *local_promotable;
  uint8_t *local_address_taken;
  AstMirVReg *local_defs;
  AstMirVReg *local_phis;
  uint8_t *local_reading;
  AstMirMemoryId current_memory;
  AstMirBlockId current_block;
  uint32_t local_count;
  uint32_t promoted_local_count;
  uint8_t has_function_refs;
} MirLowerContext;

int mir_reserve(void **ptr, size_t elem_size, size_t *capacity, size_t needed);
void *mir_malloc_array(size_t count, size_t element_size);
AstName mir_copy_name(AstArena *arena, AstName name);
AstHirText mir_copy_text(AstArena *arena, AstHirText text);
AstHirTextValue mir_copy_text_value(AstArena *arena, AstHirTextValue value);
int mir_name_eq_cstr(AstName name, const char *text);
AstMirRegBank mir_type_bank(const AstSemaType *type);
uint32_t mir_type_bit_width(const AstSemaType *type);
int mir_type_equal(const AstSemaType *a, const AstSemaType *b);
uint32_t mir_instruction_flags(uint32_t effects, AstMirOp op);
int mir_instruction_is_effectful(const AstMirInstruction *inst);

void mir_function_diag(AstMirFunction *fn, AstMirDiagCode code,
                       AstDiagSeverity severity, AstSpan span, const char *fmt,
                       ...) ASTER_PRINTF_LIKE(5, 6);
void mir_program_diag(AstMirProgram *program, AstMirDiagCode code,
                      AstDiagSeverity severity, AstSpan span, const char *fmt,
                      ...) ASTER_PRINTF_LIKE(5, 6);
void mir_merge_function_diagnostics(AstMirProgram *program);

AstMirFunction *mir_function_create(AstMirProgram *program,
                                    const AstHirFunction *hir_function);
void mir_function_destroy(AstMirFunction *fn);
AstMirVReg mir_add_vreg(AstMirFunction *fn, AstMirVRegKind kind,
                        const AstSemaType *type, AstSpan span,
                        AstMirBlockId block, uint32_t definition_index,
                        uint32_t flags);
AstMirMemoryId mir_add_memory(AstMirFunction *fn, AstMirMemoryKind kind,
                              AstMirBlockId block, uint32_t definition_index);
AstMirInstId mir_emit_instruction(AstMirFunction *fn, AstMirBlockId block,
                                  AstMirInstruction inst,
                                  const AstMirVReg *operands,
                                  uint32_t operand_count);
int mir_append_block_argument(AstMirFunction *fn, AstMirBlockId block,
                              AstMirVReg vreg, AstHirLocalId source_local);
int mir_set_terminator(AstMirFunction *fn, AstMirBlockId block,
                       AstMirTerminator term, const AstMirVReg *true_args,
                       uint32_t true_count, const AstMirVReg *false_args,
                       uint32_t false_count);

int mir_lower_function(AstMirProgram *program, AstMirFunction *function);
AstMirOp mir_op_from_hir(AstHirOp op);
AstMirTermKind mir_term_from_hir(AstHirTermKind kind);
int mir_analyze_cfg(AstMirFunction *function);
int mir_link_memory_ssa(AstMirFunction *function);
int mir_analyze_liveness(AstMirFunction *function);
int mir_verify_function(AstMirProgram *program, AstMirFunction *function);
int mir_verify_canonical_liveness(AstMirProgram *program,
                                  AstMirFunction *function);
int mir_sccp_analyze_function(const AstMirFunction *fn, AstMirSccpResult *out,
                              int original_view);
int mir_sccp_verify_certificates(AstMirProgram *program, AstMirFunction *fn);
int mir_sccp_complete_edges(const AstMirFunction *fn,
                            const AstMirSccpResult *result);
int mir_branch_verify_certificates(AstMirProgram *program,
                                   const AstMirFunction *fn);
int mir_opt_fold_unsigned(const AstMirFunction *fn,
                          const AstMirInstruction *original,
                          AstSemaConstInfo *out);
int mir_opt_gvn_match(const AstMirInstruction *a, const AstMirInstruction *b);
int mir_opt_identity_match(const AstMirFunction *fn,
                           const AstMirInstruction *original,
                           const AstMirInstruction *constant, AstMirVReg *kept);
void mir_refresh_stats_after_verify(AstMirProgram *program);
int mir_verify_optimization(AstMirProgram *program,
                            const AstMirFunction *function);
int mir_verify_structural_function(AstMirProgram *program,
                                   AstMirFunction *function);
AstMirFunction *mir_clone_function_header(const AstMirFunction *source);
AstMirFunction *mir_optimize_function(AstMirProgram *program,
                                      const AstMirFunction *source,
                                      AstMirOptimizeStats *stats);
int mir_functions_equal(const AstMirFunction *a, const AstMirFunction *b);
int mir_verify_inlining(AstMirProgram *program);
void mir_inline_proof_destroy(MirInlineProof *proof);

#endif
