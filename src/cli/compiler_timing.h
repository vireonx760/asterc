#ifndef ASTERASM_COMPILER_TIMING_H
#define ASTERASM_COMPILER_TIMING_H
#if defined(_WIN32)
#include <windows.h>
static inline double compiler_seconds(void) {
  LARGE_INTEGER counter, frequency;
  return QueryPerformanceFrequency(&frequency) &&
                 QueryPerformanceCounter(&counter) && frequency.QuadPart > 0
             ? (double)counter.QuadPart / (double)frequency.QuadPart
             : 0.0;
}
#else
#include <time.h>
static inline double compiler_seconds(void) {
  struct timespec value;
  return timespec_get(&value, TIME_UTC) == TIME_UTC
             ? (double)value.tv_sec + (double)value.tv_nsec * 1e-9
             : 0.0;
}
#endif
static inline double compiler_elapsed(double begin, double end) {
  return end >= begin ? end - begin : 0.0;
}
#endif
