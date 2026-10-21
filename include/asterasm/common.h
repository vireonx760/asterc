#ifndef ASTERASM_COMMON_H
#define ASTERASM_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* MSVC's C17 headers do not provide max_align_t in C mode. */
#if defined(_MSC_VER) && !defined(__clang__)
typedef __declspec(align(16)) struct AstMaxAlign {
  unsigned char storage[16];
} AstMaxAlign;
#else
typedef max_align_t AstMaxAlign;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(ASTERASM_SHARED)
#if defined(ASTERASM_BUILDING_SHARED)
#define ASTER_API __declspec(dllexport)
#else
#define ASTER_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(ASTERASM_SHARED)
#define ASTER_API __attribute__((visibility("default")))
#else
#define ASTER_API
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ASTER_LIKELY(x) __builtin_expect(!!(x), 1)
#define ASTER_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ASTER_NOINLINE __attribute__((noinline))
#define ASTER_FORCEINLINE static inline __attribute__((always_inline))
#if defined(__MINGW32__) && !defined(__clang__)
#define ASTER_PRINTF_LIKE(fmt_index, first_arg)                                \
  __attribute__((format(gnu_printf, fmt_index, first_arg)))
#else
#define ASTER_PRINTF_LIKE(fmt_index, first_arg)                                \
  __attribute__((format(printf, fmt_index, first_arg)))
#endif
#else
#define ASTER_LIKELY(x) (x)
#define ASTER_UNLIKELY(x) (x)
#define ASTER_NOINLINE
#define ASTER_FORCEINLINE static inline
#define ASTER_PRINTF_LIKE(fmt_index, first_arg)
#endif

#define ASTER_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define ASTER_ALIGNOF(T) _Alignof(T)
#define ASTER_UNUSED(x) ((void)(x))
#define ASTER_MIN(a, b) ((a) < (b) ? (a) : (b))
#define ASTER_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Portable stdio open helper.  Microsoft's UCRT deprecates ISO C fopen
   when compiling with the MSVC ABI; keep warnings enabled and use fopen_s
   there while preserving the ISO C path everywhere else. */
static inline FILE *ast_fopen(const char *path, const char *mode) {
#if defined(_WIN32) && defined(_MSC_VER)
  FILE *file = NULL;
  return fopen_s(&file, path, mode) == 0 ? file : NULL;
#else
  return fopen(path, mode);
#endif
}

/* Portable strerror helper.  UCRT deprecates strerror under the MSVC ABI.
   Callers supply scratch storage so the Windows path can use strerror_s while
   POSIX/MinGW targets keep the standard strerror contract. */
static inline const char *ast_strerror(int error_code, char *buffer,
                                       size_t buffer_size) {
#if defined(_WIN32) && defined(_MSC_VER)
  if (buffer != NULL && buffer_size != 0u &&
      strerror_s(buffer, buffer_size, error_code) == 0)
    return buffer;
  return "unknown error";
#else
  ASTER_UNUSED(buffer);
  ASTER_UNUSED(buffer_size);
  return strerror(error_code);
#endif
}

static inline int ast_size_add_overflow(size_t a, size_t b, size_t *out) {
  if (a > SIZE_MAX - b)
    return 1;
  *out = a + b;
  return 0;
}

static inline int ast_size_mul_overflow(size_t a, size_t b, size_t *out) {
  if (a != 0u && b > SIZE_MAX / a)
    return 1;
  *out = a * b;
  return 0;
}

static inline size_t ast_common_align_up(size_t value, size_t align) {
  return (value + (align - 1u)) & ~(align - 1u);
}

#ifdef __cplusplus
}
#endif

#endif
