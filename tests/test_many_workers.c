#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "asterasm/lexer.h"
#include "parallel.h"
#include "test_process.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { WORKERS = 96, ITEMS = 384 };
typedef struct Jobs {
  atomic_uint visits[ITEMS];
  unsigned char started[WORKERS];
} Jobs;
static void work(void *opaque, size_t index, size_t worker) {
  Jobs *jobs = (Jobs *)opaque;
  if (worker >= WORKERS || index >= ITEMS)
    abort();
  if (!jobs->started[worker]) {
    jobs->started[worker] = 1u;
#if defined(_WIN32)
    Sleep(worker ? 200u : 1u);
#else
    struct timespec delay = {0, worker ? 200000000L : 1000000L};
    (void)nanosleep(&delay, NULL);
#endif
  }
  atomic_fetch_add_explicit(&jobs->visits[index], 1u, memory_order_relaxed);
}
static int core_case(void) {
  Jobs jobs = {0};
  for (size_t i = 0; i < ITEMS; ++i)
    atomic_init(&jobs.visits[i], 0u);
  size_t used = 0;
  if (ast_parallel_for_workers(ITEMS, WORKERS, work, &jobs, &used) || !used ||
      used > WORKERS)
    return 0;
  /* Every callback has finished when the API returns. In the old Windows
     >64 handle path sleeping callbacks still referenced freed job storage. */
  for (size_t i = 0; i < ITEMS; ++i)
    if (atomic_load_explicit(&jobs.visits[i], memory_order_relaxed) != 1u)
      return 0;
  return 1;
}
static int lexer_case(void) {
  static const char line[] = "let value: u64 = 0x1234_abcd + 7; // parallel\n";
  enum { REPEATS = 256, FILES = 128 };
  char *source = (char *)malloc((sizeof(line) - 1u) * REPEATS);
  AstSourceFile inputs[FILES];
  AstLexBatch batch = {0};
  AstLexedFile serial = {0};
  if (!source)
    return 0;
  for (size_t i = 0; i < REPEATS; ++i)
    memcpy(source + i * (sizeof(line) - 1u), line, sizeof(line) - 1u);
  for (size_t i = 0; i < FILES; ++i)
    inputs[i] = (AstSourceFile){"many-workers.aa", (const uint8_t *)source,
                                (sizeof(line) - 1u) * REPEATS};
  AstLexConfig config = ast_lex_config_default();
  int ok =
      ast_lex_source(&inputs[0], &config, &serial) == 0 &&
      ast_lex_sources_parallel(inputs, FILES, &config, WORKERS, &batch) == 0 &&
      batch.count == FILES;
  for (size_t f = 0; ok && f < batch.count; ++f) {
    if (batch.files[f].error_count ||
        batch.files[f].token_count != serial.token_count) {
      ok = 0;
      break;
    }
    for (size_t i = 0; i < serial.token_count; ++i) {
      const AstToken *a = ast_token_at(&serial, i),
                     *b = ast_token_at(&batch.files[f], i);
      if (!a || !b || a->kind != b->kind || a->offset != b->offset ||
          a->length != b->length || a->line != b->line ||
          a->column != b->column) {
        ok = 0;
        break;
      }
    }
  }
  ast_lex_batch_destroy(&batch);
  ast_lexed_file_destroy(&serial);
  free(source);
  return ok;
}
int main(int argc, char **argv) {
  ast_test_process_setup();
  if (argc != 2)
    return 2;
  int ok = !strcmp(argv[1], "core")    ? core_case()
           : !strcmp(argv[1], "lexer") ? lexer_case()
                                       : 0;
  if (!ok)
    fputs("96-worker completion regression failed\n", stderr);
  return ok ? 0 : 1;
}
