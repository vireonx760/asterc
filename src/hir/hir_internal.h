#ifndef ASTERASM_HIR_INTERNAL_H
#define ASTERASM_HIR_INTERNAL_H

#include "../core/parallel.h"
#include "asterasm/hir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AST_HIR_DEFAULT_ARENA_BLOCK (128u * 1024u)
#define AST_HIR_DEFAULT_MAX_DIAGNOSTICS 1024u
#define AST_HIR_DEFAULT_RECURSION_LIMIT 256u

typedef struct AstSemaGenericBinding AstSemaGenericBinding;

typedef struct AstHirValue {
  AstHirValueKind kind;
  const AstSemaType *type;
  AstSpan span;
  AstHirBlockId block;
  uint32_t definition_index;
} AstHirValue;

typedef struct AstHirInstruction {
  AstHirOp op;
  AstHirValueId result;
  const AstSemaType *type;
  AstSpan span;
  uint32_t effects;
  AstTokenKind token_op;
  AstHirIntrinsicKind intrinsic;
  AstMemoryOrder memory_order;
  AstMemoryOrder failure_memory_order;
  AstName symbol;
  AstName module;
  AstHirLocalId local;
  uint32_t aux_index;
  uint32_t native_region;
  AstSemaConstInfo constant;
  const AstSemaType *call_signature;
  const AstExpr *source_expr;
  const AstStmt *source_stmt;
  AstHirValueId *operands;
  uint32_t operand_count;
} AstHirInstruction;

typedef struct AstHirTerminator {
  AstHirTermKind kind;
  AstSpan span;
  AstHirValueId condition_or_value;
  AstHirBlockId target_true;
  AstHirBlockId target_false;
  AstHirValueId *true_args;
  uint32_t true_arg_count;
  AstHirValueId *false_args;
  uint32_t false_arg_count;
} AstHirTerminator;

typedef struct AstHirBlock {
  AstHirBlockId id;
  AstSpan span;
  AstHirValueId *arguments;
  uint32_t argument_count;
  uint32_t argument_capacity;
  AstHirInstId *instructions;
  uint32_t instruction_count;
  uint32_t instruction_capacity;
  AstHirTerminator term;
  uint32_t predecessor_count;
  uint32_t successor_count;
  AstHirBlockId immediate_dominator;
  uint32_t reverse_postorder;
  uint8_t reachable;
} AstHirBlock;

typedef struct AstHirLocal {
  AstName name;
  const AstSemaType *type;
  AstSpan span;
  uint32_t flags;
} AstHirLocal;

typedef struct AstHirGenericParam {
  AstName name;
  AstHirGenericParamKind kind;
  const AstSemaType *const_type;
  AstSpan span;
  uint32_t index;
} AstHirGenericParam;

struct AstHirInitializer {
  AstHirInitializerKind kind;
  const AstSemaType *type;
  AstSpan span;
  AstSemaConstInfo constant;
  AstTokenKind literal_kind;
  AstHirText literal;
  AstSpan literal_span;
  AstName module;
  AstName symbol;
  int64_t addend;
  AstHirInitializer **children;
  uint32_t child_count;
};

struct AstHirNativeExpr {
  AstHirNativeExprKind kind;
  AstSpan span;
  const AstSemaType *type;
  AstSemaConstInfo constant;
  AstTokenKind token_kind;
  AstHirText literal;
  AstName name;
  AstHirNativeBindingKind binding_kind;
  uint32_t binding_index;
  AstHirNativeExpr **children;
  uint32_t child_count;
};

typedef AstHirNativeAttributeInfo AstHirNativeAttribute;
typedef AstHirNativeOperandInfo AstHirNativeOperand;

typedef struct AstHirNativeStmtCanonical {
  AstHirNativeStmtKind kind;
  AstSpan span;
  AstName mnemonic;
  AstHirNativeOperand *operands;
  uint32_t operand_count;
  AstHirNativeAttribute *attributes;
  uint32_t attribute_count;
  uint8_t *bytes;
  uint32_t byte_count;
} AstHirNativeStmtCanonical;

struct AstHirNativeRegion {
  AstName arch;
  AstSpan span;
  uint32_t flags;
  AstName *inputs;
  uint32_t input_count;
  AstName *outputs;
  uint32_t output_count;
  AstHirNativeStmtCanonical *statements;
  uint32_t statement_count;
};

typedef struct AstHirDispatchCase {
  AstSpan span;
  AstSpan condition_span;
  AstSemaConstInfo condition;
  AstName target;
  uint8_t is_else;
} AstHirDispatchCase;

typedef struct AstHirTarget {
  AstName module_name;
  uint32_t file_id;
  AstSpan span;
  AstHirText triple;
  AstSpan triple_span;
  AstHirTargetItemInfo *items;
  uint32_t item_count;
} AstHirTarget;

struct AstHirFunction {
  AstArena arena;
  AstName module_name;
  AstName name;
  const AstSemaType *type;
  AstSpan span;
  uint32_t flags;
  AstHirBlockId entry_block;
  const AstDecl *source_decl;
  uint32_t file_id;

  AstHirBlock *blocks;
  uint32_t block_count;
  uint32_t block_capacity;
  AstHirInstruction *instructions;
  uint32_t instruction_count;
  uint32_t instruction_capacity;
  AstHirValue *values;
  uint32_t value_count;
  uint32_t value_capacity;
  AstHirLocal *locals;
  uint32_t local_count;
  uint32_t local_capacity;
  AstHirGenericParam *generic_params;
  uint32_t generic_param_count;
  AstHirNativeRegion *native_regions;
  uint32_t native_region_count;
  uint32_t native_region_capacity;
  /* Function-owned while lowering in parallel; merged into module globals
     after generic specialization. Payloads and types live in this arena. */
  struct AstHirGlobal *literal_globals;
  size_t literal_global_count;
  size_t literal_global_capacity;
  AstHirDispatchCase *dispatch_cases;
  uint32_t dispatch_case_count;
  AstHirFramePolicy frame_policy;
  uint32_t stack_alignment;
  AstHirTextValue abi;
  AstHirTextValue link_name;
  AstHirTextValue target_constraint;
  AstHirTextValue requires_constraint;

  AstHirDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  uint8_t oom;
};

struct AstHirModule {
  AstName name;
  uint32_t file_id;
  uint32_t first_function;
  uint32_t function_count;
  uint32_t first_global;
  uint32_t global_count;
  const AstModule *source_module;
};

typedef struct AstHirGlobal {
  AstName module_name;
  AstName name;
  const AstSemaType *type;
  AstSpan span;
  AstHirText section_name;
  AstSpan section_span;
  uint32_t flags;
  uint32_t alignment;
  uint32_t section_alignment;
  AstHirInitializer *initializer;
  const AstInitializer *source_initializer;
  const AstDecl *source_decl;
} AstHirGlobal;

struct AstHirProgram {
  const AstFrontend *frontend;
  const AstSema *sema;
  AstHirConfig config;
  AstArena arena;
  AstHirModule *modules;
  size_t module_count;
  AstHirTarget *targets;
  size_t target_count;
  AstHirFunction **functions;
  size_t function_count;
  AstHirGlobal *globals;
  size_t global_count;
  AstHirDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  AstHirStats stats;
  uint8_t oom;
};

typedef struct HirBinding {
  AstName name;
  AstHirLocalId local;
  AstHirValueId direct_value;
  AstSemaConstInfo direct_constant;
  const AstSemaType *type;
  uint32_t scope_depth;
  uint8_t has_direct_constant;
} HirBinding;

typedef struct HirLabel {
  AstName name;
  AstHirBlockId block;
  uint32_t scope_depth;
} HirLabel;

typedef struct HirLoopFrame {
  AstHirBlockId continue_block;
  AstHirBlockId break_block;
} HirLoopFrame;

typedef struct HirBuilder {
  AstHirProgram *program;
  AstHirFunction *function;
  const AstModule *module;
  AstHirBlockId current;
  HirBinding *bindings;
  size_t binding_count;
  size_t binding_capacity;
  HirLabel *labels;
  size_t label_count;
  size_t label_capacity;
  HirLoopFrame *loops;
  size_t loop_count;
  size_t loop_capacity;
  uint32_t scope_depth;
  uint32_t recursion_depth;
  uint32_t unsafe_depth;
  const AstSemaGenericBinding *template_bindings;
  const AstSemaGenericBinding *specialization_values;
  size_t specialization_count;
} HirBuilder;

int hir_reserve(void **ptr, size_t elem_size, size_t *capacity, size_t needed);
void hir_function_diag(AstHirFunction *fn, AstHirDiagCode code,
                       AstDiagSeverity severity, AstSpan span, const char *fmt,
                       ...) ASTER_PRINTF_LIKE(5, 6);
void hir_program_diag(AstHirProgram *program, AstHirDiagCode code,
                      AstDiagSeverity severity, AstSpan span, const char *fmt,
                      ...) ASTER_PRINTF_LIKE(5, 6);
void hir_merge_function_diagnostics(AstHirProgram *program);

AstHirFunction *hir_function_create(AstHirProgram *program, AstName module_name,
                                    uint32_t file_id, const AstDecl *decl,
                                    const AstSemaType *type, uint32_t flags);
void hir_function_destroy(AstHirFunction *fn);
AstHirBlockId hir_add_block(AstHirFunction *fn, AstSpan span);
AstHirValueId hir_add_block_argument(AstHirFunction *fn, AstHirBlockId block,
                                     const AstSemaType *type, AstSpan span);
AstHirLocalId hir_add_local(AstHirFunction *fn, AstName name,
                            const AstSemaType *type, AstSpan span,
                            uint32_t flags);
AstHirInstId hir_emit_instruction(AstHirFunction *fn, AstHirBlockId block,
                                  AstHirInstruction inst,
                                  const AstHirValueId *operands,
                                  uint32_t operand_count);
AstHirValueId hir_emit_value_instruction(AstHirFunction *fn,
                                         AstHirBlockId block,
                                         AstHirInstruction inst,
                                         const AstSemaType *type,
                                         const AstHirValueId *operands,
                                         uint32_t operand_count);
int hir_set_terminator(AstHirFunction *fn, AstHirBlockId block,
                       AstHirTerminator term, const AstHirValueId *true_args,
                       uint32_t true_arg_count, const AstHirValueId *false_args,
                       uint32_t false_arg_count);
int hir_block_terminated(const AstHirFunction *fn, AstHirBlockId block);

int hir_lower_function(AstHirProgram *program, AstHirFunction *fn,
                       const AstModule *module);
int hir_lower_function_specialized(
    AstHirProgram *program, AstHirFunction *fn, const AstModule *module,
    const AstSemaGenericBinding *template_bindings,
    const AstSemaGenericBinding *specialization_values,
    size_t specialization_count);
AstHirInitializer *hir_build_initializer(AstHirProgram *program,
                                         const AstModule *module,
                                         const AstInitializer *source,
                                         const AstSemaType *type);
uint32_t hir_build_native_region(HirBuilder *builder, const AstStmt *stmt);
int hir_analyze_cfg(AstHirFunction *fn);
int hir_verify_function(AstHirProgram *program, AstHirFunction *fn);

int hir_name_eq(AstName a, AstName b);
int hir_name_eq_text(AstName name, const char *text);
int hir_type_equal(const AstSemaType *a, const AstSemaType *b);
char *hir_name_to_cstr(AstArena *arena, AstName name);
AstName hir_copy_name(AstArena *arena, AstName name);
AstHirText hir_copy_text(AstArena *arena, const char *data, size_t length);
int hir_copy_source_text(const AstHirProgram *program, AstArena *arena,
                         AstSpan span, AstHirText *out);
int hir_decode_literal_text(const AstHirProgram *program, AstArena *arena,
                            AstSpan span, AstTokenKind kind, AstHirText *out);
int hir_get_expr_fact(const AstHirProgram *program, const AstExpr *expr,
                      AstSemaExprFact *out);
const AstSemaType *hir_expr_type(const AstHirProgram *program,
                                 const AstExpr *expr);
int hir_const_bool(const AstHirProgram *program, const AstExpr *expr, int *out);
int hir_const_u64(const AstHirProgram *program, const AstExpr *expr,
                  uint64_t *out);

#endif
