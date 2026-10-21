#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include "parallel.h"
#include <stdatomic.h>
#include <stdlib.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#if defined(__linux__)
#include <sched.h>
#endif
#endif

enum { ITEMS = 257, ROUNDS = 80 };
typedef struct Counter {
  atomic_uint visits[ITEMS];
  atomic_uint bad;
} Counter;
static void visit(void *opaque, size_t index, size_t worker) {
  Counter *c = opaque;
  if (index >= ITEMS || worker >= 8u) {
    atomic_store(&c->bad, 1u);
    return;
  }
  atomic_fetch_add(&c->visits[index], 1u);
}
static int rejects(void *context, size_t index) {
  (void)context;
  return index != 7u && index != 83u;
}
static void inner(void *opaque, size_t index, size_t worker) {
  Counter *c = opaque;
  if (worker)
    atomic_store(&c->bad, 1u);
  atomic_fetch_add(&c->visits[index], 1u);
}
static void nested(void *opaque, size_t index, size_t worker) {
  (void)index;
  (void)worker;
  ast_parallel_for_workers(7u, 8u, inner, opaque, NULL);
}
static int core_test(void) {
  AstThreadPool *pool = ast_thread_pool_create(8u);
  if (!pool)
    return 0;
  AstThreadPool *previous = ast_thread_pool_bind(pool);
  Counter c = {0};
  atomic_init(&c.bad, 0u);
  for (size_t i = 0; i < ITEMS; ++i)
    atomic_init(&c.visits[i], 0u);
  int ok = 1;
  for (unsigned round = 0; round < ROUNDS; ++round) {
    size_t used = 0u, count = round % 3u ? ITEMS : 1u;
    size_t threads = round % 4u == 0u ? 1u : round % 4u == 1u ? 3u : 96u;
    ast_parallel_for_workers(count, threads, visit, &c, &used);
    if (!used || used > 8u || used > count)
      ok = 0;
    for (size_t i = 0; i < ITEMS; ++i) {
      unsigned expected = i < count;
      if (atomic_exchange(&c.visits[i], 0u) != expected)
        ok = 0;
    }
  }
  size_t failed = SIZE_MAX;
  if (ast_parallel_check(ITEMS, 8u, rejects, NULL, &failed) || failed != 7u)
    ok = 0;
  ast_parallel_for_workers(16u, 8u, nested, &c, NULL);
  for (size_t i = 0; i < 7u; ++i)
    if (atomic_load(&c.visits[i]) != 16u)
      ok = 0;
  AstThreadPoolStats stats = ast_thread_pool_stats(pool);
  if (stats.helper_threads_created > 7u || stats.jobs != ROUNDS + 2u ||
      stats.peak_workers > 8u || atomic_load(&c.bad))
    ok = 0;
  ast_thread_pool_bind(previous);
  ast_thread_pool_destroy(pool);
  return ok;
}
typedef struct Submission {
  AstThreadPool *pool;
  atomic_uint calls;
} Submission;
static void submitted(void *opaque, size_t index, size_t worker) {
  (void)index;
  (void)worker;
  Submission *s = opaque;
  atomic_fetch_add(&s->calls, 1u);
}
static void submit_jobs(Submission *s) {
  AstThreadPool *previous = ast_thread_pool_bind(s->pool);
  for (unsigned i = 0; i < 40u; ++i)
    ast_parallel_for_workers(37u, 8u, submitted, s, NULL);
  ast_thread_pool_bind(previous);
}
#if defined(_WIN32)
static DWORD WINAPI submit_entry(LPVOID opaque) {
  submit_jobs(opaque);
  return 0;
}
#else
static void *submit_entry(void *opaque) {
  submit_jobs(opaque);
  return NULL;
}
#endif
static int concurrent_test(size_t workers) {
  Submission s = {0};
  atomic_init(&s.calls, 0u);
  s.pool = ast_thread_pool_create(workers);
  if (!s.pool)
    return 0;
#if defined(_WIN32)
  HANDLE thread = CreateThread(NULL, 0, submit_entry, &s, 0, NULL);
  if (!thread) {
    ast_thread_pool_destroy(s.pool);
    return 0;
  }
#else
  pthread_t thread;
  if (pthread_create(&thread, NULL, submit_entry, &s)) {
    ast_thread_pool_destroy(s.pool);
    return 0;
  }
#endif
  submit_jobs(&s);
#if defined(_WIN32)
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
#else
  pthread_join(thread, NULL);
#endif
  int ok = atomic_load(&s.calls) == 2u * 40u * 37u;
  ast_thread_pool_destroy(s.pool);
  return ok;
}
static int affinity_test(void) {
#if defined(_WIN32)
  DWORD_PTR old = 0, system = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &old, &system) || !old)
    return 0;
  DWORD_PTR one = old & (~old + 1u);
  if (!SetProcessAffinityMask(GetCurrentProcess(), one))
    return 0;
  int ok = ast_available_cpu_count() == 1u;
  if (!SetProcessAffinityMask(GetCurrentProcess(), old))
    return 0;
  return ok;
#elif defined(__linux__)
  size_t bits = 128u;
  cpu_set_t *old = NULL;
  for (; bits <= 1048576u; bits *= 2u) {
    old = CPU_ALLOC(bits);
    if (!old)
      return 0;
    if (sched_getaffinity(0, CPU_ALLOC_SIZE(bits), old) == 0)
      break;
    CPU_FREE(old);
    old = NULL;
  }
  if (!old)
    return 0;
  cpu_set_t *one = CPU_ALLOC(bits);
  if (!one) {
    CPU_FREE(old);
    return 0;
  }
  size_t bytes = CPU_ALLOC_SIZE(bits), cpu = 0u;
  while (cpu < bits && !CPU_ISSET_S(cpu, bytes, old))
    ++cpu;
  CPU_ZERO_S(bytes, one);
  CPU_SET_S(cpu, bytes, one);
  int ok =
      sched_setaffinity(0, bytes, one) == 0 && ast_available_cpu_count() == 1u;
  if (sched_setaffinity(0, bytes, old))
    ok = 0;
  CPU_FREE(old);
  CPU_FREE(one);
  return ok;
#else
  return 1;
#endif
}
int main(int argc, char **argv) {
  if (argc == 5 && strcmp(argv[1], "quota") == 0) {
    size_t expected = (size_t)strtoul(argv[4], NULL, 10);
    return ast_cpu_limit_from_cgroup_files(argv[2], argv[3], 32u) == expected
               ? 0
               : 1;
  }
  if (!core_test() || !concurrent_test(8u) || !concurrent_test(1u) ||
      !affinity_test()) {
    fputs("thread pool/affinity test failed\n", stderr);
    return 1;
  }
  puts("PASS: pool reuse, nested jobs, ordered errors, concurrent submitters, "
       "shutdown and CPU affinity");
  return 0;
}
