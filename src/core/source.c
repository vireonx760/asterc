#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#endif
#include "asterasm/source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct AstMappedSourceInternal {
  char *path;
#if defined(_WIN32)
  HANDLE file;
  HANDLE mapping;
#else
  size_t mapped_size;
#endif
} AstMappedSourceInternal;

static char *ast_heap_strdup(const char *text) {
  if (!text)
    return NULL;
  size_t len = strlen(text);
  char *copy = (char *)malloc(len + 1u);
  if (!copy)
    return NULL;
  memcpy(copy, text, len + 1u);
  return copy;
}

int ast_source_map_file(const char *path, AstMappedSource *out) {
  if (!path || !out)
    return -1;
  memset(out, 0, sizeof(*out));
  AstMappedSourceInternal *in =
      (AstMappedSourceInternal *)calloc(1u, sizeof(*in));
  if (!in)
    return -1;
  in->path = ast_heap_strdup(path);
  if (!in->path) {
    free(in);
    return -1;
  }
#if defined(_WIN32)
  in->file = CreateFileA(
      path, GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (in->file == INVALID_HANDLE_VALUE) {
    free(in->path);
    free(in);
    return -1;
  }
  LARGE_INTEGER li;
  if (!GetFileSizeEx(in->file, &li) || li.QuadPart < 0 ||
      (uint64_t)li.QuadPart > (uint64_t)SIZE_MAX) {
    CloseHandle(in->file);
    free(in->path);
    free(in);
    return -1;
  }
  out->source.size = (size_t)li.QuadPart;
  if (out->source.size) {
    in->mapping = CreateFileMappingA(in->file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!in->mapping) {
      CloseHandle(in->file);
      free(in->path);
      free(in);
      return -1;
    }
    out->source.data =
        (const uint8_t *)MapViewOfFile(in->mapping, FILE_MAP_READ, 0, 0, 0);
    if (!out->source.data) {
      CloseHandle(in->mapping);
      CloseHandle(in->file);
      free(in->path);
      free(in);
      return -1;
    }
  }
#else
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    free(in->path);
    free(in);
    return -1;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 0 ||
      (uintmax_t)st.st_size > (uintmax_t)SIZE_MAX) {
    close(fd);
    free(in->path);
    free(in);
    return -1;
  }
  out->source.size = (size_t)st.st_size;
  in->mapped_size = out->source.size;
  if (out->source.size) {
    void *view = mmap(NULL, out->source.size, PROT_READ, MAP_PRIVATE, fd, 0);
    int saved = errno;
    close(fd);
    if (view == MAP_FAILED) {
      errno = saved;
      free(in->path);
      free(in);
      return -1;
    }
    out->source.data = (const uint8_t *)view;
  } else {
    close(fd);
  }
#endif
  out->source.path = in->path;
  out->internal = in;
  return 0;
}

void ast_source_unmap(AstMappedSource *source) {
  if (!source)
    return;
  AstMappedSourceInternal *in = (AstMappedSourceInternal *)source->internal;
  if (in) {
#if defined(_WIN32)
    if (source->source.data)
      UnmapViewOfFile(source->source.data);
    if (in->mapping)
      CloseHandle(in->mapping);
    if (in->file && in->file != INVALID_HANDLE_VALUE)
      CloseHandle(in->file);
#else
    if (source->source.data && in->mapped_size)
      munmap((void *)source->source.data, in->mapped_size);
#endif
    free(in->path);
    free(in);
  }
  memset(source, 0, sizeof(*source));
}

char *ast_path_canonicalize(const char *path) {
  if (!path)
    return NULL;
#if defined(_WIN32)
  DWORD need = GetFullPathNameA(path, 0, NULL, NULL);
  if (!need)
    return ast_heap_strdup(path);
  char *buf = (char *)malloc((size_t)need + 1u);
  if (!buf)
    return NULL;
  DWORD got = GetFullPathNameA(path, need, buf, NULL);
  if (!got || got >= need) {
    free(buf);
    return ast_heap_strdup(path);
  }
  for (char *p = buf; *p; ++p)
    if (*p == '\\')
      *p = '/';
  return buf;
#else
  char *resolved = realpath(path, NULL);
  if (resolved)
    return resolved;
  return ast_heap_strdup(path);
#endif
}

char *ast_path_module_candidate(const char *root, const char *module_name,
                                const char *extension) {
  if (!root || !module_name)
    return NULL;
  if (!extension)
    extension = ".aa";
  size_t root_len = strlen(root);
  size_t mod_len = strlen(module_name);
  size_t ext_len = strlen(extension);
  size_t total = root_len + 1u + mod_len + ext_len + 1u;
  char *out = (char *)malloc(total);
  if (!out)
    return NULL;
  size_t at = 0u;
  memcpy(out + at, root, root_len);
  at += root_len;
  if (at && out[at - 1u] != '/' && out[at - 1u] != '\\')
    out[at++] = '/';
  for (size_t i = 0; i < mod_len; ++i) {
    char c = module_name[i];
    out[at++] = (c == '.') ? '/' : c;
  }
  memcpy(out + at, extension, ext_len);
  at += ext_len;
  out[at] = '\0';
  return out;
}

int ast_path_exists_regular(const char *path) {
  if (!path)
    return 0;
#if defined(_WIN32)
  DWORD attr = GetFileAttributesA(path);
  return attr != INVALID_FILE_ATTRIBUTES &&
         (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}
