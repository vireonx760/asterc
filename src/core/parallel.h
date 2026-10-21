#ifndef ASTERASM_PARALLEL_H
#define ASTERASM_PARALLEL_H

#include "asterasm/thread_pool.h"
#include <stddef.h>

typedef void (*AstParallelFn)(void *context, size_t index);
typedef int (*AstParallelCheckFn)(void *context, size_t index);
typedef void (*AstParallelWorkerFn)(void *context, size_t index,
                                    size_t worker_index);

size_t ast_hardware_thread_count(void);
/* File-based entry point allows namespace/ancestor quota integration tests. */
size_t ast_cpu_limit_from_cgroup_files(const char *mountinfo,
                                       const char *membership, size_t limit);
int ast_parallel_for(size_t count, size_t thread_count, AstParallelFn fn,
                     void *context);
/* Complete every independent item; report the first failure in source order. */
int ast_parallel_check(size_t count, size_t thread_count, AstParallelCheckFn fn,
                       void *context, size_t *first_failed);
int ast_parallel_for_workers(size_t count, size_t thread_count,
                             AstParallelWorkerFn fn, void *context,
                             size_t *workers_used_out);

#endif
