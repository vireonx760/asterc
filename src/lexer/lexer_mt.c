#include "asterasm/lexer.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct AstMappedFile {
  const uint8_t *data;
  size_t size;
  char *path;
#if defined(_WIN32)
  HANDLE file_handle;
  HANDLE mapping_handle;
#endif
} AstMappedFile;

typedef struct AstBatchInternal {
  AstMappedFile *mappings;
  size_t mapping_count;
} AstBatchInternal;

typedef struct AstWorkerShared {
  const AstSourceFile *sources;
  AstLexedFile *outputs;
  size_t count;
  AstLexConfig config;
  atomic_size_t next_index;
  atomic_int failed;
} AstWorkerShared;

static char *ast_strdup_local(const char *s) {
  if (!s)
    return NULL;
  size_t n = strlen(s);
  char *copy = (char *)malloc(n + 1u);
  if (!copy)
    return NULL;
  memcpy(copy, s, n + 1u);
  return copy;
}

static size_t ast_default_thread_count(void) {
#if defined(_WIN32)
  DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  return count ? (size_t)count : 1u;
#else
  long count = sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? (size_t)count : 1u;
#endif
}

static int ast_map_file(const char *path, AstMappedFile *out) {
  memset(out, 0, sizeof(*out));
  out->path = ast_strdup_local(path);
  if (!out->path)
    return -1;

#if defined(_WIN32)
  out->file_handle = INVALID_HANDLE_VALUE;
  out->mapping_handle = NULL;

  HANDLE file = CreateFileA(
      path, GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (file == INVALID_HANDLE_VALUE)
    return -1;

  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
      (uint64_t)size.QuadPart > (uint64_t)SIZE_MAX) {
    CloseHandle(file);
    return -1;
  }

  out->file_handle = file;
  out->size = (size_t)size.QuadPart;
  if (out->size == 0)
    return 0;

  HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
  if (!mapping)
    return -1;
  out->mapping_handle = mapping;

  void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (!view)
    return -1;
  out->data = (const uint8_t *)view;
  return 0;
#else
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;

  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 0 ||
      (uintmax_t)st.st_size > (uintmax_t)SIZE_MAX) {
    close(fd);
    return -1;
  }

  out->size = (size_t)st.st_size;
  if (out->size == 0) {
    close(fd);
    return 0;
  }

  void *view = mmap(NULL, out->size, PROT_READ, MAP_PRIVATE, fd, 0);
  int saved_errno = errno;
  close(fd);
  if (view == MAP_FAILED) {
    errno = saved_errno;
    return -1;
  }
  out->data = (const uint8_t *)view;
  return 0;
#endif
}

static void ast_unmap_file(AstMappedFile *file) {
  if (!file)
    return;
#if defined(_WIN32)
  if (file->data)
    UnmapViewOfFile(file->data);
  if (file->mapping_handle)
    CloseHandle(file->mapping_handle);
  if (file->file_handle && file->file_handle != INVALID_HANDLE_VALUE)
    CloseHandle(file->file_handle);
#else
  if (file->data && file->size)
    munmap((void *)file->data, file->size);
#endif
  free(file->path);
  memset(file, 0, sizeof(*file));
}

static void ast_worker_run(AstWorkerShared *shared) {
  for (;;) {
    size_t index = atomic_fetch_add_explicit(&shared->next_index, 1u,
                                             memory_order_relaxed);
    if (index >= shared->count)
      return;
    if (ast_lex_source(&shared->sources[index], &shared->config,
                       &shared->outputs[index]) != 0) {
      atomic_store_explicit(&shared->failed, 1, memory_order_relaxed);
    }
  }
}

#if defined(_WIN32)
static DWORD WINAPI ast_worker_entry(LPVOID arg) {
  ast_worker_run((AstWorkerShared *)arg);
  return 0;
}
#else
static void *ast_worker_entry(void *arg) {
  ast_worker_run((AstWorkerShared *)arg);
  return NULL;
}
#endif

static int ast_run_parallel(const AstSourceFile *sources, size_t count,
                            const AstLexConfig *config, size_t thread_count,
                            AstLexedFile *outputs) {
  if (count == 0)
    return 0;
  AstWorkerShared shared;
  memset(&shared, 0, sizeof(shared));
  shared.sources = sources;
  shared.outputs = outputs;
  shared.count = count;
  shared.config = config ? *config : ast_lex_config_default();
  atomic_init(&shared.next_index, 0u);
  atomic_init(&shared.failed, 0);

  if (thread_count == 0)
    thread_count = ast_default_thread_count();
  if (thread_count > count)
    thread_count = count;
  if (thread_count <= 1u) {
    ast_worker_run(&shared);
    return atomic_load_explicit(&shared.failed, memory_order_relaxed) ? -1 : 0;
  }

  size_t helper_count = thread_count - 1u;
#if defined(_WIN32)
  HANDLE *threads = (HANDLE *)calloc(helper_count, sizeof(*threads));
  if (!threads) {
    ast_worker_run(&shared);
    return atomic_load_explicit(&shared.failed, memory_order_relaxed) ? -1 : 0;
  }
  size_t created = 0;
  for (; created < helper_count; ++created) {
    threads[created] =
        CreateThread(NULL, 0, ast_worker_entry, &shared, 0, NULL);
    if (!threads[created])
      break;
  }
  ast_worker_run(&shared);
  for (size_t i = 0u; i < created; ++i)
    WaitForSingleObject(threads[i], INFINITE);
  for (size_t i = 0; i < created; ++i)
    CloseHandle(threads[i]);
  free(threads);
#else
  pthread_t *threads = (pthread_t *)calloc(helper_count, sizeof(*threads));
  if (!threads) {
    ast_worker_run(&shared);
    return atomic_load_explicit(&shared.failed, memory_order_relaxed) ? -1 : 0;
  }
  size_t created = 0;
  for (; created < helper_count; ++created) {
    if (pthread_create(&threads[created], NULL, ast_worker_entry, &shared) != 0)
      break;
  }
  ast_worker_run(&shared);
  for (size_t i = 0; i < created; ++i)
    pthread_join(threads[i], NULL);
  free(threads);
#endif
  return atomic_load_explicit(&shared.failed, memory_order_relaxed) ? -1 : 0;
}

int ast_lex_sources_parallel(const AstSourceFile *sources, size_t count,
                             const AstLexConfig *config, size_t thread_count,
                             AstLexBatch *out) {
  if (!out || (!sources && count != 0))
    return -1;
  memset(out, 0, sizeof(*out));
  if (count == 0)
    return 0;

  AstLexedFile *files = (AstLexedFile *)calloc(count, sizeof(*files));
  if (!files)
    return -1;
  out->files = files;
  out->count = count;

  int result = ast_run_parallel(sources, count, config, thread_count, files);
  if (result != 0)
    return -1;
  return 0;
}

int ast_lex_paths_parallel(const char *const *paths, size_t count,
                           const AstLexConfig *config, size_t thread_count,
                           AstLexBatch *out) {
  if (!out || (!paths && count != 0))
    return -1;
  memset(out, 0, sizeof(*out));
  if (count == 0)
    return 0;

  AstBatchInternal *internal = (AstBatchInternal *)calloc(1, sizeof(*internal));
  AstLexedFile *files = (AstLexedFile *)calloc(count, sizeof(*files));
  AstMappedFile *mappings = (AstMappedFile *)calloc(count, sizeof(*mappings));
  AstSourceFile *sources = (AstSourceFile *)calloc(count, sizeof(*sources));
  if (!internal || !files || !mappings || !sources) {
    free(internal);
    free(files);
    free(mappings);
    free(sources);
    return -1;
  }

  internal->mappings = mappings;
  internal->mapping_count = count;
  out->files = files;
  out->count = count;
  out->internal = internal;

  for (size_t i = 0; i < count; ++i) {
    if (!paths[i] || ast_map_file(paths[i], &mappings[i]) != 0) {
      free(sources);
      ast_lex_batch_destroy(out);
      return -1;
    }
    sources[i].path = mappings[i].path;
    sources[i].data = mappings[i].data;
    sources[i].size = mappings[i].size;
  }

  int result = ast_run_parallel(sources, count, config, thread_count, files);
  free(sources);
  if (result != 0)
    return -1;
  return 0;
}

void ast_lex_batch_destroy(AstLexBatch *batch) {
  if (!batch)
    return;
  if (batch->files) {
    for (size_t i = 0; i < batch->count; ++i)
      ast_lexed_file_destroy(&batch->files[i]);
    free(batch->files);
  }
  AstBatchInternal *internal = (AstBatchInternal *)batch->internal;
  if (internal) {
    for (size_t i = 0; i < internal->mapping_count; ++i)
      ast_unmap_file(&internal->mappings[i]);
    free(internal->mappings);
    free(internal);
  }
  memset(batch, 0, sizeof(*batch));
}
