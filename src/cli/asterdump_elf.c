/* SPDX-License-Identifier: MPL-2.0 */
/* Dependency-free ELF64 ET_REL/ET_EXEC/ET_DYN section reader for asterdump.
   The on-disk structures are decoded bytewise: never cast file bytes to C
   structures or trust producer-provided sizes, offsets, names or indices. */
#include "asterdump_elf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ELF_ITEMS 1000000u
#define ELF_SECTION_BYTES 64u
#define ELF_SYMBOL_BYTES 24u

typedef struct ElfSec {
  uint32_t name, type, link, info;
  uint64_t flags, address, offset, size, alignment, entsize;
} ElfSec;
static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
         ((uint32_t)p[3] << 24u);
}
static uint64_t rd64(const uint8_t *p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4u) << 32u);
}
static int span(size_t length, uint64_t offset, uint64_t size) {
  return offset <= (uint64_t)length && size <= (uint64_t)length - offset;
}
static int add_symbol(DumpElfView *out, const DumpElfSymbol *item,
                      size_t *capacity) {
  if (out->symbol_count == MAX_ELF_ITEMS)
    return 0;
  if (out->symbol_count == *capacity) {
    size_t next = *capacity ? *capacity * 2u : 32u;
    if (next > MAX_ELF_ITEMS)
      next = MAX_ELF_ITEMS;
    DumpElfSymbol *tmp =
        (DumpElfSymbol *)realloc(out->symbols, next * sizeof(*tmp));
    if (!tmp)
      return 0;
    out->symbols = tmp;
    *capacity = next;
  }
  out->symbols[out->symbol_count++] = *item;
  return 1;
}
static int add_reloc(DumpElfView *out, const DumpElfReloc *item,
                     size_t *capacity) {
  if (out->reloc_count == MAX_ELF_ITEMS)
    return 0;
  if (out->reloc_count == *capacity) {
    size_t next = *capacity ? *capacity * 2u : 32u;
    if (next > MAX_ELF_ITEMS)
      next = MAX_ELF_ITEMS;
    DumpElfReloc *tmp =
        (DumpElfReloc *)realloc(out->relocs, next * sizeof(*tmp));
    if (!tmp)
      return 0;
    out->relocs = tmp;
    *capacity = next;
  }
  out->relocs[out->reloc_count++] = *item;
  return 1;
}
/* Strict UTF-8 validation, rejecting controls, surrogates, invalid continuation
   bytes and overlong forms. ELF names may legally contain Unicode identifiers.
 */
static int printable_utf8(const unsigned char *p, size_t length) {
  for (size_t i = 0; i < length;) {
    unsigned c = p[i++];
    if (c < 0x80u) {
      if (c < 0x20u || c == 0x7fu)
        return 0;
      continue;
    }
    unsigned trailing;
    uint32_t value, minimum;
    if (c >= 0xc2u && c <= 0xdfu) {
      trailing = 1u;
      value = c & 0x1fu;
      minimum = 0x80u;
    } else if (c >= 0xe0u && c <= 0xefu) {
      trailing = 2u;
      value = c & 0x0fu;
      minimum = 0x800u;
    } else if (c >= 0xf0u && c <= 0xf4u) {
      trailing = 3u;
      value = c & 0x07u;
      minimum = 0x10000u;
    } else
      return 0;
    if (trailing > length - i)
      return 0;
    for (unsigned j = 0; j < trailing; ++j) {
      unsigned byte = p[i++];
      if (byte < 0x80u || byte > 0xbfu)
        return 0;
      value = (value << 6u) | (byte & 0x3fu);
    }
    if (value < minimum || value > 0x10ffffu ||
        (value >= 0xd800u && value <= 0xdfffu) ||
        (value >= 0x80u && value <= 0x9fu))
      return 0;
  }
  return 1;
}
static const char *checked_name(const uint8_t *data, size_t length,
                                const ElfSec *str, uint32_t index) {
  if (str->type != 3u || index >= str->size ||
      !span(length, str->offset, str->size))
    return NULL;
  const char *name = (const char *)(data + str->offset + index);
  size_t remaining = (size_t)(str->size - index);
  const char *end = (const char *)memchr(name, '\0', remaining);
  if (!end)
    return NULL;
  /* A compiler identifier may be UTF-8, never accept malformed names. */
  if (!printable_utf8((const unsigned char *)name, (size_t)(end - name)))
    return NULL;
  return name;
}
/* Retain section/offset ordering for one-pass section inspection. This
   removes quadratic symbol/relocation lookup costs on large object files. */
static int compare_symbols(const void *a, const void *b) {
  const DumpElfSymbol *x = (const DumpElfSymbol *)a,
                      *y = (const DumpElfSymbol *)b;
  if (x->section != y->section)
    return x->section < y->section ? -1 : 1;
  if (x->offset != y->offset)
    return x->offset < y->offset ? -1 : 1;
  return strcmp(x->name, y->name);
}
static int compare_relocs(const void *a, const void *b) {
  const DumpElfReloc *x = (const DumpElfReloc *)a, *y = (const DumpElfReloc *)b;
  if (x->section != y->section)
    return x->section < y->section ? -1 : 1;
  if (x->offset != y->offset)
    return x->offset < y->offset ? -1 : 1;
  if (x->type != y->type)
    return x->type < y->type ? -1 : 1;
  return strcmp(x->symbol, y->symbol);
}
void dump_elf_destroy(DumpElfView *view) {
  if (!view)
    return;
  free(view->sections);
  free(view->symbols);
  free(view->relocs);
  memset(view, 0, sizeof(*view));
}
/* Section headers are optional for linked images. When they are absent,
   inspect only the file-backed bytes of executable PT_LOAD segments. ELF's
   zero-initialized memory tail (p_memsz > p_filesz) is NOT machine code. */
static int parse_program_headers(const uint8_t *data, size_t length,
                                 uint16_t machine, DumpElfView *out) {
  const uint64_t offset = rd64(data + 32u);
  const uint32_t count = rd16(data + 56u);
  const uint32_t entry_bytes = rd16(data + 54u);
  const uint64_t alignment = machine == 62u ? 1u : 4u;
  if (!count || count > 8192u || entry_bytes != 56u ||
      !span(length, offset, (uint64_t)count * 56u))
    return 0;
  out->sections = (DumpElfSection *)calloc(count, sizeof(*out->sections));
  if (!out->sections)
    return 0;
  for (uint32_t i = 0u; i < count; ++i) {
    const uint8_t *p = data + offset + (uint64_t)i * 56u;
    const uint32_t type = rd32(p), flags = rd32(p + 4u);
    if (type != 1u || !(flags & 1u))
      continue; /* PT_LOAD, PF_X */
    const uint64_t file_offset = rd64(p + 8u), address = rd64(p + 16u);
    const uint64_t file_bytes = rd64(p + 32u), memory_bytes = rd64(p + 40u);
    const uint64_t align = rd64(p + 48u);
    if (file_bytes > memory_bytes || !span(length, file_offset, file_bytes) ||
        (align > 1u &&
         ((align & (align - 1u)) ||
          (file_offset & (align - 1u)) != (address & (align - 1u)))) ||
        address % alignment || file_bytes % alignment ||
        file_bytes > UINT64_MAX - address)
      return 0;
    if (!file_bytes)
      continue;
    DumpElfSection *section = &out->sections[out->section_count++];
    section->name = "PT_LOAD[PF_X]";
    section->index = i;
    section->offset = file_offset;
    section->size = file_bytes;
    section->virtual_offset = address;
  }
  if (!out->section_count)
    return 0;
  out->program_headers_only = 1;
  return 1;
}

int dump_elf_parse(const uint8_t *data, size_t length, uint16_t machine,
                   DumpElfView *out, char *error, size_t error_cap) {
  const char *reason = "invalid or unsupported ELF64 section table";
  ElfSec *secs = NULL;
  size_t symcap = 0u, relcap = 0u;
  uint16_t elf_type = data && length >= 18u ? rd16(data + 16u) : 0u;
  const int linked = elf_type == 2u || elf_type == 3u;
  memset(out, 0, sizeof(*out));
  if (!data || length < 64u || memcmp(data, "\177ELF", 4u) || data[4] != 2u ||
      data[5] != 1u || data[6] != 1u || (elf_type != 1u && !linked) ||
      rd16(data + 18u) != machine || rd32(data + 20u) != 1u ||
      rd16(data + 52u) != 64u)
    goto bad;
  uint64_t shoff = rd64(data + 40u);
  uint32_t shnum = rd16(data + 60u), names = rd16(data + 62u);
  if (linked && shoff == 0u && shnum == 0u && names == 0u &&
      (rd16(data + 58u) == 0u || rd16(data + 58u) == ELF_SECTION_BYTES)) {
    if (!parse_program_headers(data, length, machine, out))
      goto bad;
    out->elf_type = elf_type;
    return 1;
  }
  if (rd16(data + 58u) != ELF_SECTION_BYTES)
    goto bad;
  if (!shnum || shnum > 8192u || names >= shnum ||
      !span(length, shoff, (uint64_t)shnum * ELF_SECTION_BYTES))
    goto bad;
  secs = (ElfSec *)calloc(shnum, sizeof(*secs));
  out->sections = (DumpElfSection *)calloc(shnum, sizeof(*out->sections));
  if (!secs || !out->sections) {
    reason = "out of memory";
    goto bad;
  }
  for (uint32_t i = 0; i < shnum; ++i) {
    const uint8_t *s = data + shoff + (uint64_t)i * ELF_SECTION_BYTES;
    ElfSec *v = &secs[i];
    v->name = rd32(s);
    v->type = rd32(s + 4u);
    v->flags = rd64(s + 8u);
    v->address = rd64(s + 16u);
    v->offset = rd64(s + 24u);
    v->size = rd64(s + 32u);
    v->link = rd32(s + 40u);
    v->info = rd32(s + 44u);
    v->alignment = rd64(s + 48u);
    v->entsize = rd64(s + 56u);
    if (v->type != 8u && !span(length, v->offset, v->size))
      goto bad;
    if (v->alignment && (v->alignment & (v->alignment - 1u)))
      goto bad;
  }
  if (secs[names].type != 3u)
    goto bad;
  const uint64_t instruction_alignment = machine == 62u ? 1u : 4u;
  uint64_t virtual_cursor = 0u;
  for (uint32_t i = 1u; i < shnum; ++i) {
    const ElfSec *s = &secs[i];
    if (s->type != 1u || !(s->flags & UINT64_C(4)) || !s->size)
      continue;
    if (s->size % instruction_alignment || !span(length, s->offset, s->size))
      goto bad;
    const char *name = checked_name(data, length, &secs[names], s->name);
    if (!name)
      goto bad;
    uint64_t align = s->alignment > instruction_alignment
                         ? s->alignment
                         : instruction_alignment;
    if (!linked) {
      if (align > 65536u || virtual_cursor > UINT64_MAX - (align - 1u))
        goto bad;
      virtual_cursor = (virtual_cursor + align - 1u) & ~(align - 1u);
      if (s->size > UINT64_MAX - virtual_cursor)
        goto bad;
    } else {
      /* For linked images st_value, sh_addr and PC-relative branches are
         expressed in virtual addresses, not ET_REL section coordinates. */
      if (s->address % instruction_alignment ||
          s->size > UINT64_MAX - s->address ||
          (align > 1u && (s->address % align)))
        goto bad;
      virtual_cursor = s->address;
    }
    DumpElfSection *target = &out->sections[out->section_count++];
    target->name = name;
    target->index = i;
    target->offset = s->offset;
    target->size = s->size;
    target->virtual_offset = virtual_cursor;
    virtual_cursor += s->size;
  }
  if (!out->section_count) {
    reason = "ELF contains no executable code sections";
    goto bad;
  }
  for (uint32_t i = 1u; i < shnum; ++i) {
    const ElfSec *s = &secs[i];
    if (s->type != 2u && !(linked && s->type == 11u))
      continue; /* SYMTAB / DYNSYM */
    if (s->link >= shnum || secs[s->link].type != 3u ||
        s->entsize != ELF_SYMBOL_BYTES || s->size % ELF_SYMBOL_BYTES)
      goto bad;
    uint64_t count = s->size / ELF_SYMBOL_BYTES;
    if (count > MAX_ELF_ITEMS)
      goto bad;
    for (uint64_t k = 0; k < count; ++k) {
      const uint8_t *p = data + s->offset + k * ELF_SYMBOL_BYTES;
      uint32_t section = rd16(p + 6u);
      uint64_t value = rd64(p + 8u);
      uint32_t index = rd32(p);
      if (!index)
        continue;
      const char *name = checked_name(data, length, &secs[s->link], index);
      if (!name)
        goto bad;
      for (size_t z = 0; z < out->section_count; ++z)
        if (out->sections[z].index == section &&
            (!linked || value >= out->sections[z].virtual_offset) &&
            (linked ? value - out->sections[z].virtual_offset : value) <
                out->sections[z].size &&
            !(value % instruction_alignment)) {
          DumpElfSymbol label = {
              name, section,
              linked ? value - out->sections[z].virtual_offset : value};
          if (!add_symbol(out, &label, &symcap)) {
            reason = "too many ELF symbols";
            goto bad;
          }
        }
    }
  }
  for (uint32_t i = 1u; i < shnum; ++i) {
    const ElfSec *s = &secs[i];
    if (s->type != 4u && s->type != 9u)
      continue; /* RELA/REL */
    if (s->link >= shnum || s->info >= shnum ||
        (secs[s->link].type != 2u && !(linked && secs[s->link].type == 11u)))
      goto bad;
    const ElfSec *symtab = &secs[s->link];
    if (symtab->link >= shnum || secs[symtab->link].type != 3u ||
        symtab->entsize != ELF_SYMBOL_BYTES || symtab->size % ELF_SYMBOL_BYTES)
      goto bad;
    uint64_t rec_size = s->type == 4u ? 24u : 16u;
    if (s->entsize != rec_size || s->size % rec_size ||
        s->size / rec_size > MAX_ELF_ITEMS)
      goto bad;
    int is_text = 0;
    for (size_t z = 0; z < out->section_count; ++z)
      if (out->sections[z].index == s->info)
        is_text = 1;
    /* Linked images may describe dynamic relocations by absolute virtual
       address (sh_info=0). Resolve each one to a known executable section
       rather than incorrectly using a section-relative offset. */
    if (!is_text && !(linked && s->info == 0u))
      continue;
    for (uint64_t k = 0; k < s->size / rec_size; ++k) {
      const uint8_t *p = data + s->offset + k * rec_size;
      uint64_t off = rd64(p), info = rd64(p + 8u);
      uint32_t symbol_index = (uint32_t)(info >> 32u);
      if ((off % instruction_alignment) ||
          symbol_index >= symtab->size / ELF_SYMBOL_BYTES)
        goto bad;
      uint32_t reloc_section = s->info;
      uint64_t reloc_offset = off;
      if (linked) {
        int matched = 0;
        for (size_t z = 0u; z < out->section_count; ++z) {
          const DumpElfSection *text = &out->sections[z];
          if ((s->info == 0u || text->index == s->info) &&
              off >= text->virtual_offset &&
              off - text->virtual_offset < text->size) {
            reloc_section = text->index;
            reloc_offset = off - text->virtual_offset;
            matched = 1;
            break;
          }
        }
        /* Non-text dynamic relocations are not displayed. A relocation
           that explicitly names a text section must never be ignored. */
        if (!matched && s->info == 0u)
          continue;
        if (!matched)
          goto bad;
      } else if (off >= secs[s->info].size)
        goto bad;
      const uint8_t *sym =
          data + symtab->offset + (uint64_t)symbol_index * ELF_SYMBOL_BYTES;
      uint32_t name_index = rd32(sym);
      const char *name =
          name_index
              ? checked_name(data, length, &secs[symtab->link], name_index)
              : "";
      if (!name)
        goto bad;
      int64_t addend = 0;
      if (s->type == 4u) {
        uint64_t raw_addend = rd64(p + 16u);
        memcpy(&addend, &raw_addend, sizeof(addend));
      }
      DumpElfReloc reloc = {name,           reloc_section, reloc_offset,
                            (uint32_t)info, addend,        s->type == 4u};
      if (!add_reloc(out, &reloc, &relcap)) {
        reason = "too many ELF relocations";
        goto bad;
      }
    }
  }
  out->elf_type = elf_type;
  /* C's qsort does not portably accept a null base even when nmemb==0.
     ASan/UBSan caught this on linked images with no text relocations. */
  if (out->symbol_count > 1u)
    qsort(out->symbols, out->symbol_count, sizeof(*out->symbols),
          compare_symbols);
  if (out->reloc_count > 1u)
    qsort(out->relocs, out->reloc_count, sizeof(*out->relocs), compare_relocs);
  free(secs);
  return 1;
bad:
  if (error && error_cap)
    (void)snprintf(error, error_cap, "%s", reason);
  free(secs);
  dump_elf_destroy(out);
  return 0;
}
