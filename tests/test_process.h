#ifndef ASTERASM_TEST_PROCESS_H
#define ASTERASM_TEST_PROCESS_H
#if defined(_WIN32)
#include <windows.h>
#endif
static void ast_test_process_setup(void) {
#if defined(_WIN32)
  /* CI failures belong in the test log. A Windows Error Reporting dialog
     must not block the suite or interrupt the user's desktop. */
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
}
#endif
