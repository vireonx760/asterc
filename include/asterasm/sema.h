#ifndef ASTERASM_SEMA_H
#define ASTERASM_SEMA_H

#include "asterasm/frontend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstSema AstSema;
typedef struct AstSemaType AstSemaType;

typedef enum AstSemaTypeKind {
  AST_SEMA_TYPE_ERROR = 0,
  AST_SEMA_TYPE_VOID,
  AST_SEMA_TYPE_BOOL,
  AST_SEMA_TYPE_INT,
  AST_SEMA_TYPE_FLOAT,
  AST_SEMA_TYPE_PTR,
  AST_SEMA_TYPE_CAP,
  AST_SEMA_TYPE_BITS,
  AST_SEMA_TYPE_VECTOR,
  AST_SEMA_TYPE_MASK,
  AST_SEMA_TYPE_SVECTOR,
  AST_SEMA_TYPE_PRED,
  AST_SEMA_TYPE_ARRAY,
  AST_SEMA_TYPE_TUPLE,
  AST_SEMA_TYPE_STRUCT,
  AST_SEMA_TYPE_FUNCTION,
  AST_SEMA_TYPE_UNTYPED_INT,
  AST_SEMA_TYPE_UNTYPED_FLOAT,
  AST_SEMA_TYPE_STRING,
  AST_SEMA_TYPE_BYTE_STRING,
  AST_SEMA_TYPE_C_STRING,
  AST_SEMA_TYPE_TYPE_PARAMETER
} AstSemaTypeKind;

typedef enum AstSemaSymbolKind {
  AST_SEMA_SYMBOL_TYPE = 0,
  AST_SEMA_SYMBOL_CONST,
  AST_SEMA_SYMBOL_GLOBAL,
  AST_SEMA_SYMBOL_FUNCTION,
  AST_SEMA_SYMBOL_EXTERN_FUNCTION,
  AST_SEMA_SYMBOL_MACRO,
  AST_SEMA_SYMBOL_PARAMETER,
  AST_SEMA_SYMBOL_LOCAL,
  AST_SEMA_SYMBOL_LOCAL_CONST,
  AST_SEMA_SYMBOL_GENERIC_TYPE,
  AST_SEMA_SYMBOL_GENERIC_CONST
} AstSemaSymbolKind;

typedef enum AstSemaDiagCode {
  AST_SEMA_DIAG_NONE = 3000,
  AST_SEMA_DIAG_DUPLICATE_SYMBOL,
  AST_SEMA_DIAG_DUPLICATE_TYPE,
  AST_SEMA_DIAG_DUPLICATE_FIELD,
  AST_SEMA_DIAG_DUPLICATE_PARAMETER,
  AST_SEMA_DIAG_DUPLICATE_LOCAL,
  AST_SEMA_DIAG_UNKNOWN_NAME,
  AST_SEMA_DIAG_UNKNOWN_TYPE,
  AST_SEMA_DIAG_UNKNOWN_MODULE,
  AST_SEMA_DIAG_SYMBOL_NOT_EXPORTED,
  AST_SEMA_DIAG_IMPORT_ALIAS_COLLISION,
  AST_SEMA_DIAG_TYPE_MISMATCH,
  AST_SEMA_DIAG_EXPECTED_BOOL,
  AST_SEMA_DIAG_EXPECTED_INTEGER,
  AST_SEMA_DIAG_EXPECTED_NUMERIC,
  AST_SEMA_DIAG_EXPECTED_POINTER,
  AST_SEMA_DIAG_INVALID_BINARY_OPERANDS,
  AST_SEMA_DIAG_INVALID_UNARY_OPERAND,
  AST_SEMA_DIAG_INVALID_CAST,
  AST_SEMA_DIAG_DIVISION_BY_ZERO,
  AST_SEMA_DIAG_INTEGER_DIVISION_OVERFLOW,
  AST_SEMA_DIAG_INTEGER_LITERAL_OVERFLOW,
  AST_SEMA_DIAG_FLOAT_LITERAL_OVERFLOW,
  AST_SEMA_DIAG_INVALID_ARRAY_SIZE,
  AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION,
  AST_SEMA_DIAG_CONST_CYCLE,
  AST_SEMA_DIAG_STATIC_ASSERT_FAILED,
  AST_SEMA_DIAG_CALL_NON_FUNCTION,
  AST_SEMA_DIAG_CALL_ARITY,
  AST_SEMA_DIAG_CALL_ARGUMENT_TYPE,
  AST_SEMA_DIAG_RETURN_TYPE,
  AST_SEMA_DIAG_MISSING_RETURN,
  AST_SEMA_DIAG_UNINITIALIZED_VALUE,
  AST_SEMA_DIAG_ASSIGN_TO_CONST,
  AST_SEMA_DIAG_BREAK_OUTSIDE_LOOP,
  AST_SEMA_DIAG_CONTINUE_OUTSIDE_LOOP,
  AST_SEMA_DIAG_DUPLICATE_LABEL,
  AST_SEMA_DIAG_UNKNOWN_LABEL,
  AST_SEMA_DIAG_INVALID_BRANCH_SCOPE,
  AST_SEMA_DIAG_INVALID_LOAD,
  AST_SEMA_DIAG_INVALID_STORE,
  AST_SEMA_DIAG_INVALID_ATOMIC_OPERATION,
  AST_SEMA_DIAG_INVALID_ATOMIC_ORDER,
  AST_SEMA_DIAG_INVALID_ATOMIC_ARGUMENTS,
  AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE,
  AST_SEMA_DIAG_GENERIC_ARITY,
  AST_SEMA_DIAG_GENERIC_INFERENCE,
  AST_SEMA_DIAG_RECURSIVE_TYPE,
  AST_SEMA_DIAG_INVALID_INITIALIZER,
  AST_SEMA_DIAG_INVALID_ATTRIBUTE,
  AST_SEMA_DIAG_INVALID_TARGET,
  AST_SEMA_DIAG_COMPTIME_ITERATION_LIMIT,
  AST_SEMA_DIAG_RECURSION_LIMIT,
  AST_SEMA_DIAG_INTERNAL,
  AST_SEMA_DIAG_TOO_MANY_DIAGNOSTICS
} AstSemaDiagCode;

typedef struct AstSemaDiagnostic {
  AstSemaDiagCode code;
  AstDiagSeverity severity;
  AstSpan span;
  const char *path;
  uint32_t line;
  uint32_t column;
  const char *message;
} AstSemaDiagnostic;

typedef struct AstSemaConfig {
  uint32_t max_diagnostics;
  uint32_t recursion_limit;
  uint32_t pointer_bits;
  uint32_t max_comptime_iterations;
  size_t arena_block_size;
  size_t thread_count;
  uint8_t warnings_as_errors;
  uint8_t check_function_bodies;
  uint8_t check_native_boundaries;
} AstSemaConfig;

typedef struct AstSemaStats {
  size_t module_count;
  size_t symbol_count;
  size_t type_count;
  size_t expression_count;
  size_t statement_count;
  size_t constant_expression_count;
  size_t function_count;
  size_t error_count;
  size_t warning_count;
  size_t arena_bytes_used;
  size_t arena_bytes_reserved;
} AstSemaStats;

typedef enum AstSemaResolutionKind {
  AST_SEMA_RESOLUTION_NONE = 0,
  AST_SEMA_RESOLUTION_TOP_LEVEL,
  AST_SEMA_RESOLUTION_PARAMETER,
  AST_SEMA_RESOLUTION_LOCAL,
  AST_SEMA_RESOLUTION_LOCAL_CONST,
  AST_SEMA_RESOLUTION_GENERIC_CONST,
  AST_SEMA_RESOLUTION_FIELD
} AstSemaResolutionKind;

typedef enum AstSemaConstKind {
  AST_SEMA_CONST_NONE = 0,
  AST_SEMA_CONST_INTEGER,
  AST_SEMA_CONST_FLOAT,
  AST_SEMA_CONST_BOOL
} AstSemaConstKind;

typedef struct AstSemaConstInfo {
  AstSemaConstKind kind;
  uint64_t lo;
  uint64_t hi;
  double floating;
  uint8_t is_negative;
  uint8_t is_signed;
} AstSemaConstInfo;

typedef enum AstSemaEffect {
  AST_SEMA_EFFECT_NONE = 0,
  AST_SEMA_EFFECT_READ_MEMORY = 1u << 0,
  AST_SEMA_EFFECT_WRITE_MEMORY = 1u << 1,
  AST_SEMA_EFFECT_ATOMIC = 1u << 2,
  AST_SEMA_EFFECT_CALL = 1u << 3,
  AST_SEMA_EFFECT_MAY_TRAP = 1u << 4,
  AST_SEMA_EFFECT_VOLATILE = 1u << 5,
  AST_SEMA_EFFECT_UNSAFE = 1u << 6,
  /* Advisory memory-access hint. Preserved through HIR/MIR for target-aware
     lowering. */
  AST_SEMA_EFFECT_NONTEMPORAL = 1u << 7
} AstSemaEffect;

typedef struct AstSemaExprFact {
  const AstSemaType *type;
  const AstSemaType *call_signature;
  AstSemaConstInfo constant;
  AstSpan declaration_span;
  AstSemaResolutionKind resolution_kind;
  uint32_t effects;
  uint8_t is_lvalue;
  uint8_t is_mutable;
  uint8_t context_dependent;
} AstSemaExprFact;

typedef struct AstSemaTypeFact {
  const AstSemaType *type;
  uint8_t context_dependent;
} AstSemaTypeFact;

typedef struct AstSemaFieldInfo {
  AstName name;
  const AstSemaType *type;
  uint64_t offset;
} AstSemaFieldInfo;

typedef struct AstSemaSymbolInfo {
  AstSemaSymbolKind kind;
  AstName name;
  const AstSemaType *type;
  const AstDecl *declaration;
  uint32_t file_id;
  uint8_t is_exported;
  uint8_t is_mutable;
} AstSemaSymbolInfo;

ASTER_API AstSemaConfig ast_sema_config_default(void);
ASTER_API AstSema *ast_sema_create(const AstFrontend *frontend,
                                   const AstSemaConfig *config);
ASTER_API int ast_sema_check(AstSema *sema);
ASTER_API void ast_sema_destroy(AstSema *sema);
ASTER_API const AstSemaDiagnostic *ast_sema_diagnostics(const AstSema *sema,
                                                        size_t *count_out);
ASTER_API AstSemaStats ast_sema_stats(const AstSema *sema);
ASTER_API const char *ast_sema_diag_code_name(AstSemaDiagCode code);
ASTER_API AstSemaTypeKind ast_sema_type_kind(const AstSemaType *type);
ASTER_API uint32_t ast_sema_type_bit_width(const AstSemaType *type);
ASTER_API uint64_t ast_sema_type_size(const AstSemaType *type);
ASTER_API uint32_t ast_sema_type_alignment(const AstSemaType *type);
ASTER_API int ast_sema_type_is_signed(const AstSemaType *type);
ASTER_API int ast_sema_find_symbol(const AstSema *sema, const char *module_name,
                                   const char *symbol_name,
                                   AstSemaSymbolInfo *out);
ASTER_API int ast_sema_expr_fact(const AstSema *sema, const AstExpr *expr,
                                 AstSemaExprFact *out);
ASTER_API int ast_sema_type_fact(const AstSema *sema, const AstType *type_node,
                                 AstSemaTypeFact *out);
ASTER_API uint32_t ast_sema_type_pointer_qualifiers(const AstSemaType *type);
ASTER_API const AstSemaType *ast_sema_type_element(const AstSemaType *type);
ASTER_API int ast_sema_type_bits_width(const AstSemaType *type, uint64_t *out);
ASTER_API int ast_sema_type_array_count(const AstSemaType *type, uint64_t *out);
ASTER_API int ast_sema_type_vector_lanes(const AstSemaType *type,
                                         uint64_t *out);
ASTER_API uint32_t ast_sema_type_tuple_count(const AstSemaType *type);
ASTER_API const AstSemaType *ast_sema_type_tuple_item(const AstSemaType *type,
                                                      uint32_t index);
ASTER_API uint32_t ast_sema_type_function_param_count(const AstSemaType *type);
ASTER_API const AstSemaType *
ast_sema_type_function_param(const AstSemaType *type, uint32_t index);
ASTER_API const AstSemaType *
ast_sema_type_function_result(const AstSemaType *type);
ASTER_API AstName ast_sema_type_struct_name(const AstSemaType *type);
ASTER_API uint32_t ast_sema_type_struct_field_count(const AstSemaType *type);
ASTER_API int ast_sema_type_struct_field(const AstSemaType *type,
                                         uint32_t index, AstSemaFieldInfo *out);

#ifdef __cplusplus
}
#endif

#endif
