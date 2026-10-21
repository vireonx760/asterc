#include "asterasm/frontend.h"
#include "../core/hashmap.h"
#include "../core/parallel.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AstFrontendRecord {
  uint32_t id;
  char *path;
  AstMappedSource mapped;
  AstLexedFile lexed;
  AstParseResult parsed;
  int load_status;
  uint8_t loaded;
} AstFrontendRecord;

struct AstFrontend {
  AstFrontendConfig config;
  char *module_extension;
  char **roots;
  size_t root_count;
  size_t root_capacity;
  AstFrontendRecord **files;
  size_t file_count;
  size_t file_capacity;
  AstStringMap path_map;
  AstStringMap module_map;
  AstImportEdge *edges;
  size_t edge_count;
  size_t edge_capacity;
  AstFrontendDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
};

typedef struct AstLoadJobs {
  AstFrontend *frontend;
  AstFrontendRecord **records;
  size_t count;
} AstLoadJobs;

static char *ast_front_name_cstr(AstName name) {
  if (!name.text)
    return NULL;
  size_t length = name.span.length;
  char *out = (char *)malloc(length + 1u);
  if (!out)
    return NULL;
  memcpy(out, name.text, length);
  out[length] = '\0';
  return out;
}

static char *ast_front_strdup(const char *text) {
  if (!text)
    return NULL;
  size_t n = strlen(text);
  char *out = (char *)malloc(n + 1u);
  if (!out)
    return NULL;
  memcpy(out, text, n + 1u);
  return out;
}

static int ast_front_reserve(void **data, size_t elem_size, size_t *capacity,
                             size_t need) {
  if (*capacity >= need)
    return 0;
  size_t cap = *capacity ? *capacity : 8u;
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

static int ast_front_diag(AstFrontend *f, AstFrontendDiagCode code,
                          AstDiagSeverity severity, AstSpan span,
                          const char *path, const char *fmt, ...)
    ASTER_PRINTF_LIKE(6, 7);

static int ast_front_diag(AstFrontend *f, AstFrontendDiagCode code,
                          AstDiagSeverity severity, AstSpan span,
                          const char *path, const char *fmt, ...) {
  if (!f || !fmt)
    return -1;
  if (ast_front_reserve((void **)&f->diagnostics, sizeof(*f->diagnostics),
                        &f->diagnostic_capacity, f->diagnostic_count + 1u) != 0)
    return -1;
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  int n = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (n < 0) {
    va_end(args);
    return -1;
  }
  char *message = (char *)malloc((size_t)n + 1u);
  if (!message) {
    va_end(args);
    return -1;
  }
  vsnprintf(message, (size_t)n + 1u, fmt, args);
  va_end(args);
  AstFrontendDiagnostic *d = &f->diagnostics[f->diagnostic_count++];
  d->code = code;
  d->severity = severity;
  d->span = span;
  d->path = path;
  d->message = message;
  return 0;
}

AstFrontendConfig ast_frontend_config_default(void) {
  AstFrontendConfig c;
  c.lex = ast_lex_config_default();
  c.parse = ast_parse_config_default();
  c.thread_count = 0u;
  c.flags = AST_FRONTEND_RESOLVE_IMPORTS | AST_FRONTEND_DETECT_IMPORT_CYCLES;
  c.module_extension = ".aa";
  return c;
}

AstFrontend *ast_frontend_create(const AstFrontendConfig *config) {
  AstFrontend *f = (AstFrontend *)calloc(1u, sizeof(*f));
  if (!f)
    return NULL;
  f->config = config ? *config : ast_frontend_config_default();
  if (!f->config.lex.arena_block_size)
    f->config.lex = ast_lex_config_default();
  if (!f->config.parse.arena_block_size)
    f->config.parse = ast_parse_config_default();
  f->module_extension = ast_front_strdup(
      f->config.module_extension ? f->config.module_extension : ".aa");
  if (!f->module_extension) {
    free(f);
    return NULL;
  }
  f->config.module_extension = f->module_extension;
  ast_string_map_init(&f->path_map);
  ast_string_map_init(&f->module_map);
  return f;
}

void ast_frontend_destroy(AstFrontend *f) {
  if (!f)
    return;
  ast_string_map_destroy(&f->path_map);
  ast_string_map_destroy(&f->module_map);
  for (size_t i = 0; i < f->diagnostic_count; ++i)
    free((void *)f->diagnostics[i].message);
  free(f->diagnostics);
  free(f->edges);
  for (size_t i = 0; i < f->file_count; ++i) {
    AstFrontendRecord *r = f->files[i];
    if (!r)
      continue;
    ast_parse_result_destroy(&r->parsed);
    ast_lexed_file_destroy(&r->lexed);
    ast_source_unmap(&r->mapped);
    free(r->path);
    free(r);
  }
  free(f->files);
  for (size_t i = 0; i < f->root_count; ++i)
    free(f->roots[i]);
  free(f->roots);
  free(f->module_extension);
  free(f);
}

int ast_frontend_add_import_root(AstFrontend *f, const char *path) {
  if (!f || !path)
    return -1;
  char *canonical = ast_path_canonicalize(path);
  if (!canonical)
    return -1;
  for (size_t i = 0; i < f->root_count; ++i) {
    if (strcmp(f->roots[i], canonical) == 0) {
      free(canonical);
      return 0;
    }
  }
  if (ast_front_reserve((void **)&f->roots, sizeof(*f->roots),
                        &f->root_capacity, f->root_count + 1u) != 0) {
    free(canonical);
    return -1;
  }
  f->roots[f->root_count++] = canonical;
  return 0;
}

static AstFrontendRecord *ast_front_get_by_id(const AstFrontend *f,
                                              uint32_t id) {
  if (!f || id == 0u || id > f->file_count)
    return NULL;
  return f->files[id - 1u];
}

static AstFrontendRecord *ast_front_add_path(AstFrontend *f, const char *path,
                                             int *inserted_out) {
  if (inserted_out)
    *inserted_out = 0;
  char *canonical = ast_path_canonicalize(path);
  if (!canonical)
    return NULL;
  uint32_t index;
  if (ast_string_map_get(&f->path_map, canonical, &index)) {
    free(canonical);
    return index < f->file_count ? f->files[index] : NULL;
  }
  if (f->file_count >= UINT32_MAX) {
    free(canonical);
    return NULL;
  }
  AstFrontendRecord *r = (AstFrontendRecord *)calloc(1u, sizeof(*r));
  if (!r) {
    free(canonical);
    return NULL;
  }
  r->id = (uint32_t)f->file_count + 1u;
  r->path = canonical;
  if (ast_front_reserve((void **)&f->files, sizeof(*f->files),
                        &f->file_capacity, f->file_count + 1u) != 0) {
    free(r->path);
    free(r);
    return NULL;
  }
  size_t index_new = f->file_count++;
  f->files[index_new] = r;
  int inserted = 0;
  if (ast_string_map_put(&f->path_map, r->path, (uint32_t)index_new,
                         &inserted) != 0 ||
      !inserted) {
    f->file_count--;
    free(r->path);
    free(r);
    return NULL;
  }
  if (inserted_out)
    *inserted_out = 1;
  return r;
}

static void ast_front_load_worker(void *ctx, size_t index) {
  AstLoadJobs *jobs = (AstLoadJobs *)ctx;
  AstFrontendRecord *r = jobs->records[index];
  AstFrontend *f = jobs->frontend;
  r->load_status = 0;
  if (ast_source_map_file(r->path, &r->mapped) != 0) {
    r->load_status = -1;
    r->loaded = 1u;
    return;
  }
  if (ast_lex_source(&r->mapped.source, &f->config.lex, &r->lexed) != 0) {
    r->load_status = -2;
    r->loaded = 1u;
    return;
  }
  if (ast_parse_file(&r->lexed, r->id, &f->config.parse, &r->parsed) != 0) {
    r->load_status = -3;
    r->loaded = 1u;
    return;
  }
  r->loaded = 1u;
}

static int ast_front_register_modules(AstFrontend *f,
                                      AstFrontendRecord **records,
                                      size_t count) {
  for (size_t i = 0; i < count; ++i) {
    AstFrontendRecord *r = records[i];
    if (r->load_status == -1) {
      ast_front_diag(f, AST_FRONTEND_DIAG_IO, AST_DIAG_ERROR, (AstSpan){0},
                     r->path, "cannot open source file '%s'", r->path);
      continue;
    }
    if (r->load_status < 0 || !r->parsed.module || !r->parsed.module->name.text)
      continue;
    uint32_t existing;
    if (ast_string_map_get_n(&f->module_map, r->parsed.module->name.text,
                             r->parsed.module->name.span.length,
                             r->parsed.module->name.hash, &existing)) {
      AstFrontendRecord *other =
          existing < f->file_count ? f->files[existing] : NULL;
      ast_front_diag(f, AST_FRONTEND_DIAG_DUPLICATE_MODULE, AST_DIAG_ERROR,
                     r->parsed.module->name.span, r->path,
                     "module '%.*s' is already defined by '%s'",
                     (int)r->parsed.module->name.span.length,
                     r->parsed.module->name.text,
                     other ? other->path : "another file");
      continue;
    }
    int inserted = 0;
    if (ast_string_map_put_n(&f->module_map, r->parsed.module->name.text,
                             r->parsed.module->name.span.length,
                             r->parsed.module->name.hash, r->id - 1u,
                             &inserted) != 0)
      return -1;
  }
  return 0;
}

static char *ast_front_dirname(const char *path) {
  const char *slash = strrchr(path, '/');
#if defined(_WIN32)
  const char *back = strrchr(path, '\\');
  if (!slash || (back && back > slash))
    slash = back;
#endif
  if (!slash)
    return ast_front_strdup(".");
  size_t n = (size_t)(slash - path);
  if (n == 0u)
    n = 1u;
  char *out = (char *)malloc(n + 1u);
  if (!out)
    return NULL;
  memcpy(out, path, n);
  out[n] = '\0';
  return out;
}

static char *ast_front_resolve_import(const AstFrontend *f,
                                      const AstFrontendRecord *from,
                                      const char *module_name) {
  for (size_t i = 0; i < f->root_count; ++i) {
    char *candidate = ast_path_module_candidate(f->roots[i], module_name,
                                                f->module_extension);
    if (!candidate)
      continue;
    if (ast_path_exists_regular(candidate)) {
      char *canonical = ast_path_canonicalize(candidate);
      free(candidate);
      return canonical;
    }
    free(candidate);
  }
  if (from && from->path) {
    char *dir = ast_front_dirname(from->path);
    if (dir) {
      char *candidate =
          ast_path_module_candidate(dir, module_name, f->module_extension);
      free(dir);
      if (candidate) {
        if (ast_path_exists_regular(candidate)) {
          char *canonical = ast_path_canonicalize(candidate);
          free(candidate);
          return canonical;
        }
        free(candidate);
      }
    }
  }
  return NULL;
}

static char *ast_front_resolve_import_name(AstFrontend *f,
                                           const AstFrontendRecord *from,
                                           AstName name) {
  char *module_name = ast_front_name_cstr(name);
  if (!module_name)
    return NULL;
  char *resolved = ast_front_resolve_import(f, from, module_name);
  free(module_name);
  return resolved;
}

static int ast_front_collect_new_imports(AstFrontend *f,
                                         AstFrontendRecord *const *from_records,
                                         size_t from_count,
                                         AstFrontendRecord ***out_records,
                                         size_t *out_count) {
  AstFrontendRecord **records = NULL;
  size_t count = 0u, capacity = 0u;
  for (size_t i = 0; i < from_count; ++i) {
    AstFrontendRecord *from = from_records[i];
    if (!from->loaded || from->load_status < 0 || !from->parsed.module)
      continue;
    AstDeclArray decls = from->parsed.module->declarations;
    for (uint32_t d = 0; d < decls.count; ++d) {
      AstDecl *decl = decls.items[d];
      if (!decl || decl->kind != AST_DECL_IMPORT ||
          !decl->as.import_decl.module.text)
        continue;
      uint32_t target_index;
      if (ast_string_map_get_n(&f->module_map, decl->as.import_decl.module.text,
                               decl->as.import_decl.module.span.length,
                               decl->as.import_decl.module.hash, &target_index))
        continue;
      char *resolved =
          ast_front_resolve_import_name(f, from, decl->as.import_decl.module);
      if (!resolved)
        continue;
      int inserted = 0;
      AstFrontendRecord *r = ast_front_add_path(f, resolved, &inserted);
      free(resolved);
      if (!r) {
        free(records);
        return -1;
      }
      if (inserted) {
        if (ast_front_reserve((void **)&records, sizeof(*records), &capacity,
                              count + 1u) != 0) {
          free(records);
          return -1;
        }
        records[count++] = r;
      }
    }
  }
  *out_records = records;
  *out_count = count;
  return 0;
}

static int ast_front_add_edge(AstFrontend *f, uint32_t from, uint32_t to,
                              AstSpan span) {
  if (ast_front_reserve((void **)&f->edges, sizeof(*f->edges),
                        &f->edge_capacity, f->edge_count + 1u) != 0)
    return -1;
  AstImportEdge *e = &f->edges[f->edge_count++];
  e->from_file_id = from;
  e->to_file_id = to;
  e->import_span = span;
  return 0;
}

static int ast_front_build_edges(AstFrontend *f) {
  f->edge_count = 0u;
  for (size_t i = 0; i < f->file_count; ++i) {
    AstFrontendRecord *from = f->files[i];
    if (from->load_status < 0 || !from->parsed.module)
      continue;
    AstDeclArray decls = from->parsed.module->declarations;
    for (uint32_t d = 0; d < decls.count; ++d) {
      AstDecl *decl = decls.items[d];
      if (!decl || decl->kind != AST_DECL_IMPORT ||
          !decl->as.import_decl.module.text)
        continue;
      uint32_t target_index;
      if (ast_string_map_get_n(&f->module_map, decl->as.import_decl.module.text,
                               decl->as.import_decl.module.span.length,
                               decl->as.import_decl.module.hash,
                               &target_index)) {
        AstFrontendRecord *to =
            target_index < f->file_count ? f->files[target_index] : NULL;
        if (to && ast_front_add_edge(f, from->id, to->id, decl->span) != 0)
          return -1;
        continue;
      }
      char *resolved =
          ast_front_resolve_import_name(f, from, decl->as.import_decl.module);
      if (resolved) {
        uint32_t path_index;
        if (ast_string_map_get(&f->path_map, resolved, &path_index) &&
            path_index < f->file_count) {
          AstFrontendRecord *actual = f->files[path_index];
          AstName actual_name = actual->parsed.module
                                    ? actual->parsed.module->name
                                    : (AstName){0};
          const char *actual_text = actual_name.text
                                        ? actual_name.text
                                        : "<missing module declaration>";
          int actual_len = actual_name.text ? (int)actual_name.span.length
                                            : (int)strlen(actual_text);
          ast_front_diag(f, AST_FRONTEND_DIAG_MODULE_PATH_MISMATCH,
                         AST_DIAG_ERROR, decl->span, from->path,
                         "import '%.*s' resolved to '%s', but that file "
                         "declares module '%.*s'",
                         (int)decl->as.import_decl.module.span.length,
                         decl->as.import_decl.module.text, resolved, actual_len,
                         actual_text);
          free(resolved);
          continue;
        }
        free(resolved);
      }
      ast_front_diag(f, AST_FRONTEND_DIAG_IMPORT_NOT_FOUND, AST_DIAG_ERROR,
                     decl->span, from->path,
                     "cannot resolve imported module '%.*s'",
                     (int)decl->as.import_decl.module.span.length,
                     decl->as.import_decl.module.text);
    }
  }
  return 0;
}

typedef struct AstCycleFrame {
  uint32_t file_id;
  size_t next_edge;
} AstCycleFrame;

static int ast_front_detect_cycles(AstFrontend *f) {
  if (!f || f->file_count == 0u)
    return 0;

  size_t node_count = f->file_count;
  size_t edge_count = f->edge_count;
  size_t offsets_count = node_count + 1u;
  size_t offsets_bytes;
  size_t edges_bytes;
  size_t spans_bytes;
  size_t frames_bytes;
  if (ast_size_mul_overflow(offsets_count, sizeof(size_t), &offsets_bytes) ||
      ast_size_mul_overflow(edge_count, sizeof(uint32_t), &edges_bytes) ||
      ast_size_mul_overflow(edge_count, sizeof(AstSpan), &spans_bytes) ||
      ast_size_mul_overflow(node_count, sizeof(AstCycleFrame), &frames_bytes))
    return -1;

  uint8_t *color = (uint8_t *)calloc(node_count, sizeof(*color));
  size_t *offsets = (size_t *)calloc(offsets_count, sizeof(*offsets));
  uint32_t *neighbors = edge_count ? (uint32_t *)malloc(edges_bytes) : NULL;
  AstSpan *edge_spans = edge_count ? (AstSpan *)malloc(spans_bytes) : NULL;
  AstCycleFrame *stack = (AstCycleFrame *)malloc(frames_bytes);
  if (!color || !offsets || !stack ||
      (edge_count != 0u && (!neighbors || !edge_spans))) {
    free(color);
    free(offsets);
    free(neighbors);
    free(edge_spans);
    free(stack);
    return -1;
  }

  for (size_t i = 0; i < edge_count; ++i) {
    const AstImportEdge *e = &f->edges[i];
    if (e->from_file_id == 0u || e->to_file_id == 0u ||
        e->from_file_id > node_count || e->to_file_id > node_count)
      continue;
    offsets[e->from_file_id]++;
  }
  for (size_t i = 1u; i < offsets_count; ++i)
    offsets[i] += offsets[i - 1u];

  size_t *cursor = (size_t *)malloc(offsets_bytes);
  if (!cursor) {
    free(color);
    free(offsets);
    free(neighbors);
    free(edge_spans);
    free(stack);
    return -1;
  }
  memcpy(cursor, offsets, offsets_bytes);
  for (size_t i = 0; i < edge_count; ++i) {
    const AstImportEdge *e = &f->edges[i];
    if (e->from_file_id == 0u || e->to_file_id == 0u ||
        e->from_file_id > node_count || e->to_file_id > node_count)
      continue;
    size_t slot = cursor[e->from_file_id - 1u]++;
    neighbors[slot] = e->to_file_id;
    edge_spans[slot] = e->import_span;
  }
  free(cursor);

  for (size_t root_index = 0u; root_index < node_count; ++root_index) {
    if (color[root_index] != 0u)
      continue;
    uint32_t root = (uint32_t)root_index + 1u;
    size_t depth = 0u;
    color[root_index] = 1u;
    stack[depth++] = (AstCycleFrame){root, offsets[root_index]};

    while (depth != 0u) {
      AstCycleFrame *frame = &stack[depth - 1u];
      size_t end_edge = offsets[frame->file_id];
      if (frame->next_edge >= end_edge) {
        color[frame->file_id - 1u] = 2u;
        depth--;
        continue;
      }

      size_t edge_index = frame->next_edge++;
      uint32_t to_id = neighbors[edge_index];
      uint8_t to_color = color[to_id - 1u];
      if (to_color == 0u) {
        color[to_id - 1u] = 1u;
        stack[depth++] = (AstCycleFrame){to_id, offsets[to_id - 1u]};
        continue;
      }
      if (to_color == 1u) {
        AstFrontendRecord *from = ast_front_get_by_id(f, frame->file_id);
        AstFrontendRecord *target = ast_front_get_by_id(f, to_id);
        AstName target_name = target && target->parsed.module
                                  ? target->parsed.module->name
                                  : (AstName){0};
        const char *target_text =
            target_name.text ? target_name.text : "<unknown>";
        int target_len = target_name.text ? (int)target_name.span.length
                                          : (int)strlen(target_text);
        if (ast_front_diag(f, AST_FRONTEND_DIAG_IMPORT_CYCLE, AST_DIAG_ERROR,
                           edge_spans[edge_index], from ? from->path : NULL,
                           "import cycle reaches module '%.*s'", target_len,
                           target_text) != 0) {
          free(color);
          free(offsets);
          free(neighbors);
          free(edge_spans);
          free(stack);
          return -1;
        }
      }
    }
  }

  free(color);
  free(offsets);
  free(neighbors);
  free(edge_spans);
  free(stack);
  return 0;
}

int ast_frontend_load(AstFrontend *f, const char *const *entry_paths,
                      size_t entry_count) {
  if (!f || (!entry_paths && entry_count))
    return -1;
  AstFrontendRecord **frontier = NULL;
  size_t frontier_count = 0u, frontier_capacity = 0u;
  for (size_t i = 0; i < entry_count; ++i) {
    if (!entry_paths[i])
      continue;
    int inserted = 0;
    AstFrontendRecord *r = ast_front_add_path(f, entry_paths[i], &inserted);
    if (!r) {
      free(frontier);
      return -1;
    }
    if (inserted) {
      if (ast_front_reserve((void **)&frontier, sizeof(*frontier),
                            &frontier_capacity, frontier_count + 1u) != 0) {
        free(frontier);
        return -1;
      }
      frontier[frontier_count++] = r;
    }
  }

  while (frontier_count) {
    AstLoadJobs jobs = {f, frontier, frontier_count};
    ast_parallel_for(frontier_count, f->config.thread_count,
                     ast_front_load_worker, &jobs);
    if (ast_front_register_modules(f, frontier, frontier_count) != 0) {
      free(frontier);
      return -1;
    }

    AstFrontendRecord **next_frontier = NULL;
    size_t next_count = 0u;
    if (f->config.flags & AST_FRONTEND_RESOLVE_IMPORTS) {
      if (ast_front_collect_new_imports(f, frontier, frontier_count,
                                        &next_frontier, &next_count) != 0) {
        free(frontier);
        return -1;
      }
    }
    free(frontier);
    frontier = next_frontier;
    frontier_count = next_count;
    frontier_capacity = next_count;
  }
  free(frontier);

  if (f->config.flags & AST_FRONTEND_RESOLVE_IMPORTS) {
    if (ast_front_build_edges(f) != 0)
      return -1;
    if ((f->config.flags & AST_FRONTEND_DETECT_IMPORT_CYCLES) &&
        ast_front_detect_cycles(f) != 0)
      return -1;
  } else {
    f->edge_count = 0u;
  }
  AstFrontendStats stats = ast_frontend_stats(f);
  return (stats.lex_error_count || stats.parse_error_count ||
          stats.frontend_error_count)
             ? 1
             : 0;
}

size_t ast_frontend_file_count(const AstFrontend *f) {
  return f ? f->file_count : 0u;
}

int ast_frontend_get_file(const AstFrontend *f, size_t index,
                          AstFrontendFile *out) {
  if (!f || !out || index >= f->file_count)
    return 0;
  AstFrontendRecord *r = f->files[index];
  out->id = r->id;
  out->path = r->path;
  out->source = r->load_status == -1 ? NULL : &r->mapped.source;
  out->lexed = r->load_status <= -2 ? NULL : &r->lexed;
  out->parsed = r->load_status <= -3 ? NULL : &r->parsed;
  /* Every AST span, including literals outside macro calls, is relative to
     the parser's expanded buffer. Sema/HIR must read that same source. */
  const AstLexedFile *expanded = ast_parse_expanded_lexed(out->parsed);
  if (expanded) {
    out->lexed = expanded;
    out->source = &expanded->source;
  }
  return 1;
}

int ast_frontend_find_module(const AstFrontend *f, const char *module_name,
                             AstFrontendFile *out) {
  if (!f || !module_name || !out)
    return 0;
  uint32_t index;
  if (!ast_string_map_get(&f->module_map, module_name, &index))
    return 0;
  return ast_frontend_get_file(f, index, out);
}

const AstImportEdge *ast_frontend_import_edges(const AstFrontend *f,
                                               size_t *count_out) {
  if (count_out)
    *count_out = f ? f->edge_count : 0u;
  return f ? f->edges : NULL;
}

const AstFrontendDiagnostic *ast_frontend_diagnostics(const AstFrontend *f,
                                                      size_t *count_out) {
  if (count_out)
    *count_out = f ? f->diagnostic_count : 0u;
  return f ? f->diagnostics : NULL;
}

AstFrontendStats ast_frontend_stats(const AstFrontend *f) {
  AstFrontendStats s;
  memset(&s, 0, sizeof(s));
  if (!f)
    return s;
  s.file_count = f->file_count;
  s.import_edge_count = f->edge_count;
  s.module_count = f->module_map.count;
  for (size_t i = 0; i < f->file_count; ++i) {
    const AstFrontendRecord *r = f->files[i];
    if (r->load_status == -1)
      continue;
    s.source_bytes += r->mapped.source.size;
    s.token_count += r->lexed.token_count;
    s.lex_error_count += r->lexed.error_count;
    s.ast_nodes += r->parsed.stats.ast_nodes;
    s.parse_error_count += r->parsed.error_count;
  }
  for (size_t i = 0; i < f->diagnostic_count; ++i)
    if (f->diagnostics[i].severity == AST_DIAG_ERROR)
      s.frontend_error_count++;
  return s;
}

const char *ast_frontend_diag_code_name(AstFrontendDiagCode code) {
  switch (code) {
  case AST_FRONTEND_DIAG_NONE:
    return "none";
  case AST_FRONTEND_DIAG_IO:
    return "io";
  case AST_FRONTEND_DIAG_IMPORT_NOT_FOUND:
    return "import_not_found";
  case AST_FRONTEND_DIAG_DUPLICATE_MODULE:
    return "duplicate_module";
  case AST_FRONTEND_DIAG_MODULE_PATH_MISMATCH:
    return "module_path_mismatch";
  case AST_FRONTEND_DIAG_IMPORT_CYCLE:
    return "import_cycle";
  case AST_FRONTEND_DIAG_INTERNAL:
    return "internal";
  default:
    return "unknown";
  }
}
