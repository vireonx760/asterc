#include "parser_internal.h"

static AstParseConfig ast_normalize_config(const AstParseConfig *cfg) {
  AstParseConfig out = cfg ? *cfg : ast_parse_config_default();
  if (!out.recursion_limit)
    out.recursion_limit = ASTER_DEFAULT_PARSE_RECURSION_LIMIT;
  if (!out.max_diagnostics)
    out.max_diagnostics = ASTER_DEFAULT_MAX_PARSE_DIAGNOSTICS;
  if (!out.arena_block_size)
    out.arena_block_size = 128u * 1024u;
  return out;
}

static void ast_node_pool_init(AstNodePool *pool, size_t elem_size,
                               size_t elem_align);

void ast_vec_init(AstBuildVec *v, size_t elem_size, size_t elem_align) {
  v->data = v->inline_storage.bytes;
  v->count = 0u;
  v->capacity = elem_size != 0u && elem_align <= ASTER_ALIGNOF(AstMaxAlign)
                    ? (uint32_t)(AST_BUILD_VEC_INLINE_BYTES / elem_size)
                    : 0u;
  v->elem_size = elem_size;
  v->elem_align = elem_align;
}

void *ast_vec_push(AstParser *p, AstBuildVec *v, const void *element) {
  if (!p || !v || !element || p->out_of_memory)
    return NULL;
  if (v->count == v->capacity) {
    uint32_t next = v->capacity ? v->capacity * 2u : 8u;
    if (next < v->capacity) {
      p->out_of_memory = 1u;
      return NULL;
    }
    size_t bytes;
    if (ast_size_mul_overflow((size_t)next, v->elem_size, &bytes)) {
      p->out_of_memory = 1u;
      return NULL;
    }
    void *fresh = ast_arena_alloc(&p->scratch, bytes, v->elem_align);
    if (!fresh) {
      p->out_of_memory = 1u;
      return NULL;
    }
    if (v->data && v->count)
      memcpy(fresh, v->data, (size_t)v->count * v->elem_size);
    v->data = fresh;
    v->capacity = next;
  }
  unsigned char *slot =
      (unsigned char *)v->data + (size_t)v->count * v->elem_size;
  memcpy(slot, element, v->elem_size);
  v->count++;
  return slot;
}

void *ast_vec_finish(AstParser *p, AstBuildVec *v) {
  if (!v || !v->count)
    return NULL;
  size_t bytes;
  if (ast_size_mul_overflow((size_t)v->count, v->elem_size, &bytes)) {
    p->out_of_memory = 1u;
    return NULL;
  }
  void *out = ast_arena_alloc(&p->result->arena, bytes, v->elem_align);
  if (!out) {
    p->out_of_memory = 1u;
    return NULL;
  }
  memcpy(out, v->data, bytes);
  return out;
}

void ast_parser_init(AstParser *p, const AstLexedFile *lexed, uint32_t file_id,
                     const AstParseConfig *config, AstParseResult *result) {
  memset(p, 0, sizeof(*p));
  p->lexed = lexed;
  p->source = &lexed->source;
  p->result = result;
  p->config = ast_normalize_config(config);
  p->file_id = file_id;
  ast_arena_init(&p->scratch, 64u * 1024u);
  ast_vec_init(&p->diagnostics, sizeof(AstParseDiagnostic),
               ASTER_ALIGNOF(AstParseDiagnostic));
  ast_node_pool_init(&p->expr_pool, sizeof(AstExpr), ASTER_ALIGNOF(AstExpr));
  ast_node_pool_init(&p->type_pool, sizeof(AstType), ASTER_ALIGNOF(AstType));
  ast_node_pool_init(&p->stmt_pool, sizeof(AstStmt), ASTER_ALIGNOF(AstStmt));
  ast_node_pool_init(&p->decl_pool, sizeof(AstDecl), ASTER_ALIGNOF(AstDecl));
  ast_node_pool_init(&p->block_pool, sizeof(AstBlock), ASTER_ALIGNOF(AstBlock));
  p->cursor = ast_token_cursor_begin(lexed);
  p->current = ast_token_cursor_get(&p->cursor);
}

void ast_parser_finish(AstParser *p) {
  if (!p)
    return;
  p->result->diagnostic_count = p->diagnostics.count;
  p->result->diagnostics =
      (AstParseDiagnostic *)ast_vec_finish(p, &p->diagnostics);
  p->result->stats.tokens_consumed = p->token_index;
  p->result->stats.ast_nodes = p->ast_nodes;
  p->result->stats.expr_nodes = p->expr_node_count;
  p->result->stats.type_nodes = p->type_node_count;
  p->result->stats.arena_bytes_used = p->result->arena.bytes_used;
  p->result->stats.arena_bytes_reserved = p->result->arena.bytes_reserved;
  p->result->stats.error_count = p->result->error_count;
  ast_arena_destroy(&p->scratch);
}

AstTokenKind ast_p_kind(const AstParser *p) {
  if (p && p->split_gt_pending)
    return (AstTokenKind)p->split_gt_second.kind;
  return p && p->current ? (AstTokenKind)p->current->kind : AST_TOK_EOF;
}

const AstToken *ast_p_peek_token(const AstParser *p, size_t distance) {
  if (!p || !p->current)
    return NULL;
  AstTokenCursor c = p->cursor;
  const AstToken *t = ast_token_cursor_get(&c);
  for (size_t i = 0; i < distance && t; ++i) {
    if (!ast_token_cursor_next(&c))
      return NULL;
    t = ast_token_cursor_get(&c);
  }
  return t;
}

AstTokenKind ast_p_peek_kind(const AstParser *p, size_t distance) {
  if (p && p->split_gt_pending) {
    if (distance == 0u)
      return (AstTokenKind)p->split_gt_second.kind;
    return ast_p_peek_kind(
        &(AstParser){.cursor = p->cursor, .current = p->current},
        distance - 1u);
  }
  const AstToken *t = ast_p_peek_token(p, distance);
  return t ? (AstTokenKind)t->kind : AST_TOK_EOF;
}

const AstToken *ast_p_advance(AstParser *p) {
  if (!p)
    return NULL;
  if (p->split_gt_pending) {
    p->split_gt_pending = 0u;
    p->previous = &p->split_gt_second;
    p->panic_mode = 0u;
    return p->previous;
  }
  if (!p->current)
    return NULL;
  const AstToken *old = p->current;
  p->previous = old;
  if ((AstTokenKind)old->kind != AST_TOK_EOF) {
    ast_token_cursor_next(&p->cursor);
    p->current = ast_token_cursor_get(&p->cursor);
    p->token_index++;
  }
  p->panic_mode = 0u;
  return old;
}

int ast_p_match(AstParser *p, AstTokenKind kind) {
  if (ast_p_kind(p) != kind)
    return 0;
  ast_p_advance(p);
  return 1;
}

const AstToken *ast_p_expect(AstParser *p, AstTokenKind kind,
                             const char *what) {
  if (ast_p_kind(p) == kind)
    return ast_p_advance(p);
  if (kind == AST_TOK_GT && p && !p->split_gt_pending && p->current &&
      ((AstTokenKind)p->current->kind == AST_TOK_SHIFT_RIGHT ||
       (AstTokenKind)p->current->kind == AST_TOK_GT_EQ)) {
    p->split_gt_first = *p->current;
    p->split_gt_first.kind = (uint16_t)AST_TOK_GT;
    p->split_gt_first.length = 1u;
    p->split_gt_second = p->split_gt_first;
    p->split_gt_second.kind =
        (uint16_t)((AstTokenKind)p->current->kind == AST_TOK_GT_EQ
                       ? AST_TOK_ASSIGN
                       : AST_TOK_GT);
    p->split_gt_second.offset += 1u;
    p->split_gt_second.column += 1u;
    (void)ast_p_advance(p);
    p->split_gt_pending = 1u;
    p->previous = &p->split_gt_first;
    return p->previous;
  }
  ast_p_diag(p, AST_PARSE_DIAG_EXPECTED_TOKEN, AST_DIAG_ERROR,
             ast_span_current(p), "expected %s, found %s",
             what ? what : ast_token_kind_name(kind),
             ast_token_kind_name(ast_p_kind(p)));
  return NULL;
}

int ast_p_is_word(AstTokenKind kind) {
  return kind == AST_TOK_IDENTIFIER ||
         (kind >= AST_TOK_KW_MODULE && kind < AST_TOK_COUNT);
}

/* These tokens carry keyword spellings, but are contextual: only the
   corresponding grammar production reserves them. Unlike `if`/`fn`, they
   remain legal as user declarations, parameters and local identifiers. */
int ast_p_is_contextual_name(AstTokenKind kind) {
  switch (kind) {
  case AST_TOK_KW_TARGET:
  case AST_TOK_KW_FEATURE:
  case AST_TOK_KW_REG:
  case AST_TOK_KW_INPUTS:
  case AST_TOK_KW_OUTPUTS:
  case AST_TOK_KW_TO:
  case AST_TOK_KW_WHEN:
  case AST_TOK_KW_AS:
  case AST_TOK_KW_IN:
  case AST_TOK_KW_CONTROL:
  case AST_TOK_KW_BYTES:
    return 1;
  default:
    return 0;
  }
}

int ast_p_token_text_eq(const AstParser *p, const AstToken *tok,
                        const char *text) {
  if (!p || !tok || !text)
    return 0;
  size_t n = strlen(text);
  return tok->length == n && tok->offset <= p->source->size &&
         n <= p->source->size - (size_t)tok->offset &&
         memcmp(p->source->data + tok->offset, text, n) == 0;
}

AstSpan ast_span_token(const AstParser *p, const AstToken *tok) {
  AstSpan s;
  memset(&s, 0, sizeof(s));
  s.file_id = p ? p->file_id : 0u;
  if (tok) {
    s.offset = tok->offset;
    s.length = tok->length;
  }
  return s;
}

AstSpan ast_span_current(const AstParser *p) {
  return ast_span_token(
      p, p ? (p->split_gt_pending ? &p->split_gt_second : p->current) : NULL);
}

AstSpan ast_span_join(AstSpan a, AstSpan b) {
  if (a.length == 0u)
    return b;
  if (b.length == 0u)
    return a;
  uint64_t a_end = a.offset + a.length;
  uint64_t b_end = b.offset + b.length;
  uint64_t end = a_end > b_end ? a_end : b_end;
  AstSpan out = a.offset <= b.offset ? a : b;
  out.length = end > out.offset && end - out.offset <= UINT32_MAX
                   ? (uint32_t)(end - out.offset)
                   : UINT32_MAX;
  return out;
}

AstName ast_name_from_span(AstParser *p, AstSpan span) {
  AstName n;
  memset(&n, 0, sizeof(n));
  n.span = span;
  if (!p || span.offset > p->source->size ||
      span.length > p->source->size - (size_t)span.offset)
    return n;
  const char *text = (const char *)(p->source->data + span.offset);
  n.text = text;
  n.hash = 0u;
  return n;
}

AstName ast_name_from_token(AstParser *p, const AstToken *tok) {
  return ast_name_from_span(p, ast_span_token(p, tok));
}

int ast_parse_name(AstParser *p, AstName *out, int allow_keyword) {
  AstTokenKind k = ast_p_kind(p);
  if (k != AST_TOK_IDENTIFIER && !ast_p_is_contextual_name(k) &&
      !(allow_keyword && ast_p_is_word(k))) {
    ast_p_diag(p, AST_PARSE_DIAG_EXPECTED_IDENTIFIER, AST_DIAG_ERROR,
               ast_span_current(p), "expected identifier, found %s",
               ast_token_kind_name(k));
    if (out)
      memset(out, 0, sizeof(*out));
    return 0;
  }
  const AstToken *tok = ast_p_advance(p);
  if (out)
    *out = ast_name_from_token(p, tok);
  return 1;
}

int ast_parse_qualified_name(AstParser *p, AstName *out) {
  if (!ast_p_is_word(ast_p_kind(p))) {
    ast_p_diag(p, AST_PARSE_DIAG_EXPECTED_IDENTIFIER, AST_DIAG_ERROR,
               ast_span_current(p), "expected qualified name");
    if (out)
      memset(out, 0, sizeof(*out));
    return 0;
  }
  AstSpan span = ast_span_token(p, ast_p_advance(p));
  while (ast_p_kind(p) == AST_TOK_DOT &&
         ast_p_is_word(ast_p_peek_kind(p, 1u))) {
    ast_p_advance(p);
    span = ast_span_join(span, ast_span_token(p, ast_p_advance(p)));
  }
  if (out)
    *out = ast_name_from_span(p, span);
  return 1;
}

static void ast_p_diag_v(AstParser *p, AstParseDiagCode code,
                         AstDiagSeverity severity, AstSpan span,
                         const char *fmt, va_list args) ASTER_PRINTF_LIKE(5, 0);

static void ast_p_diag_v(AstParser *p, AstParseDiagCode code,
                         AstDiagSeverity severity, AstSpan span,
                         const char *fmt, va_list args) {
  if (!p || !fmt)
    return;
  if (p->diagnostics.count >= p->config.max_diagnostics) {
    if (!p->diag_limit_reported) {
      p->diag_limit_reported = 1u;
      AstParseDiagnostic d;
      memset(&d, 0, sizeof(d));
      d.code = AST_PARSE_DIAG_TOO_MANY_DIAGNOSTICS;
      d.severity = AST_DIAG_ERROR;
      d.span = span;
      d.message = "too many parser diagnostics; further diagnostics suppressed";
      ast_vec_push(p, &p->diagnostics, &d);
      p->result->error_count++;
    }
    return;
  }

  va_list copy;
  va_copy(copy, args);
  int need = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (need < 0)
    return;
  char *message =
      (char *)ast_arena_alloc(&p->result->arena, (size_t)need + 1u, 1u);
  if (!message) {
    p->out_of_memory = 1u;
    return;
  }
  vsnprintf(message, (size_t)need + 1u, fmt, args);

  AstParseDiagnostic d;
  d.code = code;
  d.severity = severity;
  d.span = span;
  d.message = message;
  ast_vec_push(p, &p->diagnostics, &d);
  if (severity == AST_DIAG_ERROR)
    p->result->error_count++;
  p->panic_mode = 1u;
}

void ast_p_diag(AstParser *p, AstParseDiagCode code, AstDiagSeverity severity,
                AstSpan span, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ast_p_diag_v(p, code, severity, span, fmt, args);
  va_end(args);
}

void ast_p_diag_current(AstParser *p, AstParseDiagCode code, const char *fmt,
                        ...) {
  va_list args;
  va_start(args, fmt);
  ast_p_diag_v(p, code, AST_DIAG_ERROR, ast_span_current(p), fmt, args);
  va_end(args);
}

int ast_is_top_level_start(AstTokenKind k) {
  switch (k) {
  case AST_TOK_AT:
  case AST_TOK_KW_TARGET:
  case AST_TOK_KW_IMPORT:
  case AST_TOK_KW_EXPORT:
  case AST_TOK_KW_TYPE:
  case AST_TOK_KW_CONST:
  case AST_TOK_KW_SECTION:
  case AST_TOK_KW_EXTERN:
  case AST_TOK_KW_FN:
  case AST_TOK_KW_DISPATCH:
  case AST_TOK_KW_MACRO:
  case AST_TOK_KW_STATIC_ASSERT:
    return 1;
  default:
    return 0;
  }
}

int ast_is_statement_start(AstTokenKind k) {
  switch (k) {
  case AST_TOK_LBRACE:
  case AST_TOK_KW_LET:
  case AST_TOK_KW_CONST:
  case AST_TOK_KW_STORE:
  case AST_TOK_KW_IF:
  case AST_TOK_KW_WHILE:
  case AST_TOK_KW_LOOP:
  case AST_TOK_KW_RETURN:
  case AST_TOK_KW_BREAK:
  case AST_TOK_KW_CONTINUE:
  case AST_TOK_KW_BLOCK:
  case AST_TOK_KW_BR:
  case AST_TOK_KW_UNSAFE:
  case AST_TOK_KW_COMPTIME:
  case AST_TOK_KW_FOR:
  case AST_TOK_KW_REQUIRES:
  case AST_TOK_KW_BIND:
  case AST_TOK_KW_NATIVE:
  case AST_TOK_KW_EXACT:
  case AST_TOK_KW_STATIC_ASSERT:
    return 1;
  default:
    return k == AST_TOK_IDENTIFIER || ast_p_is_contextual_name(k) ||
           k == AST_TOK_LPAREN || k == AST_TOK_INTEGER || k == AST_TOK_FLOAT ||
           k == AST_TOK_STRING || k == AST_TOK_CHAR || k == AST_TOK_KW_LOAD ||
           k == AST_TOK_KW_ATOMIC || k == AST_TOK_KW_CAST ||
           k == AST_TOK_KW_TRUNC || k == AST_TOK_KW_ZEXT ||
           k == AST_TOK_KW_SEXT || k == AST_TOK_KW_BITCAST ||
           k == AST_TOK_KW_PTRCAST;
  }
}

void ast_p_sync_statement(AstParser *p) {
  p->panic_mode = 0u;
  while (ast_p_kind(p) != AST_TOK_EOF) {
    if (p->previous && p->previous->kind == AST_TOK_SEMICOLON)
      return;
    if (ast_p_kind(p) == AST_TOK_RBRACE ||
        ast_is_statement_start(ast_p_kind(p)))
      return;
    ast_p_advance(p);
  }
}

void ast_p_sync_top_level(AstParser *p) {
  p->panic_mode = 0u;
  while (ast_p_kind(p) != AST_TOK_EOF) {
    if (p->previous && p->previous->kind == AST_TOK_SEMICOLON)
      return;
    if (ast_p_kind(p) == AST_TOK_RBRACE ||
        ast_is_top_level_start(ast_p_kind(p)))
      return;
    ast_p_advance(p);
  }
}

int ast_p_enter(AstParser *p) {
  if (++p->depth <= p->config.recursion_limit)
    return 1;
  ast_p_diag(p, AST_PARSE_DIAG_RECURSION_LIMIT, AST_DIAG_ERROR,
             ast_span_current(p), "parser recursion limit (%u) exceeded",
             p->config.recursion_limit);
  return 0;
}

void ast_p_leave(AstParser *p) {
  if (p->depth)
    p->depth--;
}

static void ast_node_pool_init(AstNodePool *pool, size_t elem_size,
                               size_t elem_align) {
  pool->data = NULL;
  pool->next_index = 0u;
  pool->capacity = 0u;
  pool->elem_size = elem_size;
  pool->elem_align = elem_align;
}

static void *ast_node_pool_alloc(AstParser *p, AstNodePool *pool) {
  enum { AST_NODE_POOL_CHUNK = 256 };
  if (ASTER_UNLIKELY(pool->next_index == pool->capacity)) {
    size_t bytes;
    if (ast_size_mul_overflow((size_t)AST_NODE_POOL_CHUNK, pool->elem_size,
                              &bytes)) {
      p->out_of_memory = 1u;
      return NULL;
    }
    pool->data = (unsigned char *)ast_arena_calloc(&p->result->arena, 1u, bytes,
                                                   pool->elem_align);
    if (!pool->data) {
      p->out_of_memory = 1u;
      return NULL;
    }
    pool->next_index = 0u;
    pool->capacity = AST_NODE_POOL_CHUNK;
  }
  void *node = pool->data + (size_t)pool->next_index * pool->elem_size;
  pool->next_index++;
  p->ast_nodes++;
  return node;
}

AstExpr *ast_new_expr(AstParser *p, AstExprKind kind, AstSpan span) {
  AstExpr *n = (AstExpr *)ast_node_pool_alloc(p, &p->expr_pool);
  if (n) {
    n->kind = kind;
    n->node_id = p->expr_node_count++;
    n->span = span;
  }
  return n;
}

AstType *ast_new_type(AstParser *p, AstTypeKind kind, AstSpan span) {
  AstType *n = (AstType *)ast_node_pool_alloc(p, &p->type_pool);
  if (n) {
    n->kind = kind;
    n->node_id = p->type_node_count++;
    n->span = span;
  }
  return n;
}

AstStmt *ast_new_stmt(AstParser *p, AstStmtKind kind, AstSpan span) {
  AstStmt *n = (AstStmt *)ast_node_pool_alloc(p, &p->stmt_pool);
  if (n) {
    n->kind = kind;
    n->span = span;
  }
  return n;
}

AstDecl *ast_new_decl(AstParser *p, AstDeclKind kind, AstSpan span) {
  AstDecl *n = (AstDecl *)ast_node_pool_alloc(p, &p->decl_pool);
  if (n) {
    n->kind = kind;
    n->span = span;
  }
  return n;
}

AstBlock *ast_new_block(AstParser *p, AstSpan span) {
  AstBlock *n = (AstBlock *)ast_node_pool_alloc(p, &p->block_pool);
  if (n)
    n->span = span;
  return n;
}

int ast_is_type_start(AstTokenKind k) {
  if (ast_p_is_contextual_name(k))
    return 1;
  switch (k) {
  case AST_TOK_IDENTIFIER:
  case AST_TOK_KW_TARGET:
  case AST_TOK_KW_FEATURE:
  case AST_TOK_KW_REG:
  case AST_TOK_KW_VOID:
  case AST_TOK_KW_BOOL:
  case AST_TOK_KW_I8:
  case AST_TOK_KW_U8:
  case AST_TOK_KW_I16:
  case AST_TOK_KW_U16:
  case AST_TOK_KW_I32:
  case AST_TOK_KW_U32:
  case AST_TOK_KW_I64:
  case AST_TOK_KW_U64:
  case AST_TOK_KW_I128:
  case AST_TOK_KW_U128:
  case AST_TOK_KW_ISIZE:
  case AST_TOK_KW_USIZE:
  case AST_TOK_KW_F16:
  case AST_TOK_KW_BF16:
  case AST_TOK_KW_F32:
  case AST_TOK_KW_F64:
  case AST_TOK_KW_PTR:
  case AST_TOK_KW_CAP:
  case AST_TOK_KW_BITS:
  case AST_TOK_KW_VEC:
  case AST_TOK_KW_MASK:
  case AST_TOK_KW_SVEC:
  case AST_TOK_KW_PRED:
  case AST_TOK_LBRACKET:
  case AST_TOK_LPAREN:
    return 1;
  default:
    return 0;
  }
}
