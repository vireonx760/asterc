#include "../sema/sema_internal.h"
#include "hir_internal.h"

#include <limits.h>

int hir_reserve(void **ptr, size_t elem_size, size_t *capacity, size_t needed) {
  if (needed <= *capacity)
    return 1;
  size_t cap = *capacity ? *capacity : 8u;
  while (cap < needed) {
    if (cap > SIZE_MAX / 2u)
      return 0;
    cap *= 2u;
  }
  size_t bytes = 0u;
  if (ast_size_mul_overflow(cap, elem_size, &bytes))
    return 0;
  void *next = realloc(*ptr, bytes);
  if (!next)
    return 0;
  *ptr = next;
  *capacity = cap;
  return 1;
}

int hir_name_eq(AstName a, AstName b) {
  if (a.span.length != b.span.length)
    return 0;
  if (a.hash && b.hash && a.hash != b.hash)
    return 0;
  if (a.span.length == 0u)
    return 1;
  return a.text && b.text && memcmp(a.text, b.text, a.span.length) == 0;
}

int hir_name_eq_text(AstName name, const char *text) {
  if (!text || !name.text)
    return 0;
  size_t n = strlen(text);
  return n == name.span.length && memcmp(name.text, text, n) == 0;
}

static int hir_type_equal_impl(const AstSemaType *a, const AstSemaType *b,
                               uint32_t depth) {
  if (a == b)
    return 1;
  if (!a || !b || depth > 64u)
    return 0;
  AstSemaTypeKind ak = ast_sema_type_kind(a), bk = ast_sema_type_kind(b);
  if (ak != bk)
    return 0;
  switch (ak) {
  case AST_SEMA_TYPE_INT:
  case AST_SEMA_TYPE_FLOAT:
    return ast_sema_type_bit_width(a) == ast_sema_type_bit_width(b) &&
           ast_sema_type_is_signed(a) == ast_sema_type_is_signed(b);
  case AST_SEMA_TYPE_BITS: {
    uint64_t aw = 0u, bw = 0u;
    return ast_sema_type_bits_width(a, &aw) &&
           ast_sema_type_bits_width(b, &bw) && aw == bw;
  }
  case AST_SEMA_TYPE_PTR:
  case AST_SEMA_TYPE_CAP:
    return ast_sema_type_pointer_qualifiers(a) ==
               ast_sema_type_pointer_qualifiers(b) &&
           hir_type_equal_impl(ast_sema_type_element(a),
                               ast_sema_type_element(b), depth + 1u);
  case AST_SEMA_TYPE_ARRAY: {
    uint64_t ac = 0u, bc = 0u;
    return ast_sema_type_array_count(a, &ac) &&
           ast_sema_type_array_count(b, &bc) && ac == bc &&
           hir_type_equal_impl(ast_sema_type_element(a),
                               ast_sema_type_element(b), depth + 1u);
  }
  case AST_SEMA_TYPE_VECTOR:
  case AST_SEMA_TYPE_MASK: {
    uint64_t al = 0u, bl = 0u;
    if (!ast_sema_type_vector_lanes(a, &al) ||
        !ast_sema_type_vector_lanes(b, &bl) || al != bl)
      return 0;
    return ak == AST_SEMA_TYPE_MASK ||
           hir_type_equal_impl(ast_sema_type_element(a),
                               ast_sema_type_element(b), depth + 1u);
  }
  case AST_SEMA_TYPE_SVECTOR:
  case AST_SEMA_TYPE_PRED:
    return ak == AST_SEMA_TYPE_PRED ||
           hir_type_equal_impl(ast_sema_type_element(a),
                               ast_sema_type_element(b), depth + 1u);
  case AST_SEMA_TYPE_TUPLE: {
    uint32_t ac = ast_sema_type_tuple_count(a),
             bc = ast_sema_type_tuple_count(b);
    if (ac != bc)
      return 0;
    for (uint32_t i = 0u; i < ac; ++i) {
      if (!hir_type_equal_impl(ast_sema_type_tuple_item(a, i),
                               ast_sema_type_tuple_item(b, i), depth + 1u))
        return 0;
    }
    return 1;
  }
  case AST_SEMA_TYPE_STRUCT: {
    AstName an = ast_sema_type_struct_name(a),
            bn = ast_sema_type_struct_name(b);
    return hir_name_eq(an, bn);
  }
  case AST_SEMA_TYPE_FUNCTION: {
    uint32_t ac = ast_sema_type_function_param_count(a),
             bc = ast_sema_type_function_param_count(b);
    if (ac != bc)
      return 0;
    for (uint32_t i = 0u; i < ac; ++i) {
      if (!hir_type_equal_impl(ast_sema_type_function_param(a, i),
                               ast_sema_type_function_param(b, i), depth + 1u))
        return 0;
    }
    return hir_type_equal_impl(ast_sema_type_function_result(a),
                               ast_sema_type_function_result(b), depth + 1u);
  }
  default:
    return ast_sema_type_size(a) == ast_sema_type_size(b) &&
           ast_sema_type_alignment(a) == ast_sema_type_alignment(b);
  }
}

int hir_type_equal(const AstSemaType *a, const AstSemaType *b) {
  return hir_type_equal_impl(a, b, 0u);
}

char *hir_name_to_cstr(AstArena *arena, AstName name) {
  if (!arena || !name.text)
    return NULL;
  return ast_arena_strndup(arena, name.text, name.span.length);
}

AstName hir_copy_name(AstArena *arena, AstName name) {
  AstName out = name;
  if (!arena || !name.text || name.span.length == 0u)
    return out;
  char *copy = ast_arena_strndup(arena, name.text, name.span.length);
  if (copy)
    out.text = copy;
  return out;
}

AstHirText hir_copy_text(AstArena *arena, const char *data, size_t length) {
  AstHirText out;
  memset(&out, 0, sizeof(out));
  if (!arena || (!data && length != 0u) || length > UINT32_MAX)
    return out;
  char *copy = ast_arena_strndup(arena, data ? data : "", length);
  if (!copy)
    return out;
  out.data = copy;
  out.length = (uint32_t)length;
  return out;
}

int hir_copy_source_text(const AstHirProgram *program, AstArena *arena,
                         AstSpan span, AstHirText *out) {
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  if (!program || !program->frontend || !arena || span.file_id == 0u)
    return 0;
  AstFrontendFile file;
  if (!ast_frontend_get_file(program->frontend, (size_t)span.file_id - 1u,
                             &file) ||
      !file.source || !file.source->data)
    return 0;
  if (span.offset > file.source->size ||
      span.length > file.source->size - (size_t)span.offset)
    return 0;
  *out = hir_copy_text(arena, (const char *)file.source->data + span.offset,
                       span.length);
  return out->data != NULL;
}

static int hir_hex_value(unsigned char c) {
  if (c >= '0' && c <= '9')
    return (int)(c - '0');
  if (c >= 'a' && c <= 'f')
    return 10 + (int)(c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + (int)(c - 'A');
  return -1;
}

static size_t hir_utf8_encode(uint32_t cp, unsigned char out[4]) {
  if (cp <= 0x7fu) {
    out[0] = (unsigned char)cp;
    return 1u;
  }
  if (cp <= 0x7ffu) {
    out[0] = (unsigned char)(0xc0u | (cp >> 6));
    out[1] = (unsigned char)(0x80u | (cp & 0x3fu));
    return 2u;
  }
  if (cp <= 0xffffu) {
    if (cp >= 0xd800u && cp <= 0xdfffu)
      return 0u;
    out[0] = (unsigned char)(0xe0u | (cp >> 12));
    out[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3fu));
    out[2] = (unsigned char)(0x80u | (cp & 0x3fu));
    return 3u;
  }
  if (cp <= 0x10ffffu) {
    out[0] = (unsigned char)(0xf0u | (cp >> 18));
    out[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3fu));
    out[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3fu));
    out[3] = (unsigned char)(0x80u | (cp & 0x3fu));
    return 4u;
  }
  return 0u;
}

int hir_decode_literal_text(const AstHirProgram *program, AstArena *arena,
                            AstSpan span, AstTokenKind kind, AstHirText *out) {
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  if (!program || !program->frontend || !arena || span.file_id == 0u)
    return 0;
  AstFrontendFile file;
  if (!ast_frontend_get_file(program->frontend, (size_t)span.file_id - 1u,
                             &file) ||
      !file.source || !file.source->data)
    return 0;
  if (span.offset > file.source->size ||
      span.length > file.source->size - (size_t)span.offset)
    return 0;
  const unsigned char *src = file.source->data + span.offset;
  size_t len = span.length;
  size_t prefix = 0u;
  if (kind == AST_TOK_BYTE_STRING || kind == AST_TOK_C_STRING)
    prefix = 1u;
  if (kind != AST_TOK_STRING && kind != AST_TOK_BYTE_STRING &&
      kind != AST_TOK_C_STRING)
    return 0;
  if (len < prefix + 2u || src[prefix] != '"' || src[len - 1u] != '"')
    return 0;
  size_t begin = prefix + 1u;
  size_t end = len - 1u;
  size_t cap = len + 1u;
  unsigned char *dst = (unsigned char *)ast_arena_alloc(arena, cap + 1u, 1u);
  if (!dst)
    return 0;
  size_t w = 0u;
  for (size_t i = begin; i < end; ++i) {
    unsigned char c = src[i];
    if (c != '\\') {
      dst[w++] = c;
      continue;
    }
    if (++i >= end)
      return 0;
    c = src[i];
    switch (c) {
    case '\\':
      dst[w++] = '\\';
      break;
    case '"':
      dst[w++] = '"';
      break;
    case '\'':
      dst[w++] = '\'';
      break;
    case 'n':
      dst[w++] = '\n';
      break;
    case 'r':
      dst[w++] = '\r';
      break;
    case 't':
      dst[w++] = '\t';
      break;
    case '0':
      dst[w++] = '\0';
      break;
    case 'a':
      dst[w++] = '\a';
      break;
    case 'b':
      dst[w++] = '\b';
      break;
    case 'f':
      dst[w++] = '\f';
      break;
    case 'v':
      dst[w++] = '\v';
      break;
    case 'x': {
      if (i + 2u >= end)
        return 0;
      int h0 = hir_hex_value(src[i + 1u]), h1 = hir_hex_value(src[i + 2u]);
      if (h0 < 0 || h1 < 0)
        return 0;
      dst[w++] = (unsigned char)((h0 << 4) | h1);
      i += 2u;
      break;
    }
    case 'u': {
      if (i + 1u >= end || src[i + 1u] != '{')
        return 0;
      i += 2u;
      uint32_t cp = 0u;
      unsigned digits = 0u;
      while (i < end && src[i] != '}') {
        int h = hir_hex_value(src[i]);
        if (h < 0 || digits >= 6u || cp > 0x10ffffu / 16u)
          return 0;
        cp = cp * 16u + (uint32_t)h;
        digits++;
        i++;
      }
      if (i >= end || src[i] != '}' || digits == 0u || cp > 0x10ffffu ||
          (cp >= 0xd800u && cp <= 0xdfffu))
        return 0;
      unsigned char enc[4];
      size_t n = hir_utf8_encode(cp, enc);
      if (!n || w + n > cap)
        return 0;
      memcpy(dst + w, enc, n);
      w += n;
      break;
    }
    default:
      return 0;
    }
  }
  if (kind == AST_TOK_C_STRING)
    dst[w++] = '\0';
  dst[w] = '\0';
  if (w > UINT32_MAX)
    return 0;
  out->data = (const char *)dst;
  out->length = (uint32_t)w;
  return 1;
}

static void hir_line_column(const AstHirProgram *program, AstSpan span,
                            uint32_t *line, uint32_t *column,
                            const char **path) {
  *line = 1u;
  *column = 1u;
  *path = NULL;
  if (!program || !program->frontend || span.file_id == 0u)
    return;
  AstFrontendFile file;
  if (!ast_frontend_get_file(program->frontend, (size_t)span.file_id - 1u,
                             &file))
    return;
  *path = file.path;
  if (!file.source || !file.source->data)
    return;
  uint64_t limit = span.offset;
  if (limit > file.source->size)
    limit = file.source->size;
  uint32_t l = 1u;
  uint32_t c = 1u;
  for (uint64_t i = 0u; i < limit; ++i) {
    unsigned char ch = (unsigned char)file.source->data[i];
    if (ch == '\n') {
      l++;
      c = 1u;
    } else
      c++;
  }
  *line = l;
  *column = c;
}

static char *hir_vformat(const char *fmt, va_list ap) ASTER_PRINTF_LIKE(1, 0);

static char *hir_vformat(const char *fmt, va_list ap) {
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(NULL, 0u, fmt, copy);
  va_end(copy);
  if (n < 0)
    return NULL;
  size_t size = (size_t)n + 1u;
  char *text = (char *)malloc(size);
  if (!text)
    return NULL;
  if (vsnprintf(text, size, fmt, ap) < 0) {
    free(text);
    return NULL;
  }
  return text;
}

void hir_function_diag(AstHirFunction *fn, AstHirDiagCode code,
                       AstDiagSeverity severity, AstSpan span, const char *fmt,
                       ...) {
  if (!fn || fn->diagnostic_count >= AST_HIR_DEFAULT_MAX_DIAGNOSTICS)
    return;
  if (!hir_reserve((void **)&fn->diagnostics, sizeof(*fn->diagnostics),
                   &fn->diagnostic_capacity, fn->diagnostic_count + 1u)) {
    fn->oom = 1u;
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  char *message = hir_vformat(fmt, ap);
  va_end(ap);
  if (!message) {
    fn->oom = 1u;
    return;
  }
  AstHirDiagnostic *d = &fn->diagnostics[fn->diagnostic_count++];
  memset(d, 0, sizeof(*d));
  d->code = code;
  d->severity = severity;
  d->span = span;
  d->message = message;
}

void hir_program_diag(AstHirProgram *program, AstHirDiagCode code,
                      AstDiagSeverity severity, AstSpan span, const char *fmt,
                      ...) {
  if (!program)
    return;
  if (program->diagnostic_count >= program->config.max_diagnostics)
    return;
  if (!hir_reserve((void **)&program->diagnostics,
                   sizeof(*program->diagnostics), &program->diagnostic_capacity,
                   program->diagnostic_count + 1u)) {
    program->oom = 1u;
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  char *message = hir_vformat(fmt, ap);
  va_end(ap);
  if (!message) {
    program->oom = 1u;
    return;
  }
  AstHirDiagnostic *d = &program->diagnostics[program->diagnostic_count++];
  memset(d, 0, sizeof(*d));
  d->code = code;
  d->severity = severity;
  d->span = span;
  d->message = message;
  hir_line_column(program, span, &d->line, &d->column, &d->path);
}

static int diag_compare(const void *lhs, const void *rhs) {
  const AstHirDiagnostic *a = (const AstHirDiagnostic *)lhs;
  const AstHirDiagnostic *b = (const AstHirDiagnostic *)rhs;
  if (a->span.file_id != b->span.file_id)
    return a->span.file_id < b->span.file_id ? -1 : 1;
  if (a->span.offset != b->span.offset)
    return a->span.offset < b->span.offset ? -1 : 1;
  if (a->code != b->code)
    return a->code < b->code ? -1 : 1;
  const char *am = a->message ? a->message : "";
  const char *bm = b->message ? b->message : "";
  return strcmp(am, bm);
}

void hir_merge_function_diagnostics(AstHirProgram *program) {
  if (!program)
    return;
  for (size_t fi = 0u; fi < program->function_count; ++fi) {
    AstHirFunction *fn = program->functions[fi];
    if (!fn)
      continue;
    for (size_t di = 0u; di < fn->diagnostic_count; ++di) {
      AstHirDiagnostic src = fn->diagnostics[di];
      if (program->diagnostic_count >= program->config.max_diagnostics)
        break;
      if (!hir_reserve(
              (void **)&program->diagnostics, sizeof(*program->diagnostics),
              &program->diagnostic_capacity, program->diagnostic_count + 1u)) {
        program->oom = 1u;
        return;
      }
      AstHirDiagnostic *dst =
          &program->diagnostics[program->diagnostic_count++];
      *dst = src;
      src.message = NULL;
      hir_line_column(program, dst->span, &dst->line, &dst->column, &dst->path);
      fn->diagnostics[di].message = NULL;
    }
  }
  if (program->diagnostic_count > 1u)
    qsort(program->diagnostics, program->diagnostic_count,
          sizeof(*program->diagnostics), diag_compare);
  program->stats.error_count = 0u;
  program->stats.warning_count = 0u;
  for (size_t i = 0u; i < program->diagnostic_count; ++i) {
    if (program->diagnostics[i].severity == AST_DIAG_ERROR)
      program->stats.error_count++;
    else if (program->diagnostics[i].severity == AST_DIAG_WARNING)
      program->stats.warning_count++;
  }
}

int hir_get_expr_fact(const AstHirProgram *program, const AstExpr *expr,
                      AstSemaExprFact *out) {
  if (!program || !program->sema || !expr || !out)
    return 0;
  return ast_sema_expr_fact(program->sema, expr, out);
}

const AstSemaType *hir_expr_type(const AstHirProgram *program,
                                 const AstExpr *expr) {
  AstSemaExprFact fact;
  memset(&fact, 0, sizeof(fact));
  return hir_get_expr_fact(program, expr, &fact) ? fact.type : NULL;
}

int hir_const_bool(const AstHirProgram *program, const AstExpr *expr,
                   int *out) {
  AstSemaExprFact fact;
  memset(&fact, 0, sizeof(fact));
  if (!out || !hir_get_expr_fact(program, expr, &fact) ||
      fact.constant.kind != AST_SEMA_CONST_BOOL)
    return 0;
  *out = (fact.constant.lo != 0u || fact.constant.hi != 0u);
  return 1;
}

int hir_const_u64(const AstHirProgram *program, const AstExpr *expr,
                  uint64_t *out) {
  AstSemaExprFact fact;
  memset(&fact, 0, sizeof(fact));
  if (!out || !hir_get_expr_fact(program, expr, &fact) ||
      fact.constant.kind != AST_SEMA_CONST_INTEGER || fact.constant.hi != 0u ||
      fact.constant.is_negative)
    return 0;
  *out = fact.constant.lo;
  return 1;
}

AstHirFunction *hir_function_create(AstHirProgram *program, AstName module_name,
                                    uint32_t file_id, const AstDecl *decl,
                                    const AstSemaType *type, uint32_t flags) {
  AstHirFunction *fn = (AstHirFunction *)calloc(1u, sizeof(*fn));
  if (!fn) {
    if (program)
      program->oom = 1u;
    return NULL;
  }
  ast_arena_init(&fn->arena, program && program->config.arena_block_size
                                 ? program->config.arena_block_size
                                 : AST_HIR_DEFAULT_ARENA_BLOCK);
  fn->module_name = hir_copy_name(&fn->arena, module_name);
  fn->file_id = file_id;
  fn->source_decl = decl;
  fn->type = type;
  fn->flags = flags;
  fn->entry_block = AST_HIR_INVALID_ID;
  if (decl) {
    fn->span = decl->span;
    if (decl->kind == AST_DECL_FN) {
      fn->name = hir_copy_name(&fn->arena, decl->as.fn_decl.name);
      fn->generic_param_count = decl->as.fn_decl.generics.count;
      if (fn->generic_param_count) {
        fn->generic_params = (AstHirGenericParam *)calloc(
            fn->generic_param_count, sizeof(*fn->generic_params));
        if (!fn->generic_params) {
          fn->oom = 1u;
          return fn;
        }
        for (uint32_t i = 0u; i < fn->generic_param_count; ++i) {
          const AstGenericParam *src = &decl->as.fn_decl.generics.items[i];
          AstHirGenericParam *dst = &fn->generic_params[i];
          dst->name = hir_copy_name(&fn->arena, src->name);
          dst->kind = src->kind == AST_GENERIC_CONST ? AST_HIR_GENERIC_CONST
                                                     : AST_HIR_GENERIC_TYPE;
          dst->span = src->span;
          dst->index = i;
          if (src->kind == AST_GENERIC_CONST && src->const_type) {
            AstSemaTypeFact tf;
            memset(&tf, 0, sizeof(tf));
            if (ast_sema_type_fact(program->sema, src->const_type, &tf))
              dst->const_type = tf.type;
          }
        }
      }
    } else if (decl->kind == AST_DECL_EXTERN_FN)
      fn->name = hir_copy_name(&fn->arena, decl->as.extern_fn.name);
  }
  return fn;
}

void hir_function_destroy(AstHirFunction *fn) {
  if (!fn)
    return;
  free(fn->literal_globals);
  for (uint32_t i = 0u; i < fn->block_count; ++i) {
    free(fn->blocks[i].arguments);
    free(fn->blocks[i].instructions);
  }
  for (size_t i = 0u; i < fn->diagnostic_count; ++i)
    free((void *)fn->diagnostics[i].message);
  free(fn->diagnostics);
  free(fn->blocks);
  free(fn->instructions);
  free(fn->values);
  free(fn->locals);
  free(fn->native_regions);
  free(fn->generic_params);
  ast_arena_destroy(&fn->arena);
  free(fn);
}

AstHirBlockId hir_add_block(AstHirFunction *fn, AstSpan span) {
  if (!fn)
    return AST_HIR_INVALID_ID;
  size_t cap = fn->block_capacity;
  if (!hir_reserve((void **)&fn->blocks, sizeof(*fn->blocks), &cap,
                   (size_t)fn->block_count + 1u)) {
    fn->oom = 1u;
    return AST_HIR_INVALID_ID;
  }
  fn->block_capacity = (uint32_t)cap;
  AstHirBlockId id = fn->block_count++;
  AstHirBlock *b = &fn->blocks[id];
  memset(b, 0, sizeof(*b));
  b->id = id;
  b->span = span;
  b->term.kind = AST_HIR_TERM_NONE;
  b->term.condition_or_value = AST_HIR_INVALID_ID;
  b->term.target_true = AST_HIR_INVALID_ID;
  b->term.target_false = AST_HIR_INVALID_ID;
  b->immediate_dominator = AST_HIR_INVALID_ID;
  b->reverse_postorder = UINT32_MAX;
  return id;
}

AstHirValueId hir_add_block_argument(AstHirFunction *fn, AstHirBlockId block,
                                     const AstSemaType *type, AstSpan span) {
  if (!fn || block >= fn->block_count)
    return AST_HIR_INVALID_ID;
  size_t vcap = fn->value_capacity;
  if (!hir_reserve((void **)&fn->values, sizeof(*fn->values), &vcap,
                   (size_t)fn->value_count + 1u)) {
    fn->oom = 1u;
    return AST_HIR_INVALID_ID;
  }
  fn->value_capacity = (uint32_t)vcap;
  AstHirBlock *b = &fn->blocks[block];
  size_t acap = b->argument_capacity;
  if (!hir_reserve((void **)&b->arguments, sizeof(*b->arguments), &acap,
                   (size_t)b->argument_count + 1u)) {
    fn->oom = 1u;
    return AST_HIR_INVALID_ID;
  }
  b->argument_capacity = (uint32_t)acap;
  AstHirValueId id = fn->value_count++;
  AstHirValue *v = &fn->values[id];
  memset(v, 0, sizeof(*v));
  v->kind = AST_HIR_VALUE_BLOCK_ARGUMENT;
  v->type = type;
  v->span = span;
  v->block = block;
  v->definition_index = b->argument_count;
  b->arguments[b->argument_count++] = id;
  return id;
}

AstHirLocalId hir_add_local(AstHirFunction *fn, AstName name,
                            const AstSemaType *type, AstSpan span,
                            uint32_t flags) {
  if (!fn)
    return AST_HIR_INVALID_ID;
  size_t cap = fn->local_capacity;
  if (!hir_reserve((void **)&fn->locals, sizeof(*fn->locals), &cap,
                   (size_t)fn->local_count + 1u)) {
    fn->oom = 1u;
    return AST_HIR_INVALID_ID;
  }
  fn->local_capacity = (uint32_t)cap;
  AstHirLocalId id = fn->local_count++;
  AstHirLocal *local = &fn->locals[id];
  memset(local, 0, sizeof(*local));
  local->name = hir_copy_name(&fn->arena, name);
  local->type = type;
  local->span = span;
  local->flags = flags;
  return id;
}

static int block_append_inst(AstHirFunction *fn, AstHirBlock *block,
                             AstHirInstId id) {
  size_t cap = block->instruction_capacity;
  if (!hir_reserve((void **)&block->instructions, sizeof(*block->instructions),
                   &cap, (size_t)block->instruction_count + 1u)) {
    fn->oom = 1u;
    return 0;
  }
  block->instruction_capacity = (uint32_t)cap;
  block->instructions[block->instruction_count++] = id;
  return 1;
}

AstHirInstId hir_emit_instruction(AstHirFunction *fn, AstHirBlockId block,
                                  AstHirInstruction inst,
                                  const AstHirValueId *operands,
                                  uint32_t operand_count) {
  if (!fn || block >= fn->block_count || hir_block_terminated(fn, block))
    return AST_HIR_INVALID_ID;
  size_t cap = fn->instruction_capacity;
  if (!hir_reserve((void **)&fn->instructions, sizeof(*fn->instructions), &cap,
                   (size_t)fn->instruction_count + 1u)) {
    fn->oom = 1u;
    return AST_HIR_INVALID_ID;
  }
  fn->instruction_capacity = (uint32_t)cap;
  if (operand_count) {
    inst.operands = AST_ARENA_ARRAY(&fn->arena, AstHirValueId, operand_count);
    if (!inst.operands) {
      fn->oom = 1u;
      return AST_HIR_INVALID_ID;
    }
    memcpy(inst.operands, operands, (size_t)operand_count * sizeof(*operands));
  } else
    inst.operands = NULL;
  inst.operand_count = operand_count;
  if (inst.symbol.text && inst.symbol.span.length)
    inst.symbol = hir_copy_name(&fn->arena, inst.symbol);
  if (inst.module.text && inst.module.span.length)
    inst.module = hir_copy_name(&fn->arena, inst.module);
  AstHirInstId id = fn->instruction_count++;
  fn->instructions[id] = inst;
  if (!block_append_inst(fn, &fn->blocks[block], id))
    return AST_HIR_INVALID_ID;
  return id;
}

AstHirValueId hir_emit_value_instruction(AstHirFunction *fn,
                                         AstHirBlockId block,
                                         AstHirInstruction inst,
                                         const AstSemaType *type,
                                         const AstHirValueId *operands,
                                         uint32_t operand_count) {
  if (!fn || !type)
    return AST_HIR_INVALID_ID;
  size_t vcap = fn->value_capacity;
  if (!hir_reserve((void **)&fn->values, sizeof(*fn->values), &vcap,
                   (size_t)fn->value_count + 1u)) {
    fn->oom = 1u;
    return AST_HIR_INVALID_ID;
  }
  fn->value_capacity = (uint32_t)vcap;
  AstHirValueId result = fn->value_count;
  inst.result = result;
  inst.type = type;
  AstHirInstId iid =
      hir_emit_instruction(fn, block, inst, operands, operand_count);
  if (iid == AST_HIR_INVALID_ID)
    return AST_HIR_INVALID_ID;
  AstHirValue *v = &fn->values[fn->value_count++];
  memset(v, 0, sizeof(*v));
  v->kind = AST_HIR_VALUE_INSTRUCTION_RESULT;
  v->type = type;
  v->span = inst.span;
  v->block = block;
  v->definition_index = iid;
  return result;
}

int hir_set_terminator(AstHirFunction *fn, AstHirBlockId block,
                       AstHirTerminator term, const AstHirValueId *true_args,
                       uint32_t true_arg_count, const AstHirValueId *false_args,
                       uint32_t false_arg_count) {
  if (!fn || block >= fn->block_count || hir_block_terminated(fn, block))
    return 0;
  if (true_arg_count) {
    term.true_args = AST_ARENA_ARRAY(&fn->arena, AstHirValueId, true_arg_count);
    if (!term.true_args) {
      fn->oom = 1u;
      return 0;
    }
    memcpy(term.true_args, true_args,
           (size_t)true_arg_count * sizeof(*true_args));
  }
  if (false_arg_count) {
    term.false_args =
        AST_ARENA_ARRAY(&fn->arena, AstHirValueId, false_arg_count);
    if (!term.false_args) {
      fn->oom = 1u;
      return 0;
    }
    memcpy(term.false_args, false_args,
           (size_t)false_arg_count * sizeof(*false_args));
  }
  term.true_arg_count = true_arg_count;
  term.false_arg_count = false_arg_count;
  fn->blocks[block].term = term;
  return 1;
}

int hir_block_terminated(const AstHirFunction *fn, AstHirBlockId block) {
  return fn && block < fn->block_count &&
         fn->blocks[block].term.kind != AST_HIR_TERM_NONE;
}

AstHirConfig ast_hir_config_default(void) {
  AstHirConfig c;
  memset(&c, 0, sizeof(c));
  c.thread_count = 0u;
  c.arena_block_size = AST_HIR_DEFAULT_ARENA_BLOCK;
  c.max_diagnostics = AST_HIR_DEFAULT_MAX_DIAGNOSTICS;
  c.recursion_limit = AST_HIR_DEFAULT_RECURSION_LIMIT;
  c.verify_after_build = 1u;
  return c;
}

static uint32_t function_flags(const AstDecl *decl) {
  uint32_t flags = 0u;
  if (!decl)
    return flags;
  if (decl->is_export)
    flags |= AST_HIR_FUNCTION_EXPORT;
  if (decl->kind == AST_DECL_EXTERN_FN)
    flags |= AST_HIR_FUNCTION_EXTERN;
  if (decl->kind == AST_DECL_FN) {
    if (decl->as.fn_decl.is_dispatch)
      flags |= AST_HIR_FUNCTION_DISPATCH;
    if (decl->as.fn_decl.generics.count)
      flags |= AST_HIR_FUNCTION_GENERIC;
    for (uint32_t i = 0u; i < decl->attributes.count; ++i) {
      const AstAttribute *a = &decl->attributes.items[i];
      if (hir_name_eq_text(a->name, "naked"))
        flags |= AST_HIR_FUNCTION_NAKED;
      if (hir_name_eq_text(a->name, "leaf"))
        flags |= AST_HIR_FUNCTION_LEAF;
      if (hir_name_eq_text(a->name, "no_unwind"))
        flags |= AST_HIR_FUNCTION_NO_UNWIND;
      if (hir_name_eq_text(a->name, "inline"))
        flags |= AST_HIR_FUNCTION_INLINE;
      if (hir_name_eq_text(a->name, "noinline"))
        flags |= AST_HIR_FUNCTION_NO_INLINE;
    }
  }
  return flags;
}

static int find_symbol_type(AstHirProgram *program, AstName module,
                            AstName symbol, const AstSemaType **out) {
  char *mc = hir_name_to_cstr(&program->arena, module);
  char *sc = hir_name_to_cstr(&program->arena, symbol);
  if (!mc || !sc) {
    program->oom = 1u;
    return 0;
  }
  AstSemaSymbolInfo info;
  memset(&info, 0, sizeof(info));
  if (!ast_sema_find_symbol(program->sema, mc, sc, &info))
    return 0;
  *out = info.type;
  return 1;
}

static int global_attr(AstAttributeArray attrs, const char *name) {
  for (uint32_t i = 0u; i < attrs.count; ++i)
    if (hir_name_eq_text(attrs.items[i].name, name))
      return 1;
  return 0;
}

static const AstAttribute *find_attribute(AstAttributeArray attrs,
                                          const char *name) {
  for (uint32_t i = 0u; i < attrs.count; ++i) {
    if (hir_name_eq_text(attrs.items[i].name, name))
      return &attrs.items[i];
  }
  return NULL;
}

static AstTokenKind literal_token_kind(const AstExpr *expr) {
  if (!expr)
    return AST_TOK_INVALID;
  switch (expr->kind) {
  case AST_EXPR_INTEGER:
  case AST_EXPR_FLOAT:
  case AST_EXPR_STRING:
  case AST_EXPR_BYTE_STRING:
  case AST_EXPR_C_STRING:
  case AST_EXPR_CHAR:
  case AST_EXPR_BOOL:
    return expr->as.literal.token_kind;
  default:
    return AST_TOK_INVALID;
  }
}

static AstHirTextValue text_value_from_expr(AstHirProgram *program,
                                            AstArena *arena,
                                            const AstExpr *expr) {
  AstHirTextValue out;
  memset(&out, 0, sizeof(out));
  if (!program || !arena || !expr)
    return out;
  out.source_span = expr->span;
  if (expr->kind == AST_EXPR_NAME) {
    out.text = hir_copy_text(arena, expr->as.name.name.text,
                             expr->as.name.name.span.length);
    out.source_kind = AST_TOK_IDENTIFIER;
    out.is_present = out.text.data != NULL;
    return out;
  }
  AstTokenKind kind = literal_token_kind(expr);
  if (kind == AST_TOK_STRING || kind == AST_TOK_BYTE_STRING ||
      kind == AST_TOK_C_STRING) {
    out.source_kind = kind;
    out.is_literal = 1u;
    out.is_present =
        hir_decode_literal_text(program, arena, expr->span, kind, &out.text)
            ? 1u
            : 0u;
  }
  return out;
}

static uint32_t attribute_u32(const AstSema *sema, const AstAttribute *attr) {
  if (!sema || !attr || attr->args.count != 1u || !attr->args.items[0])
    return 0u;
  AstSemaExprFact fact;
  memset(&fact, 0, sizeof(fact));
  if (!ast_sema_expr_fact(sema, attr->args.items[0], &fact) ||
      fact.constant.kind != AST_SEMA_CONST_INTEGER ||
      fact.constant.is_negative || fact.constant.hi != 0u ||
      fact.constant.lo > UINT32_MAX)
    return 0u;
  return (uint32_t)fact.constant.lo;
}

static void collect_function_metadata(AstHirProgram *program,
                                      AstHirFunction *fn, const AstDecl *decl) {
  if (!program || !fn || !decl)
    return;
  fn->frame_policy = (fn->flags & AST_HIR_FUNCTION_NAKED)
                         ? AST_HIR_FRAME_NAKED
                         : AST_HIR_FRAME_DEFAULT;
  const AstAttribute *attr = find_attribute(decl->attributes, "frame");
  if (attr && attr->args.count == 1u && attr->args.items[0] &&
      attr->args.items[0]->kind == AST_EXPR_NAME) {
    if (hir_name_eq_text(attr->args.items[0]->as.name.name, "auto"))
      fn->frame_policy = AST_HIR_FRAME_AUTO;
    else if (hir_name_eq_text(attr->args.items[0]->as.name.name, "validate"))
      fn->frame_policy = AST_HIR_FRAME_VALIDATE;
  }
  attr = find_attribute(decl->attributes, "stack_align");
  if (attr)
    fn->stack_alignment = attribute_u32(program->sema, attr);
  attr = find_attribute(decl->attributes, "abi");
  if (attr && attr->args.count == 1u)
    fn->abi = text_value_from_expr(program, &fn->arena, attr->args.items[0]);
  attr = find_attribute(decl->attributes, "link_name");
  if (attr && attr->args.count == 1u)
    fn->link_name =
        text_value_from_expr(program, &fn->arena, attr->args.items[0]);
  attr = find_attribute(decl->attributes, "target");
  if (attr && attr->args.count == 1u)
    fn->target_constraint =
        text_value_from_expr(program, &fn->arena, attr->args.items[0]);
  attr = find_attribute(decl->attributes, "requires");
  if (attr && attr->args.count == 1u)
    fn->requires_constraint =
        text_value_from_expr(program, &fn->arena, attr->args.items[0]);

  if (decl->kind == AST_DECL_FN && decl->as.fn_decl.is_dispatch) {
    fn->dispatch_case_count = decl->as.fn_decl.dispatch_cases.count;
    if (fn->dispatch_case_count) {
      fn->dispatch_cases = AST_ARENA_ARRAY(&fn->arena, AstHirDispatchCase,
                                           fn->dispatch_case_count);
      if (!fn->dispatch_cases) {
        fn->oom = 1u;
        return;
      }
      memset(fn->dispatch_cases, 0,
             (size_t)fn->dispatch_case_count * sizeof(*fn->dispatch_cases));
      for (uint32_t i = 0u; i < fn->dispatch_case_count; ++i) {
        const AstDispatchCase *src = &decl->as.fn_decl.dispatch_cases.items[i];
        AstHirDispatchCase *dst = &fn->dispatch_cases[i];
        dst->span = src->span;
        dst->target = hir_copy_name(&fn->arena, src->target);
        dst->is_else = src->is_else;
        if (src->condition) {
          dst->condition_span = src->condition->span;
          AstSemaExprFact fact;
          memset(&fact, 0, sizeof(fact));
          if (ast_sema_expr_fact(program->sema, src->condition, &fact))
            dst->condition = fact.constant;
        }
      }
    }
  }
}

static void collect_target(AstHirProgram *program, AstHirTarget *target,
                           const AstModule *module, uint32_t file_id,
                           const AstDecl *decl) {
  memset(target, 0, sizeof(*target));
  target->module_name = hir_copy_name(&program->arena, module->name);
  target->file_id = file_id;
  target->span = decl->span;
  target->triple_span = decl->as.target_decl.triple;
  if (!hir_decode_literal_text(program, &program->arena, target->triple_span,
                               AST_TOK_STRING, &target->triple)) {
    hir_program_diag(program, AST_HIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                     target->triple_span,
                     "cannot canonicalize target triple string");
  }
  target->item_count = decl->as.target_decl.items.count;
  if (!target->item_count)
    return;
  target->items = AST_ARENA_ARRAY(&program->arena, AstHirTargetItemInfo,
                                  target->item_count);
  if (!target->items) {
    program->oom = 1u;
    return;
  }
  memset(target->items, 0, (size_t)target->item_count * sizeof(*target->items));
  for (uint32_t i = 0u; i < target->item_count; ++i) {
    const AstTargetItem *src = &decl->as.target_decl.items.items[i];
    AstHirTargetItemInfo *dst = &target->items[i];
    dst->span = src->span;
    dst->name = hir_copy_name(&program->arena, src->name);
    dst->feature_span = src->feature_string;
    if (src->is_feature &&
        !hir_decode_literal_text(program, &program->arena, src->feature_string,
                                 AST_TOK_STRING, &dst->feature)) {
      hir_program_diag(program, AST_HIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                       src->feature_string,
                       "cannot canonicalize target feature string");
    }
    dst->feature_sign = src->feature_sign;
    dst->is_feature = src->is_feature;
    if (src->value) {
      dst->value_span = src->value->span;
      dst->value_literal_kind = literal_token_kind(src->value);
      AstSemaExprFact fact;
      memset(&fact, 0, sizeof(fact));
      if (ast_sema_expr_fact(program->sema, src->value, &fact))
        dst->constant = fact.constant;
      if ((dst->value_literal_kind == AST_TOK_STRING ||
           dst->value_literal_kind == AST_TOK_BYTE_STRING ||
           dst->value_literal_kind == AST_TOK_C_STRING) &&
          !hir_decode_literal_text(program, &program->arena, src->value->span,
                                   dst->value_literal_kind, &dst->value_text)) {
        hir_program_diag(program, AST_HIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                         src->value->span,
                         "cannot canonicalize target property string");
      }
    }
  }
}

static int collect_program(AstHirProgram *program) {
  size_t file_count = ast_frontend_file_count(program->frontend);
  program->modules = (AstHirModule *)calloc(file_count ? file_count : 1u,
                                            sizeof(*program->modules));
  if (!program->modules)
    return 0;
  program->module_count = file_count;
  size_t function_count = 0u;
  size_t global_count = 0u;
  size_t target_count = 0u;
  for (size_t fi = 0u; fi < file_count; ++fi) {
    AstFrontendFile file;
    if (!ast_frontend_get_file(program->frontend, fi, &file) || !file.parsed ||
        !file.parsed->module)
      continue;
    const AstModule *m = file.parsed->module;
    for (uint32_t di = 0u; di < m->declarations.count; ++di) {
      const AstDecl *d = m->declarations.items[di];
      if (!d)
        continue;
      if (d->kind == AST_DECL_FN || d->kind == AST_DECL_EXTERN_FN)
        function_count++;
      else if (d->kind == AST_DECL_SECTION)
        global_count += d->as.section_decl.data.count;
      else if (d->kind == AST_DECL_TARGET)
        target_count++;
    }
  }
  program->targets = (AstHirTarget *)calloc(target_count ? target_count : 1u,
                                            sizeof(*program->targets));
  program->functions = (AstHirFunction **)calloc(
      function_count ? function_count : 1u, sizeof(*program->functions));
  program->globals = (AstHirGlobal *)calloc(global_count ? global_count : 1u,
                                            sizeof(*program->globals));
  if (!program->targets || !program->functions || !program->globals)
    return 0;
  program->target_count = target_count;
  program->function_count = function_count;
  program->global_count = global_count;

  size_t target_index = 0u;
  size_t function_index = 0u;
  size_t global_index = 0u;
  for (size_t fi = 0u; fi < file_count; ++fi) {
    AstFrontendFile file;
    if (!ast_frontend_get_file(program->frontend, fi, &file) || !file.parsed ||
        !file.parsed->module)
      continue;
    const AstModule *m = file.parsed->module;
    AstHirModule *hm = &program->modules[fi];
    hm->name = hir_copy_name(&program->arena, m->name);
    hm->file_id = file.id;
    hm->source_module = m;
    hm->first_function = (uint32_t)function_index;
    hm->first_global = (uint32_t)global_index;
    for (uint32_t di = 0u; di < m->declarations.count; ++di) {
      const AstDecl *d = m->declarations.items[di];
      if (!d)
        continue;
      if (d->kind == AST_DECL_TARGET) {
        collect_target(program, &program->targets[target_index++], m, file.id,
                       d);
        if (program->oom)
          return 0;
      } else if (d->kind == AST_DECL_FN || d->kind == AST_DECL_EXTERN_FN) {
        AstName name =
            d->kind == AST_DECL_FN ? d->as.fn_decl.name : d->as.extern_fn.name;
        const AstSemaType *type = NULL;
        if (!find_symbol_type(program, m->name, name, &type)) {
          hir_program_diag(program, AST_HIR_DIAG_MISSING_SEMA_FACT,
                           AST_DIAG_ERROR, d->span,
                           "cannot find semantic function type for '%.*s'",
                           (int)name.span.length, name.text);
        }
        AstHirFunction *fn = hir_function_create(program, m->name, file.id, d,
                                                 type, function_flags(d));
        if (!fn)
          return 0;
        collect_function_metadata(program, fn, d);
        if (fn->oom)
          return 0;
        program->functions[function_index++] = fn;
        hm->function_count++;
      } else if (d->kind == AST_DECL_SECTION) {
        uint32_t section_flags = 0u;
        if (global_attr(d->as.section_decl.section_attributes, "readonly"))
          section_flags |= AST_HIR_GLOBAL_READONLY;
        if (global_attr(d->as.section_decl.section_attributes, "writable"))
          section_flags |= AST_HIR_GLOBAL_WRITABLE;
        uint32_t section_alignment = attribute_u32(
            program->sema,
            find_attribute(d->as.section_decl.section_attributes, "align"));
        for (uint32_t gi = 0u; gi < d->as.section_decl.data.count; ++gi) {
          const AstDataDecl *src = &d->as.section_decl.data.items[gi];
          AstHirGlobal *g = &program->globals[global_index++];
          memset(g, 0, sizeof(*g));
          g->module_name = hir_copy_name(&program->arena, m->name);
          g->name = hir_copy_name(&program->arena, src->name);
          g->span = src->span;
          g->section_span = d->as.section_decl.name;
          if (!hir_decode_literal_text(program, &program->arena,
                                       g->section_span, AST_TOK_STRING,
                                       &g->section_name)) {
            hir_program_diag(program, AST_HIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                             g->section_span,
                             "cannot canonicalize section name string");
          }
          g->source_initializer = src->initializer;
          g->source_decl = d;
          g->flags = section_flags;
          g->section_alignment = section_alignment;
          g->alignment = attribute_u32(
              program->sema, find_attribute(src->attributes, "align"));
          if (src->is_const)
            g->flags |= AST_HIR_GLOBAL_CONST;
          if (src->is_export)
            g->flags |= AST_HIR_GLOBAL_EXPORT;
          if (!src->initializer)
            g->flags |= AST_HIR_GLOBAL_BSS;
          if (!find_symbol_type(program, m->name, src->name, &g->type)) {
            hir_program_diag(program, AST_HIR_DIAG_MISSING_SEMA_FACT,
                             AST_DIAG_ERROR, src->span,
                             "cannot find semantic global type for '%.*s'",
                             (int)src->name.span.length, src->name.text);
          }
          if (g->type) {
            g->initializer =
                hir_build_initializer(program, m, src->initializer, g->type);
            if (!g->initializer)
              return 0;
          }
          hm->global_count++;
        }
      }
    }
  }
  return 1;
}

typedef struct LowerParallelCtx {
  AstHirProgram *program;
  size_t chunk_size;
} LowerParallelCtx;

static const AstModule *find_source_module(const AstHirProgram *program,
                                           AstName name) {
  for (size_t i = 0u; i < program->module_count; ++i) {
    if (hir_name_eq(program->modules[i].name, name))
      return program->modules[i].source_module;
  }
  return NULL;
}

typedef struct HirMonoEntry {
  AstHirFunction *template_fn;
  const AstSemaType *signature;
  AstHirFunction *specialized_fn;
} HirMonoEntry;

static AstHirInstruction *hir_direct_call_ref(AstHirFunction *fn,
                                              AstHirInstruction *call) {
  if (!fn || !call || call->op != AST_HIR_OP_CALL || call->operand_count == 0u)
    return NULL;
  AstHirValueId value = call->operands[0];
  if (value >= fn->value_count)
    return NULL;
  AstHirValue *v = &fn->values[value];
  if (v->kind != AST_HIR_VALUE_INSTRUCTION_RESULT ||
      v->definition_index >= fn->instruction_count)
    return NULL;
  AstHirInstruction *ref = &fn->instructions[v->definition_index];
  return ref->op == AST_HIR_OP_FUNCTION_REF ? ref : NULL;
}

static AstHirFunction *hir_find_generic_template(AstHirProgram *program,
                                                 AstName module,
                                                 AstName symbol) {
  if (!program)
    return NULL;
  for (size_t i = 0u; i < program->function_count; ++i) {
    AstHirFunction *fn = program->functions[i];
    if (!fn || !(fn->flags & AST_HIR_FUNCTION_GENERIC))
      continue;
    if (hir_name_eq(fn->module_name, module) && hir_name_eq(fn->name, symbol))
      return fn;
  }
  return NULL;
}

static AstName hir_generated_mono_name(AstHirFunction *fn,
                                       const AstHirFunction *template_fn,
                                       size_t ordinal) {
  AstName out;
  memset(&out, 0, sizeof(out));
  if (!fn || !template_fn || !template_fn->name.text)
    return out;
  char suffix[64];
  int written = snprintf(suffix, sizeof(suffix), "$mono$%zu", ordinal);
  if (written < 0)
    return out;
  size_t base = template_fn->name.span.length, tail = (size_t)written;
  if (base > SIZE_MAX - tail || base + tail > UINT32_MAX)
    return out;
  char *text = (char *)ast_arena_alloc(&fn->arena, base + tail + 1u, 1u);
  if (!text) {
    fn->oom = 1u;
    return out;
  }
  memcpy(text, template_fn->name.text, base);
  memcpy(text + base, suffix, tail);
  text[base + tail] = '\0';
  out.text = text;
  out.span = template_fn->name.span;
  out.span.length = (uint32_t)(base + tail);
  out.hash = 0u;
  return out;
}

static int hir_append_function(AstHirProgram *program, AstHirFunction *fn) {
  if (!program || !fn || program->function_count == SIZE_MAX)
    return 0;
  size_t bytes = 0u;
  if (ast_size_mul_overflow(program->function_count + 1u,
                            sizeof(*program->functions), &bytes))
    return 0;
  AstHirFunction **next = (AstHirFunction **)realloc(program->functions, bytes);
  if (!next)
    return 0;
  program->functions = next;
  program->functions[program->function_count++] = fn;
  return 1;
}

static AstSemaSymbol *hir_template_sema_symbol(AstHirProgram *program,
                                               AstHirFunction *template_fn,
                                               AstSemaModule **module_out) {
  if (!program || !template_fn)
    return NULL;
  AstSema *sema = (AstSema *)program->sema;
  AstSemaModule *module = sema_module_by_file_id(sema, template_fn->file_id);
  if (!module)
    return NULL;
  AstSemaSymbol *symbol = sema_module_find_value(module, template_fn->name);
  if (!symbol || symbol->kind != AST_SEMA_SYMBOL_FUNCTION ||
      symbol->generic_count == 0u)
    return NULL;
  if (module_out)
    *module_out = module;
  return symbol;
}

static AstHirFunction *hir_find_existing_mono(HirMonoEntry *entries,
                                              size_t entry_count,
                                              AstHirFunction *template_fn,
                                              const AstSemaType *signature) {
  for (size_t i = 0u; i < entry_count; ++i)
    if (entries[i].template_fn == template_fn &&
        sema_type_equal(entries[i].signature, signature))
      return entries[i].specialized_fn;
  return NULL;
}

static AstHirFunction *hir_create_mono(AstHirProgram *program,
                                       AstHirFunction *template_fn,
                                       const AstSemaType *signature,
                                       size_t ordinal) {
  AstSemaModule *sema_module = NULL;
  AstSemaSymbol *symbol =
      hir_template_sema_symbol(program, template_fn, &sema_module);
  if (!symbol || !sema_module ||
      symbol->generic_count > SIZE_MAX / sizeof(AstSemaGenericBinding))
    return NULL;
  AstSemaGenericBinding *values =
      (AstSemaGenericBinding *)calloc(symbol->generic_count, sizeof(*values));
  if (!values) {
    program->oom = 1u;
    return NULL;
  }
  if (!sema_infer_generic_signature_bindings(
          (AstSema *)program->sema, sema_module, symbol, signature, values,
          symbol->generic_count, template_fn->span)) {
    free(values);
    hir_program_diag(
        program, AST_HIR_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, template_fn->span,
        "cannot infer a complete machine specialization for generic function "
        "'%.*s'",
        (int)template_fn->name.span.length, template_fn->name.text);
    return NULL;
  }
  uint32_t flags = template_fn->flags & ~(uint32_t)(AST_HIR_FUNCTION_GENERIC |
                                                    AST_HIR_FUNCTION_EXPORT);
  AstHirFunction *fn = hir_function_create(
      program, template_fn->module_name, template_fn->file_id,
      template_fn->source_decl, signature, flags);
  if (!fn) {
    free(values);
    return NULL;
  }
  collect_function_metadata(program, fn, template_fn->source_decl);
  free(fn->generic_params);
  fn->generic_params = NULL;
  fn->generic_param_count = 0u;
  memset(&fn->link_name, 0, sizeof(fn->link_name));
  fn->name = hir_generated_mono_name(fn, template_fn, ordinal);
  if (!fn->name.text || fn->oom) {
    free(values);
    hir_function_destroy(fn);
    return NULL;
  }
  const AstModule *source_module =
      find_source_module(program, template_fn->module_name);
  if (!source_module ||
      !hir_lower_function_specialized(program, fn, source_module,
                                      symbol->generic_bindings, values,
                                      symbol->generic_count)) {
    free(values);
    hir_function_destroy(fn);
    return NULL;
  }
  free(values);
  return fn;
}

static int hir_monomorphize(AstHirProgram *program) {
  if (!program)
    return 0;
  HirMonoEntry *entries = NULL;
  size_t entry_count = 0u, entry_capacity = 0u, ordinal = 0u;
  for (size_t fi = 0u; fi < program->function_count && !program->oom; ++fi) {
    AstHirFunction *fn = program->functions[fi];
    if (!fn || (fn->flags & AST_HIR_FUNCTION_GENERIC) ||
        (fn->flags & (AST_HIR_FUNCTION_EXTERN | AST_HIR_FUNCTION_DISPATCH)))
      continue;
    for (uint32_t ii = 0u; ii < fn->instruction_count; ++ii) {
      AstHirInstruction *call = &fn->instructions[ii];
      if (call->op != AST_HIR_OP_CALL || !call->call_signature)
        continue;
      AstHirInstruction *ref = hir_direct_call_ref(fn, call);
      if (!ref)
        continue;
      AstHirFunction *template_fn =
          hir_find_generic_template(program, ref->module, ref->symbol);
      if (!template_fn)
        continue;
      AstHirFunction *mono = hir_find_existing_mono(
          entries, entry_count, template_fn, call->call_signature);
      if (!mono) {
        mono = hir_create_mono(program, template_fn, call->call_signature,
                               ordinal++);
        if (!mono) {
          free(entries);
          return 0;
        }
        if (entry_count == entry_capacity) {
          size_t nc = entry_capacity ? entry_capacity * 2u : 8u;
          if (nc < entry_capacity || nc > SIZE_MAX / sizeof(*entries)) {
            hir_function_destroy(mono);
            program->oom = 1u;
            free(entries);
            return 0;
          }
          HirMonoEntry *next =
              (HirMonoEntry *)realloc(entries, nc * sizeof(*entries));
          if (!next) {
            hir_function_destroy(mono);
            program->oom = 1u;
            free(entries);
            return 0;
          }
          entries = next;
          entry_capacity = nc;
        }
        entries[entry_count++] =
            (HirMonoEntry){template_fn, call->call_signature, mono};
        if (!hir_append_function(program, mono)) {
          hir_function_destroy(mono);
          program->oom = 1u;
          free(entries);
          return 0;
        }
      }
      ref->symbol = hir_copy_name(&fn->arena, mono->name);
      ref->module = hir_copy_name(&fn->arena, mono->module_name);
      ref->type = mono->type;
      if (ref->result != AST_HIR_INVALID_ID && ref->result < fn->value_count)
        fn->values[ref->result].type = mono->type;
    }
  }
  free(entries);
  return !program->oom;
}

/* Publish literal objects only after all parallel lowering and specialization.
   Rebuild the module ranges so public global iteration remains contiguous. */
static int collect_literal_globals(AstHirProgram *program) {
  size_t added = 0u;
  for (size_t i = 0u; i < program->function_count; ++i) {
    size_t n = program->functions[i]->literal_global_count;
    if (n > UINT32_MAX - added)
      return 0;
    added += n;
  }
  if (!added)
    return 1;
  if (program->global_count > UINT32_MAX - added)
    return 0;
  size_t count = program->global_count + added;
  size_t *counts = calloc(program->module_count, sizeof(*counts));
  size_t *cursor = calloc(program->module_count, sizeof(*cursor));
  size_t *owner = calloc(program->function_count, sizeof(*owner));
  AstHirGlobal *globals = calloc(count, sizeof(*globals));
  if (!counts || !cursor || !owner || !globals)
    goto fail;
  for (size_t i = 0u; i < program->function_count; ++i) {
    AstHirFunction *fn = program->functions[i];
    if (!fn->literal_global_count)
      continue;
    size_t m = 0u;
    while (m < program->module_count &&
           !hir_name_eq(fn->module_name, program->modules[m].name))
      ++m;
    if (m == program->module_count)
      goto fail;
    owner[i] = m;
    counts[m] += fn->literal_global_count;
  }
  size_t offset = 0u;
  for (size_t m = 0u; m < program->module_count; ++m) {
    AstHirModule *module = &program->modules[m];
    if (module->global_count)
      memcpy(globals + offset, program->globals + module->first_global,
             module->global_count * sizeof(*globals));
    cursor[m] = offset + module->global_count;
    offset += module->global_count + counts[m];
  }
  for (size_t i = 0u; i < program->function_count; ++i) {
    AstHirFunction *fn = program->functions[i];
    if (!fn->literal_global_count)
      continue;
    memcpy(globals + cursor[owner[i]], fn->literal_globals,
           fn->literal_global_count * sizeof(*globals));
    cursor[owner[i]] += fn->literal_global_count;
    free(fn->literal_globals);
    fn->literal_globals = NULL;
    fn->literal_global_count = fn->literal_global_capacity = 0u;
  }
  offset = 0u;
  for (size_t m = 0u; m < program->module_count; ++m) {
    AstHirModule *module = &program->modules[m];
    module->first_global = (uint32_t)offset;
    module->global_count += (uint32_t)counts[m];
    offset += module->global_count;
  }
  free(program->globals);
  program->globals = globals;
  program->global_count = count;
  free(counts);
  free(cursor);
  free(owner);
  return 1;
fail:
  free(counts);
  free(cursor);
  free(owner);
  free(globals);
  return 0;
}

static void lower_one_function(LowerParallelCtx *ctx, size_t index) {
  AstHirFunction *fn = ctx->program->functions[index];
  if (!fn || !fn->source_decl ||
      (fn->flags & (AST_HIR_FUNCTION_EXTERN | AST_HIR_FUNCTION_DISPATCH)))
    return;
  const AstModule *module = find_source_module(ctx->program, fn->module_name);
  if (!module || !hir_lower_function(ctx->program, fn, module)) {
    if (!fn->oom && fn->diagnostic_count == 0u)
      hir_function_diag(fn, AST_HIR_DIAG_INTERNAL, AST_DIAG_ERROR, fn->span,
                        "HIR lowering failed");
  }
}

static void lower_function_chunk(void *opaque, size_t chunk_index,
                                 size_t worker_index) {
  ASTER_UNUSED(worker_index);
  LowerParallelCtx *ctx = (LowerParallelCtx *)opaque;
  size_t begin = chunk_index * ctx->chunk_size;
  size_t end = begin + ctx->chunk_size;
  if (end > ctx->program->function_count)
    end = ctx->program->function_count;
  for (size_t index = begin; index < end; ++index)
    lower_one_function(ctx, index);
}

AstHirProgram *ast_hir_build(const AstFrontend *frontend, const AstSema *sema,
                             const AstHirConfig *config) {
  if (!frontend || !sema)
    return NULL;
  AstHirProgram *program = (AstHirProgram *)calloc(1u, sizeof(*program));
  if (!program)
    return NULL;
  program->frontend = frontend;
  program->sema = sema;
  program->config = config ? *config : ast_hir_config_default();
  if (!program->config.max_diagnostics)
    program->config.max_diagnostics = AST_HIR_DEFAULT_MAX_DIAGNOSTICS;
  if (!program->config.recursion_limit)
    program->config.recursion_limit = AST_HIR_DEFAULT_RECURSION_LIMIT;
  if (!program->config.arena_block_size)
    program->config.arena_block_size = AST_HIR_DEFAULT_ARENA_BLOCK;
  ast_arena_init(&program->arena, program->config.arena_block_size);
  AstSemaStats ss = ast_sema_stats(sema);
  if (ss.error_count != 0u) {
    hir_program_diag(
        program, AST_HIR_DIAG_SEMA_REQUIRED, AST_DIAG_ERROR, (AstSpan){0},
        "HIR requires a semantic analysis result with zero errors");
    return program;
  }
  if (!collect_program(program)) {
    program->oom = 1u;
    hir_program_diag(program, AST_HIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                     (AstSpan){0},
                     "out of memory while collecting HIR program");
    return program;
  }
  LowerParallelCtx ctx;
  ctx.program = program;
  size_t workers = program->config.thread_count ? program->config.thread_count
                                                : ast_hardware_thread_count();
  if (workers == 0u)
    workers = 1u;
  if (workers > program->function_count && program->function_count != 0u)
    workers = program->function_count;
  size_t target_chunks = workers > SIZE_MAX / 8u ? workers : workers * 8u;
  ctx.chunk_size =
      target_chunks
          ? (program->function_count + target_chunks - 1u) / target_chunks
          : 1u;
  if (ctx.chunk_size == 0u)
    ctx.chunk_size = 1u;
  size_t chunk_count =
      program->function_count
          ? (program->function_count + ctx.chunk_size - 1u) / ctx.chunk_size
          : 0u;
  if (ast_parallel_for_workers(chunk_count, workers, lower_function_chunk, &ctx,
                               NULL) != 0) {
    hir_program_diag(program, AST_HIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                     (AstSpan){0}, "parallel HIR lowering failed");
  }
  hir_merge_function_diagnostics(program);
  if (program->stats.error_count == 0u && !hir_monomorphize(program)) {
    if (!program->oom && program->diagnostic_count == 0u)
      hir_program_diag(program, AST_HIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                       (AstSpan){0},
                       "generic function monomorphization failed");
  }
  hir_merge_function_diagnostics(program);
  if (program->stats.error_count == 0u && !collect_literal_globals(program)) {
    program->oom = 1u;
    hir_program_diag(program, AST_HIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                     (AstSpan){0}, "cannot collect runtime string literals");
  }
  if (program->config.verify_after_build && program->stats.error_count == 0u)
    ast_hir_verify(program);

  program->stats.module_count = program->module_count;
  program->stats.function_count = program->function_count;
  program->stats.global_count = program->global_count;
  for (size_t i = 0u; i < program->function_count; ++i) {
    AstHirFunction *fn = program->functions[i];
    if (!fn)
      continue;
    if (fn->flags & AST_HIR_FUNCTION_EXTERN)
      program->stats.extern_function_count++;
    if (fn->flags & AST_HIR_FUNCTION_DISPATCH)
      program->stats.dispatch_function_count++;
    program->stats.block_count += fn->block_count;
    program->stats.instruction_count += fn->instruction_count;
    program->stats.value_count += fn->value_count;
    program->stats.local_count += fn->local_count;
    program->stats.arena_bytes_used += fn->arena.bytes_used;
    program->stats.arena_bytes_reserved += fn->arena.bytes_reserved;
    for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
      if (fn->blocks[bi].term.kind != AST_HIR_TERM_NONE)
        program->stats.terminator_count++;
      if (!fn->blocks[bi].reachable)
        program->stats.unreachable_block_count++;
    }
  }
  program->stats.arena_bytes_used += program->arena.bytes_used;
  program->stats.arena_bytes_reserved += program->arena.bytes_reserved;
  return program;
}

void ast_hir_destroy(AstHirProgram *program) {
  if (!program)
    return;
  for (size_t i = 0u; i < program->function_count; ++i)
    hir_function_destroy(program->functions[i]);
  for (size_t i = 0u; i < program->diagnostic_count; ++i)
    free((void *)program->diagnostics[i].message);
  free(program->diagnostics);
  free(program->modules);
  free(program->targets);
  free(program->functions);
  free(program->globals);
  ast_arena_destroy(&program->arena);
  free(program);
}

AstHirStats ast_hir_stats(const AstHirProgram *program) {
  return program ? program->stats : (AstHirStats){0};
}

const AstHirDiagnostic *ast_hir_diagnostics(const AstHirProgram *program,
                                            size_t *count_out) {
  if (count_out)
    *count_out = program ? program->diagnostic_count : 0u;
  return program ? program->diagnostics : NULL;
}

size_t ast_hir_module_count(const AstHirProgram *program) {
  return program ? program->module_count : 0u;
}
size_t ast_hir_target_count(const AstHirProgram *program) {
  return program ? program->target_count : 0u;
}
size_t ast_hir_function_count(const AstHirProgram *program) {
  return program ? program->function_count : 0u;
}
size_t ast_hir_global_count(const AstHirProgram *program) {
  return program ? program->global_count : 0u;
}

int ast_hir_module_at(const AstHirProgram *program, size_t index,
                      AstHirModuleInfo *out) {
  if (!program || !out || index >= program->module_count)
    return 0;
  const AstHirModule *m = &program->modules[index];
  out->name = m->name;
  out->file_id = m->file_id;
  out->first_function = m->first_function;
  out->function_count = m->function_count;
  out->first_global = m->first_global;
  out->global_count = m->global_count;
  return 1;
}

int ast_hir_target_at(const AstHirProgram *program, size_t index,
                      AstHirTargetInfo *out) {
  if (!program || !out || index >= program->target_count)
    return 0;
  const AstHirTarget *target = &program->targets[index];
  memset(out, 0, sizeof(*out));
  out->module_name = target->module_name;
  out->file_id = target->file_id;
  out->span = target->span;
  out->triple = target->triple;
  out->triple_span = target->triple_span;
  out->items = target->items;
  out->item_count = target->item_count;
  return 1;
}

const AstHirFunction *ast_hir_function_at(const AstHirProgram *program,
                                          size_t index) {
  return program && index < program->function_count ? program->functions[index]
                                                    : NULL;
}

int ast_hir_function_info(const AstHirFunction *fn, AstHirFunctionInfo *out) {
  if (!fn || !out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->module_name = fn->module_name;
  out->name = fn->name;
  out->type = fn->type;
  out->span = fn->span;
  out->flags = fn->flags;
  out->entry_block = fn->entry_block;
  out->block_count = fn->block_count;
  out->instruction_count = fn->instruction_count;
  out->value_count = fn->value_count;
  out->local_count = fn->local_count;
  out->generic_param_count = fn->generic_param_count;
  out->native_region_count = fn->native_region_count;
  out->dispatch_case_count = fn->dispatch_case_count;
  out->frame_policy = fn->frame_policy;
  out->stack_alignment = fn->stack_alignment;
  out->abi = fn->abi;
  out->link_name = fn->link_name;
  out->target_constraint = fn->target_constraint;
  out->requires_constraint = fn->requires_constraint;
  return 1;
}

static AstHirTerminatorInfo public_term(const AstHirTerminator *t) {
  AstHirTerminatorInfo out;
  memset(&out, 0, sizeof(out));
  if (!t)
    return out;
  out.kind = t->kind;
  out.span = t->span;
  out.condition_or_value = t->condition_or_value;
  out.target_true = t->target_true;
  out.target_false = t->target_false;
  out.true_args = t->true_args;
  out.true_arg_count = t->true_arg_count;
  out.false_args = t->false_args;
  out.false_arg_count = t->false_arg_count;
  return out;
}

int ast_hir_function_block(const AstHirFunction *fn, AstHirBlockId block,
                           AstHirBlockInfo *out) {
  if (!fn || !out || block >= fn->block_count)
    return 0;
  const AstHirBlock *b = &fn->blocks[block];
  memset(out, 0, sizeof(*out));
  out->id = b->id;
  out->span = b->span;
  out->arguments = b->arguments;
  out->argument_count = b->argument_count;
  out->instructions = b->instructions;
  out->instruction_count = b->instruction_count;
  out->terminator = public_term(&b->term);
  out->predecessor_count = b->predecessor_count;
  out->successor_count = b->successor_count;
  out->immediate_dominator = b->immediate_dominator;
  out->reverse_postorder = b->reverse_postorder;
  out->reachable = b->reachable;
  return 1;
}

int ast_hir_function_instruction(const AstHirFunction *fn, AstHirInstId inst,
                                 AstHirInstructionInfo *out) {
  if (!fn || !out || inst >= fn->instruction_count)
    return 0;
  const AstHirInstruction *i = &fn->instructions[inst];
  memset(out, 0, sizeof(*out));
  out->op = i->op;
  out->result = i->result;
  out->type = i->type;
  out->span = i->span;
  out->effects = i->effects;
  out->token_op = i->token_op;
  out->intrinsic = i->intrinsic;
  out->memory_order = i->memory_order;
  out->failure_memory_order = i->failure_memory_order;
  out->symbol = i->symbol;
  out->module = i->module;
  out->local = i->local;
  out->aux_index = i->aux_index;
  out->native_region = i->native_region;
  out->constant = i->constant;
  out->call_signature = i->call_signature;
  out->operands = i->operands;
  out->operand_count = i->operand_count;
  return 1;
}

int ast_hir_function_value(const AstHirFunction *fn, AstHirValueId value,
                           AstHirValueInfo *out) {
  if (!fn || !out || value >= fn->value_count)
    return 0;
  const AstHirValue *v = &fn->values[value];
  out->kind = v->kind;
  out->type = v->type;
  out->span = v->span;
  out->block = v->block;
  out->definition_index = v->definition_index;
  return 1;
}

int ast_hir_function_local(const AstHirFunction *fn, AstHirLocalId local,
                           AstHirLocalInfo *out) {
  if (!fn || !out || local >= fn->local_count)
    return 0;
  const AstHirLocal *l = &fn->locals[local];
  out->name = l->name;
  out->type = l->type;
  out->span = l->span;
  out->flags = l->flags;
  return 1;
}

int ast_hir_function_generic_param(const AstHirFunction *fn, uint32_t index,
                                   AstHirGenericParamInfo *out) {
  if (!fn || !out || index >= fn->generic_param_count)
    return 0;
  const AstHirGenericParam *p = &fn->generic_params[index];
  out->name = p->name;
  out->kind = p->kind;
  out->const_type = p->const_type;
  out->span = p->span;
  out->index = p->index;
  return 1;
}

int ast_hir_function_dispatch_case(const AstHirFunction *fn, uint32_t index,
                                   AstHirDispatchCaseInfo *out) {
  if (!fn || !out || index >= fn->dispatch_case_count)
    return 0;
  const AstHirDispatchCase *c = &fn->dispatch_cases[index];
  memset(out, 0, sizeof(*out));
  out->span = c->span;
  out->condition_span = c->condition_span;
  out->condition = c->condition;
  out->target = c->target;
  out->is_else = c->is_else;
  return 1;
}

int ast_hir_global_at(const AstHirProgram *program, size_t index,
                      AstHirGlobalInfo *out) {
  if (!program || !out || index >= program->global_count)
    return 0;
  const AstHirGlobal *g = &program->globals[index];
  out->module_name = g->module_name;
  out->name = g->name;
  out->type = g->type;
  out->span = g->span;
  out->section_name = g->section_name;
  out->section_span = g->section_span;
  out->flags = g->flags;
  out->alignment = g->alignment;
  out->section_alignment = g->section_alignment;
  out->initializer = g->initializer;
  return 1;
}

int ast_hir_find_function(const AstHirProgram *program, const char *module_name,
                          const char *function_name,
                          const AstHirFunction **out) {
  if (out)
    *out = NULL;
  if (!program || !module_name || !function_name)
    return 0;
  size_t ml = strlen(module_name), fl = strlen(function_name);
  for (size_t i = 0u; i < program->function_count; ++i) {
    const AstHirFunction *fn = program->functions[i];
    if (!fn || fn->module_name.span.length != ml || fn->name.span.length != fl)
      continue;
    if (memcmp(fn->module_name.text, module_name, ml) == 0 &&
        memcmp(fn->name.text, function_name, fl) == 0) {
      if (out)
        *out = fn;
      return 1;
    }
  }
  return 0;
}

const char *ast_hir_diag_code_name(AstHirDiagCode code) {
  switch (code) {
  case AST_HIR_DIAG_NONE:
    return "none";
  case AST_HIR_DIAG_SEMA_REQUIRED:
    return "sema_required";
  case AST_HIR_DIAG_MISSING_SEMA_FACT:
    return "missing_sema_fact";
  case AST_HIR_DIAG_INVALID_COMPTIME:
    return "invalid_comptime";
  case AST_HIR_DIAG_UNSUPPORTED_MACRO_VALUE:
    return "unsupported_macro_value";
  case AST_HIR_DIAG_UNKNOWN_BINDING:
    return "unknown_binding";
  case AST_HIR_DIAG_INVALID_CFG:
    return "invalid_cfg";
  case AST_HIR_DIAG_MISSING_TERMINATOR:
    return "missing_terminator";
  case AST_HIR_DIAG_INVALID_SUCCESSOR:
    return "invalid_successor";
  case AST_HIR_DIAG_BRANCH_ARGUMENT_MISMATCH:
    return "branch_argument_mismatch";
  case AST_HIR_DIAG_INVALID_OPERAND:
    return "invalid_operand";
  case AST_HIR_DIAG_TYPE_MISMATCH:
    return "type_mismatch";
  case AST_HIR_DIAG_DOMINANCE:
    return "dominance";
  case AST_HIR_DIAG_INVALID_EFFECTS:
    return "invalid_effects";
  case AST_HIR_DIAG_INVALID_INITIALIZER:
    return "invalid_initializer";
  case AST_HIR_DIAG_INVALID_NATIVE_REGION:
    return "invalid_native_region";
  case AST_HIR_DIAG_RECURSION_LIMIT:
    return "recursion_limit";
  case AST_HIR_DIAG_INTERNAL:
    return "internal";
  case AST_HIR_DIAG_TOO_MANY_DIAGNOSTICS:
    return "too_many_diagnostics";
  default:
    return "unknown_hir_diagnostic";
  }
}

const char *ast_memory_order_name(AstMemoryOrder order) {
  switch (order) {
  case AST_MEMORY_ORDER_NONE:
    return "none";
  case AST_MEMORY_ORDER_RELAXED:
    return "relaxed";
  case AST_MEMORY_ORDER_ACQUIRE:
    return "acquire";
  case AST_MEMORY_ORDER_RELEASE:
    return "release";
  case AST_MEMORY_ORDER_ACQ_REL:
    return "acq_rel";
  case AST_MEMORY_ORDER_SEQ_CST:
    return "seq_cst";
  default:
    return "invalid";
  }
}

const char *ast_hir_op_name(AstHirOp op) {
  static const char *names[] = {
      "invalid",    "const",         "local.load",  "local.store",
      "local.addr", "global.addr",   "global.load", "global.store",
      "func.ref",   "generic.const", "unary",       "binary",
      "cast",       "select",        "call",        "index",
      "member",     "addr.of",       "load",        "store",
      "atomic",     "fence",         "intrinsic",   "bind",
      "native",     "native.result", "undef"};
  return (unsigned)op < ASTER_ARRAY_COUNT(names) ? names[(unsigned)op]
                                                 : "unknown";
}

const char *ast_hir_term_kind_name(AstHirTermKind kind) {
  switch (kind) {
  case AST_HIR_TERM_NONE:
    return "none";
  case AST_HIR_TERM_BR:
    return "br";
  case AST_HIR_TERM_COND_BR:
    return "cond_br";
  case AST_HIR_TERM_RETURN:
    return "return";
  case AST_HIR_TERM_UNREACHABLE:
    return "unreachable";
  default:
    return "unknown";
  }
}

const char *ast_hir_intrinsic_name(AstHirIntrinsicKind kind) {
  switch (kind) {
  case AST_HIR_INTRINSIC_NONE:
    return "none";
  case AST_HIR_INTRINSIC_SELECT:
    return "select";
  case AST_HIR_INTRINSIC_CLZ:
    return "clz";
  case AST_HIR_INTRINSIC_CTZ:
    return "ctz";
  case AST_HIR_INTRINSIC_POPCNT:
    return "popcnt";
  case AST_HIR_INTRINSIC_BSWAP:
    return "bswap";
  case AST_HIR_INTRINSIC_BITREV:
    return "bitrev";
  case AST_HIR_INTRINSIC_ROL:
    return "rol";
  case AST_HIR_INTRINSIC_ROR:
    return "ror";
  case AST_HIR_INTRINSIC_ADD_CHECKED:
    return "add.checked";
  case AST_HIR_INTRINSIC_SUB_CHECKED:
    return "sub.checked";
  case AST_HIR_INTRINSIC_MUL_CHECKED:
    return "mul.checked";
  case AST_HIR_INTRINSIC_ADD_WRAP:
    return "add.wrap";
  case AST_HIR_INTRINSIC_SUB_WRAP:
    return "sub.wrap";
  case AST_HIR_INTRINSIC_MUL_WRAP:
    return "mul.wrap";
  case AST_HIR_INTRINSIC_ADD_SAT:
    return "add.sat";
  case AST_HIR_INTRINSIC_SUB_SAT:
    return "sub.sat";
  case AST_HIR_INTRINSIC_PTR_TO_INT:
    return "ptr_to_int";
  case AST_HIR_INTRINSIC_INT_TO_PTR:
    return "int_to_ptr";
  case AST_HIR_INTRINSIC_SIZEOF:
    return "sizeof";
  case AST_HIR_INTRINSIC_OFFSETOF:
    return "offsetof";
  case AST_HIR_INTRINSIC_SYMBOL_ADDR:
    return "symbol_addr";
  case AST_HIR_INTRINSIC_FEATURE:
    return "feature";
  default:
    return "unknown";
  }
}
