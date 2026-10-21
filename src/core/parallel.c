#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "parallel.h"
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#if defined(__linux__)
#include <sched.h>
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION PoolMutex;
typedef CONDITION_VARIABLE PoolCondition;
#define POOL_TLS __declspec(thread)
static int pool_mutex_init(PoolMutex *m) {
  return InitializeCriticalSectionEx(m, 0, 0) != 0;
}
static void pool_lock(PoolMutex *m) { EnterCriticalSection(m); }
static void pool_unlock(PoolMutex *m) { LeaveCriticalSection(m); }
static void pool_mutex_destroy(PoolMutex *m) { DeleteCriticalSection(m); }
static int pool_condition_init(PoolCondition *c) {
  InitializeConditionVariable(c);
  return 1;
}
static void pool_wait(PoolCondition *c, PoolMutex *m) {
  SleepConditionVariableCS(c, m, INFINITE);
}
static void pool_wake(PoolCondition *c) { WakeAllConditionVariable(c); }
static void pool_condition_destroy(PoolCondition *c) { (void)c; }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t PoolMutex;
typedef pthread_cond_t PoolCondition;
#define POOL_TLS _Thread_local
static int pool_mutex_init(PoolMutex *m) {
  return pthread_mutex_init(m, NULL) == 0;
}
static void pool_lock(PoolMutex *m) { pthread_mutex_lock(m); }
static void pool_unlock(PoolMutex *m) { pthread_mutex_unlock(m); }
static void pool_mutex_destroy(PoolMutex *m) { pthread_mutex_destroy(m); }
static int pool_condition_init(PoolCondition *c) {
  return pthread_cond_init(c, NULL) == 0;
}
static void pool_wait(PoolCondition *c, PoolMutex *m) {
  pthread_cond_wait(c, m);
}
static void pool_wake(PoolCondition *c) { pthread_cond_broadcast(c); }
static void pool_condition_destroy(PoolCondition *c) {
  pthread_cond_destroy(c);
}
#endif
typedef struct AstParallelJob {
  atomic_size_t next;
  size_t count, workers;
  AstParallelWorkerFn fn;
  void *context;
} AstParallelJob;
typedef struct PoolThread {
  struct PoolThread *next;
  AstThreadPool *pool;
  size_t index, generation;
#if defined(_WIN32)
  HANDLE handle;
#else
  pthread_t handle;
#endif
} PoolThread;
struct AstThreadPool {
  PoolMutex submit, state;
  PoolCondition wake, done;
  PoolThread *threads;
  AstParallelJob *job;
  size_t max_workers, helper_count, generation, remaining;
  int stop;
  AstThreadPoolStats stats;
};
static POOL_TLS AstThreadPool *bound_pool;
static POOL_TLS unsigned executing_job;

static int csv_has(const char *text, const char *word) {
  size_t n = strlen(word);
  while (text && *text) {
    const char *end = strchr(text, ',');
    size_t len = end ? (size_t)(end - text) : strlen(text);
    if (len == n && memcmp(text, word, n) == 0)
      return 1;
    text = end ? end + 1 : NULL;
  }
  return 0;
}
static void unescape_mount(char *s) {
  char *out = s;
  while (*s) {
    if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' &&
        s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
      *out++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + s[3] - '0');
      s += 4;
    } else
      *out++ = *s++;
  }
  *out = 0;
}
static unsigned long long read_quota_number(const char *s) {
  if (!s || *s < '0' || *s > '9')
    return 0;
  errno = 0;
  char *end;
  unsigned long long n = strtoull(s, &end, 10);
  return !errno && !*end ? n : 0;
}
static size_t directory_quota(const char *directory, int unified,
                              size_t limit) {
  char path[8192], quota[128] = {0}, period[128] = {0};
  int n = snprintf(path, sizeof(path), "%s/%s", directory,
                   unified ? "cpu.max" : "cpu.cfs_quota_us");
  if (n < 0 || (size_t)n >= sizeof(path))
    return limit;
  FILE *f = fopen(path, "r");
  if (!f)
    return limit;
  int fields = unified ? fscanf(f, "%127s %127s", quota, period)
                       : fscanf(f, "%127s", quota);
  fclose(f);
  if (fields != (unified ? 2 : 1))
    return limit;
  if (!unified) {
    n = snprintf(path, sizeof(path), "%s/cpu.cfs_period_us", directory);
    if (n < 0 || (size_t)n >= sizeof(path) || !(f = fopen(path, "r")))
      return limit;
    fields = fscanf(f, "%127s", period);
    fclose(f);
    if (fields != 1)
      return limit;
  }
  unsigned long long q = read_quota_number(quota),
                     p = read_quota_number(period);
  if (q && p) {
    unsigned long long cpus = q / p + (q % p != 0u);
    if (cpus < limit)
      limit = (size_t)cpus;
  }
  return limit;
}
size_t ast_cpu_limit_from_cgroup_files(const char *mountinfo,
                                       const char *membership, size_t limit) {
  char unified_group[4096] = {0}, cpu_group[4096] = {0}, line[16384];
  FILE *f = fopen(membership, "r");
  if (!f)
    return limit;
  while (fgets(line, sizeof(line), f)) {
    char *controllers = strchr(line, ':');
    if (!controllers)
      continue;
    char *group = strchr(++controllers, ':');
    if (!group)
      continue;
    *group++ = 0;
    group[strcspn(group, "\r\n")] = 0;
    char *destination = !*controllers                 ? unified_group
                        : csv_has(controllers, "cpu") ? cpu_group
                                                      : NULL;
    if (destination && strlen(group) < 4096u)
      strcpy(destination, group);
  }
  fclose(f);
  f = fopen(mountinfo, "r");
  if (!f)
    return limit;
  while (fgets(line, sizeof(line), f)) {
    char root[4096], mount[4096], type[32], options[512], path[8192];
    char *separator = strstr(line, " - ");
    if (!separator)
      continue;
    *separator = 0;
    if (sscanf(line, "%*u %*u %*s %4095s %4095s", root, mount) != 2 ||
        sscanf(separator + 3, "%31s %*s %511s", type, options) != 2)
      continue;
    int unified = strcmp(type, "cgroup2") == 0;
    if (!unified && (strcmp(type, "cgroup") != 0 || !csv_has(options, "cpu")))
      continue;
    const char *group = unified ? unified_group : cpu_group;
    if (!*group)
      continue;
    unescape_mount(root);
    unescape_mount(mount);
    size_t rn = strlen(root), mn = strlen(mount);
    while (mn > 1u && mount[mn - 1u] == '/')
      mount[--mn] = 0;
    const char *suffix = group;
    if (rn > 1u) {
      if (strncmp(group, root, rn) == 0 && (!group[rn] || group[rn] == '/'))
        suffix = group + rn;
      else if (strcmp(group, "/") == 0)
        suffix = "";
      else
        continue;
    }
    /* Namespace paths containing .. cannot escape the controller mount. */
    if (strstr(suffix, "/../") || strcmp(suffix, "/..") == 0 ||
        (strlen(suffix) >= 3u &&
         strcmp(suffix + strlen(suffix) - 3u, "/..") == 0))
      suffix = "";
    int n = snprintf(path, sizeof(path), "%s%s", mount, suffix);
    if (n < 0 || (size_t)n >= sizeof(path))
      continue;
    while (strlen(path) > mn && path[strlen(path) - 1u] == '/')
      path[strlen(path) - 1u] = 0;
    for (;;) {
      limit = directory_quota(path, unified, limit);
      if (strlen(path) <= mn)
        break;
      char *slash = strrchr(path, '/');
      if (!slash || (size_t)(slash - path) < mn)
        break;
      *slash = 0;
    }
  }
  fclose(f);
  return limit;
}
size_t ast_available_cpu_count(void) {
#if defined(_WIN32)
  size_t count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  DWORD_PTR process_mask = 0, system_mask = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask,
                             &system_mask) &&
      process_mask && process_mask != system_mask) {
    count = 0u;
    while (process_mask) {
      ++count;
      process_mask &= process_mask - 1u;
    }
  }
  typedef BOOL(WINAPI * CpuSetMasksFn)(HANDLE, PGROUP_AFFINITY, USHORT,
                                       PUSHORT);
  HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
  CpuSetMasksFn get_masks = (CpuSetMasksFn)(void (*)(void))GetProcAddress(
      kernel, "GetProcessDefaultCpuSetMasks");
  USHORT needed = 0;
  if (get_masks) {
    (void)get_masks(GetCurrentProcess(), NULL, 0, &needed);
    if (needed) {
      GROUP_AFFINITY *masks = calloc(needed, sizeof(*masks));
      if (masks && get_masks(GetCurrentProcess(), masks, needed, &needed)) {
        size_t selected = 0u;
        for (USHORT i = 0; i < needed; ++i) {
          KAFFINITY mask = masks[i].Mask;
          while (mask) {
            ++selected;
            mask &= mask - 1u;
          }
        }
        if (selected && selected < count)
          count = selected;
      }
      free(masks);
    }
  }
#else
  long online = sysconf(_SC_NPROCESSORS_ONLN);
  size_t count = online > 0 ? (size_t)online : 1u;
#if defined(__linux__)
  for (size_t bits = 128u; bits <= 1048576u; bits *= 2u) {
    size_t bytes = CPU_ALLOC_SIZE(bits);
    cpu_set_t *mask = CPU_ALLOC(bits);
    if (!mask)
      break;
    CPU_ZERO_S(bytes, mask);
    int rc = sched_getaffinity(0, bytes, mask), saved_errno = errno;
    if (rc == 0) {
      size_t allowed = (size_t)CPU_COUNT_S(bytes, mask);
      if (allowed && allowed < count)
        count = allowed;
    }
    CPU_FREE(mask);
    if (rc == 0 || saved_errno != EINVAL)
      break;
  }
  count = ast_cpu_limit_from_cgroup_files("/proc/self/mountinfo",
                                          "/proc/self/cgroup", count);
#endif
#endif
  return count ? count : 1u;
}
size_t ast_hardware_thread_count(void) { return ast_available_cpu_count(); }
AstThreadPool *ast_thread_pool_create(size_t max_workers) {
  AstThreadPool *p = calloc(1u, sizeof(*p));
  if (!p)
    return NULL;
  if (!pool_mutex_init(&p->submit))
    goto fail;
  if (!pool_mutex_init(&p->state))
    goto fail_submit;
  if (!pool_condition_init(&p->wake))
    goto fail_state;
  if (!pool_condition_init(&p->done))
    goto fail_wake;
  p->max_workers = max_workers;
  return p;
fail_wake:
  pool_condition_destroy(&p->wake);
fail_state:
  pool_mutex_destroy(&p->state);
fail_submit:
  pool_mutex_destroy(&p->submit);
fail:
  free(p);
  return NULL;
}
AstThreadPool *ast_thread_pool_bind(AstThreadPool *pool) {
  AstThreadPool *previous = bound_pool;
  bound_pool = pool;
  return previous;
}
static void run_job(AstParallelJob *job, size_t worker) {
  ++executing_job;
  for (;;) {
    size_t index =
        atomic_fetch_add_explicit(&job->next, 1u, memory_order_relaxed);
    if (index >= job->count)
      break;
    job->fn(job->context, index, worker);
  }
  --executing_job;
}
#if defined(_WIN32)
static DWORD WINAPI pool_entry(LPVOID opaque) {
#else
static void *pool_entry(void *opaque) {
#endif
  PoolThread *thread = opaque;
  AstThreadPool *p = thread->pool;
  bound_pool = p;
  size_t generation = thread->generation;
  pool_lock(&p->state);
  for (;;) {
    while (!p->stop && generation == p->generation)
      pool_wait(&p->wake, &p->state);
    if (p->stop)
      break;
    generation = p->generation;
    AstParallelJob *job = p->job;
    if (!job || thread->index >= job->workers)
      continue;
    pool_unlock(&p->state);
    run_job(job, thread->index);
    pool_lock(&p->state);
    if (--p->remaining == 0u)
      pool_wake(&p->done);
  }
  pool_unlock(&p->state);
#if defined(_WIN32)
  return 0;
#else
  return NULL;
#endif
}
static void grow_pool(AstThreadPool *p, size_t helpers) {
  while (p->helper_count < helpers) {
    PoolThread *thread = calloc(1u, sizeof(*thread));
    if (!thread)
      break;
    thread->pool = p;
    thread->index = p->helper_count + 1u;
    thread->generation = p->generation;
#if defined(_WIN32)
    thread->handle = CreateThread(NULL, 0, pool_entry, thread, 0, NULL);
    if (!thread->handle) {
      free(thread);
      break;
    }
#else
    if (pthread_create(&thread->handle, NULL, pool_entry, thread)) {
      free(thread);
      break;
    }
#endif
    thread->next = p->threads;
    p->threads = thread;
    ++p->helper_count;
    ++p->stats.helper_threads_created;
  }
}
AstThreadPoolStats ast_thread_pool_stats(AstThreadPool *p) {
  AstThreadPoolStats stats = {0};
  if (p) {
    pool_lock(&p->state);
    stats = p->stats;
    pool_unlock(&p->state);
  }
  return stats;
}
void ast_thread_pool_destroy(AstThreadPool *p) {
  if (!p)
    return;
  pool_lock(&p->submit);
  pool_lock(&p->state);
  p->stop = 1;
  pool_wake(&p->wake);
  pool_unlock(&p->state);
  while (p->threads) {
    PoolThread *thread = p->threads;
    p->threads = thread->next;
#if defined(_WIN32)
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
#else
    pthread_join(thread->handle, NULL);
#endif
    free(thread);
  }
  pool_unlock(&p->submit);
  pool_mutex_destroy(&p->submit);
  pool_mutex_destroy(&p->state);
  pool_condition_destroy(&p->wake);
  pool_condition_destroy(&p->done);
  free(p);
}
int ast_parallel_for_workers(size_t count, size_t thread_count,
                             AstParallelWorkerFn fn, void *context,
                             size_t *workers_used_out) {
  if (workers_used_out)
    *workers_used_out = 0u;
  if (!fn || !count)
    return 0;
  size_t workers = thread_count ? thread_count : ast_available_cpu_count();
  if (workers > count)
    workers = count;
  AstThreadPool *p = bound_pool;
  if (p) {
    size_t limit = p->max_workers ? p->max_workers : ast_available_cpu_count();
    if (workers > limit)
      workers = limit;
  }
  if (!workers || executing_job)
    workers = 1u;
  if (workers == 1u) {
    if (workers_used_out)
      *workers_used_out = 1u;
    int serialize = p && !executing_job;
    if (serialize) {
      pool_lock(&p->submit);
      pool_lock(&p->state);
      ++p->stats.jobs;
      if (!p->stats.peak_workers)
        p->stats.peak_workers = 1u;
      pool_unlock(&p->state);
    }
    ++executing_job;
    for (size_t i = 0u; i < count; ++i)
      fn(context, i, 0u);
    --executing_job;
    if (serialize)
      pool_unlock(&p->submit);
    return 0;
  }
  int owned = !p;
  if (!p)
    p = ast_thread_pool_create(workers);
  if (!p) {
    if (workers_used_out)
      *workers_used_out = 1u;
    ++executing_job;
    for (size_t i = 0u; i < count; ++i)
      fn(context, i, 0u);
    --executing_job;
    return 0;
  }
  pool_lock(&p->submit);
  pool_lock(&p->state);
  grow_pool(p, workers - 1u);
  if (workers > p->helper_count + 1u)
    workers = p->helper_count + 1u;
  AstParallelJob job;
  atomic_init(&job.next, 0u);
  job.count = count;
  job.workers = workers;
  job.fn = fn;
  job.context = context;
  p->job = &job;
  p->remaining = workers - 1u;
  ++p->generation;
  ++p->stats.jobs;
  if (p->stats.peak_workers < workers)
    p->stats.peak_workers = workers;
  pool_wake(&p->wake);
  pool_unlock(&p->state);
  run_job(&job, 0u);
  pool_lock(&p->state);
  while (p->remaining)
    pool_wait(&p->done, &p->state);
  p->job = NULL;
  pool_unlock(&p->state);
  pool_unlock(&p->submit);
  if (workers_used_out)
    *workers_used_out = workers;
  if (owned)
    ast_thread_pool_destroy(p);
  return 0;
}
typedef struct AstParallelCompatContext {
  AstParallelFn fn;
  void *context;
} AstParallelCompatContext;
static void compat_worker(void *context, size_t index, size_t worker) {
  AstParallelCompatContext *c = context;
  (void)worker;
  c->fn(c->context, index);
}
int ast_parallel_for(size_t count, size_t thread_count, AstParallelFn fn,
                     void *context) {
  if (!fn || !count)
    return 0;
  AstParallelCompatContext c = {fn, context};
  return ast_parallel_for_workers(count, thread_count, compat_worker, &c, NULL);
}

typedef struct ParallelCheck {
  AstParallelCheckFn fn;
  void *context;
  atomic_size_t first;
} ParallelCheck;
static void check_worker(void *opaque, size_t index) {
  ParallelCheck *c = opaque;
  if (!c->fn(c->context, index)) {
    size_t old = atomic_load_explicit(&c->first, memory_order_relaxed);
    while (index < old && !atomic_compare_exchange_weak_explicit(
                              &c->first, &old, index, memory_order_relaxed,
                              memory_order_relaxed)) {
    }
  }
}
int ast_parallel_check(size_t count, size_t threads, AstParallelCheckFn fn,
                       void *context, size_t *first_failed) {
  if (first_failed)
    *first_failed = SIZE_MAX;
  if (!fn)
    return 0;
  ParallelCheck c;
  c.fn = fn;
  c.context = context;
  atomic_init(&c.first, count);
  if (ast_parallel_for(count, threads, check_worker, &c))
    return 0;
  size_t first = atomic_load_explicit(&c.first, memory_order_relaxed);
  if (first_failed && first != count)
    *first_failed = first;
  return first == count;
}
