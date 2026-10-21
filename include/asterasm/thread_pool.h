#ifndef ASTERASM_THREAD_POOL_H
#define ASTERASM_THREAD_POOL_H
#include "common.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AstThreadPool AstThreadPool;
typedef struct AstThreadPoolStats {
  size_t helper_threads_created;
  size_t jobs;
  size_t peak_workers;
} AstThreadPoolStats;
/* Zero selects the available CPU budget. Helpers are created lazily and reused.
   Binding is local to the calling thread; restore the previous binding before
   destroying a pool. Submissions are synchronous; recursive jobs run serially.
   The owner must join submitting threads before destroying the pool. */
ASTER_API AstThreadPool *ast_thread_pool_create(size_t max_workers);
ASTER_API AstThreadPool *ast_thread_pool_bind(AstThreadPool *pool);
ASTER_API AstThreadPoolStats ast_thread_pool_stats(AstThreadPool *pool);
ASTER_API void ast_thread_pool_destroy(AstThreadPool *pool);
ASTER_API size_t ast_available_cpu_count(void);
#ifdef __cplusplus
}
#endif
#endif
