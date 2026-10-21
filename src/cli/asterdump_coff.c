/* SPDX-License-Identifier: MPL-2.0 */
/* Strict bytewise COFF object / PE32+ x64 and Arm64 reader.
 * This is a bounded inspector, not a Windows image loader. Never cast
 * untrusted file input to packed C structs or assume an implicit NUL. */
#include "asterdump_coff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SECTIONS 4096u
#define MAX_SYMBOL_RECORDS 262144u
#define MAX_RELOCS 524288u
#define COFF_SCN_EXEC UINT32_C(0x20000000)
#define COFF_SCN_RELOC_OVERFLOW UINT32_C(0x01000000)

static uint16_t u16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}
static uint32_t u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
         ((uint32_t)p[3] << 24u);
}
static uint64_t u64(const uint8_t *p) {
  return (uint64_t)u32(p) | ((uint64_t)u32(p + 4u) << 32u);
}
static int span(size_t len, uint64_t pos, uint64_t sz) {
  return pos <= len && sz <= len - pos;
}
static int store_name(DumpCoffView *o, const uint8_t *bytes, size_t len,
                      const char **out) {
  if (len > 4095u || o->name_count >= MAX_SYMBOL_RECORDS + MAX_SECTIONS)
    return 0;
  if (o->name_count == o->name_capacity) {
    size_t cap = o->name_capacity ? o->name_capacity * 2u : 32u;
    if (cap > MAX_SYMBOL_RECORDS + MAX_SECTIONS)
      cap = MAX_SYMBOL_RECORDS + MAX_SECTIONS;
    char **next = (char **)realloc(o->names, cap * sizeof(*next));
    if (!next)
      return 0;
    o->names = next;
    o->name_capacity = cap;
  }
  char *name = (char *)malloc(len + 1u);
  if (!name)
    return 0;
  if (len)
    memcpy(name, bytes, len);
  name[len] = 0;
  o->names[o->name_count++] = name;
  *out = name;
  return 1;
}
/* Names are either inline eight-byte fields, or offsets into the COFF
   string table (whose initial DWORD includes its own size). */
static int decode_name(DumpCoffView *out, const uint8_t *name,
                       const uint8_t *str, uint32_t strsz, int section_name,
                       const char **dst) {
  const uint8_t *bytes = name;
  size_t n = 0u;
  if (!section_name && u32(name) == 0u && u32(name + 4u) != 0u) {
    uint32_t off = u32(name + 4u);
    if (!str || off < 4u || off >= strsz)
      return 0;
    bytes = str + off;
    while (off + n < strsz && bytes[n])
      ++n;
    if (off + n == strsz)
      return 0;
  } else if (section_name && name[0] == '/') {
    uint64_t off = 0u;
    size_t k = 1u;
    for (; k < 8u && name[k]; ++k) {
      if (name[k] < '0' || name[k] > '9' || off > (UINT32_MAX - 9u) / 10u)
        return 0;
      off = off * 10u + (name[k] - '0');
    }
    if (k == 1u || !str || off < 4u || off >= strsz)
      return 0;
    bytes = str + off;
    while (off + n < strsz && bytes[n])
      ++n;
    if (off + n == strsz)
      return 0;
  } else {
    while (n < 8u && bytes[n])
      ++n;
  }
  /* Reject embedded control bytes which can corrupt text output. */
  for (size_t i = 0; i < n; ++i)
    if (bytes[i] < 32u || bytes[i] > 126u)
      return 0;
  return store_name(out, bytes, n, dst);
}
static uint32_t coff_reloc_width(uint16_t machine, uint16_t type) {
  if (machine == 0x8664u) {
    if (type == 0x0001u)
      return 8u;
    if (type == 0x0002u || type == 0x0003u ||
        (type >= 0x0004u && type <= 0x0009u) || type == 0x000bu)
      return 4u;
  } else if (machine == 0xaa64u) {
    if (type == 0x000eu)
      return 8u;
    if (type >= 0x0001u && type <= 0x0007u)
      return 4u;
  }
  return 0u; /* unknown relocation: preserve numeric type, never guess */
}
static int cmp_syms(const void *a, const void *b) {
  const DumpElfSymbol *x = (const DumpElfSymbol *)a,
                      *y = (const DumpElfSymbol *)b;
  if (x->section != y->section)
    return x->section < y->section ? -1 : 1;
  if (x->offset != y->offset)
    return x->offset < y->offset ? -1 : 1;
  return strcmp(x->name, y->name);
}
static int cmp_relocs(const void *a, const void *b) {
  const DumpElfReloc *x = (const DumpElfReloc *)a, *y = (const DumpElfReloc *)b;
  if (x->section != y->section)
    return x->section < y->section ? -1 : 1;
  if (x->offset != y->offset)
    return x->offset < y->offset ? -1 : 1;
  return strcmp(x->symbol, y->symbol);
}
void dump_coff_destroy(DumpCoffView *o) {
  if (!o)
    return;
  dump_elf_destroy(&o->object);
  for (size_t i = 0; i < o->name_count; ++i)
    free(o->names[i]);
  free(o->names);
  memset(o, 0, sizeof(*o));
}
const char *dump_coff_reloc_name(uint16_t machine, uint32_t type) {
  if (machine == 0x8664u) {
    switch (type) {
    case 0x0000:
      return "IMAGE_REL_AMD64_ABSOLUTE";
    case 0x0001:
      return "IMAGE_REL_AMD64_ADDR64";
    case 0x0002:
      return "IMAGE_REL_AMD64_ADDR32";
    case 0x0003:
      return "IMAGE_REL_AMD64_ADDR32NB";
    case 0x0004:
      return "IMAGE_REL_AMD64_REL32";
    case 0x0005:
      return "IMAGE_REL_AMD64_REL32_1";
    case 0x0006:
      return "IMAGE_REL_AMD64_REL32_2";
    case 0x0007:
      return "IMAGE_REL_AMD64_REL32_3";
    case 0x0008:
      return "IMAGE_REL_AMD64_REL32_4";
    case 0x0009:
      return "IMAGE_REL_AMD64_REL32_5";
    case 0x000b:
      return "IMAGE_REL_AMD64_SECREL";
    default:
      return NULL;
    }
  }
  if (machine == 0xaa64u) {
    switch (type) {
    case 0x0000:
      return "IMAGE_REL_ARM64_ABSOLUTE";
    case 0x0001:
      return "IMAGE_REL_ARM64_ADDR32";
    case 0x0002:
      return "IMAGE_REL_ARM64_ADDR32NB";
    case 0x0003:
      return "IMAGE_REL_ARM64_BRANCH26";
    case 0x0004:
      return "IMAGE_REL_ARM64_PAGEBASE_REL21";
    case 0x0006:
      return "IMAGE_REL_ARM64_PAGEOFFSET_12A";
    case 0x0007:
      return "IMAGE_REL_ARM64_PAGEOFFSET_12L";
    case 0x000e:
      return "IMAGE_REL_ARM64_ADDR64";
    default:
      return NULL;
    }
  }
  return NULL;
}
int dump_coff_parse(const uint8_t *data, size_t length,
                    uint16_t expected_machine, DumpCoffView *out, char *error,
                    size_t error_cap) {
  const char *reason = "malformed or unsupported COFF/PE container";
  uint64_t coff_at = 0u, sec_at, sympos = 0u;
  uint32_t symcount = 0u;
  uint16_t count, optsz, machine;
  uint32_t *selected = NULL;
  const char **symbol_names = NULL;
  const uint8_t *strings = NULL;
  uint32_t string_size = 0u;
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  if (!data || length < 20u)
    goto bad;
  if (length >= 2u && data[0] == 'M' && data[1] == 'Z') {
    if (!span(length, 0x3cu, 4u))
      goto bad;
    coff_at = u32(data + 0x3cu);
    if (!span(length, coff_at, 24u) || memcmp(data + coff_at, "PE\0\0", 4u))
      goto bad;
    coff_at += 4u;
    out->is_pe = 1;
  }
  if (!span(length, coff_at, 20u))
    goto bad;
  const uint8_t *h = data + coff_at;
  machine = u16(h);
  count = u16(h + 2u);
  sympos = u32(h + 8u);
  symcount = u32(h + 12u);
  optsz = u16(h + 16u);
  if (machine != expected_machine || !count || count > MAX_SECTIONS ||
      symcount > MAX_SYMBOL_RECORDS || (symcount == 0u && sympos != 0u))
    goto bad;
  sec_at = coff_at + 20u + optsz;
  if (!span(length, sec_at, (uint64_t)count * 40u))
    goto bad;
  if (out->is_pe) {
    uint64_t opt = coff_at + 20u;
    if (optsz < 112u || !span(length, opt, optsz) || u16(data + opt) != 0x20bu)
      goto bad;
    out->image_base = u64(data + opt + 24u);
    /* PE section RVA and image-base arithmetic must be representable. */
  } else if (optsz != 0u)
    goto bad;
  if (symcount) {
    if (sympos < sec_at + (uint64_t)count * 40u ||
        !span(length, sympos, (uint64_t)symcount * 18u) ||
        !span(length, sympos + (uint64_t)symcount * 18u, 4u))
      goto bad;
    uint64_t str_pos = sympos + (uint64_t)symcount * 18u;
    string_size = u32(data + str_pos);
    if (string_size < 4u || !span(length, str_pos, string_size))
      goto bad;
    strings = data + str_pos;
  }
  selected = (uint32_t *)calloc((size_t)count + 1u, sizeof(*selected));
  if (!selected)
    goto bad;
  out->object.sections =
      (DumpElfSection *)calloc(count, sizeof(*out->object.sections));
  if (!out->object.sections)
    goto bad;
  for (uint32_t j = 0u; j < count; ++j) {
    const uint8_t *s = data + sec_at + (uint64_t)j * 40u;
    uint32_t vsize = u32(s + 8u), vaddr = u32(s + 12u), rawsize = u32(s + 16u),
             rawpos = u32(s + 20u);
    uint32_t relpos = u32(s + 24u), nrel = u16(s + 32u), flags = u32(s + 36u);
    if (!span(length, rawpos, rawsize) ||
        (rawsize && rawpos < sec_at + (uint64_t)count * 40u) ||
        (!out->is_pe && nrel && (relpos < sec_at + (uint64_t)count * 40u)) ||
        (out->is_pe && nrel) || (out->is_pe && u32(s + 28u)))
      goto bad;
    if (!out->is_pe && nrel && !span(length, relpos, (uint64_t)nrel * 10u))
      goto bad;
    if (flags & COFF_SCN_RELOC_OVERFLOW) {
      reason = "COFF extended relocation overflow is not supported";
      goto bad;
    }
    if (!(flags & COFF_SCN_EXEC) || !rawsize)
      continue;
    uint32_t body = rawsize;
    if (out->is_pe && vsize && vsize < body)
      body = vsize;
    if (!body || (machine == 0xaa64u && body % 4u))
      goto bad;
    if (out->is_pe && out->image_base > UINT64_MAX - vaddr)
      goto bad;
    uint64_t vbase = out->is_pe ? out->image_base + vaddr : 0u;
    if (vbase > UINT64_MAX - body)
      goto bad;
    /* Reject ambiguous executable overlaps, either in file storage or
       at image virtual addresses. A disassembler must not guess which
       overlapping section supplies the actual instruction bytes. */
    for (size_t q = 0u; q < out->object.section_count; ++q) {
      const DumpElfSection *earlier = &out->object.sections[q];
      if ((uint64_t)rawpos < earlier->offset + earlier->size &&
          earlier->offset < (uint64_t)rawpos + body)
        goto bad;
      if (out->is_pe && vbase < earlier->virtual_offset + earlier->size &&
          earlier->virtual_offset < vbase + body)
        goto bad;
    }
    DumpElfSection *d = &out->object.sections[out->object.section_count++];
    if (!decode_name(out, s, strings, string_size, 1, &d->name))
      goto bad;
    d->index = j + 1u;
    d->offset = rawpos;
    d->size = body;
    d->virtual_offset = out->is_pe ? out->image_base + vaddr : 0u;
    selected[j + 1u] = (uint32_t)out->object.section_count;
  }
  if (!out->object.section_count) {
    reason = "COFF/PE contains no executable sections";
    goto bad;
  }
  if (symcount) {
    symbol_names = (const char **)calloc(symcount, sizeof(*symbol_names));
    if (!symbol_names)
      goto bad;
    out->object.symbols =
        (DumpElfSymbol *)calloc(symcount, sizeof(*out->object.symbols));
    if (!out->object.symbols)
      goto bad;
    for (uint32_t i = 0u; i < symcount;) {
      const uint8_t *p = data + sympos + (uint64_t)i * 18u;
      uint16_t section = u16(p + 12u);
      uint32_t aux = p[17u];
      if (aux >= symcount - i)
        goto bad;
      if (!decode_name(out, p, strings, string_size, 0, &symbol_names[i]))
        goto bad;
      if (section <= count && section && selected[section]) {
        const DumpElfSection *sec =
            &out->object.sections[selected[section] - 1u];
        uint64_t at = u32(p + 8u);
        if (at > sec->size)
          goto bad;
        DumpElfSymbol *item = &out->object.symbols[out->object.symbol_count++];
        item->name = symbol_names[i];
        item->section = section;
        item->offset = at;
      }
      i += aux + 1u;
    }
    if (out->object.symbol_count > 1u)
      qsort(out->object.symbols, out->object.symbol_count,
            sizeof(*out->object.symbols), cmp_syms);
  }
  if (!out->is_pe) {
    uint64_t all_rel = 0u;
    for (uint32_t j = 0u; j < count; ++j) {
      const uint8_t *s = data + sec_at + (uint64_t)j * 40u;
      uint32_t nrel = u16(s + 32u);
      if (!selected[j + 1u])
        continue;
      if (all_rel > MAX_RELOCS - nrel)
        goto bad;
      all_rel += nrel;
    }
    out->object.relocs = (DumpElfReloc *)calloc(
        (size_t)(all_rel ? all_rel : 1u), sizeof(*out->object.relocs));
    if (!out->object.relocs)
      goto bad;
    for (uint32_t j = 0u; j < count; ++j) {
      const uint8_t *s = data + sec_at + (uint64_t)j * 40u;
      if (!selected[j + 1u])
        continue;
      uint32_t nrel = u16(s + 32u), relpos = u32(s + 24u);
      const DumpElfSection *sec = &out->object.sections[selected[j + 1u] - 1u];
      for (uint32_t k = 0u; k < nrel; ++k) {
        const uint8_t *p = data + relpos + (uint64_t)k * 10u;
        uint32_t pos = u32(p), index = u32(p + 4u);
        uint16_t type = u16(p + 8u);
        uint32_t width = coff_reloc_width(machine, type);
        if (!symbol_names || index >= symcount || !symbol_names[index] ||
            pos >= sec->size || (width && width > sec->size - pos) ||
            (machine == 0xaa64u && pos % 4u))
          goto bad;
        DumpElfReloc *r = &out->object.relocs[out->object.reloc_count++];
        r->symbol = symbol_names[index];
        r->section = j + 1u;
        r->offset = pos;
        r->type = type;
        r->addend = 0;
        r->has_addend = 0;
      }
    }
    if (out->object.reloc_count > 1u)
      qsort(out->object.relocs, out->object.reloc_count,
            sizeof(*out->object.relocs), cmp_relocs);
  }
  out->object.elf_type = out->is_pe ? 2u : 1u;
  free(selected);
  free(symbol_names);
  return 1;
bad:
  if (error && error_cap)
    (void)snprintf(error, error_cap, "%s", reason);
  free(selected);
  free(symbol_names);
  dump_coff_destroy(out);
  return 0;
}
