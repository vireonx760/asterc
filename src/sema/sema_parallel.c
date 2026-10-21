#include "sema_internal.h"

typedef struct AstSemaBodyJob {
  size_t module_index;
  size_t symbol_index;
} AstSemaBodyJob;

typedef struct AstSemaWorker {
  AstSema sema;
  int initialized;
} AstSemaWorker;

typedef struct AstSemaParallelBodyContext {
  AstSema *parent;
  AstSemaBodyJob *jobs;
  AstSemaWorker *workers;
  size_t worker_count;
  size_t job_count;
} AstSemaParallelBodyContext;

static int worker_clone_modules(AstSema *worker, const AstSema *parent) {
  worker->module_count = parent->module_count;
  if (parent->module_count == 0u)
    return 0;
  worker->modules =
      (AstSemaModule *)calloc(parent->module_count, sizeof(*worker->modules));
  if (!worker->modules)
    return -1;

  for (size_t i = 0u; i < parent->module_count; ++i) {
    const AstSemaModule *src = &parent->modules[i];
    AstSemaModule *dst = &worker->modules[i];
    *dst = *src;
    dst->symbols = NULL;
    dst->imports = NULL;
    if (src->symbol_count) {
      dst->symbols =
          (AstSemaSymbol *)malloc(src->symbol_count * sizeof(*dst->symbols));
      if (!dst->symbols)
        return -1;
      memcpy(dst->symbols, src->symbols,
             src->symbol_count * sizeof(*dst->symbols));
      dst->symbol_capacity = src->symbol_count;
    }
    if (src->import_count) {
      dst->imports =
          (AstSemaImport *)malloc(src->import_count * sizeof(*dst->imports));
      if (!dst->imports)
        return -1;
      memcpy(dst->imports, src->imports,
             src->import_count * sizeof(*dst->imports));
      dst->import_capacity = src->import_count;
    }
  }

  for (size_t i = 0u; i < parent->module_count; ++i) {
    const AstSemaModule *src = &parent->modules[i];
    AstSemaModule *dst = &worker->modules[i];
    for (size_t j = 0u; j < src->import_count; ++j) {
      const AstSemaModule *target = src->imports[j].target;
      if (!target) {
        dst->imports[j].target = NULL;
        continue;
      }
      size_t target_index = target->index;
      dst->imports[j].target = target_index < worker->module_count
                                   ? &worker->modules[target_index]
                                   : NULL;
    }
  }
  return 0;
}

static int worker_init_fact_files(AstSema *worker, const AstSema *parent) {
  worker->fact_file_count = parent->fact_file_count;
  if (parent->fact_file_count == 0u)
    return 0;
  worker->fact_files = (AstSemaFactFile *)calloc(parent->fact_file_count,
                                                 sizeof(*worker->fact_files));
  if (!worker->fact_files)
    return -1;
  worker->owns_fact_files = 1u;
  worker->owns_expr_fact_storage = 0u;
  worker->owns_type_fact_storage = 1u;
  for (size_t i = 0u; i < parent->fact_file_count; ++i) {
    worker->fact_files[i].expr_facts = parent->fact_files[i].expr_facts;
    worker->fact_files[i].expr_capacity = parent->fact_files[i].expr_capacity;
    worker->fact_files[i].type_capacity = parent->fact_files[i].type_capacity;
    if (worker->fact_files[i].type_capacity) {
      worker->fact_files[i].type_facts = (AstSemaTypeFactSlot *)calloc(
          worker->fact_files[i].type_capacity,
          sizeof(*worker->fact_files[i].type_facts));
      if (!worker->fact_files[i].type_facts)
        return -1;
    }
  }
  return 0;
}

static void worker_destroy(AstSemaWorker *worker) {
  if (!worker || !worker->initialized)
    return;
  AstSema *s = &worker->sema;
  if (s->modules) {
    for (size_t i = 0u; i < s->module_count; ++i) {
      free(s->modules[i].symbols);
      free(s->modules[i].imports);
    }
    free(s->modules);
  }
  for (size_t i = 0u; i < s->diagnostic_count; ++i)
    free((void *)s->diagnostics[i].message);
  free(s->diagnostics);
  sema_destroy_fact_files(s);
  ast_arena_destroy(&s->arena);
  memset(worker, 0, sizeof(*worker));
}

static int worker_init(AstSemaWorker *worker, const AstSema *parent) {
  memset(worker, 0, sizeof(*worker));
  AstSema *s = &worker->sema;
  s->frontend = parent->frontend;
  s->config = parent->config;
  s->config.thread_count = 1u;
  s->module_map = parent->module_map;
  s->builtin_view =
      parent->builtin_view ? parent->builtin_view : parent->builtin;
  ast_arena_init(&s->arena, s->config.arena_block_size);
  worker->initialized = 1;
  if (worker_clone_modules(s, parent) != 0 ||
      worker_init_fact_files(s, parent) != 0) {
    worker_destroy(worker);
    return -1;
  }
  return 0;
}

static void body_worker_partition(void *context, size_t partition_index,
                                  size_t worker_index) {
  AstSemaParallelBodyContext *ctx = (AstSemaParallelBodyContext *)context;
  if (!ctx || worker_index >= ctx->worker_count ||
      partition_index >= ctx->worker_count)
    return;
  AstSemaWorker *worker = &ctx->workers[worker_index];
  if (!worker->initialized || worker->sema.oom)
    return;

  size_t begin = (partition_index * ctx->job_count) / ctx->worker_count;
  size_t end = ((partition_index + 1u) * ctx->job_count) / ctx->worker_count;
  for (size_t index = begin; index < end; ++index) {
    AstSemaBodyJob job = ctx->jobs[index];
    if (job.module_index >= worker->sema.module_count) {
      worker->sema.oom = 1u;
      return;
    }
    AstSemaModule *module = &worker->sema.modules[job.module_index];
    if (job.symbol_index >= module->symbol_count) {
      worker->sema.oom = 1u;
      return;
    }
    sema_check_function_body(&worker->sema, module,
                             &module->symbols[job.symbol_index]);
    if (worker->sema.oom)
      return;
  }
}

static int reserve_diagnostics(AstSema *s, size_t need) {
  if (s->diagnostic_capacity >= need)
    return 0;
  size_t cap = s->diagnostic_capacity ? s->diagnostic_capacity : 64u;
  while (cap < need) {
    if (cap > SIZE_MAX / 2u)
      return -1;
    cap *= 2u;
  }
  AstSemaDiagnostic *fresh =
      (AstSemaDiagnostic *)realloc(s->diagnostics, cap * sizeof(*fresh));
  if (!fresh)
    return -1;
  s->diagnostics = fresh;
  s->diagnostic_capacity = cap;
  return 0;
}

static void merge_worker(AstSema *parent, AstSemaWorker *worker) {
  AstSema *s = &worker->sema;
  if (s->oom)
    parent->oom = 1u;
  parent->stats.expression_count += s->stats.expression_count;
  parent->stats.statement_count += s->stats.statement_count;
  parent->stats.constant_expression_count += s->stats.constant_expression_count;
  parent->stats.type_count += s->stats.type_count;
  parent->stats.error_count += s->stats.error_count;
  parent->stats.warning_count += s->stats.warning_count;

  parent->expr_fact_count += s->expr_fact_count;
  if (sema_merge_type_facts(parent, s) != 0)
    parent->oom = 1u;
  ast_arena_absorb(&parent->arena, &s->arena);

  if (s->diagnostic_count == 0u)
    return;
  size_t need = parent->diagnostic_count + s->diagnostic_count;
  if (reserve_diagnostics(parent, need) != 0) {
    parent->oom = 1u;
    return;
  }
  memcpy(parent->diagnostics + parent->diagnostic_count, s->diagnostics,
         s->diagnostic_count * sizeof(*s->diagnostics));
  parent->diagnostic_count = need;
  for (size_t i = 0u; i < s->diagnostic_count; ++i)
    s->diagnostics[i].message = NULL;
  s->diagnostic_count = 0u;
}

int sema_check_functions_parallel(AstSema *s, const size_t *module_indices,
                                  const size_t *symbol_indices, size_t count) {
  if (!s || !module_indices || !symbol_indices || count == 0u)
    return 0;
  size_t workers = s->config.thread_count ? s->config.thread_count
                                          : ast_hardware_thread_count();
  if (workers > count)
    workers = count;
  if (workers <= 1u) {
    for (size_t i = 0u; i < count; ++i) {
      sema_check_function_body(
          s, &s->modules[module_indices[i]],
          &s->modules[module_indices[i]].symbols[symbol_indices[i]]);
    }
    return s->oom ? -1 : 0;
  }

  AstSemaBodyJob *jobs = (AstSemaBodyJob *)malloc(count * sizeof(*jobs));
  AstSemaWorker *worker_states =
      (AstSemaWorker *)calloc(workers, sizeof(*worker_states));
  if (!jobs || !worker_states) {
    free(jobs);
    free(worker_states);
    s->oom = 1u;
    return -1;
  }
  for (size_t i = 0u; i < count; ++i) {
    jobs[i].module_index = module_indices[i];
    jobs[i].symbol_index = symbol_indices[i];
  }
  for (size_t i = 0u; i < workers; ++i) {
    if (worker_init(&worker_states[i], s) != 0) {
      s->oom = 1u;
      for (size_t j = 0u; j <= i; ++j)
        worker_destroy(&worker_states[j]);
      free(worker_states);
      free(jobs);
      return -1;
    }
  }

  AstSemaParallelBodyContext context;
  context.parent = s;
  context.jobs = jobs;
  context.workers = worker_states;
  context.worker_count = workers;
  context.job_count = count;
  size_t workers_used = 0u;
  (void)ast_parallel_for_workers(workers, workers, body_worker_partition,
                                 &context, &workers_used);
  if (workers_used < workers)
    workers = workers_used;

  for (size_t i = 0u; i < workers; ++i)
    merge_worker(s, &worker_states[i]);
  for (size_t i = 0u; i < context.worker_count; ++i)
    worker_destroy(&worker_states[i]);
  free(worker_states);
  free(jobs);
  return s->oom ? -1 : 0;
}
