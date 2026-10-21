/* Mixed Aster/C atomics and release/acquire publication on real host threads.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#define THREAD_RETURN DWORD WINAPI
#define THREAD_DONE return 0
#else
#include <pthread.h>
#define THREAD_RETURN void *
#define THREAD_DONE return NULL
#endif
extern uint64_t ast_add(_Atomic uint64_t *);
extern bool ast_cas(_Atomic uint64_t *, uint64_t, uint64_t);
extern uint64_t ast_load(_Atomic uint64_t *);
extern void ast_store(_Atomic uint64_t *, uint64_t);
static _Atomic uint64_t counter, cas_counter, flag;
static _Atomic unsigned ready, start, errors;
static uint64_t payload;
enum { WORKERS = 8, ITERATIONS = 20000 };
static THREAD_RETURN worker(void *argument) {
  uintptr_t id = (uintptr_t)argument;
  atomic_fetch_add_explicit(&ready, 1, memory_order_release);
  while (!atomic_load_explicit(&start, memory_order_acquire)) {
  }
  for (unsigned j = 0; j < ITERATIONS; ++j) {
    if (id & 1)
      atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
    else
      ast_add(&counter);
    uint64_t old = ast_load(&cas_counter);
    while (!ast_cas(&cas_counter, old, old + 1))
      old = ast_load(&cas_counter);
  }
  THREAD_DONE;
}
static THREAD_RETURN consumer(void *unused) {
  (void)unused;
  for (unsigned j = 1; j <= ITERATIONS; ++j) {
    while (ast_load(&flag) != 1) {
    }
    if (payload != (uint64_t)j * 17)
      atomic_fetch_add(&errors, 1);
    ast_store(&flag, 0);
  }
  THREAD_DONE;
}
int main(void) {
#ifdef _WIN32
  HANDLE threads[WORKERS];
  for (uintptr_t j = 0; j < WORKERS; ++j) {
    threads[j] = CreateThread(NULL, 0, worker, (void *)j, 0, NULL);
    if (!threads[j])
      return 2;
  }
#else
  pthread_t threads[WORKERS];
  for (uintptr_t j = 0; j < WORKERS; ++j)
    if (pthread_create(&threads[j], NULL, worker, (void *)j))
      return 2;
#endif
  while (atomic_load_explicit(&ready, memory_order_acquire) != WORKERS) {
  }
  atomic_store_explicit(&start, 1, memory_order_release);
#ifdef _WIN32
  if (WaitForMultipleObjects(WORKERS, threads, TRUE, 60000) != WAIT_OBJECT_0)
    return 3;
  for (unsigned j = 0; j < WORKERS; ++j)
    CloseHandle(threads[j]);
  HANDLE reader = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
  if (!reader)
    return 2;
#else
  for (unsigned j = 0; j < WORKERS; ++j)
    pthread_join(threads[j], NULL);
  pthread_t reader;
  if (pthread_create(&reader, NULL, consumer, NULL))
    return 2;
#endif
  for (unsigned j = 1; j <= ITERATIONS; ++j) {
    while (ast_load(&flag) != 0) {
    }
    payload = (uint64_t)j * 17;
    ast_store(&flag, 1);
  }
#ifdef _WIN32
  if (WaitForSingleObject(reader, 60000) != WAIT_OBJECT_0)
    return 3;
  CloseHandle(reader);
#else
  pthread_join(reader, NULL);
#endif
  if (counter != WORKERS * ITERATIONS || cas_counter != WORKERS * ITERATIONS ||
      errors) {
    fprintf(stderr, "counter=%llu cas=%llu errors=%u\n",
            (unsigned long long)counter, (unsigned long long)cas_counter,
            (unsigned)errors);
    return 1;
  }
  puts("PASS: 160000 mixed C/Aster increments, 160000 contended CAS "
       "increments, 20000 publications");
  return 0;
}
