/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ASTERDUMP_COFF_H
#define ASTERDUMP_COFF_H
#include "asterdump_elf.h"
/* Shared bounded section/symbol/relocation presentation, independent of the
   on-disk container. The COFF parser owns short/long name copies. */
typedef struct DumpCoffView {
  DumpElfView object;
  char **names;
  size_t name_count;
  size_t name_capacity;
  int is_pe;
  uint64_t image_base;
} DumpCoffView;
int dump_coff_parse(const uint8_t *data, size_t length,
                    uint16_t expected_machine, DumpCoffView *out, char *error,
                    size_t error_cap);
void dump_coff_destroy(DumpCoffView *out);
const char *dump_coff_reloc_name(uint16_t machine, uint32_t type);
#endif
