/* Narrow, standalone ELF64 RELA writer used when the optional Rust object
 * writer is unavailable. This is not a general ELF/COFF/Mach-O replacement.
 * All host integers are written explicitly in little-endian byte order. */
#include "elf64_fallback.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Buffer {
  uint8_t *bytes;
  size_t used, cap;
} Buffer;
typedef struct ElfSection {
  const char *name;
  uint32_t name_offset, type, link, info;
  uint64_t flags, address, size, align, entsize, offset;
  Buffer body;
} ElfSection;

static int put(Buffer *b, const void *bytes, size_t n) {
  if (n > SIZE_MAX - b->used)
    return 0;
  size_t need = b->used + n;
  if (need > b->cap) {
    size_t cap = b->cap ? b->cap : 128u;
    while (cap < need) {
      if (cap > SIZE_MAX / 2u) {
        cap = need;
        break;
      }
      cap *= 2u;
    }
    uint8_t *p = (uint8_t *)realloc(b->bytes, cap);
    if (!p)
      return 0;
    b->bytes = p;
    b->cap = cap;
  }
  if (n) {
    if (bytes)
      memcpy(b->bytes + b->used, bytes, n);
    else
      memset(b->bytes + b->used, 0, n);
  }
  b->used += n;
  return 1;
}
static int number(Buffer *b, uint64_t value, size_t count) {
  uint8_t data[8];
  for (size_t i = 0; i < count; ++i)
    data[i] = (uint8_t)(value >> (i * 8u));
  return put(b, data, count);
}
static int str(Buffer *b, const uint8_t *v, size_t n, uint32_t *index) {
  if (!v || !n || n > 255u || b->used > UINT32_MAX - n - 1u)
    return 0;
  *index = (uint32_t)b->used;
  return put(b, v, n) && number(b, 0u, 1u);
}
static int aligned(Buffer *b, uint64_t a) {
  if (!a || a > 1048576u || (a & (a - 1u)))
    return 0;
  size_t mask = (size_t)a - 1u;
  if (b->used > SIZE_MAX - mask)
    return 0;
  size_t extra = ((b->used + mask) & ~mask) - b->used;
  return put(b, NULL, extra);
}
static uint32_t load32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
         ((uint32_t)p[3] << 24u);
}
static int section_header(Buffer *b, const ElfSection *s) {
  return number(b, s->name_offset, 4) && number(b, s->type, 4) &&
         number(b, s->flags, 8) && number(b, s->address, 8) &&
         number(b, s->offset, 8) && number(b, s->size, 8) &&
         number(b, s->link, 4) && number(b, s->info, 4) &&
         number(b, s->align, 8) && number(b, s->entsize, 8);
}
static uint32_t reloc_type(const AstObjectModule *m,
                           const AstObjectRelocation *r,
                           const AstObjectSection *section) {
  if (!section->data.data || r->offset > section->size)
    return 0;
  if (m->architecture == AST_OBJECT_AARCH64 &&
      r->kind == AST_OBJECT_A64_CALL26) {
    if (r->offset > section->size - (section->size >= 4 ? 4u : 0u) ||
        section->size < 4u || (r->offset & 3u) || (r->addend & 3))
      return 0;
    uint32_t w = load32(section->data.data + r->offset);
    return w == UINT32_C(0x94000000) ? 283u : 0u; /* R_AARCH64_CALL26 */
  }
  if (m->architecture == AST_OBJECT_RISCV64 && r->kind == AST_OBJECT_RV_CALL) {
    if (section->size < 8u || r->offset > section->size - 8u ||
        (r->offset & 3u) || (r->addend & 1))
      return 0;
    return load32(section->data.data + r->offset) == UINT32_C(0x00000097) &&
                   load32(section->data.data + r->offset + 4u) ==
                       UINT32_C(0x000080e7)
               ? 19u
               : 0u; /* R_RISCV_CALL_PLT */
  }
  if (r->kind == AST_OBJECT_ABS64 && r->offset <= section->size &&
      section->size - r->offset >= 8u) {
    uint64_t w = 0;
    for (unsigned i = 0; i < 8u; ++i)
      w |= (uint64_t)section->data.data[r->offset + i] << (8u * i);
    if (!w)
      return m->architecture == AST_OBJECT_AARCH64 ? 257u : 2u;
  }
  return 0;
}
static int section_name(Buffer *names, ElfSection *s, const char *value) {
  return str(names, (const uint8_t *)value, strlen(value), &s->name_offset);
}
static int write_impl(const AstObjectModule *m, AstObjectOutput *out) {
  if (!m || m->abi_version != AST_OBJECT_ABI_VERSION ||
      m->format != AST_OBJECT_ELF ||
      (m->architecture != AST_OBJECT_AARCH64 &&
       m->architecture != AST_OBJECT_RISCV64) ||
      m->section_count > 64u || m->symbol_count > 8192u ||
      m->relocation_count > 32768u || (m->section_count && !m->sections) ||
      (m->symbol_count && !m->symbols) ||
      (m->relocation_count && !m->relocations))
    return 0;
  const size_t nsrc = (size_t)m->section_count, nsym = (size_t)m->symbol_count;
  /* Worst case: a .rela.* companion for every input section. */
  size_t capacity = 1u + nsrc * 2u + 3u;
  ElfSection *secs = (ElfSection *)calloc(capacity, sizeof(*secs));
  uint32_t *symbol_map =
      (uint32_t *)calloc(nsym ? nsym : 1u, sizeof(*symbol_map));
  Buffer strtab = {0}, shstr = {0}, symtab = {0}, image = {0};
  int ok = secs && symbol_map && number(&strtab, 0, 1) &&
           number(&shstr, 0, 1) && put(&symtab, NULL, 24u);
  uint64_t total_section_bytes = 0u;
  uint32_t local_count = 1u;
  size_t num = 1u;
  for (size_t k = 0; ok && k < nsrc; ++k) {
    const AstObjectSection *src = &m->sections[k];
    ElfSection *s = &secs[num++];
    if (!src->name.data || !src->name.size || src->name.size > 255u ||
        (src->kind < AST_OBJECT_TEXT || src->kind > AST_OBJECT_BSS) ||
        src->size > UINT32_C(16777216) ||
        src->size > UINT32_C(67108864) - total_section_bytes ||
        !src->alignment || (src->alignment & (src->alignment - 1u)) ||
        src->alignment > 1048576u ||
        (src->kind != AST_OBJECT_BSS &&
         (src->data.size != src->size || (src->size && !src->data.data))) ||
        (src->kind == AST_OBJECT_BSS && src->data.size)) {
      ok = 0;
      break;
    }
    total_section_bytes += src->size;
    ok = str(&shstr, src->name.data, (size_t)src->name.size, &s->name_offset);
    s->type = src->kind == AST_OBJECT_BSS ? 8u : 1u;
    s->flags = src->kind == AST_OBJECT_TEXT     ? 6u
               : src->kind == AST_OBJECT_RODATA ? 2u
                                                : 3u;
    s->align = src->alignment;
    s->size = src->size;
    if (ok && s->type != 8u)
      ok = put(&s->body, src->data.data, (size_t)src->size);
  }
  /* STB_LOCAL entries precede globals; maintain explicit input->output map. */
  for (unsigned pass = 0; ok && pass < 2; ++pass)
    for (size_t k = 0; ok && k < nsym; ++k) {
      const AstObjectSymbol *src = &m->symbols[k];
      if ((!src->binding) != (!pass))
        continue;
      if (!src->name.data || !src->name.size || src->name.size > 255u ||
          (src->section >= nsrc && src->section != AST_OBJECT_UNDEFINED) ||
          (src->section < nsrc &&
           src->value > m->sections[src->section].size) ||
          src->kind < AST_OBJECT_SYMBOL_CODE ||
          src->kind > AST_OBJECT_SYMBOL_DATA ||
          src->binding > AST_OBJECT_GLOBAL) {
        ok = 0;
        break;
      }
      uint32_t name = 0u;
      if (!str(&strtab, src->name.data, (size_t)src->name.size, &name)) {
        ok = 0;
        break;
      }
      symbol_map[k] = (uint32_t)(symtab.used / 24u);
      uint16_t shndx = src->section == AST_OBJECT_UNDEFINED
                           ? 0u
                           : (uint16_t)(src->section + 1u);
      ok = number(&symtab, name, 4u) &&
           number(&symtab,
                  ((uint32_t)(src->binding ? 1u : 0u) << 4u) |
                      (src->kind == AST_OBJECT_SYMBOL_CODE ? 2u : 1u),
                  1u) &&
           number(&symtab, 0u, 1u) && number(&symtab, shndx, 2u) &&
           number(&symtab, src->value, 8u) && number(&symtab, src->size, 8u);
      if (!pass)
        ++local_count;
    }
  /* Every relocation section references an original input section. */
  for (size_t k = 0; ok && k < nsrc; ++k) {
    size_t found = 0u;
    for (size_t j = 0; j < (size_t)m->relocation_count; ++j)
      if (m->relocations[j].section == k)
        ++found;
    if (!found)
      continue;
    const AstObjectSection *src = &m->sections[k];
    if (src->name.size > 230u) {
      ok = 0;
      break;
    }
    char section_name_buf[256] = ".rela";
    memcpy(section_name_buf + 5u, src->name.data, (size_t)src->name.size);
    section_name_buf[5u + src->name.size] = 0;
    ElfSection *s = &secs[num++];
    ok = section_name(&shstr, s, section_name_buf);
    s->type = 4u;
    s->align = 8u;
    s->entsize = 24u;
    s->info = (uint32_t)(k + 1u);
    /* Reject duplicate/overlapping fixup sites before serializing the
       relocation table, even for adversarial source modules. */
    uint8_t *occupied =
        (uint8_t *)calloc((size_t)(src->size ? src->size : 1u), 1u);
    if (!occupied) {
      ok = 0;
      break;
    }
    for (size_t j = 0; ok && j < (size_t)m->relocation_count; ++j) {
      const AstObjectRelocation *rel = &m->relocations[j];
      if (rel->section != k)
        continue;
      if (rel->symbol >= nsym || rel->offset > src->size) {
        ok = 0;
        break;
      }
      uint32_t kind = reloc_type(m, rel, src);
      size_t width =
          rel->kind == AST_OBJECT_RV_CALL || rel->kind == AST_OBJECT_ABS64 ? 8u
                                                                           : 4u;
      if (!kind || rel->offset > src->size || width > src->size - rel->offset) {
        ok = 0;
        break;
      }
      for (size_t t = 0; t < width; ++t)
        if (occupied[(size_t)rel->offset + t])
          ok = 0;
      if (!ok)
        break;
      for (size_t t = 0; t < width; ++t)
        occupied[(size_t)rel->offset + t] = 1u;
      uint64_t info = ((uint64_t)symbol_map[rel->symbol] << 32u) | kind;
      ok = number(&s->body, rel->offset, 8u) && number(&s->body, info, 8u) &&
           number(&s->body, (uint64_t)rel->addend, 8u);
    }
    free(occupied);
    s->size = s->body.used;
  }
  size_t symidx = num++;
  if (ok) {
    secs[symidx].type = 2u;
    secs[symidx].align = 8u;
    secs[symidx].entsize = 24u;
    secs[symidx].info = local_count;
    secs[symidx].body = symtab;
    secs[symidx].size = symtab.used;
    symtab = (Buffer){0};
    ok = section_name(&shstr, &secs[symidx], ".symtab");
  }
  size_t stridx = num++;
  if (ok) {
    secs[stridx].type = 3u;
    secs[stridx].align = 1u;
    secs[stridx].body = strtab;
    secs[stridx].size = strtab.used;
    strtab = (Buffer){0};
    ok = section_name(&shstr, &secs[stridx], ".strtab");
  }
  size_t shidx = num++;
  if (ok) {
    secs[shidx].type = 3u;
    secs[shidx].align = 1u;
    ok = section_name(&shstr, &secs[shidx], ".shstrtab");
    secs[shidx].body = shstr;
    secs[shidx].size = shstr.used;
    shstr = (Buffer){0};
  }
  if (ok) {
    secs[symidx].link = (uint32_t)stridx;
    for (size_t j = 1; j < num; ++j)
      if (secs[j].type == 4u)
        secs[j].link = (uint32_t)symidx;
    ok = put(&image, NULL, 64u);
  }
  for (size_t j = 1; ok && j < num; ++j) {
    ElfSection *s = &secs[j];
    if (!aligned(&image, s->align)) {
      ok = 0;
      break;
    }
    s->offset = image.used;
    if (s->type != 8u)
      ok = put(&image, s->body.bytes, s->body.used);
  }
  if (ok)
    ok = aligned(&image, 8u);
  size_t shoff = image.used;
  for (size_t j = 0; ok && j < num; ++j)
    ok = section_header(&image, &secs[j]);
  if (ok) {
    uint8_t ident[16] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    memcpy(image.bytes, ident, 16);
    /* ELF64 ET_REL, e_machine, e_version, e_shoff, header sizes. */
    image.bytes[16] = 1;
    image.bytes[18] = m->architecture == AST_OBJECT_AARCH64 ? 183u : 243u;
    image.bytes[20] = 1;
    for (unsigned i = 0; i < 8u; ++i)
      image.bytes[40u + i] = (uint8_t)((uint64_t)shoff >> (8u * i));
    uint32_t flags = m->architecture == AST_OBJECT_RISCV64
                         ? (m->riscv_float_abi == 2u   ? 4u
                            : m->riscv_float_abi == 1u ? 2u
                                                       : 0u)
                         : 0u;
    for (unsigned i = 0; i < 4u; ++i)
      image.bytes[48u + i] = (uint8_t)(flags >> (8u * i));
    image.bytes[52] = 64;
    image.bytes[58] = 64;
    image.bytes[60] = (uint8_t)num;
    image.bytes[61] = (uint8_t)(num >> 8u);
    image.bytes[62] = (uint8_t)shidx;
    image.bytes[63] = (uint8_t)(shidx >> 8u);
    out->data = image.bytes;
    out->size = image.used;
    out->status = AST_OBJECT_OK;
    image.bytes = NULL;
  }
  for (size_t j = 0; j < capacity; ++j)
    free(secs ? secs[j].body.bytes : NULL);
  free(secs);
  free(symbol_map);
  free(strtab.bytes);
  free(shstr.bytes);
  free(symtab.bytes);
  free(image.bytes);
  return ok;
}
uint32_t aster_elf64_fallback_write(const AstObjectModule *m,
                                    AstObjectOutput *out) {
  if (!out)
    return AST_OBJECT_INVALID;
  memset(out, 0, sizeof(*out));
  if (write_impl(m, out))
    return AST_OBJECT_OK;
  out->status = AST_OBJECT_UNSUPPORTED;
  (void)snprintf(out->message, sizeof(out->message),
                 "Standalone C ELF64 fallback supports AArch64/RV64 "
                 "CALL26/CALL_PLT/ABS64 only; unsupported or malformed module");
  return out->status;
}
void aster_elf64_fallback_free(AstObjectOutput *out) {
  if (!out)
    return;
  free(out->data);
  memset(out, 0, sizeof(*out));
}
