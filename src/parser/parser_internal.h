#ifndef ASTERASM_PARSER_INTERNAL_H
#define ASTERASM_PARSER_INTERNAL_H

#include "asterasm/parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AST_BUILD_VEC_INLINE_BYTES 256u

typedef struct AstNodePool {
  unsigned char *data;
  uint32_t next_index;
  uint32_t capacity;
  size_t elem_size;
  size_t elem_align;
} AstNodePool;

typedef struct AstBuildVec {
  void *data;
  uint32_t count;
  uint32_t capacity;
  size_t elem_size;
  size_t elem_align;
  union {
    AstMaxAlign align;
    unsigned char bytes[AST_BUILD_VEC_INLINE_BYTES];
  } inline_storage;
} AstBuildVec;

typedef struct AstParser {
  const AstLexedFile *lexed;
  const AstSourceFile *source;
  AstParseResult *result;
  AstParseConfig config;
  AstTokenCursor cursor;
  const AstToken *current;
  const AstToken *previous;
  AstToken split_gt_first;
  AstToken split_gt_second;
  uint8_t split_gt_pending;
  size_t token_index;
  uint32_t file_id;
  uint32_t depth;
  uint32_t generic_const_expr_depth;
  uint8_t panic_mode;
  uint8_t out_of_memory;
  uint8_t diag_limit_reported;
  AstArena scratch;
  AstBuildVec diagnostics;
  AstNodePool expr_pool;
  AstNodePool type_pool;
  AstNodePool stmt_pool;
  AstNodePool decl_pool;
  AstNodePool block_pool;
  size_t ast_nodes;
  uint32_t expr_node_count;
  uint32_t type_node_count;
} AstParser;

void ast_parser_init(AstParser *p, const AstLexedFile *lexed, uint32_t file_id,
                     const AstParseConfig *config, AstParseResult *result);
void ast_parser_finish(AstParser *p);

void ast_vec_init(AstBuildVec *v, size_t elem_size, size_t elem_align);
void *ast_vec_push(AstParser *p, AstBuildVec *v, const void *element);
void *ast_vec_finish(AstParser *p, AstBuildVec *v);

AstTokenKind ast_p_kind(const AstParser *p);
AstTokenKind ast_p_peek_kind(const AstParser *p, size_t distance);
const AstToken *ast_p_peek_token(const AstParser *p, size_t distance);
const AstToken *ast_p_advance(AstParser *p);
int ast_p_match(AstParser *p, AstTokenKind kind);
const AstToken *ast_p_expect(AstParser *p, AstTokenKind kind, const char *what);
int ast_p_is_word(AstTokenKind kind);
int ast_p_is_contextual_name(AstTokenKind kind);
int ast_p_token_text_eq(const AstParser *p, const AstToken *tok,
                        const char *text);

AstSpan ast_span_token(const AstParser *p, const AstToken *tok);
AstSpan ast_span_join(AstSpan a, AstSpan b);
AstSpan ast_span_current(const AstParser *p);
AstName ast_name_from_token(AstParser *p, const AstToken *tok);
AstName ast_name_from_span(AstParser *p, AstSpan span);
int ast_parse_name(AstParser *p, AstName *out, int allow_keyword);
int ast_parse_qualified_name(AstParser *p, AstName *out);

void ast_p_diag(AstParser *p, AstParseDiagCode code, AstDiagSeverity severity,
                AstSpan span, const char *fmt, ...) ASTER_PRINTF_LIKE(5, 6);
void ast_p_diag_current(AstParser *p, AstParseDiagCode code, const char *fmt,
                        ...) ASTER_PRINTF_LIKE(3, 4);
void ast_p_sync_statement(AstParser *p);
void ast_p_sync_top_level(AstParser *p);
int ast_p_enter(AstParser *p);
void ast_p_leave(AstParser *p);

AstExpr *ast_parse_expression(AstParser *p);
AstExpr *ast_parse_expression_bp(AstParser *p, int min_bp);
AstType *ast_parse_type(AstParser *p);
AstAttributeArray ast_parse_attributes(AstParser *p);
AstBlock *ast_parse_block(AstParser *p);
AstStmt *ast_parse_statement(AstParser *p);
AstDecl *ast_parse_top_decl(AstParser *p);
AstInitializer *ast_parse_initializer(AstParser *p);
AstGenericParamArray ast_parse_generic_params(AstParser *p);
AstParamArray ast_parse_params(AstParser *p);

AstExpr *ast_new_expr(AstParser *p, AstExprKind kind, AstSpan span);
AstType *ast_new_type(AstParser *p, AstTypeKind kind, AstSpan span);
AstStmt *ast_new_stmt(AstParser *p, AstStmtKind kind, AstSpan span);
AstDecl *ast_new_decl(AstParser *p, AstDeclKind kind, AstSpan span);
AstBlock *ast_new_block(AstParser *p, AstSpan span);

int ast_is_type_start(AstTokenKind kind);
int ast_is_top_level_start(AstTokenKind kind);
int ast_is_statement_start(AstTokenKind kind);

#endif
