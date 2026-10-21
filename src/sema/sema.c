#include "sema_internal.h"

#include <limits.h>

static char *sema_heap_strdup(const char *text) {
  if (!text)
    return NULL;
  size_t n = strlen(text);
  char *out = (char *)malloc(n + 1u);
  if (!out)
    return NULL;
  memcpy(out, text, n + 1u);
  return out;
}

static void sema_line_column(AstSema *s, uint32_t file_id, uint64_t offset,
                             uint32_t *line, uint32_t *column) {
  *line = 1u;
  *column = 1u;
  if (!s || !file_id)
    return;
  AstFrontendFile f;
  if (!ast_frontend_get_file(s->frontend, (size_t)file_id - 1u, &f) ||
      !f.source)
    return;
  size_t limit = offset > f.source->size ? f.source->size : (size_t)offset;
  uint32_t ln = 1u, col = 1u;
  for (size_t i = 0u; i < limit; ++i) {
    uint8_t c = f.source->data[i];
    if (c == '\n') {
      ln++;
      col = 1u;
    } else
      col++;
  }
  *line = ln;
  *column = col;
}

const uint8_t *sema_span_bytes(AstSema *s, AstSpan span, size_t *length_out) {
  if (length_out)
    *length_out = 0u;
  if (!s || span.file_id == 0u)
    return NULL;
  AstFrontendFile f;
  if (!ast_frontend_get_file(s->frontend, (size_t)span.file_id - 1u, &f) ||
      !f.source)
    return NULL;
  if (span.offset > f.source->size ||
      span.length > f.source->size - (size_t)span.offset)
    return NULL;
  if (length_out)
    *length_out = span.length;
  return f.source->data + (size_t)span.offset;
}

static AstSemaConfig sema_normalize_config(const AstSemaConfig *config) {
  AstSemaConfig out = config ? *config : ast_sema_config_default();
  if (!out.max_diagnostics)
    out.max_diagnostics = AST_SEMA_DEFAULT_MAX_DIAGNOSTICS;
  if (!out.recursion_limit)
    out.recursion_limit = AST_SEMA_DEFAULT_RECURSION_LIMIT;
  if (!out.pointer_bits)
    out.pointer_bits = 64u;
  if (!out.max_comptime_iterations)
    out.max_comptime_iterations = 65536u;
  if (!out.arena_block_size)
    out.arena_block_size = 256u * 1024u;
  return out;
}

AstSemaConfig ast_sema_config_default(void) {
  AstSemaConfig c;
  c.max_diagnostics = AST_SEMA_DEFAULT_MAX_DIAGNOSTICS;
  c.recursion_limit = AST_SEMA_DEFAULT_RECURSION_LIMIT;
  c.pointer_bits = 64u;
  c.max_comptime_iterations = 65536u;
  c.arena_block_size = 256u * 1024u;
  c.thread_count = 0u;
  c.warnings_as_errors = 0u;
  c.check_function_bodies = 1u;
  c.check_native_boundaries = 1u;
  return c;
}

static void sema_init_builtin(AstSema *s, size_t slot, AstSemaTypeKind kind,
                              uint32_t width, uint32_t alignment, uint64_t size,
                              uint32_t flags) {
  AstSemaType *t = &s->builtin[slot];
  memset(t, 0, sizeof(*t));
  t->kind = kind;
  t->bit_width = width;
  t->alignment = alignment;
  t->size = size;
  t->flags = flags | AST_SEMA_TYPEF_COMPLETE;
  t->hash = ((uint64_t)kind << 48) ^ ((uint64_t)width << 16) ^ flags;
  s->stats.type_count++;
}

enum {
  BT_ERROR = 0,
  BT_VOID,
  BT_BOOL,
  BT_I8,
  BT_U8,
  BT_I16,
  BT_U16,
  BT_I32,
  BT_U32,
  BT_I64,
  BT_U64,
  BT_I128,
  BT_U128,
  BT_ISIZE,
  BT_USIZE,
  BT_F16,
  BT_BF16,
  BT_F32,
  BT_F64,
  BT_UNTYPED_INT,
  BT_UNTYPED_FLOAT,
  BT_STRING,
  BT_BYTE_STRING,
  BT_C_STRING,
  BT_COUNT
};

static void sema_init_builtins(AstSema *s) {
  uint32_t ptr_bytes = s->config.pointer_bits / 8u;
  if (!ptr_bytes)
    ptr_bytes = 8u;
  sema_init_builtin(s, BT_ERROR, AST_SEMA_TYPE_ERROR, 0, 1, 0, 0);
  sema_init_builtin(s, BT_VOID, AST_SEMA_TYPE_VOID, 0, 1, 0, 0);
  sema_init_builtin(s, BT_BOOL, AST_SEMA_TYPE_BOOL, 1, 1, 1, 0);
  sema_init_builtin(s, BT_I8, AST_SEMA_TYPE_INT, 8, 1, 1,
                    AST_SEMA_TYPEF_SIGNED);
  sema_init_builtin(s, BT_U8, AST_SEMA_TYPE_INT, 8, 1, 1, 0);
  sema_init_builtin(s, BT_I16, AST_SEMA_TYPE_INT, 16, 2, 2,
                    AST_SEMA_TYPEF_SIGNED);
  sema_init_builtin(s, BT_U16, AST_SEMA_TYPE_INT, 16, 2, 2, 0);
  sema_init_builtin(s, BT_I32, AST_SEMA_TYPE_INT, 32, 4, 4,
                    AST_SEMA_TYPEF_SIGNED);
  sema_init_builtin(s, BT_U32, AST_SEMA_TYPE_INT, 32, 4, 4, 0);
  sema_init_builtin(s, BT_I64, AST_SEMA_TYPE_INT, 64, 8, 8,
                    AST_SEMA_TYPEF_SIGNED);
  sema_init_builtin(s, BT_U64, AST_SEMA_TYPE_INT, 64, 8, 8, 0);
  sema_init_builtin(s, BT_I128, AST_SEMA_TYPE_INT, 128, 16, 16,
                    AST_SEMA_TYPEF_SIGNED);
  sema_init_builtin(s, BT_U128, AST_SEMA_TYPE_INT, 128, 16, 16, 0);
  sema_init_builtin(s, BT_ISIZE, AST_SEMA_TYPE_INT, s->config.pointer_bits,
                    ptr_bytes, ptr_bytes, AST_SEMA_TYPEF_SIGNED);
  sema_init_builtin(s, BT_USIZE, AST_SEMA_TYPE_INT, s->config.pointer_bits,
                    ptr_bytes, ptr_bytes, 0);
  sema_init_builtin(s, BT_F16, AST_SEMA_TYPE_FLOAT, 16, 2, 2, 0);
  sema_init_builtin(s, BT_BF16, AST_SEMA_TYPE_FLOAT, 16, 2, 2,
                    AST_SEMA_TYPEF_BFLOAT);
  sema_init_builtin(s, BT_F32, AST_SEMA_TYPE_FLOAT, 32, 4, 4, 0);
  sema_init_builtin(s, BT_F64, AST_SEMA_TYPE_FLOAT, 64, 8, 8, 0);
  sema_init_builtin(s, BT_UNTYPED_INT, AST_SEMA_TYPE_UNTYPED_INT, 128, 1, 0, 0);
  sema_init_builtin(s, BT_UNTYPED_FLOAT, AST_SEMA_TYPE_UNTYPED_FLOAT, 64, 1, 0,
                    0);
  sema_init_builtin(s, BT_STRING, AST_SEMA_TYPE_STRING, s->config.pointer_bits,
                    ptr_bytes, ptr_bytes, 0);
  sema_init_builtin(s, BT_BYTE_STRING, AST_SEMA_TYPE_BYTE_STRING,
                    s->config.pointer_bits, ptr_bytes, ptr_bytes, 0);
  sema_init_builtin(s, BT_C_STRING, AST_SEMA_TYPE_C_STRING,
                    s->config.pointer_bits, ptr_bytes, ptr_bytes, 0);
}

AstSema *ast_sema_create(const AstFrontend *frontend,
                         const AstSemaConfig *config) {
  if (!frontend)
    return NULL;
  AstSema *s = (AstSema *)calloc(1u, sizeof(*s));
  if (!s)
    return NULL;
  s->frontend = frontend;
  s->config = sema_normalize_config(config);
  ast_arena_init(&s->arena, s->config.arena_block_size);
  ast_string_map_init(&s->module_map);
  sema_init_builtins(s);
  s->builtin_view = s->builtin;
  if (sema_prepare_fact_files(s) != 0) {
    ast_string_map_destroy(&s->module_map);
    ast_arena_destroy(&s->arena);
    free(s);
    return NULL;
  }
  return s;
}

static void sema_module_destroy(AstSemaModule *m) {
  if (!m)
    return;
  ast_string_map_destroy(&m->value_map);
  ast_string_map_destroy(&m->type_map);
  ast_string_map_destroy(&m->macro_map);
  free(m->symbols);
  free(m->imports);
  memset(m, 0, sizeof(*m));
}

void ast_sema_destroy(AstSema *s) {
  if (!s)
    return;
  for (size_t i = 0u; i < s->module_count; ++i)
    sema_module_destroy(&s->modules[i]);
  for (size_t i = 0u; i < s->diagnostic_count; ++i)
    free((void *)s->diagnostics[i].message);
  free(s->diagnostics);
  sema_destroy_fact_files(s);
  free(s->modules);
  ast_string_map_destroy(&s->module_map);
  ast_arena_destroy(&s->arena);
  free(s);
}

void *sema_arena_alloc(AstSema *s, size_t size, size_t align) {
  if (!s || s->oom)
    return NULL;
  void *p = ast_arena_calloc(&s->arena, 1u, size, align);
  if (!p)
    s->oom = 1u;
  return p;
}

char *sema_name_cstr(AstSema *s, AstName name) {
  if (!s || !name.text)
    return NULL;
  char *out = (char *)sema_arena_alloc(s, (size_t)name.span.length + 1u, 1u);
  if (!out)
    return NULL;
  memcpy(out, name.text, name.span.length);
  out[name.span.length] = '\0';
  return out;
}

int sema_name_eq(AstName a, AstName b) {
  if (!a.text || !b.text || a.span.length != b.span.length)
    return 0;
  if (a.hash != 0u && b.hash != 0u && a.hash != b.hash)
    return 0;
  return memcmp(a.text, b.text, a.span.length) == 0;
}

int sema_name_eq_text(AstName a, const char *b) {
  if (!a.text || !b)
    return 0;
  size_t n = strlen(b);
  return n == a.span.length && memcmp(a.text, b, n) == 0;
}

static const char *sema_path_for_file(AstSema *s, uint32_t file_id) {
  if (!s || !file_id)
    return NULL;
  AstFrontendFile file;
  if (!ast_frontend_get_file(s->frontend, (size_t)file_id - 1u, &file))
    return NULL;
  return file.path;
}

void sema_diag(AstSema *s, AstSemaDiagCode code, AstDiagSeverity severity,
               AstSpan span, uint32_t file_id, const char *fmt, ...) {
  if (!s || !fmt)
    return;
  if (s->config.warnings_as_errors && severity == AST_DIAG_WARNING)
    severity = AST_DIAG_ERROR;
  if (s->diagnostic_count >= s->config.max_diagnostics) {
    if (s->diagnostic_count == s->config.max_diagnostics) {
      size_t cap = s->diagnostic_capacity;
      if (cap <= s->diagnostic_count) {
        size_t next = cap ? cap * 2u : 64u;
        AstSemaDiagnostic *fresh =
            (AstSemaDiagnostic *)realloc(s->diagnostics, next * sizeof(*fresh));
        if (!fresh) {
          s->oom = 1u;
          return;
        }
        s->diagnostics = fresh;
        s->diagnostic_capacity = next;
      }
      AstSemaDiagnostic *d = &s->diagnostics[s->diagnostic_count++];
      memset(d, 0, sizeof(*d));
      d->code = AST_SEMA_DIAG_TOO_MANY_DIAGNOSTICS;
      d->severity = AST_DIAG_ERROR;
      d->span = span;
      d->path = sema_path_for_file(s, file_id ? file_id : span.file_id);
      d->message = sema_heap_strdup(
          "too many semantic diagnostics; further diagnostics suppressed");
      sema_line_column(s, file_id ? file_id : span.file_id, span.offset,
                       &d->line, &d->column);
      s->stats.error_count++;
    }
    return;
  }
  if (s->diagnostic_count == s->diagnostic_capacity) {
    size_t next = s->diagnostic_capacity ? s->diagnostic_capacity * 2u : 64u;
    AstSemaDiagnostic *fresh =
        (AstSemaDiagnostic *)realloc(s->diagnostics, next * sizeof(*fresh));
    if (!fresh) {
      s->oom = 1u;
      return;
    }
    s->diagnostics = fresh;
    s->diagnostic_capacity = next;
  }
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (needed < 0) {
    va_end(args);
    return;
  }
  char *message = (char *)malloc((size_t)needed + 1u);
  if (!message) {
    va_end(args);
    s->oom = 1u;
    return;
  }
  vsnprintf(message, (size_t)needed + 1u, fmt, args);
  va_end(args);
  AstSemaDiagnostic *d = &s->diagnostics[s->diagnostic_count++];
  d->code = code;
  d->severity = severity;
  d->span = span;
  d->path = sema_path_for_file(s, file_id ? file_id : span.file_id);
  sema_line_column(s, file_id ? file_id : span.file_id, span.offset, &d->line,
                   &d->column);
  d->message = message;
  if (severity == AST_DIAG_ERROR)
    s->stats.error_count++;
  else if (severity == AST_DIAG_WARNING)
    s->stats.warning_count++;
}

static int sema_reserve(void **data, size_t elem_size, size_t *capacity,
                        size_t need) {
  if (*capacity >= need)
    return 0;
  size_t cap = *capacity ? *capacity : 16u;
  while (cap < need) {
    if (cap > SIZE_MAX / 2u)
      return -1;
    cap *= 2u;
  }
  size_t bytes;
  if (ast_size_mul_overflow(cap, elem_size, &bytes))
    return -1;
  void *fresh = realloc(*data, bytes);
  if (!fresh)
    return -1;
  *data = fresh;
  *capacity = cap;
  return 0;
}

AstSemaModule *sema_module_by_file_id(AstSema *s, uint32_t file_id) {
  if (!s || file_id == 0u || file_id > s->module_count)
    return NULL;
  AstSemaModule *m = &s->modules[file_id - 1u];
  return m->ast ? m : NULL;
}

AstSemaModule *sema_module_by_name(AstSema *s, AstName name) {
  if (!s || !name.text)
    return NULL;
  uint32_t index = 0u;
  if (!ast_string_map_get_n(&s->module_map, name.text, name.span.length,
                            name.hash, &index))
    return NULL;
  return index < s->module_count && s->modules[index].ast ? &s->modules[index]
                                                          : NULL;
}

static AstName sema_last_name_segment(AstName full) {
  AstName out = full;
  if (!full.text || full.span.length == 0u)
    return out;
  uint32_t start = 0u;
  for (uint32_t i = 0u; i < full.span.length; ++i)
    if (full.text[i] == '.')
      start = i + 1u;
  out.text = full.text + start;
  out.span.offset += start;
  out.span.length -= start;
  out.hash = ast_hash_bytes(out.text, out.span.length);
  return out;
}

static AstSemaSymbol *sema_add_symbol(AstSema *s, AstSemaModule *m,
                                      AstSemaSymbolKind kind, AstName name,
                                      const AstDecl *decl,
                                      const AstDataDecl *data_decl,
                                      uint32_t flags, AstStringMap *map,
                                      AstSemaDiagCode duplicate_code) {
  uint32_t existing = 0u;
  if (ast_string_map_get_n(map, name.text, name.span.length, name.hash,
                           &existing)) {
    sema_diag(s, duplicate_code, AST_DIAG_ERROR, name.span, m->file.id,
              "duplicate declaration of '%.*s'", (int)name.span.length,
              name.text);
    return existing < m->symbol_count ? &m->symbols[existing] : NULL;
  }
  if (sema_reserve((void **)&m->symbols, sizeof(*m->symbols),
                   &m->symbol_capacity, m->symbol_count + 1u) != 0) {
    s->oom = 1u;
    return NULL;
  }
  size_t index = m->symbol_count++;
  AstSemaSymbol *sym = &m->symbols[index];
  memset(sym, 0, sizeof(*sym));
  sym->kind = kind;
  sym->name = name;
  sym->decl = decl;
  sym->data_decl = data_decl;
  sym->file_id = m->file.id;
  sym->flags = flags;
  sym->index = (uint32_t)index;
  int inserted = 0;
  if (ast_string_map_put_n(map, name.text, name.span.length, name.hash,
                           (uint32_t)index, &inserted) != 0) {
    s->oom = 1u;
    return NULL;
  }
  s->stats.symbol_count++;
  return sym;
}

AstSemaSymbol *sema_module_find_value(AstSemaModule *m, AstName name) {
  uint32_t index = 0u;
  if (!m || !name.text ||
      !ast_string_map_get_n(&m->value_map, name.text, name.span.length,
                            name.hash, &index))
    return NULL;
  return index < m->symbol_count ? &m->symbols[index] : NULL;
}

AstSemaSymbol *sema_module_find_type(AstSemaModule *m, AstName name) {
  uint32_t index = 0u;
  if (!m || !name.text ||
      !ast_string_map_get_n(&m->type_map, name.text, name.span.length,
                            name.hash, &index))
    return NULL;
  return index < m->symbol_count ? &m->symbols[index] : NULL;
}

AstSemaSymbol *sema_module_find_macro(AstSemaModule *m, AstName name) {
  uint32_t index = 0u;
  if (!m || !name.text ||
      !ast_string_map_get_n(&m->macro_map, name.text, name.span.length,
                            name.hash, &index))
    return NULL;
  return index < m->symbol_count ? &m->symbols[index] : NULL;
}

static int sema_collect_module_symbols(AstSema *s, AstSemaModule *m) {
  AstDeclArray decls = m->ast->declarations;
  for (uint32_t i = 0u; i < decls.count; ++i) {
    const AstDecl *d = decls.items[i];
    if (!d)
      continue;
    uint32_t exported = d->is_export ? AST_SEMA_SYMF_EXPORTED : 0u;
    switch (d->kind) {
    case AST_DECL_TYPE:
      sema_add_symbol(s, m, AST_SEMA_SYMBOL_TYPE, d->as.type_decl.name, d, NULL,
                      exported, &m->type_map, AST_SEMA_DIAG_DUPLICATE_TYPE);
      break;
    case AST_DECL_CONST:
      sema_add_symbol(s, m, AST_SEMA_SYMBOL_CONST, d->as.const_decl.name, d,
                      NULL, exported, &m->value_map,
                      AST_SEMA_DIAG_DUPLICATE_SYMBOL);
      break;
    case AST_DECL_EXTERN_FN:
      sema_add_symbol(s, m, AST_SEMA_SYMBOL_EXTERN_FUNCTION,
                      d->as.extern_fn.name, d, NULL, AST_SEMA_SYMF_EXPORTED,
                      &m->value_map, AST_SEMA_DIAG_DUPLICATE_SYMBOL);
      break;
    case AST_DECL_FN:
      sema_add_symbol(s, m, AST_SEMA_SYMBOL_FUNCTION, d->as.fn_decl.name, d,
                      NULL, exported, &m->value_map,
                      AST_SEMA_DIAG_DUPLICATE_SYMBOL);
      s->stats.function_count++;
      break;
    case AST_DECL_MACRO:
      sema_add_symbol(s, m, AST_SEMA_SYMBOL_MACRO, d->as.macro_decl.name, d,
                      NULL, 0u, &m->macro_map, AST_SEMA_DIAG_DUPLICATE_SYMBOL);
      break;
    case AST_DECL_SECTION:
      for (uint32_t j = 0u; j < d->as.section_decl.data.count; ++j) {
        const AstDataDecl *data = &d->as.section_decl.data.items[j];
        uint32_t df = data->is_export ? AST_SEMA_SYMF_EXPORTED : 0u;
        if (!data->is_const)
          df |= AST_SEMA_SYMF_MUTABLE;
        sema_add_symbol(s, m,
                        data->is_const ? AST_SEMA_SYMBOL_CONST
                                       : AST_SEMA_SYMBOL_GLOBAL,
                        data->name, d, data, df, &m->value_map,
                        AST_SEMA_DIAG_DUPLICATE_SYMBOL);
      }
      break;
    default:
      break;
    }
  }
  return s->oom ? -1 : 0;
}

static int sema_collect_imports(AstSema *s, AstSemaModule *m) {
  AstDeclArray decls = m->ast->declarations;
  for (uint32_t i = 0u; i < decls.count; ++i) {
    const AstDecl *d = decls.items[i];
    if (!d || d->kind != AST_DECL_IMPORT)
      continue;
    if (sema_reserve((void **)&m->imports, sizeof(*m->imports),
                     &m->import_capacity, m->import_count + 1u) != 0) {
      s->oom = 1u;
      return -1;
    }
    AstSemaImport *imp = &m->imports[m->import_count++];
    memset(imp, 0, sizeof(*imp));
    imp->module_name = d->as.import_decl.module;
    imp->has_alias = d->as.import_decl.has_alias;
    imp->alias = imp->has_alias ? d->as.import_decl.alias
                                : sema_last_name_segment(imp->module_name);
    imp->span = d->span;
    imp->target = sema_module_by_name(s, imp->module_name);
    if (!imp->target) {
      sema_diag(s, AST_SEMA_DIAG_UNKNOWN_MODULE, AST_DIAG_ERROR, d->span,
                m->file.id, "semantic module '%.*s' is unavailable",
                (int)imp->module_name.span.length, imp->module_name.text);
      continue;
    }
    for (size_t j = 0u; j + 1u < m->import_count; ++j) {
      if (sema_name_eq(m->imports[j].alias, imp->alias)) {
        sema_diag(s, AST_SEMA_DIAG_IMPORT_ALIAS_COLLISION, AST_DIAG_ERROR,
                  imp->alias.span, m->file.id,
                  "import alias '%.*s' is already used",
                  (int)imp->alias.span.length, imp->alias.text);
        break;
      }
    }
    if (sema_module_find_value(m, imp->alias) ||
        sema_module_find_type(m, imp->alias)) {
      sema_diag(s, AST_SEMA_DIAG_IMPORT_ALIAS_COLLISION, AST_DIAG_ERROR,
                imp->alias.span, m->file.id,
                "import alias '%.*s' conflicts with a module declaration",
                (int)imp->alias.span.length, imp->alias.text);
    }
  }
  return s->oom ? -1 : 0;
}

int sema_collect_modules(AstSema *s) {
  size_t count = ast_frontend_file_count(s->frontend);
  if (count == 0u)
    return 0;
  s->modules = (AstSemaModule *)calloc(count, sizeof(*s->modules));
  if (!s->modules) {
    s->oom = 1u;
    return -1;
  }
  s->module_count = count;
  s->stats.module_count = count;
  for (size_t i = 0u; i < count; ++i) {
    AstSemaModule *m = &s->modules[i];
    m->index = (uint32_t)i;
    ast_string_map_init(&m->value_map);
    ast_string_map_init(&m->type_map);
    ast_string_map_init(&m->macro_map);
    if (!ast_frontend_get_file(s->frontend, i, &m->file))
      continue;
    if (!m->file.parsed || m->file.parsed->error_count ||
        !m->file.parsed->module)
      continue;
    m->ast = m->file.parsed->module;
    uint32_t existing_module = 0u;
    if (ast_string_map_get_n(&s->module_map, m->ast->name.text,
                             m->ast->name.span.length, m->ast->name.hash,
                             &existing_module)) {
      sema_diag(s, AST_SEMA_DIAG_DUPLICATE_SYMBOL, AST_DIAG_ERROR,
                m->ast->name.span, m->file.id,
                "duplicate semantic module '%.*s'",
                (int)m->ast->name.span.length, m->ast->name.text);
    } else {
      int inserted = 0;
      if (ast_string_map_put_n(&s->module_map, m->ast->name.text,
                               m->ast->name.span.length, m->ast->name.hash,
                               (uint32_t)i, &inserted) != 0) {
        s->oom = 1u;
        return -1;
      }
    }
    if (sema_collect_module_symbols(s, m) != 0)
      return -1;
  }
  for (size_t i = 0u; i < count; ++i) {
    if (s->modules[i].ast && sema_collect_imports(s, &s->modules[i]) != 0)
      return -1;
  }
  return 0;
}

static int sema_diag_compare(const void *lhs, const void *rhs) {
  const AstSemaDiagnostic *a = (const AstSemaDiagnostic *)lhs;
  const AstSemaDiagnostic *b = (const AstSemaDiagnostic *)rhs;
  if (a->span.file_id != b->span.file_id)
    return a->span.file_id < b->span.file_id ? -1 : 1;
  if (a->span.offset != b->span.offset)
    return a->span.offset < b->span.offset ? -1 : 1;
  if (a->span.length != b->span.length)
    return a->span.length < b->span.length ? -1 : 1;
  if (a->severity != b->severity)
    return a->severity < b->severity ? -1 : 1;
  if (a->code != b->code)
    return a->code < b->code ? -1 : 1;
  if (!a->message && !b->message)
    return 0;
  if (!a->message)
    return -1;
  if (!b->message)
    return 1;
  return strcmp(a->message, b->message);
}

void sema_finalize_diagnostics(AstSema *s) {
  if (!s)
    return;
  if (s->diagnostic_count > 1u) {
    qsort(s->diagnostics, s->diagnostic_count, sizeof(*s->diagnostics),
          sema_diag_compare);
    size_t write_index = 1u;
    for (size_t read_index = 1u; read_index < s->diagnostic_count;
         ++read_index) {
      if (sema_diag_compare(&s->diagnostics[write_index - 1u],
                            &s->diagnostics[read_index]) == 0) {
        free((void *)s->diagnostics[read_index].message);
        s->diagnostics[read_index].message = NULL;
        continue;
      }
      if (write_index != read_index)
        s->diagnostics[write_index] = s->diagnostics[read_index];
      write_index++;
    }
    s->diagnostic_count = write_index;
  }

  size_t hard_limit = s->config.max_diagnostics;
  if (hard_limit == 0u)
    hard_limit = AST_SEMA_DEFAULT_MAX_DIAGNOSTICS;
  if (s->diagnostic_count > hard_limit) {
    AstSpan cutoff = s->diagnostics[hard_limit].span;
    uint32_t cutoff_file = cutoff.file_id;
    for (size_t i = hard_limit; i < s->diagnostic_count; ++i) {
      free((void *)s->diagnostics[i].message);
      s->diagnostics[i].message = NULL;
    }
    s->diagnostic_count = hard_limit;
    if (s->diagnostic_capacity <= hard_limit) {
      size_t next = hard_limit + 1u;
      AstSemaDiagnostic *fresh =
          (AstSemaDiagnostic *)realloc(s->diagnostics, next * sizeof(*fresh));
      if (!fresh) {
        s->oom = 1u;
        return;
      }
      s->diagnostics = fresh;
      s->diagnostic_capacity = next;
    }
    AstSemaDiagnostic *d = &s->diagnostics[s->diagnostic_count++];
    memset(d, 0, sizeof(*d));
    d->code = AST_SEMA_DIAG_TOO_MANY_DIAGNOSTICS;
    d->severity = AST_DIAG_ERROR;
    d->span = cutoff;
    d->path = sema_path_for_file(s, cutoff_file);
    sema_line_column(s, cutoff_file, cutoff.offset, &d->line, &d->column);
    d->message = sema_heap_strdup(
        "too many semantic diagnostics; further diagnostics suppressed");
    if (!d->message)
      s->oom = 1u;
  }

  s->stats.error_count = 0u;
  s->stats.warning_count = 0u;
  for (size_t i = 0u; i < s->diagnostic_count; ++i) {
    if (s->diagnostics[i].severity == AST_DIAG_ERROR)
      s->stats.error_count++;
    else if (s->diagnostics[i].severity == AST_DIAG_WARNING)
      s->stats.warning_count++;
  }
}

int ast_sema_check(AstSema *s) {
  if (!s)
    return -1;
  if (s->checked)
    return s->stats.error_count ? 1 : 0;
  s->checked = 1u;
  if (sema_collect_modules(s) != 0 || s->oom)
    return -1;
  if (sema_resolve_module_interfaces(s) != 0 || s->oom)
    return -1;
  if (s->config.check_function_bodies && sema_check_module_bodies(s) != 0)
    return -1;
  sema_finalize_diagnostics(s);
  s->stats.arena_bytes_used += s->arena.bytes_used;
  s->stats.arena_bytes_reserved += s->arena.bytes_reserved;
  return s->oom ? -1 : (s->stats.error_count ? 1 : 0);
}

const AstSemaDiagnostic *ast_sema_diagnostics(const AstSema *s,
                                              size_t *count_out) {
  if (count_out)
    *count_out = s ? s->diagnostic_count : 0u;
  return s ? s->diagnostics : NULL;
}

AstSemaStats ast_sema_stats(const AstSema *s) {
  AstSemaStats zero;
  memset(&zero, 0, sizeof(zero));
  return s ? s->stats : zero;
}

AstSemaTypeKind ast_sema_type_kind(const AstSemaType *type) {
  return type ? type->kind : AST_SEMA_TYPE_ERROR;
}
uint32_t ast_sema_type_bit_width(const AstSemaType *type) {
  return type ? type->bit_width : 0u;
}
uint64_t ast_sema_type_size(const AstSemaType *type) {
  return type ? type->size : AST_SEMA_UNKNOWN_SIZE;
}
uint32_t ast_sema_type_alignment(const AstSemaType *type) {
  return type ? type->alignment : 0u;
}
int ast_sema_type_is_signed(const AstSemaType *type) {
  return type && (type->flags & AST_SEMA_TYPEF_SIGNED) != 0u;
}

int ast_sema_find_symbol(const AstSema *const_s, const char *module_name,
                         const char *symbol_name, AstSemaSymbolInfo *out) {
  if (!const_s || !module_name || !symbol_name || !out)
    return 0;
  AstSema *s = (AstSema *)const_s;
  for (size_t i = 0u; i < s->module_count; ++i) {
    AstSemaModule *m = &s->modules[i];
    if (!m->ast || !ast_name_equal_cstr(m->ast->name, module_name))
      continue;
    size_t n = strlen(symbol_name);
    AstName probe;
    memset(&probe, 0, sizeof(probe));
    probe.text = symbol_name;
    probe.span.length = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
    probe.hash = ast_hash_bytes(symbol_name, n);
    AstSemaSymbol *sym = sema_module_find_value(m, probe);
    if (!sym)
      sym = sema_module_find_type(m, probe);
    if (!sym)
      sym = sema_module_find_macro(m, probe);
    if (!sym)
      return 0;
    out->kind = sym->kind;
    out->name = sym->name;
    out->type = sym->type;
    out->declaration = sym->decl;
    out->file_id = sym->file_id;
    out->is_exported = (sym->flags & AST_SEMA_SYMF_EXPORTED) != 0u;
    out->is_mutable = (sym->flags & AST_SEMA_SYMF_MUTABLE) != 0u;
    return 1;
  }
  return 0;
}

const char *ast_sema_diag_code_name(AstSemaDiagCode code) {
  switch (code) {
  case AST_SEMA_DIAG_NONE:
    return "none";
  case AST_SEMA_DIAG_DUPLICATE_SYMBOL:
    return "duplicate_symbol";
  case AST_SEMA_DIAG_DUPLICATE_TYPE:
    return "duplicate_type";
  case AST_SEMA_DIAG_DUPLICATE_FIELD:
    return "duplicate_field";
  case AST_SEMA_DIAG_DUPLICATE_PARAMETER:
    return "duplicate_parameter";
  case AST_SEMA_DIAG_DUPLICATE_LOCAL:
    return "duplicate_local";
  case AST_SEMA_DIAG_UNKNOWN_NAME:
    return "unknown_name";
  case AST_SEMA_DIAG_UNKNOWN_TYPE:
    return "unknown_type";
  case AST_SEMA_DIAG_UNKNOWN_MODULE:
    return "unknown_module";
  case AST_SEMA_DIAG_SYMBOL_NOT_EXPORTED:
    return "symbol_not_exported";
  case AST_SEMA_DIAG_IMPORT_ALIAS_COLLISION:
    return "import_alias_collision";
  case AST_SEMA_DIAG_TYPE_MISMATCH:
    return "type_mismatch";
  case AST_SEMA_DIAG_EXPECTED_BOOL:
    return "expected_bool";
  case AST_SEMA_DIAG_EXPECTED_INTEGER:
    return "expected_integer";
  case AST_SEMA_DIAG_EXPECTED_NUMERIC:
    return "expected_numeric";
  case AST_SEMA_DIAG_EXPECTED_POINTER:
    return "expected_pointer";
  case AST_SEMA_DIAG_INVALID_BINARY_OPERANDS:
    return "invalid_binary_operands";
  case AST_SEMA_DIAG_INVALID_UNARY_OPERAND:
    return "invalid_unary_operand";
  case AST_SEMA_DIAG_INVALID_CAST:
    return "invalid_cast";
  case AST_SEMA_DIAG_DIVISION_BY_ZERO:
    return "division_by_zero";
  case AST_SEMA_DIAG_INTEGER_DIVISION_OVERFLOW:
    return "integer_division_overflow";
  case AST_SEMA_DIAG_INTEGER_LITERAL_OVERFLOW:
    return "integer_literal_overflow";
  case AST_SEMA_DIAG_FLOAT_LITERAL_OVERFLOW:
    return "float_literal_overflow";
  case AST_SEMA_DIAG_INVALID_ARRAY_SIZE:
    return "invalid_array_size";
  case AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION:
    return "non_constant_expression";
  case AST_SEMA_DIAG_CONST_CYCLE:
    return "const_cycle";
  case AST_SEMA_DIAG_STATIC_ASSERT_FAILED:
    return "static_assert_failed";
  case AST_SEMA_DIAG_CALL_NON_FUNCTION:
    return "call_non_function";
  case AST_SEMA_DIAG_CALL_ARITY:
    return "call_arity";
  case AST_SEMA_DIAG_CALL_ARGUMENT_TYPE:
    return "call_argument_type";
  case AST_SEMA_DIAG_RETURN_TYPE:
    return "return_type";
  case AST_SEMA_DIAG_MISSING_RETURN:
    return "missing_return";
  case AST_SEMA_DIAG_UNINITIALIZED_VALUE:
    return "uninitialized_value";
  case AST_SEMA_DIAG_ASSIGN_TO_CONST:
    return "assign_to_const";
  case AST_SEMA_DIAG_BREAK_OUTSIDE_LOOP:
    return "break_outside_loop";
  case AST_SEMA_DIAG_CONTINUE_OUTSIDE_LOOP:
    return "continue_outside_loop";
  case AST_SEMA_DIAG_DUPLICATE_LABEL:
    return "duplicate_label";
  case AST_SEMA_DIAG_UNKNOWN_LABEL:
    return "unknown_label";
  case AST_SEMA_DIAG_INVALID_BRANCH_SCOPE:
    return "invalid_branch_scope";
  case AST_SEMA_DIAG_INVALID_LOAD:
    return "invalid_load";
  case AST_SEMA_DIAG_INVALID_STORE:
    return "invalid_store";
  case AST_SEMA_DIAG_INVALID_ATOMIC_OPERATION:
    return "invalid_atomic_operation";
  case AST_SEMA_DIAG_INVALID_ATOMIC_ORDER:
    return "invalid_atomic_order";
  case AST_SEMA_DIAG_INVALID_ATOMIC_ARGUMENTS:
    return "invalid_atomic_arguments";
  case AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE:
    return "invalid_native_bridge";
  case AST_SEMA_DIAG_GENERIC_ARITY:
    return "generic_arity";
  case AST_SEMA_DIAG_GENERIC_INFERENCE:
    return "generic_inference";
  case AST_SEMA_DIAG_RECURSIVE_TYPE:
    return "recursive_type";
  case AST_SEMA_DIAG_INVALID_INITIALIZER:
    return "invalid_initializer";
  case AST_SEMA_DIAG_INVALID_ATTRIBUTE:
    return "invalid_attribute";
  case AST_SEMA_DIAG_INVALID_TARGET:
    return "invalid_target";
  case AST_SEMA_DIAG_COMPTIME_ITERATION_LIMIT:
    return "comptime_iteration_limit";
  case AST_SEMA_DIAG_RECURSION_LIMIT:
    return "semantic_recursion_limit";
  case AST_SEMA_DIAG_INTERNAL:
    return "internal";
  case AST_SEMA_DIAG_TOO_MANY_DIAGNOSTICS:
    return "too_many_diagnostics";
  default:
    return "unknown";
  }
}

static const AstSemaType *sema_builtin_base(const AstSema *s) {
  return s && s->builtin_view ? s->builtin_view : (s ? s->builtin : NULL);
}

const AstSemaType *sema_builtin_for_token(AstSema *s, AstTokenKind token) {
  const AstSemaType *b = sema_builtin_base(s);
  if (!b)
    return NULL;
  switch (token) {
  case AST_TOK_KW_VOID:
    return &b[BT_VOID];
  case AST_TOK_KW_BOOL:
    return &b[BT_BOOL];
  case AST_TOK_KW_I8:
    return &b[BT_I8];
  case AST_TOK_KW_U8:
    return &b[BT_U8];
  case AST_TOK_KW_I16:
    return &b[BT_I16];
  case AST_TOK_KW_U16:
    return &b[BT_U16];
  case AST_TOK_KW_I32:
    return &b[BT_I32];
  case AST_TOK_KW_U32:
    return &b[BT_U32];
  case AST_TOK_KW_I64:
    return &b[BT_I64];
  case AST_TOK_KW_U64:
    return &b[BT_U64];
  case AST_TOK_KW_I128:
    return &b[BT_I128];
  case AST_TOK_KW_U128:
    return &b[BT_U128];
  case AST_TOK_KW_ISIZE:
    return &b[BT_ISIZE];
  case AST_TOK_KW_USIZE:
    return &b[BT_USIZE];
  case AST_TOK_KW_F16:
    return &b[BT_F16];
  case AST_TOK_KW_BF16:
    return &b[BT_BF16];
  case AST_TOK_KW_F32:
    return &b[BT_F32];
  case AST_TOK_KW_F64:
    return &b[BT_F64];
  default:
    return &b[BT_ERROR];
  }
}

const AstSemaType *sema_builtin_untyped_int(AstSema *s) {
  const AstSemaType *b = sema_builtin_base(s);
  return b ? &b[BT_UNTYPED_INT] : NULL;
}
const AstSemaType *sema_builtin_untyped_float(AstSema *s) {
  const AstSemaType *b = sema_builtin_base(s);
  return b ? &b[BT_UNTYPED_FLOAT] : NULL;
}
const AstSemaType *sema_builtin_bool(AstSema *s) {
  const AstSemaType *b = sema_builtin_base(s);
  return b ? &b[BT_BOOL] : NULL;
}
const AstSemaType *sema_builtin_void(AstSema *s) {
  const AstSemaType *b = sema_builtin_base(s);
  return b ? &b[BT_VOID] : NULL;
}
const AstSemaType *sema_builtin_error(AstSema *s) {
  const AstSemaType *b = sema_builtin_base(s);
  return b ? &b[BT_ERROR] : NULL;
}
const AstSemaType *sema_builtin_usize(const AstSema *s) {
  const AstSemaType *b = sema_builtin_base(s);
  return b ? &b[BT_USIZE] : NULL;
}
const AstSemaType *sema_builtin_string(AstSema *s, AstExprKind kind) {
  const AstSemaType *b = sema_builtin_base(s);
  if (!b)
    return NULL;
  if (kind == AST_EXPR_BYTE_STRING)
    return &b[BT_BYTE_STRING];
  if (kind == AST_EXPR_C_STRING)
    return &b[BT_C_STRING];
  return &b[BT_STRING];
}

uint32_t ast_sema_type_pointer_qualifiers(const AstSemaType *type) {
  if (!type ||
      (type->kind != AST_SEMA_TYPE_PTR && type->kind != AST_SEMA_TYPE_CAP))
    return 0u;
  return type->as.pointer.qualifiers;
}

const AstSemaType *ast_sema_type_element(const AstSemaType *type) {
  if (!type)
    return NULL;
  switch (type->kind) {
  case AST_SEMA_TYPE_PTR:
  case AST_SEMA_TYPE_CAP:
    return type->as.pointer.element;
  case AST_SEMA_TYPE_ARRAY:
    return type->as.array.element;
  case AST_SEMA_TYPE_VECTOR:
    return type->as.vector.element;
  case AST_SEMA_TYPE_SVECTOR:
  case AST_SEMA_TYPE_PRED:
    return type->as.scalable.element;
  default:
    return NULL;
  }
}

int ast_sema_type_bits_width(const AstSemaType *type, uint64_t *out) {
  if (!type || type->kind != AST_SEMA_TYPE_BITS || type->as.bits.dependent)
    return 0;
  if (out)
    *out = type->as.bits.width;
  return 1;
}

int ast_sema_type_array_count(const AstSemaType *type, uint64_t *out) {
  if (!type || type->kind != AST_SEMA_TYPE_ARRAY || type->as.array.dependent)
    return 0;
  if (out)
    *out = type->as.array.count;
  return 1;
}

int ast_sema_type_vector_lanes(const AstSemaType *type, uint64_t *out) {
  if (!type)
    return 0;
  if (type->kind == AST_SEMA_TYPE_VECTOR) {
    if (type->as.vector.dependent)
      return 0;
    if (out)
      *out = type->as.vector.lanes;
    return 1;
  }
  if (type->kind == AST_SEMA_TYPE_MASK) {
    if (type->as.mask.dependent)
      return 0;
    if (out)
      *out = type->as.mask.lanes;
    return 1;
  }
  return 0;
}

uint32_t ast_sema_type_tuple_count(const AstSemaType *type) {
  return type && type->kind == AST_SEMA_TYPE_TUPLE ? type->as.tuple.count : 0u;
}

const AstSemaType *ast_sema_type_tuple_item(const AstSemaType *type,
                                            uint32_t index) {
  if (!type || type->kind != AST_SEMA_TYPE_TUPLE ||
      index >= type->as.tuple.count)
    return NULL;
  return type->as.tuple.items[index];
}

uint32_t ast_sema_type_function_param_count(const AstSemaType *type) {
  return type && type->kind == AST_SEMA_TYPE_FUNCTION
             ? type->as.function.param_count
             : 0u;
}

const AstSemaType *ast_sema_type_function_param(const AstSemaType *type,
                                                uint32_t index) {
  if (!type || type->kind != AST_SEMA_TYPE_FUNCTION ||
      index >= type->as.function.param_count)
    return NULL;
  return type->as.function.params[index];
}

const AstSemaType *ast_sema_type_function_result(const AstSemaType *type) {
  return type && type->kind == AST_SEMA_TYPE_FUNCTION ? type->as.function.result
                                                      : NULL;
}

AstName ast_sema_type_struct_name(const AstSemaType *type) {
  AstName empty;
  memset(&empty, 0, sizeof(empty));
  return type && type->kind == AST_SEMA_TYPE_STRUCT ? type->as.record.name
                                                    : empty;
}

uint32_t ast_sema_type_struct_field_count(const AstSemaType *type) {
  return type && type->kind == AST_SEMA_TYPE_STRUCT
             ? type->as.record.field_count
             : 0u;
}

int ast_sema_type_struct_field(const AstSemaType *type, uint32_t index,
                               AstSemaFieldInfo *out) {
  if (!type || type->kind != AST_SEMA_TYPE_STRUCT ||
      index >= type->as.record.field_count || !out)
    return 0;
  out->name = type->as.record.fields[index].name;
  out->type = type->as.record.fields[index].type;
  out->offset = type->as.record.fields[index].offset;
  return 1;
}
