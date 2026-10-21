#ifndef ASTERASM_SEMA_INTERNAL_H
#define ASTERASM_SEMA_INTERNAL_H

#include "../core/hashmap.h"
#include "../core/parallel.h"
#include "asterasm/sema.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define AST_SEMA_ISFINITE(value) __builtin_isfinite((double)(value))
#define AST_SEMA_SIGNBIT(value) __builtin_signbit((double)(value))
#else
#define AST_SEMA_ISFINITE(value) isfinite((double)(value))
#define AST_SEMA_SIGNBIT(value) signbit((double)(value))
#endif

#define AST_SEMA_DEFAULT_MAX_DIAGNOSTICS 1024u
#define AST_SEMA_DEFAULT_RECURSION_LIMIT 256u
#define AST_SEMA_UNKNOWN_SIZE UINT64_MAX

#define AST_SEMA_TYPEF_SIGNED (1u << 0)
#define AST_SEMA_TYPEF_CONST (1u << 1)
#define AST_SEMA_TYPEF_VOLATILE (1u << 2)
#define AST_SEMA_TYPEF_COMPLETE (1u << 3)
#define AST_SEMA_TYPEF_RESOLVING (1u << 4)
#define AST_SEMA_TYPEF_BFLOAT (1u << 5)

#define AST_SEMA_SYMF_EXPORTED (1u << 0)
#define AST_SEMA_SYMF_MUTABLE (1u << 1)
#define AST_SEMA_SYMF_RESOLVING (1u << 2)
#define AST_SEMA_SYMF_RESOLVED (1u << 3)
#define AST_SEMA_SYMF_CONST_EVAL (1u << 4)
#define AST_SEMA_SYMF_CONST_BUSY (1u << 5)

#define AST_SEMA_CONSTF_VALID (1u << 0)
#define AST_SEMA_CONSTF_SIGNED (1u << 1)
#define AST_SEMA_CONSTF_BOOL (1u << 2)
#define AST_SEMA_CONSTF_NEGATIVE (1u << 3)
#define AST_SEMA_CONSTF_FLOAT (1u << 4)

typedef struct AstSemaConstValue AstSemaConstValue;

struct AstSemaType {
  AstSemaTypeKind kind;
  uint32_t flags;
  uint32_t bit_width;
  uint32_t alignment;
  uint64_t size;
  uint64_t hash;
  union {
    struct {
      const AstSemaType *element;
      uint32_t qualifiers;
    } pointer;
    struct {
      uint64_t width;
      AstName parameter;
      const AstExpr *dependent_expr;
      uint8_t dependent;
    } bits;
    struct {
      uint64_t lanes;
      const AstSemaType *element;
      AstName parameter;
      const AstExpr *dependent_expr;
      uint8_t dependent;
    } vector;
    struct {
      uint64_t lanes;
      AstName parameter;
      const AstExpr *dependent_expr;
      uint8_t dependent;
    } mask;
    struct {
      const AstSemaType *element;
    } scalable;
    struct {
      const AstSemaType *element;
      uint64_t count;
      AstName parameter;
      const AstExpr *dependent_expr;
      uint8_t dependent;
    } array;
    struct {
      const AstSemaType **items;
      uint32_t count;
    } tuple;
    struct {
      AstName name;
      struct AstSemaField *fields;
      uint32_t field_count;
      const AstDecl *decl;
      const AstSemaType **generic_type_args;
      AstSemaConstValue *generic_const_args;
      AstName *generic_const_parameters;
      uint8_t *generic_const_dependent;
      uint32_t generic_arg_count;
    } record;
    struct {
      const AstSemaType **params;
      uint32_t param_count;
      const AstSemaType *result;
    } function;
  } as;
};

typedef struct AstSemaField {
  AstName name;
  const AstSemaType *type;
  uint64_t offset;
} AstSemaField;

struct AstSemaConstValue {
  uint64_t lo;
  uint64_t hi;
  double floating;
  uint32_t flags;
  const AstSemaType *type;
};

typedef struct AstSemaTypeInstantiation {
  struct AstSemaTypeInstantiation *next;
  const AstSemaType **type_args;
  AstSemaConstValue *const_args;
  AstName *const_parameters;
  uint8_t *const_dependent;
  AstSemaType *type;
  uint32_t arg_count;
  uint8_t resolving;
} AstSemaTypeInstantiation;

typedef struct AstSemaSymbol {
  AstSemaSymbolKind kind;
  AstName name;
  const AstSemaType *type;
  const AstDecl *decl;
  const AstDataDecl *data_decl;
  uint32_t file_id;
  uint32_t flags;
  uint32_t index;
  AstSemaConstValue const_value;
  struct AstSemaGenericBinding *generic_bindings;
  size_t generic_count;
  AstSemaTypeInstantiation *instantiations;
} AstSemaSymbol;

typedef struct AstSemaImport {
  AstName module_name;
  AstName alias;
  struct AstSemaModule *target;
  AstSpan span;
  uint8_t has_alias;
} AstSemaImport;

typedef struct AstSemaModule {
  const AstFrontendFile *file_view;
  AstFrontendFile file;
  const AstModule *ast;
  AstSemaSymbol *symbols;
  size_t symbol_count;
  size_t symbol_capacity;
  AstStringMap value_map;
  AstStringMap type_map;
  AstStringMap macro_map;
  AstSemaImport *imports;
  size_t import_count;
  size_t import_capacity;
  uint32_t index;
} AstSemaModule;

typedef struct AstSemaLocal {
  AstName name;
  AstSpan declaration_span;
  const AstSemaType *type;
  AstSemaSymbolKind kind;
  uint32_t depth;
  uint8_t mutable_value;
  uint8_t initialized;
  AstSemaConstValue const_value;
} AstSemaLocal;

typedef struct AstSemaLabel {
  AstName name;
  uint32_t depth;
  uint8_t referenced;
} AstSemaLabel;

typedef struct AstSemaLoopFrame {
  uint8_t *break_state;
  size_t local_count;
  uint8_t has_break;
} AstSemaLoopFrame;

typedef struct AstSemaGenericBinding {
  AstName name;
  const AstSemaType *type_value;
  AstSemaConstValue const_value;
  AstName dependent_const;
  uint8_t is_const;
  uint8_t has_dependent_const;
} AstSemaGenericBinding;

typedef struct AstSemaFnCtx {
  struct AstSema *sema;
  AstSemaModule *module;
  const AstDecl *function_decl;
  const AstSemaType *return_type;
  AstSemaLocal *locals;
  size_t local_count;
  size_t local_capacity;
  AstSemaLabel *labels;
  size_t label_count;
  size_t label_capacity;
  uint32_t scope_depth;
  uint32_t loop_depth;
  uint32_t unsafe_depth;
  uint32_t unreachable_depth;
  AstSemaLoopFrame *loop_frames;
  size_t loop_frame_count;
  size_t loop_frame_capacity;
  uint32_t recursion_depth;
  uint8_t is_leaf;
  uint8_t is_naked;
  AstSemaGenericBinding *generics;
  size_t generic_count;
} AstSemaFnCtx;

typedef struct AstSemaConstCtx {
  struct AstSema *sema;
  AstSemaModule *module;
  AstSemaFnCtx *fn;
  uint32_t depth;
} AstSemaConstCtx;

typedef struct AstSemaExprFactSlot {
  const AstExpr *expr;
  AstSemaExprFact fact;
} AstSemaExprFactSlot;

typedef struct AstSemaTypeFactSlot {
  const AstType *node;
  AstSemaTypeFact fact;
} AstSemaTypeFactSlot;

typedef struct AstSemaFactFile {
  AstSemaExprFactSlot *expr_facts;
  size_t expr_capacity;
  AstSemaTypeFactSlot *type_facts;
  size_t type_capacity;
} AstSemaFactFile;

struct AstSema {
  const AstFrontend *frontend;
  AstSemaConfig config;
  AstArena arena;
  AstSemaModule *modules;
  size_t module_count;
  AstStringMap module_map;
  AstSemaDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  AstSemaType builtin[32];
  const AstSemaType *builtin_view;
  AstSemaStats stats;
  AstSemaFactFile *fact_files;
  size_t fact_file_count;
  size_t expr_fact_count;
  size_t type_fact_count;
  uint8_t owns_fact_files;
  uint8_t owns_expr_fact_storage;
  uint8_t owns_type_fact_storage;
  uint8_t checked;
  uint8_t oom;
};

typedef enum AstFlowKind {
  AST_FLOW_FALLTHROUGH = 0,
  AST_FLOW_RETURN,
  AST_FLOW_BREAK,
  AST_FLOW_CONTINUE,
  AST_FLOW_BRANCH,
  AST_FLOW_DIVERGE
} AstFlowKind;

typedef struct AstExprInfo {
  const AstSemaType *type;
  const AstSemaType *call_signature;
  AstSemaConstValue constant;
  AstSpan declaration_span;
  AstSemaResolutionKind resolution_kind;
  uint8_t is_lvalue;
  uint8_t is_mutable;
} AstExprInfo;

void sema_diag(AstSema *s, AstSemaDiagCode code, AstDiagSeverity severity,
               AstSpan span, uint32_t file_id, const char *fmt, ...)
    ASTER_PRINTF_LIKE(6, 7);
void *sema_arena_alloc(AstSema *s, size_t size, size_t align);
char *sema_name_cstr(AstSema *s, AstName name);
int sema_name_eq(AstName a, AstName b);
int sema_name_eq_text(AstName a, const char *b);

AstSemaModule *sema_module_by_file_id(AstSema *s, uint32_t file_id);
AstSemaModule *sema_module_by_name(AstSema *s, AstName name);
AstSemaSymbol *sema_module_find_value(AstSemaModule *m, AstName name);
AstSemaSymbol *sema_module_find_type(AstSemaModule *m, AstName name);
AstSemaSymbol *sema_module_find_macro(AstSemaModule *m, AstName name);
AstSemaSymbol *sema_resolve_import_member(AstSema *s, AstSemaModule *m,
                                          const AstExpr *expr,
                                          AstName *final_name, int want_type);
const AstSemaType *sema_resolve_type_operand(AstSema *s, AstSemaModule *m,
                                             AstSemaFnCtx *fn,
                                             const AstExpr *expr,
                                             int *is_type_name);
const AstSemaField *sema_find_struct_field(const AstSemaType *type,
                                           AstName name);

const AstSemaType *sema_builtin_for_token(AstSema *s, AstTokenKind token);
const AstSemaType *sema_builtin_untyped_int(AstSema *s);
const AstSemaType *sema_builtin_untyped_float(AstSema *s);
const AstSemaType *sema_builtin_bool(AstSema *s);
const AstSemaType *sema_builtin_void(AstSema *s);
const AstSemaType *sema_builtin_error(AstSema *s);
const AstSemaType *sema_builtin_usize(const AstSema *s);
const AstSemaType *sema_builtin_string(AstSema *s, AstExprKind kind);
const AstSemaType *sema_resolve_type(AstSema *s, AstSemaModule *m,
                                     const AstType *type, AstSemaFnCtx *fn,
                                     uint32_t depth);
const AstSemaType *sema_resolve_type_with_bindings(
    AstSema *s, AstSemaModule *m, const AstType *type,
    AstSemaGenericBinding *bindings, size_t binding_count, uint32_t depth);
const AstSemaType *sema_make_pointer(AstSema *s, AstSemaTypeKind kind,
                                     const AstSemaType *element,
                                     uint32_t qualifiers);
const AstSemaType *sema_make_array(AstSema *s, const AstSemaType *element,
                                   uint64_t count);
const AstSemaType *sema_make_function(AstSema *s, const AstSemaType **params,
                                      uint32_t count,
                                      const AstSemaType *result);
const AstSemaType *sema_make_tuple(AstSema *s, const AstSemaType **items,
                                   uint32_t count);
const AstSemaType *sema_make_mask(AstSema *s, uint64_t lanes);
const AstSemaType *sema_make_pred(AstSema *s, const AstSemaType *element);
int sema_type_is_lane_scalar(const AstSemaType *t);
int sema_type_is_vector_numeric(const AstSemaType *t);
int sema_type_is_vector_integer(const AstSemaType *t);
int sema_type_equal(const AstSemaType *a, const AstSemaType *b);
int sema_type_can_assign(const AstSemaType *dst, AstExprInfo src);
int sema_type_assignable(AstSema *s, const AstSemaType *dst, AstExprInfo src,
                         AstSpan span, uint32_t file_id);
int sema_type_is_integer(const AstSemaType *t);
int sema_type_is_numeric(const AstSemaType *t);
int sema_type_is_pointer(const AstSemaType *t);
int sema_type_is_bool(const AstSemaType *t);
int sema_type_is_value_type(const AstSemaType *t);
int sema_type_is_dependent(const AstSemaType *type, uint32_t depth);
int sema_type_contains_parameter(const AstSemaType *type,
                                 const AstSemaGenericBinding *bindings,
                                 size_t binding_count);
const AstSemaType *
sema_substitute_type(AstSema *s, const AstSemaType *type,
                     const AstSemaGenericBinding *template_bindings,
                     const AstSemaGenericBinding *values, size_t binding_count,
                     AstSpan span, uint32_t file_id, uint32_t depth);
int sema_compute_layout(AstSema *s, AstSemaType *type, AstSpan span,
                        uint32_t file_id, uint32_t depth);
const uint8_t *sema_span_bytes(AstSema *s, AstSpan span, size_t *length_out);
AstSemaSymbol *sema_resolve_value_name(AstSema *s, AstSemaModule *m,
                                       AstName name, int require_export,
                                       AstSpan use_span);

AstSemaConstValue sema_const_invalid(void);
AstSemaConstValue sema_const_u64(uint64_t value, const AstSemaType *type);
int sema_const_is_valid(AstSemaConstValue value);
int sema_const_is_zero(AstSemaConstValue value);
int sema_const_fits_type(const AstSemaType *dst, AstSemaConstValue value);
int sema_const_to_u64(AstSemaConstValue value, uint64_t *out);
AstSemaConstValue sema_eval_const_expr(AstSemaConstCtx *ctx,
                                       const AstExpr *expr,
                                       const AstSemaType *expected);

AstSemaLocal *sema_fn_find_local(AstSemaFnCtx *fn, AstName name);
AstExprInfo sema_check_expr(AstSemaFnCtx *fn, const AstExpr *expr,
                            const AstSemaType *expected);
int sema_infer_generic_signature_bindings(AstSema *s, AstSemaModule *module,
                                          const AstSemaSymbol *symbol,
                                          const AstSemaType *concrete_signature,
                                          AstSemaGenericBinding *values,
                                          size_t value_count, AstSpan span);
int sema_prepare_fact_files(AstSema *s);
void sema_destroy_fact_files(AstSema *s);
int sema_record_expr_fact(AstSema *s, const AstExpr *expr, AstExprInfo info);
int sema_refine_expr_fact_type(AstSema *s, const AstExpr *expr,
                               const AstSemaType *type);
int sema_merge_expr_facts(AstSema *dst, const AstSema *src);
int sema_record_type_fact(AstSema *s, const AstType *node,
                          const AstSemaType *type);
int sema_merge_type_facts(AstSema *dst, const AstSema *src);
AstFlowKind sema_check_block(AstSemaFnCtx *fn, const AstBlock *block,
                             int create_scope);
AstFlowKind sema_check_stmt(AstSemaFnCtx *fn, const AstStmt *stmt);

int sema_collect_modules(AstSema *s);
int sema_resolve_module_interfaces(AstSema *s);
AstSemaGenericBinding *sema_build_generic_bindings(AstSema *s, AstSemaModule *m,
                                                   AstGenericParamArray gp,
                                                   size_t *count_out);
void sema_check_function_body(AstSema *s, AstSemaModule *m, AstSemaSymbol *sym);
int sema_check_module_bodies(AstSema *s);
int sema_check_functions_parallel(AstSema *s, const size_t *module_indices,
                                  const size_t *symbol_indices, size_t count);
void sema_finalize_diagnostics(AstSema *s);

const AstAttribute *sema_find_attribute(AstAttributeArray attrs,
                                        const char *name);
int sema_attribute_const_u64(AstSema *s, AstSemaModule *m, AstSemaFnCtx *fn,
                             const AstAttribute *attr, uint64_t *out);
void sema_validate_module_attributes(AstSema *s, AstSemaModule *m);
void sema_validate_memory_attributes(AstSemaFnCtx *fn, AstAttributeArray attrs,
                                     int is_store);
int sema_decl_has_attribute(const AstDecl *decl, const char *name);

#endif
