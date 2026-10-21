#ifndef ASTERASM_AST_H
#define ASTERASM_AST_H

#include "asterasm/arena.h"
#include "asterasm/hash.h"
#include "asterasm/lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstExpr AstExpr;
typedef struct AstType AstType;
typedef struct AstStmt AstStmt;
typedef struct AstDecl AstDecl;
typedef struct AstBlock AstBlock;
typedef struct AstInitializer AstInitializer;
typedef struct AstAttribute AstAttribute;

#define AST_NODE_ID_SYNTHETIC UINT32_MAX

typedef struct AstSpan {
  uint64_t offset;
  uint32_t file_id;
  uint32_t length;
} AstSpan;

typedef struct AstName {
  AstSpan span;
  const char *text;
  uint64_t hash;
} AstName;

typedef struct AstExprArray {
  AstExpr **items;
  uint32_t count;
} AstExprArray;
typedef struct AstTypeArray {
  AstType **items;
  uint32_t count;
} AstTypeArray;
typedef struct AstStmtArray {
  AstStmt **items;
  uint32_t count;
} AstStmtArray;
typedef struct AstDeclArray {
  AstDecl **items;
  uint32_t count;
} AstDeclArray;
typedef struct AstAttributeArray {
  AstAttribute *items;
  uint32_t count;
} AstAttributeArray;

typedef enum AstTypeQualifier {
  AST_TYPE_QUAL_NONE = 0,
  AST_TYPE_QUAL_CONST = 1u << 0,
  AST_TYPE_QUAL_VOLATILE = 1u << 1
} AstTypeQualifier;

typedef enum AstTypeKind {
  AST_TYPE_ERROR = 0,
  AST_TYPE_PRIMITIVE,
  AST_TYPE_NAMED,
  AST_TYPE_PTR,
  AST_TYPE_CAP,
  AST_TYPE_BITS,
  AST_TYPE_VEC,
  AST_TYPE_MASK,
  AST_TYPE_SVEC,
  AST_TYPE_PRED,
  AST_TYPE_ARRAY,
  AST_TYPE_TUPLE
} AstTypeKind;

typedef enum AstTypeArgKind {
  AST_TYPE_ARG_TYPE = 0,
  AST_TYPE_ARG_CONST
} AstTypeArgKind;

typedef struct AstTypeArg {
  AstTypeArgKind kind;
  AstSpan span;
  union {
    AstType *type;
    AstExpr *expr;
  } as;
} AstTypeArg;

typedef struct AstTypeArgArray {
  AstTypeArg *items;
  uint32_t count;
} AstTypeArgArray;

struct AstType {
  AstTypeKind kind;
  uint32_t node_id;
  AstSpan span;
  union {
    struct {
      AstTokenKind token_kind;
    } primitive;
    struct {
      AstName name;
      AstTypeArgArray args;
    } named;
    struct {
      AstType *element;
      uint32_t qualifiers;
    } pointer;
    struct {
      AstExpr *width;
    } bits;
    struct {
      AstExpr *lanes;
      AstType *element;
    } vector;
    struct {
      AstExpr *lanes;
    } mask;
    struct {
      AstType *element;
    } scalable;
    struct {
      AstType *element;
      AstExpr *count;
    } array;
    struct {
      AstTypeArray elements;
    } tuple;
  } as;
};

typedef enum AstExprKind {
  AST_EXPR_ERROR = 0,
  AST_EXPR_NAME,
  AST_EXPR_TYPE,
  AST_EXPR_INTEGER,
  AST_EXPR_FLOAT,
  AST_EXPR_STRING,
  AST_EXPR_BYTE_STRING,
  AST_EXPR_C_STRING,
  AST_EXPR_CHAR,
  AST_EXPR_BOOL,
  AST_EXPR_UNARY,
  AST_EXPR_BINARY,
  AST_EXPR_CONDITIONAL,
  AST_EXPR_CALL,
  AST_EXPR_INDEX,
  AST_EXPR_MEMBER,
  AST_EXPR_LOAD,
  AST_EXPR_CAST,
  AST_EXPR_ATOMIC,
  AST_EXPR_ATTR,
  AST_EXPR_MACRO_REF
} AstExprKind;

struct AstExpr {
  AstExprKind kind;
  uint32_t node_id;
  AstSpan span;
  union {
    struct {
      AstName name;
    } name;
    struct {
      AstType *type;
    } type_operand;
    struct {
      AstTokenKind token_kind;
    } literal;
    struct {
      AstTokenKind op;
      AstExpr *operand;
    } unary;
    struct {
      AstTokenKind op;
      AstExpr *left;
      AstExpr *right;
    } binary;
    struct {
      AstExpr *condition;
      AstExpr *then_expr;
      AstExpr *else_expr;
    } conditional;
    struct {
      AstExpr *callee;
      AstExprArray args;
    } call;
    struct {
      AstExpr *base;
      AstExpr *index;
    } index;
    struct {
      AstExpr *base;
      AstName member;
    } member;
    struct {
      AstType *type;
      AstExpr *address;
      AstAttributeArray attributes;
    } load;
    struct {
      AstTokenKind op;
      AstType *target;
      AstExpr *value;
    } cast;
    struct {
      AstName op;
      AstType *type;
      AstExpr *address;
      AstExprArray args;
      AstAttributeArray attributes;
    } atomic;
    struct {
      AstExpr *base;
      AstAttribute *attribute;
    } attributed;
    struct {
      AstName name;
    } macro_ref;
  } as;
};

struct AstAttribute {
  AstSpan span;
  AstName name;
  AstExprArray args;
};

typedef struct AstParam {
  AstSpan span;
  AstName name;
  AstType *type;
  AstAttributeArray attributes;
} AstParam;

typedef struct AstParamArray {
  AstParam *items;
  uint32_t count;
} AstParamArray;

typedef enum AstGenericParamKind {
  AST_GENERIC_TYPE = 0,
  AST_GENERIC_CONST
} AstGenericParamKind;

typedef struct AstGenericParam {
  AstSpan span;
  AstName name;
  AstGenericParamKind kind;
  AstType *const_type;
} AstGenericParam;

typedef struct AstGenericParamArray {
  AstGenericParam *items;
  uint32_t count;
} AstGenericParamArray;

typedef struct AstField {
  AstSpan span;
  AstName name;
  AstType *type;
  AstAttributeArray attributes;
} AstField;

typedef struct AstFieldArray {
  AstField *items;
  uint32_t count;
} AstFieldArray;

typedef enum AstInitializerKind {
  AST_INIT_EXPR = 0,
  AST_INIT_LIST
} AstInitializerKind;

typedef struct AstInitializerArray {
  AstInitializer **items;
  uint32_t count;
} AstInitializerArray;

struct AstInitializer {
  AstInitializerKind kind;
  AstSpan span;
  union {
    AstExpr *expr;
    AstInitializerArray list;
  } as;
};

typedef struct AstNativeOperand {
  AstSpan span;
  AstName name;
  AstExpr *value;
  AstType *memory_type;
  AstExpr *memory_address;
  uint8_t is_memory;
} AstNativeOperand;

typedef struct AstNativeOperandArray {
  AstNativeOperand *items;
  uint32_t count;
} AstNativeOperandArray;

typedef struct AstNativeBridge {
  AstName *inputs;
  uint32_t input_count;
  AstName *outputs;
  uint32_t output_count;
} AstNativeBridge;

typedef enum AstNativeStmtKind {
  AST_NATIVE_INSTRUCTION = 0,
  AST_NATIVE_BYTES
} AstNativeStmtKind;

typedef struct AstNativeStmt {
  AstNativeStmtKind kind;
  AstSpan span;
  AstName mnemonic;
  AstNativeOperandArray operands;
  AstAttributeArray attributes;
  AstExprArray bytes;
} AstNativeStmt;

typedef struct AstNativeStmtArray {
  AstNativeStmt *items;
  uint32_t count;
} AstNativeStmtArray;

typedef enum AstStmtKind {
  AST_STMT_ERROR = 0,
  AST_STMT_BLOCK,
  AST_STMT_LET,
  AST_STMT_CONST,
  AST_STMT_ASSIGN,
  AST_STMT_EXPR,
  AST_STMT_STORE,
  AST_STMT_IF,
  AST_STMT_WHILE,
  AST_STMT_LOOP,
  AST_STMT_RETURN,
  AST_STMT_BREAK,
  AST_STMT_CONTINUE,
  AST_STMT_BLOCK_LABEL,
  AST_STMT_BRANCH,
  AST_STMT_UNSAFE,
  AST_STMT_COMPTIME,
  AST_STMT_FOR,
  AST_STMT_REQUIRES,
  AST_STMT_BIND,
  AST_STMT_NATIVE,
  AST_STMT_EXACT,
  AST_STMT_STATIC_ASSERT
} AstStmtKind;

struct AstBlock {
  AstSpan span;
  AstStmtArray statements;
};

struct AstStmt {
  AstStmtKind kind;
  AstSpan span;
  union {
    struct {
      AstBlock *block;
    } block_stmt;
    struct {
      AstName name;
      AstType *type;
      AstExpr *value;
    } let_stmt;
    struct {
      AstName name;
      AstType *type;
      AstExpr *value;
    } const_stmt;
    struct {
      AstName name;
      AstExpr *value;
    } assign_stmt;
    struct {
      AstExpr *expr;
    } expr_stmt;
    struct {
      AstType *type;
      AstExpr *address;
      AstExpr *value;
      AstAttributeArray attributes;
    } store_stmt;
    struct {
      AstExpr *condition;
      AstBlock *then_block;
      AstStmt *else_branch;
    } if_stmt;
    struct {
      AstExpr *condition;
      AstBlock *body;
    } while_stmt;
    struct {
      AstBlock *body;
    } loop_stmt;
    struct {
      AstExpr *value;
    } return_stmt;
    struct {
      AstName label;
      uint8_t has_label;
    } jump_stmt;
    struct {
      AstName label;
    } block_label;
    struct {
      AstName label;
    } branch_stmt;
    struct {
      AstBlock *body;
      uint8_t control;
    } unsafe_stmt;
    struct {
      AstStmt *inner;
      AstBlock *body;
    } comptime_stmt;
    struct {
      AstName variable;
      AstExpr *begin;
      AstExpr *end;
      AstBlock *body;
    } for_stmt;
    struct {
      AstExpr *condition;
      AstBlock *body;
    } requires_stmt;
    struct {
      AstName value;
      AstName target;
      uint8_t target_is_reg_role;
    } bind_stmt;
    struct {
      AstName arch;
      AstNativeBridge bridge;
      AstNativeStmtArray statements;
      uint8_t unsafe_exact;
    } native_stmt;
    struct {
      AstExpr *condition;
      AstExpr *message;
    } static_assert_stmt;
  } as;
};

typedef struct AstTargetItem {
  AstSpan span;
  AstName name;
  AstExpr *value;
  AstSpan feature_string;
  int8_t feature_sign;
  uint8_t is_feature;
} AstTargetItem;

typedef struct AstTargetItemArray {
  AstTargetItem *items;
  uint32_t count;
} AstTargetItemArray;

typedef struct AstDataDecl {
  AstSpan span;
  AstName name;
  AstType *type;
  AstInitializer *initializer;
  AstAttributeArray attributes;
  uint8_t is_export;
  uint8_t is_const;
} AstDataDecl;

typedef struct AstDataDeclArray {
  AstDataDecl *items;
  uint32_t count;
} AstDataDeclArray;

typedef struct AstDispatchCase {
  AstSpan span;
  AstExpr *condition;
  AstName target;
  uint8_t is_else;
} AstDispatchCase;

typedef struct AstDispatchCaseArray {
  AstDispatchCase *items;
  uint32_t count;
} AstDispatchCaseArray;

typedef struct AstMacroParam {
  AstSpan span;
  AstName name;
  AstName category;
  AstType *const_type;
  uint8_t is_const;
} AstMacroParam;

typedef struct AstMacroParamArray {
  AstMacroParam *items;
  uint32_t count;
} AstMacroParamArray;

typedef struct AstTokenRange {
  size_t first_token;
  size_t token_count;
  AstSpan span;
} AstTokenRange;

typedef enum AstDeclKind {
  AST_DECL_ERROR = 0,
  AST_DECL_TARGET,
  AST_DECL_IMPORT,
  AST_DECL_TYPE,
  AST_DECL_CONST,
  AST_DECL_SECTION,
  AST_DECL_EXTERN_FN,
  AST_DECL_FN,
  AST_DECL_MACRO,
  AST_DECL_STATIC_ASSERT
} AstDeclKind;

struct AstDecl {
  AstDeclKind kind;
  AstSpan span;
  AstAttributeArray attributes;
  uint8_t is_export;
  union {
    struct {
      AstSpan triple;
      AstTargetItemArray items;
    } target_decl;
    struct {
      AstName module;
      AstName alias;
      uint8_t has_alias;
    } import_decl;
    struct {
      AstName name;
      AstGenericParamArray generics;
      AstType *type;
      AstFieldArray fields;
      AstAttributeArray struct_attributes;
      uint8_t is_struct;
    } type_decl;
    struct {
      AstName name;
      AstType *type;
      AstExpr *value;
    } const_decl;
    struct {
      AstSpan name;
      AstAttributeArray section_attributes;
      AstDataDeclArray data;
    } section_decl;
    struct {
      AstName name;
      AstParamArray params;
      AstType *return_type;
    } extern_fn;
    struct {
      AstName name;
      AstGenericParamArray generics;
      AstParamArray params;
      AstType *return_type;
      AstBlock *body;
      AstDispatchCaseArray dispatch_cases;
      uint8_t is_dispatch;
    } fn_decl;
    struct {
      AstName name;
      AstMacroParamArray params;
      AstTokenRange body_tokens;
    } macro_decl;
    struct {
      AstExpr *condition;
      AstExpr *message;
    } static_assert_decl;
  } as;
};

typedef struct AstModule {
  AstSpan span;
  AstName name;
  AstAttributeArray attributes;
  AstDeclArray declarations;
  uint32_t file_id;
} AstModule;

ASTER_API const char *ast_type_kind_name(AstTypeKind kind);
ASTER_API const char *ast_expr_kind_name(AstExprKind kind);
ASTER_API const char *ast_stmt_kind_name(AstStmtKind kind);
ASTER_API const char *ast_decl_kind_name(AstDeclKind kind);
ASTER_API int ast_name_equal(AstName a, AstName b);
ASTER_API int ast_name_equal_cstr(AstName name, const char *text);
ASTER_API char *ast_name_dup_cstr(const AstName *name);

#ifdef __cplusplus
}
#endif

#endif
