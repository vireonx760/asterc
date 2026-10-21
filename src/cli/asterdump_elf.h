/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ASTERDUMP_ELF_H
#define ASTERDUMP_ELF_H
#include <stddef.h>
#include <stdint.h>

typedef struct DumpElfSection {
  const char *name;
  uint32_t index;
  uint64_t offset;
  uint64_t size;
  uint64_t virtual_offset;
} DumpElfSection;
typedef struct DumpElfSymbol {
  const char *name;
  uint32_t section;
  uint64_t offset;
} DumpElfSymbol;
typedef struct DumpElfReloc {
  const char *symbol;
  uint32_t section;
  uint64_t offset;
  uint32_t type;
  int64_t addend;
  int has_addend;
} DumpElfReloc;
typedef struct DumpElfView {
  DumpElfSection *sections;
  size_t section_count;
  DumpElfSymbol *symbols;
  size_t symbol_count;
  DumpElfReloc *relocs;
  size_t reloc_count;
  uint16_t elf_type;        /* ET_REL=1, ET_EXEC=2, ET_DYN=3 */
  int program_headers_only; /* sectionless PT_LOAD fallback */
} DumpElfView;

/* ELF64 little-endian ET_REL, ET_EXEC or ET_DYN with section headers.
   Sectionless linked images are decoded from executable PT_LOAD segments.
   Decoding is fail-closed: malformed or unsupported containers fail entirely.
 */
int dump_elf_parse(const uint8_t *data, size_t length, uint16_t machine,
                   DumpElfView *out, char *error, size_t error_cap);
void dump_elf_destroy(DumpElfView *view);
#endif
