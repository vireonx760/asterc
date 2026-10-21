#ifndef ASTERASM_COMPILER_PATHS_H
#define ASTERASM_COMPILER_PATHS_H
#include "asterasm/source.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif
static inline int compiler_paths_alias(const char *a, const char *b) {
  char *left = ast_path_canonicalize(a), *right = ast_path_canonicalize(b);
  int same = 0;
  if (left && right) {
#if defined(_WIN32)
    for (char *p = left; *p; ++p)
      *p = (char)tolower((unsigned char)*p);
    for (char *p = right; *p; ++p)
      *p = (char)tolower((unsigned char)*p);
#endif
    same = !strcmp(left, right);
  }
  free(left);
  free(right);
  if (same)
    return 1;
#if defined(_WIN32)
  HANDLE ah =
      CreateFileA(a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  NULL, OPEN_EXISTING, 0, NULL);
  HANDLE bh =
      CreateFileA(b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  NULL, OPEN_EXISTING, 0, NULL);
  BY_HANDLE_FILE_INFORMATION ai, bi;
  if (ah != INVALID_HANDLE_VALUE && bh != INVALID_HANDLE_VALUE &&
      GetFileInformationByHandle(ah, &ai) &&
      GetFileInformationByHandle(bh, &bi))
    same = ai.dwVolumeSerialNumber == bi.dwVolumeSerialNumber &&
           ai.nFileIndexHigh == bi.nFileIndexHigh &&
           ai.nFileIndexLow == bi.nFileIndexLow;
  if (ah != INVALID_HANDLE_VALUE)
    CloseHandle(ah);
  if (bh != INVALID_HANDLE_VALUE)
    CloseHandle(bh);
#else
  struct stat ai, bi;
  if (!stat(a, &ai) && !stat(b, &bi))
    same = ai.st_dev == bi.st_dev && ai.st_ino == bi.st_ino;
#endif
  return same;
}
#endif
