/* SPDX-License-Identifier: Apache-2.0 */
/* Observe generated-code faults before Windows attempts stack unwinding. */
#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static uintptr_t code_begin;
static size_t code_size;

static LONG CALLBACK observe_fault(EXCEPTION_POINTERS *info) {
  DWORD status = info->ExceptionRecord->ExceptionCode;
  uintptr_t pc = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
  if ((status == EXCEPTION_INT_DIVIDE_BY_ZERO ||
       status == EXCEPTION_INT_OVERFLOW) &&
      pc >= code_begin && pc - code_begin < code_size) {
    /* A nonzero child exit plus the exact code/address proves the hardware
       exception. Other crashes, including fast-fail, remain test failures. */
    printf("{\"exception_code\":%lu,\"offset\":%llu}\n",
           (unsigned long)status, (unsigned long long)(pc - code_begin));
    fflush(stdout);
    ExitProcess(status);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

static int parse_value(const wchar_t *text, int is_signed, uint64_t *value) {
  wchar_t *end;
  errno = 0;
  if (is_signed)
    *value = (uint64_t)wcstoll(text, &end, 10);
  else
    *value = wcstoull(text, &end, 10);
  return errno == 0 && end != text && *end == L'\0';
}

#define CALL_WITH(type)                                                        \
  do {                                                                         \
    typedef type (*Function)(type, type);                                       \
    Function function;                                                         \
    _Static_assert(sizeof(function) == sizeof(code), "function pointer size");   \
    memcpy(&function, &code, sizeof(function));                                 \
    answer = (uint64_t)function((type)x, (type)y);                                \
  } while (0)

int wmain(int argc, wchar_t **argv);
int wmain(int argc, wchar_t **argv) {
  if (argc != 6) {
    fputs("usage: fault-runner binary bits signed x y\n", stderr);
    return 90;
  }
  uint64_t bits, is_signed, x, y;
  if (!parse_value(argv[2], 0, &bits) || !parse_value(argv[3], 0, &is_signed) ||
      (bits != 8u && bits != 16u && bits != 32u && bits != 64u) || is_signed > 1u ||
      !parse_value(argv[4], (int)is_signed, &x) ||
      !parse_value(argv[5], (int)is_signed, &y)) {
    fputs("invalid runner arguments\n", stderr);
    return 90;
  }
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  LARGE_INTEGER length;
  if (file == INVALID_HANDLE_VALUE)
    return 91;
  if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0 ||
      length.QuadPart > 1024 * 1024) {
    CloseHandle(file);
    return 91;
  }
  code_size = (size_t)length.QuadPart;
  void *code = VirtualAlloc(NULL, code_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!code) {
    CloseHandle(file);
    return 92;
  }
  DWORD read = 0;
  BOOL loaded = ReadFile(file, code, (DWORD)code_size, &read, NULL);
  CloseHandle(file);
  DWORD previous;
  if (!loaded || read != code_size ||
      !VirtualProtect(code, code_size, PAGE_EXECUTE_READ, &previous) ||
      !FlushInstructionCache(GetCurrentProcess(), code, code_size)) {
    VirtualFree(code, 0, MEM_RELEASE);
    return 92;
  }
  code_begin = (uintptr_t)code;
  PVOID handler = AddVectoredExceptionHandler(1u, observe_fault);
  if (!handler) {
    VirtualFree(code, 0, MEM_RELEASE);
    return 93;
  }
  uint64_t answer = 0;
  if (is_signed) {
    switch (bits) {
    case 8: CALL_WITH(int8_t); break;
    case 16: CALL_WITH(int16_t); break;
    case 32: CALL_WITH(int32_t); break;
    case 64: CALL_WITH(int64_t); break;
    }
  } else {
    switch (bits) {
    case 8: CALL_WITH(uint8_t); break;
    case 16: CALL_WITH(uint16_t); break;
    case 32: CALL_WITH(uint32_t); break;
    case 64: CALL_WITH(uint64_t); break;
    }
  }
  RemoveVectoredExceptionHandler(handler);
  VirtualFree(code, 0, MEM_RELEASE);
  printf("{\"value\":%llu}\n", (unsigned long long)answer);
  return 0;
}
