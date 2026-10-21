#ifndef ASTERASM_HIR_H
#define ASTERASM_HIR_H

#include "asterasm/sema.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstHirProgram AstHirProgram;
typedef struct AstHirModule AstHirModule;
typedef struct AstHirFunction AstHirFunction;
typedef struct AstHirInitializer AstHirInitializer;
typedef struct AstHirNativeRegion AstHirNativeRegion;
typedef struct AstHirNativeExpr AstHirNativeExpr;

typedef uint32_t AstHirValueId;
typedef uint32_t AstHirBlockId;
typedef uint32_t AstHirInstId;
typedef uint32_t AstHirLocalId;

#define AST_HIR_INVALID_ID UINT32_MAX

typedef enum AstHirOp {
  AST_HIR_OP_INVALID = 0,
  AST_HIR_OP_CONSTANT,
  AST_HIR_OP_LOCAL_LOAD,
  AST_HIR_OP_LOCAL_STORE,
  AST_HIR_OP_LOCAL_ADDR,
  AST_HIR_OP_GLOBAL_ADDR,
  AST_HIR_OP_GLOBAL_LOAD,
  AST_HIR_OP_GLOBAL_STORE,
  AST_HIR_OP_FUNCTION_REF,
  AST_HIR_OP_GENERIC_CONST,
  AST_HIR_OP_UNARY,
  AST_HIR_OP_BINARY,
  AST_HIR_OP_CAST,
  AST_HIR_OP_SELECT,
  AST_HIR_OP_CALL,
  AST_HIR_OP_INDEX,
  AST_HIR_OP_MEMBER,
  AST_HIR_OP_ADDRESS_OF,
  AST_HIR_OP_LOAD,
  AST_HIR_OP_STORE,
  AST_HIR_OP_ATOMIC,
  AST_HIR_OP_FENCE,
  AST_HIR_OP_INTRINSIC,
  AST_HIR_OP_BIND,
  AST_HIR_OP_NATIVE,
  AST_HIR_OP_NATIVE_RESULT,
  AST_HIR_OP_UNDEF
} AstHirOp;

typedef enum AstHirIntrinsicKind {
  AST_HIR_INTRINSIC_NONE = 0,
  AST_HIR_INTRINSIC_SELECT,
  AST_HIR_INTRINSIC_CLZ,
  AST_HIR_INTRINSIC_CTZ,
  AST_HIR_INTRINSIC_POPCNT,
  AST_HIR_INTRINSIC_BSWAP,
  AST_HIR_INTRINSIC_BITREV,
  AST_HIR_INTRINSIC_ROL,
  AST_HIR_INTRINSIC_ROR,
  AST_HIR_INTRINSIC_ADD_CHECKED,
  AST_HIR_INTRINSIC_SUB_CHECKED,
  AST_HIR_INTRINSIC_MUL_CHECKED,
  AST_HIR_INTRINSIC_ADD_WRAP,
  AST_HIR_INTRINSIC_SUB_WRAP,
  AST_HIR_INTRINSIC_MUL_WRAP,
  AST_HIR_INTRINSIC_ADD_SAT,
  AST_HIR_INTRINSIC_SUB_SAT,
  AST_HIR_INTRINSIC_PTR_TO_INT,
  AST_HIR_INTRINSIC_INT_TO_PTR,
  AST_HIR_INTRINSIC_SIZEOF,
  AST_HIR_INTRINSIC_OFFSETOF,
  AST_HIR_INTRINSIC_SYMBOL_ADDR,
  AST_HIR_INTRINSIC_FEATURE
} AstHirIntrinsicKind;

typedef enum AstMemoryOrder {
  AST_MEMORY_ORDER_NONE = 0,
  AST_MEMORY_ORDER_RELAXED,
  AST_MEMORY_ORDER_ACQUIRE,
  AST_MEMORY_ORDER_RELEASE,
  AST_MEMORY_ORDER_ACQ_REL,
  AST_MEMORY_ORDER_SEQ_CST
} AstMemoryOrder;

ASTER_API const char *ast_memory_order_name(AstMemoryOrder order);

typedef enum AstHirTermKind {
  AST_HIR_TERM_NONE = 0,
  AST_HIR_TERM_BR,
  AST_HIR_TERM_COND_BR,
  AST_HIR_TERM_RETURN,
  AST_HIR_TERM_UNREACHABLE
} AstHirTermKind;

typedef enum AstHirValueKind {
  AST_HIR_VALUE_INVALID = 0,
  AST_HIR_VALUE_BLOCK_ARGUMENT,
  AST_HIR_VALUE_INSTRUCTION_RESULT
} AstHirValueKind;

typedef enum AstHirLocalFlags {
  AST_HIR_LOCAL_NONE = 0,
  AST_HIR_LOCAL_MUTABLE = 1u << 0,
  AST_HIR_LOCAL_PARAMETER = 1u << 1,
  AST_HIR_LOCAL_CONST = 1u << 2,
  AST_HIR_LOCAL_SYNTHETIC = 1u << 3
} AstHirLocalFlags;

typedef struct AstHirText {
  const char *data;
  uint32_t length;
} AstHirText;

typedef struct AstHirTextValue {
  AstHirText text;
  AstSpan source_span;
  AstTokenKind source_kind;
  uint8_t is_literal;
  uint8_t is_present;
} AstHirTextValue;

typedef enum AstHirFramePolicy {
  AST_HIR_FRAME_DEFAULT = 0,
  AST_HIR_FRAME_AUTO,
  AST_HIR_FRAME_VALIDATE,
  AST_HIR_FRAME_NAKED
} AstHirFramePolicy;

typedef struct AstHirDispatchCaseInfo {
  AstSpan span;
  AstSpan condition_span;
  AstSemaConstInfo condition;
  AstName target;
  uint8_t is_else;
} AstHirDispatchCaseInfo;

typedef struct AstHirTargetItemInfo {
  AstSpan span;
  AstName name;
  AstSpan value_span;
  AstTokenKind value_literal_kind;
  AstSemaConstInfo constant;
  AstHirText value_text;
  AstHirText feature;
  AstSpan feature_span;
  int8_t feature_sign;
  uint8_t is_feature;
} AstHirTargetItemInfo;

typedef struct AstHirTargetInfo {
  AstName module_name;
  uint32_t file_id;
  AstSpan span;
  AstHirText triple;
  AstSpan triple_span;
  const AstHirTargetItemInfo *items;
  uint32_t item_count;
} AstHirTargetInfo;

typedef enum AstHirGlobalFlags {
  AST_HIR_GLOBAL_NONE = 0,
  AST_HIR_GLOBAL_CONST = 1u << 0,
  AST_HIR_GLOBAL_EXPORT = 1u << 1,
  AST_HIR_GLOBAL_READONLY = 1u << 2,
  AST_HIR_GLOBAL_WRITABLE = 1u << 3,
  AST_HIR_GLOBAL_BSS = 1u << 4
} AstHirGlobalFlags;

typedef enum AstHirFunctionFlags {
  AST_HIR_FUNCTION_NONE = 0,
  AST_HIR_FUNCTION_EXPORT = 1u << 0,
  AST_HIR_FUNCTION_EXTERN = 1u << 1,
  AST_HIR_FUNCTION_DISPATCH = 1u << 2,
  AST_HIR_FUNCTION_NAKED = 1u << 3,
  AST_HIR_FUNCTION_LEAF = 1u << 4,
  AST_HIR_FUNCTION_GENERIC = 1u << 5,
  AST_HIR_FUNCTION_NO_UNWIND = 1u << 6,
  AST_HIR_FUNCTION_INLINE = 1u << 7,
  AST_HIR_FUNCTION_NO_INLINE = 1u << 8
} AstHirFunctionFlags;

typedef enum AstHirInitializerKind {
  AST_HIR_INIT_NONE = 0,
  AST_HIR_INIT_ZERO,
  AST_HIR_INIT_CONSTANT,
  AST_HIR_INIT_LITERAL,
  AST_HIR_INIT_SYMBOL,
  AST_HIR_INIT_AGGREGATE
} AstHirInitializerKind;

typedef enum AstHirNativeRegionFlags {
  AST_HIR_NATIVE_NONE = 0,
  AST_HIR_NATIVE_EXACT = 1u << 0,
  AST_HIR_NATIVE_UNSAFE_EXACT = 1u << 1
} AstHirNativeRegionFlags;

typedef enum AstHirNativeExprKind {
  AST_HIR_NATIVE_EXPR_INVALID = 0,
  AST_HIR_NATIVE_EXPR_NAME,
  AST_HIR_NATIVE_EXPR_CONSTANT,
  AST_HIR_NATIVE_EXPR_LITERAL,
  AST_HIR_NATIVE_EXPR_TYPE,
  AST_HIR_NATIVE_EXPR_UNARY,
  AST_HIR_NATIVE_EXPR_BINARY,
  AST_HIR_NATIVE_EXPR_CONDITIONAL,
  AST_HIR_NATIVE_EXPR_CALL,
  AST_HIR_NATIVE_EXPR_INDEX,
  AST_HIR_NATIVE_EXPR_MEMBER,
  AST_HIR_NATIVE_EXPR_LOAD,
  AST_HIR_NATIVE_EXPR_CAST,
  AST_HIR_NATIVE_EXPR_ATOMIC,
  AST_HIR_NATIVE_EXPR_ATTRIBUTE
} AstHirNativeExprKind;

typedef enum AstHirNativeBindingKind {
  AST_HIR_NATIVE_BINDING_TARGET = 0,
  AST_HIR_NATIVE_BINDING_INPUT,
  AST_HIR_NATIVE_BINDING_OUTPUT
} AstHirNativeBindingKind;

typedef enum AstHirNativeStmtKind {
  AST_HIR_NATIVE_INSTRUCTION = 0,
  AST_HIR_NATIVE_BYTES
} AstHirNativeStmtKind;

typedef struct AstHirInitializerInfo {
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
  uint32_t child_count;
} AstHirInitializerInfo;

typedef struct AstHirNativeExprInfo {
  AstHirNativeExprKind kind;
  AstSpan span;
  const AstSemaType *type;
  AstSemaConstInfo constant;
  AstTokenKind token_kind;
  AstHirText literal;
  AstName name;
  AstHirNativeBindingKind binding_kind;
  uint32_t binding_index;
  uint32_t child_count;
} AstHirNativeExprInfo;

typedef struct AstHirNativeAttributeInfo {
  AstName name;
  AstSpan span;
  const AstHirNativeExpr *const *args;
  uint32_t arg_count;
} AstHirNativeAttributeInfo;

typedef struct AstHirNativeOperandInfo {
  AstName name;
  AstSpan span;
  uint8_t is_memory;
  const AstSemaType *memory_type;
  const AstHirNativeExpr *value;
  const AstHirNativeExpr *memory_address;
} AstHirNativeOperandInfo;

typedef struct AstHirNativeStmtInfo {
  AstHirNativeStmtKind kind;
  AstSpan span;
  AstName mnemonic;
  const AstHirNativeOperandInfo *operands;
  uint32_t operand_count;
  const AstHirNativeAttributeInfo *attributes;
  uint32_t attribute_count;
  const uint8_t *bytes;
  uint32_t byte_count;
} AstHirNativeStmtInfo;

typedef struct AstHirNativeRegionInfo {
  AstName arch;
  AstSpan span;
  uint32_t flags;
  const AstName *inputs;
  uint32_t input_count;
  const AstName *outputs;
  uint32_t output_count;
  uint32_t statement_count;
} AstHirNativeRegionInfo;

typedef enum AstHirDiagCode {
  AST_HIR_DIAG_NONE = 4000,
  AST_HIR_DIAG_SEMA_REQUIRED,
  AST_HIR_DIAG_MISSING_SEMA_FACT,
  AST_HIR_DIAG_INVALID_COMPTIME,
  AST_HIR_DIAG_UNSUPPORTED_MACRO_VALUE,
  AST_HIR_DIAG_UNKNOWN_BINDING,
  AST_HIR_DIAG_INVALID_CFG,
  AST_HIR_DIAG_MISSING_TERMINATOR,
  AST_HIR_DIAG_INVALID_SUCCESSOR,
  AST_HIR_DIAG_BRANCH_ARGUMENT_MISMATCH,
  AST_HIR_DIAG_INVALID_OPERAND,
  AST_HIR_DIAG_TYPE_MISMATCH,
  AST_HIR_DIAG_DOMINANCE,
  AST_HIR_DIAG_INVALID_EFFECTS,
  AST_HIR_DIAG_INVALID_INITIALIZER,
  AST_HIR_DIAG_INVALID_NATIVE_REGION,
  AST_HIR_DIAG_RECURSION_LIMIT,
  AST_HIR_DIAG_INTERNAL,
  AST_HIR_DIAG_TOO_MANY_DIAGNOSTICS
} AstHirDiagCode;

typedef struct AstHirDiagnostic {
  AstHirDiagCode code;
  AstDiagSeverity severity;
  AstSpan span;
  const char *path;
  uint32_t line;
  uint32_t column;
  const char *message;
} AstHirDiagnostic;

typedef struct AstHirConfig {
  size_t thread_count;
  size_t arena_block_size;
  uint32_t max_diagnostics;
  uint32_t recursion_limit;
  uint8_t verify_after_build;
} AstHirConfig;

typedef struct AstHirStats {
  size_t module_count;
  size_t function_count;
  size_t extern_function_count;
  size_t dispatch_function_count;
  size_t global_count;
  size_t block_count;
  size_t instruction_count;
  size_t value_count;
  size_t local_count;
  size_t terminator_count;
  size_t unreachable_block_count;
  size_t error_count;
  size_t warning_count;
  size_t arena_bytes_used;
  size_t arena_bytes_reserved;
} AstHirStats;

typedef struct AstHirValueInfo {
  AstHirValueKind kind;
  const AstSemaType *type;
  AstSpan span;
  AstHirBlockId block;
  uint32_t definition_index;
} AstHirValueInfo;

typedef struct AstHirLocalInfo {
  AstName name;
  const AstSemaType *type;
  AstSpan span;
  uint32_t flags;
} AstHirLocalInfo;

typedef enum AstHirGenericParamKind {
  AST_HIR_GENERIC_TYPE = 0,
  AST_HIR_GENERIC_CONST
} AstHirGenericParamKind;

typedef struct AstHirGenericParamInfo {
  AstName name;
  AstHirGenericParamKind kind;
  const AstSemaType *const_type;
  AstSpan span;
  uint32_t index;
} AstHirGenericParamInfo;

typedef struct AstHirInstructionInfo {
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
  const AstHirValueId *operands;
  uint32_t operand_count;
} AstHirInstructionInfo;

typedef struct AstHirTerminatorInfo {
  AstHirTermKind kind;
  AstSpan span;
  AstHirValueId condition_or_value;
  AstHirBlockId target_true;
  AstHirBlockId target_false;
  const AstHirValueId *true_args;
  uint32_t true_arg_count;
  const AstHirValueId *false_args;
  uint32_t false_arg_count;
} AstHirTerminatorInfo;

typedef struct AstHirBlockInfo {
  AstHirBlockId id;
  AstSpan span;
  const AstHirValueId *arguments;
  uint32_t argument_count;
  const AstHirInstId *instructions;
  uint32_t instruction_count;
  AstHirTerminatorInfo terminator;
  uint32_t predecessor_count;
  uint32_t successor_count;
  AstHirBlockId immediate_dominator;
  uint32_t reverse_postorder;
  uint8_t reachable;
} AstHirBlockInfo;

typedef struct AstHirModuleInfo {
  AstName name;
  uint32_t file_id;
  uint32_t first_function;
  uint32_t function_count;
  uint32_t first_global;
  uint32_t global_count;
} AstHirModuleInfo;

typedef struct AstHirFunctionInfo {
  AstName module_name;
  AstName name;
  const AstSemaType *type;
  AstSpan span;
  uint32_t flags;
  AstHirBlockId entry_block;
  uint32_t block_count;
  uint32_t instruction_count;
  uint32_t value_count;
  uint32_t local_count;
  uint32_t generic_param_count;
  uint32_t native_region_count;
  uint32_t dispatch_case_count;
  AstHirFramePolicy frame_policy;
  uint32_t stack_alignment;
  AstHirTextValue abi;
  AstHirTextValue link_name;
  AstHirTextValue target_constraint;
  AstHirTextValue requires_constraint;
} AstHirFunctionInfo;

typedef struct AstHirGlobalInfo {
  AstName module_name;
  AstName name;
  const AstSemaType *type;
  AstSpan span;
  AstHirText section_name;
  AstSpan section_span;
  uint32_t flags;
  uint32_t alignment;
  uint32_t section_alignment;
  const AstHirInitializer *initializer;
} AstHirGlobalInfo;

ASTER_API AstHirConfig ast_hir_config_default(void);
ASTER_API AstHirProgram *ast_hir_build(const AstFrontend *frontend,
                                       const AstSema *sema,
                                       const AstHirConfig *config);
ASTER_API void ast_hir_destroy(AstHirProgram *program);
ASTER_API int ast_hir_verify(AstHirProgram *program);
ASTER_API AstHirStats ast_hir_stats(const AstHirProgram *program);
ASTER_API const AstHirDiagnostic *
ast_hir_diagnostics(const AstHirProgram *program, size_t *count_out);
ASTER_API const char *ast_hir_diag_code_name(AstHirDiagCode code);
ASTER_API const char *ast_hir_op_name(AstHirOp op);
ASTER_API const char *ast_hir_term_kind_name(AstHirTermKind kind);
ASTER_API const char *ast_hir_intrinsic_name(AstHirIntrinsicKind kind);

ASTER_API size_t ast_hir_module_count(const AstHirProgram *program);
ASTER_API int ast_hir_module_at(const AstHirProgram *program, size_t index,
                                AstHirModuleInfo *out);
ASTER_API size_t ast_hir_target_count(const AstHirProgram *program);
ASTER_API int ast_hir_target_at(const AstHirProgram *program, size_t index,
                                AstHirTargetInfo *out);
ASTER_API size_t ast_hir_function_count(const AstHirProgram *program);
ASTER_API size_t ast_hir_global_count(const AstHirProgram *program);
ASTER_API const AstHirFunction *
ast_hir_function_at(const AstHirProgram *program, size_t index);
ASTER_API int ast_hir_function_info(const AstHirFunction *function,
                                    AstHirFunctionInfo *out);
ASTER_API int ast_hir_function_block(const AstHirFunction *function,
                                     AstHirBlockId block, AstHirBlockInfo *out);
ASTER_API int ast_hir_function_instruction(const AstHirFunction *function,
                                           AstHirInstId inst,
                                           AstHirInstructionInfo *out);
ASTER_API int ast_hir_function_value(const AstHirFunction *function,
                                     AstHirValueId value, AstHirValueInfo *out);
ASTER_API int ast_hir_function_local(const AstHirFunction *function,
                                     AstHirLocalId local, AstHirLocalInfo *out);
ASTER_API int ast_hir_function_generic_param(const AstHirFunction *function,
                                             uint32_t index,
                                             AstHirGenericParamInfo *out);
ASTER_API int ast_hir_function_dispatch_case(const AstHirFunction *function,
                                             uint32_t index,
                                             AstHirDispatchCaseInfo *out);
ASTER_API int ast_hir_global_at(const AstHirProgram *program, size_t index,
                                AstHirGlobalInfo *out);
ASTER_API int ast_hir_find_function(const AstHirProgram *program,
                                    const char *module_name,
                                    const char *function_name,
                                    const AstHirFunction **out);

ASTER_API const AstHirInitializer *
ast_hir_global_initializer(const AstHirProgram *program, size_t global_index);
ASTER_API int ast_hir_initializer_info(const AstHirInitializer *initializer,
                                       AstHirInitializerInfo *out);
ASTER_API const AstHirInitializer *
ast_hir_initializer_child(const AstHirInitializer *initializer, uint32_t index);

ASTER_API uint32_t
ast_hir_function_native_region_count(const AstHirFunction *function);
ASTER_API const AstHirNativeRegion *
ast_hir_function_native_region(const AstHirFunction *function, uint32_t index);
ASTER_API int ast_hir_native_region_info(const AstHirNativeRegion *region,
                                         AstHirNativeRegionInfo *out);
ASTER_API int ast_hir_native_region_statement(const AstHirNativeRegion *region,
                                              uint32_t index,
                                              AstHirNativeStmtInfo *out);
ASTER_API int ast_hir_native_expr_info(const AstHirNativeExpr *expr,
                                       AstHirNativeExprInfo *out);
ASTER_API const AstHirNativeExpr *
ast_hir_native_expr_child(const AstHirNativeExpr *expr, uint32_t index);

ASTER_API void ast_hir_dump_program(
    const AstHirProgram *program,
    void (*write_fn)(void *user, const char *text, size_t length), void *user);
ASTER_API void ast_hir_dump_function(
    const AstHirFunction *function,
    void (*write_fn)(void *user, const char *text, size_t length), void *user);

#ifdef __cplusplus
}
#endif

#endif
