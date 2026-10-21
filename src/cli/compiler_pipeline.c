#include "compiler_pipeline.h"
#include "../backend/common/backend_compile.h"
#include "compiler_paths.h"
#include "compiler_timing.h"

#include "asterasm/object_writer.h"
#ifndef ASTERASM_HAS_OBJECT_WRITER
#include "elf64_fallback.h"
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct BuildSection {
  const char *name;
  uint32_t name_length;
  uint32_t kind;
  uint64_t alignment;
  uint64_t offset;
  uint64_t size;
  uint8_t *data;
  size_t capacity;
} BuildSection;

typedef struct BuildSymbol {
  AstName module;
  AstName source_name;
  const char *emit_name;
  uint32_t emit_name_length;
  uint64_t offset;
  uint64_t address;
  uint64_t size;
  uint32_t section;
  uint32_t binding;
  uint32_t kind;
  uint8_t defined;
  uint8_t is_function;
  uint32_t alignment;
} BuildSymbol;

typedef struct DataRelocation {
  uint32_t section;
  uint64_t offset;
  AstName module;
  AstName symbol;
  int64_t addend;
} DataRelocation;

typedef struct BuildImage {
  const AstHirProgram *hir;
  const AstMirProgram *mir;
  AstEncoderModuleOutput encoded;
  BuildSection *sections;
  size_t section_count;
  size_t section_capacity;
  BuildSymbol *symbols;
  BuildSymbol **symbol_index;
  size_t symbol_count;
  size_t symbol_capacity;
  DataRelocation *data_relocs;
  size_t data_reloc_count;
  size_t data_reloc_capacity;
  uint64_t base_address;
  uint32_t pointer_bytes;
  uint32_t function_alignment;
  size_t omitted_functions;
  AstTargetProgram *target;
  AstAbiProgram *abi;
  AstMachineProgram *machine;
  AstRegAllocProgram *allocation;
  AstRegAllocStats allocation_stats;
  char backend_diagnostic[512];
} BuildImage;

static uint64_t align_up_u64(uint64_t value, uint64_t alignment, int *ok) {
  if (!alignment)
    alignment = 1u;
  if ((alignment & (alignment - 1u)) != 0u ||
      value > UINT64_MAX - (alignment - 1u)) {
    *ok = 0;
    return 0u;
  }
  return (value + alignment - 1u) & ~(alignment - 1u);
}

static int compare_name(AstName a, AstName b) {
  if (a.span.length != b.span.length)
    return a.span.length < b.span.length ? -1 : 1;
  if (!a.span.length)
    return 0;
  return memcmp(a.text, b.text, a.span.length);
}

static int grow_array(void **ptr, size_t *capacity, size_t count,
                      size_t item_size) {
  if (count < *capacity)
    return 1;
  size_t next = *capacity ? *capacity * 2u : 8u;
  if (next <= count)
    next = count + 1u;
  if (next > SIZE_MAX / item_size)
    return 0;
  void *p = realloc(*ptr, next * item_size);
  if (!p)
    return 0;
  *ptr = p;
  *capacity = next;
  return 1;
}

static int section_reserve(BuildSection *section, uint64_t required) {
  if (section->kind == 4u)
    return required <= SIZE_MAX;
  if (required > SIZE_MAX)
    return 0;
  size_t need = (size_t)required;
  if (need <= section->capacity)
    return 1;
  size_t next = section->capacity ? section->capacity : 64u;
  while (next < need) {
    if (next > SIZE_MAX / 2u) {
      next = need;
      break;
    }
    next *= 2u;
  }
  uint8_t *p = (uint8_t *)realloc(section->data, next);
  if (!p)
    return 0;
  if (next > section->capacity)
    memset(p + section->capacity, 0, next - section->capacity);
  section->data = p;
  section->capacity = next;
  return 1;
}

static uint32_t add_section(BuildImage *image, const char *name,
                            uint32_t name_length, uint32_t kind,
                            uint64_t alignment) {
  for (size_t i = 0; i < image->section_count; ++i) {
    BuildSection *section = &image->sections[i];
    if (section->kind == kind && section->name_length == name_length &&
        memcmp(section->name, name, name_length) == 0) {
      if (alignment > section->alignment)
        section->alignment = alignment;
      return (uint32_t)i;
    }
  }
  if (image->section_count > UINT32_MAX ||
      !grow_array((void **)&image->sections, &image->section_capacity,
                  image->section_count, sizeof(*image->sections)))
    return UINT32_MAX;
  BuildSection *section = &image->sections[image->section_count];
  memset(section, 0, sizeof(*section));
  section->name = name;
  section->name_length = name_length;
  section->kind = kind;
  section->alignment = alignment ? alignment : 1u;
  return (uint32_t)image->section_count++;
}

static BuildSymbol *add_symbol(BuildImage *image) {
  if (image->symbol_count > UINT32_MAX ||
      !grow_array((void **)&image->symbols, &image->symbol_capacity,
                  image->symbol_count, sizeof(*image->symbols)))
    return NULL;
  BuildSymbol *symbol = &image->symbols[image->symbol_count++];
  memset(symbol, 0, sizeof(*symbol));
  symbol->section = UINT32_MAX;
  return symbol;
}

static int add_data_relocation(BuildImage *image, uint32_t section,
                               uint64_t offset,
                               const AstHirInitializerInfo *info) {
  if (!grow_array((void **)&image->data_relocs, &image->data_reloc_capacity,
                  image->data_reloc_count, sizeof(*image->data_relocs)))
    return 0;
  DataRelocation *reloc = &image->data_relocs[image->data_reloc_count++];
  reloc->section = section;
  reloc->offset = offset;
  reloc->module = info->module;
  reloc->symbol = info->symbol;
  reloc->addend = info->addend;
  return 1;
}

static int write_integer(uint8_t *destination, uint64_t size,
                         AstSemaConstInfo constant) {
  if (size > 16u)
    return 0;
  uint64_t lo = constant.lo, hi = constant.hi;
  if (constant.is_negative) {
    lo = ~lo + UINT64_C(1);
    hi = ~hi + (lo == 0u ? UINT64_C(1) : UINT64_C(0));
  }
  for (uint64_t i = 0; i < size; ++i) {
    destination[i] = (uint8_t)(i < 8u ? lo >> (8u * i) : hi >> (8u * (i - 8u)));
  }
  return 1;
}

static int serialize_initializer(BuildImage *image, uint32_t section_index,
                                 uint64_t base,
                                 const AstHirInitializer *initializer,
                                 const AstSemaType *type);

static int serialize_aggregate(BuildImage *image, uint32_t section_index,
                               uint64_t base,
                               const AstHirInitializer *initializer,
                               const AstHirInitializerInfo *info,
                               const AstSemaType *type) {
  /* Sections are initially zeroed. A short initializer supplies a prefix;
   * the omitted array elements/fields (and their padding) stay zero. */
  AstSemaTypeKind kind = ast_sema_type_kind(type);
  if (kind == AST_SEMA_TYPE_ARRAY) {
    const AstSemaType *element = ast_sema_type_element(type);
    uint64_t count = 0u, element_size = ast_sema_type_size(element);
    if (!element || !element_size || !ast_sema_type_array_count(type, &count) ||
        info->child_count > count)
      return 0;
    for (uint32_t i = 0; i < info->child_count; ++i) {
      const AstHirInitializer *child =
          ast_hir_initializer_child(initializer, i);
      if (!child || !serialize_initializer(image, section_index,
                                           base + (uint64_t)i * element_size,
                                           child, element))
        return 0;
    }
    return 1;
  }
  if (kind == AST_SEMA_TYPE_TUPLE) {
    uint32_t count = ast_sema_type_tuple_count(type);
    if (info->child_count > count)
      return 0;
    uint64_t offset = 0u;
    for (uint32_t i = 0; i < info->child_count; ++i) {
      const AstSemaType *item = ast_sema_type_tuple_item(type, i);
      uint64_t alignment = ast_sema_type_alignment(item);
      int ok = 1;
      offset = align_up_u64(offset, alignment, &ok);
      const AstHirInitializer *child =
          ast_hir_initializer_child(initializer, i);
      if (!ok || !item || !child ||
          !serialize_initializer(image, section_index, base + offset, child,
                                 item))
        return 0;
      if (offset > UINT64_MAX - ast_sema_type_size(item))
        return 0;
      offset += ast_sema_type_size(item);
    }
    return 1;
  }
  if (kind == AST_SEMA_TYPE_STRUCT) {
    uint32_t count = ast_sema_type_struct_field_count(type);
    if (info->child_count > count)
      return 0;
    for (uint32_t i = 0; i < info->child_count; ++i) {
      AstSemaFieldInfo field;
      const AstHirInitializer *child =
          ast_hir_initializer_child(initializer, i);
      if (!child || !ast_sema_type_struct_field(type, i, &field) ||
          !serialize_initializer(image, section_index, base + field.offset,
                                 child, field.type))
        return 0;
    }
    return 1;
  }
  return 0;
}

static int serialize_initializer(BuildImage *image, uint32_t section_index,
                                 uint64_t base,
                                 const AstHirInitializer *initializer,
                                 const AstSemaType *type) {
  AstHirInitializerInfo info;
  if (!initializer || section_index >= image->section_count ||
      !ast_hir_initializer_info(initializer, &info) || !type)
    return 0;
  BuildSection *section = &image->sections[section_index];
  uint64_t size = ast_sema_type_size(type);
  if (size == UINT64_MAX || base > section->size || size > section->size - base)
    return 0;
  if (section->kind == 4u)
    return info.kind == AST_HIR_INIT_ZERO || info.kind == AST_HIR_INIT_NONE;
  if (!section_reserve(section, section->size))
    return 0;
  uint8_t *destination = section->data + (size_t)base;
  switch (info.kind) {
  case AST_HIR_INIT_NONE:
  case AST_HIR_INIT_ZERO:
    return 1;
  case AST_HIR_INIT_CONSTANT:
    if (info.constant.kind == AST_SEMA_CONST_FLOAT) {
      if (size == 4u) {
        float value = (float)info.constant.floating;
        memcpy(destination, &value, 4u);
        return 1;
      }
      if (size == 8u) {
        double value = info.constant.floating;
        memcpy(destination, &value, 8u);
        return 1;
      }
      return 0;
    }
    if (info.constant.kind == AST_SEMA_CONST_INTEGER ||
        info.constant.kind == AST_SEMA_CONST_BOOL)
      return write_integer(destination, size, info.constant);
    return 0;
  case AST_HIR_INIT_LITERAL:
    if (info.literal.length > size)
      return 0;
    memcpy(destination, info.literal.data, info.literal.length);
    return 1;
  case AST_HIR_INIT_SYMBOL:
    if (size != image->pointer_bytes)
      return 0;
    memset(destination, 0, (size_t)size);
    return add_data_relocation(image, section_index, base, &info);
  case AST_HIR_INIT_AGGREGATE:
    return serialize_aggregate(image, section_index, base, initializer, &info,
                               type);
  default:
    return 0;
  }
}

static int add_functions(BuildImage *image) {
  const size_t count = ast_mir_function_count(image->mir);
  if (image->encoded.function_count != count)
    return 0;
  BuildSection *text = &image->sections[0];
  int ok = 1;
  uint64_t cursor = 0u;
  for (size_t f = 0; f < count; ++f) {
    const AstMirFunction *function = ast_mir_function_at(image->mir, f);
    AstMirFunctionInfo info;
    if (!function || !ast_mir_function_info(function, &info))
      return 0;
    BuildSymbol *symbol = add_symbol(image);
    if (!symbol)
      return 0;
    symbol->module = info.module_name;
    symbol->source_name = info.name;
    if (info.link_name.is_present) {
      symbol->emit_name = info.link_name.text.data;
      symbol->emit_name_length = info.link_name.text.length;
    } else {
      symbol->emit_name = info.name.text;
      symbol->emit_name_length = info.name.span.length;
    }
    symbol->binding =
        (info.flags & (AST_HIR_FUNCTION_EXPORT | AST_HIR_FUNCTION_EXTERN)) ? 1u
                                                                           : 0u;
    symbol->kind = 1u;
    symbol->is_function = 1u;
    symbol->alignment = image->function_alignment;
    for (uint32_t i = 0; i < info.instruction_count; ++i) {
      AstMirInstructionInfo inst;
      if (!ast_mir_function_instruction(function, i, &inst))
        return 0;
      if (inst.op == AST_MIR_OP_NATIVE || inst.op == AST_MIR_OP_NATIVE_RESULT)
        symbol->alignment = 16u;
    }
    if (info.flags & AST_HIR_FUNCTION_EXTERN) {
      symbol->defined = 0u;
      continue;
    }
    cursor = align_up_u64(cursor, symbol->alignment, &ok);
    if (!ok || image->encoded.functions[f].size > UINT64_MAX - cursor)
      return 0;
    symbol->defined = 1u;
    symbol->section = 0u;
    symbol->offset = cursor;
    symbol->size = image->encoded.functions[f].size;
    cursor += image->encoded.functions[f].size;
  }
  text->size = cursor;
  if (!section_reserve(text, text->size))
    return 0;
  for (size_t f = 0; f < count; ++f) {
    BuildSymbol *symbol = &image->symbols[f];
    if (!symbol->defined || !symbol->size)
      continue;
    memcpy(text->data + (size_t)symbol->offset,
           image->encoded.functions[f].bytes, (size_t)symbol->size);
  }
  return 1;
}

static uint32_t global_section_kind(const AstHirGlobalInfo *global) {
  if (global->flags & AST_HIR_GLOBAL_BSS)
    return 4u;
  if (global->flags & (AST_HIR_GLOBAL_READONLY | AST_HIR_GLOBAL_CONST))
    return 2u;
  return 3u;
}

static int add_globals(BuildImage *image) {
  const size_t count = ast_hir_global_count(image->hir);
  for (size_t g = 0; g < count; ++g) {
    AstHirGlobalInfo global;
    if (!ast_hir_global_at(image->hir, g, &global))
      return 0;
    uint64_t size = ast_sema_type_size(global.type);
    uint64_t alignment = global.alignment
                             ? global.alignment
                             : ast_sema_type_alignment(global.type);
    uint32_t kind = global_section_kind(&global);
    const char *name = global.section_name.data;
    uint32_t name_length = global.section_name.length;
    if (!name_length) {
      if (kind == 2u) {
        name = ".rodata";
        name_length = 7u;
      } else if (kind == 4u) {
        name = ".bss";
        name_length = 4u;
      } else {
        name = ".data";
        name_length = 5u;
      }
    }
    uint64_t section_alignment = global.section_alignment > alignment
                                     ? global.section_alignment
                                     : alignment;
    uint32_t section_index =
        add_section(image, name, name_length, kind, section_alignment);
    if (section_index == UINT32_MAX || size == UINT64_MAX)
      return 0;
    BuildSection *section = &image->sections[section_index];
    int ok = 1;
    uint64_t offset = align_up_u64(section->size, alignment, &ok);
    if (!ok || size > UINT64_MAX - offset)
      return 0;
    section->size = offset + size;
    if (!section_reserve(section, section->size))
      return 0;
    BuildSymbol *symbol = add_symbol(image);
    if (!symbol)
      return 0;
    symbol->module = global.module_name;
    symbol->source_name = global.name;
    symbol->emit_name = global.name.text;
    symbol->emit_name_length = global.name.span.length;
    symbol->offset = offset;
    symbol->size = size;
    symbol->section = section_index;
    symbol->binding = (global.flags & AST_HIR_GLOBAL_EXPORT) ? 1u : 0u;
    symbol->kind = 2u;
    symbol->defined = 1u;
    const AstHirInitializer *initializer =
        ast_hir_global_initializer(image->hir, g);
    if (initializer && !serialize_initializer(image, section_index, offset,
                                              initializer, global.type)) {
      fprintf(stderr, "asterc: cannot serialize initializer for %.*s\n",
              (int)global.name.span.length, global.name.text);
      return 0;
    }
  }
  return 1;
}

static int compare_symbol_pointer(const void *left, const void *right) {
  const BuildSymbol *a = *(BuildSymbol *const *)left;
  const BuildSymbol *b = *(BuildSymbol *const *)right;
  int order = compare_name(a->source_name, b->source_name);
  return order ? order : compare_name(a->module, b->module);
}

/* Build only after all symbols have been appended: the pointers then remain
 * stable. Sorting this separate index preserves object symbol/output order.
 * Relocation lookup becomes O(log S) instead of scanning S symbols per use. */
static int index_symbols(BuildImage *image) {
  if (!image->symbol_count)
    return 1;
  if (image->symbol_count > SIZE_MAX / sizeof(*image->symbol_index))
    return 0;
  image->symbol_index = (BuildSymbol **)malloc(image->symbol_count *
                                               sizeof(*image->symbol_index));
  if (!image->symbol_index)
    return 0;
  for (size_t i = 0; i < image->symbol_count; ++i)
    image->symbol_index[i] = &image->symbols[i];
  qsort(image->symbol_index, image->symbol_count, sizeof(*image->symbol_index),
        compare_symbol_pointer);
  return 1;
}

static BuildSymbol *find_symbol_exact(BuildImage *image, AstName module,
                                      AstName name) {
  if (!name.text || !image->symbol_index)
    return NULL;
  int qualified = module.text && module.span.length;
  size_t lo = 0u, hi = image->symbol_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    BuildSymbol *candidate = image->symbol_index[mid];
    int order = compare_name(candidate->source_name, name);
    if (!order && qualified)
      order = compare_name(candidate->module, module);
    if (order < 0)
      lo = mid + 1u;
    else
      hi = mid;
  }
  if (lo == image->symbol_count)
    return NULL;
  BuildSymbol *match = image->symbol_index[lo];
  if (compare_name(match->source_name, name) ||
      (qualified && compare_name(match->module, module)))
    return NULL;
  /* Preserve the existing fail-closed ambiguity contract. */
  if (lo + 1u < image->symbol_count) {
    BuildSymbol *next = image->symbol_index[lo + 1u];
    if (!compare_name(next->source_name, name) &&
        (!qualified || !compare_name(next->module, module)))
      return NULL;
  }
  return match;
}

/* Collect from actual emitted relocations, including data initializers and
 * native symbol references. Function indices stay stable for backend proofs. */
static int compact_functions(BuildImage *image) {
  size_t count = image->encoded.function_count;
  if (count > SIZE_MAX / sizeof(size_t))
    return 0;
  uint8_t *live = (uint8_t *)calloc(count ? count : 1u, 1u);
  size_t *queue = (size_t *)malloc((count ? count : 1u) * sizeof(*queue));
  if (!live || !queue) {
    free(live);
    free(queue);
    return 0;
  }
  size_t head = 0, tail = 0;
  int entry = 0, ok = 1;
  for (size_t f = 0; f < count; ++f) {
    BuildSymbol *symbol = &image->symbols[f];
    AstMirFunctionInfo info;
    if (!ast_mir_function_info(ast_mir_function_at(image->mir, f), &info)) {
      ok = 0;
      break;
    }
    if (symbol->binding || info.link_name.is_present ||
        (!entry && symbol->defined && symbol->size)) {
      live[f] = 1u;
      queue[tail++] = f;
    }
    if (symbol->defined && symbol->size)
      entry = 1;
  }
  for (size_t r = 0; r < image->data_reloc_count && ok; ++r) {
    const DataRelocation *reloc = &image->data_relocs[r];
    BuildSymbol *symbol =
        find_symbol_exact(image, reloc->module, reloc->symbol);
    if (!symbol) {
      ok = 0;
      break;
    }
    size_t f = (size_t)(symbol - image->symbols);
    if (f < count && !live[f]) {
      live[f] = 1u;
      queue[tail++] = f;
    }
  }
  while (head < tail && ok) {
    size_t owner = queue[head++];
    for (size_t r = 0; r < image->encoded.functions[owner].fixup_count; ++r) {
      const AstEncoderFixup *fixup = &image->encoded.functions[owner].fixups[r];
      BuildSymbol *symbol =
          find_symbol_exact(image, fixup->module, fixup->symbol);
      if (!symbol) {
        ok = 0;
        break;
      }
      size_t target = (size_t)(symbol - image->symbols);
      if (target < count && !live[target]) {
        live[target] = 1u;
        queue[tail++] = target;
      }
    }
  }
  uint64_t cursor = 0;
  for (size_t f = 0; f < count && ok; ++f) {
    BuildSymbol *symbol = &image->symbols[f];
    if (!symbol->defined)
      continue;
    if (!live[f]) {
      symbol->defined = 0;
      symbol->size = 0;
      image->omitted_functions++;
      continue;
    }
    cursor = align_up_u64(cursor, symbol->alignment, &ok);
    if (!ok || symbol->size > UINT64_MAX - cursor) {
      ok = 0;
      break;
    }
    symbol->offset = cursor;
    cursor += symbol->size;
  }
  if (ok) {
    BuildSection *text = &image->sections[0];
    if (!section_reserve(text, cursor))
      ok = 0;
    else {
      if (cursor)
        memset(text->data, 0, (size_t)cursor);
      text->size = cursor;
      for (size_t f = 0; f < count; ++f) {
        const BuildSymbol *symbol = &image->symbols[f];
        if (symbol->defined && symbol->size)
          memcpy(text->data + (size_t)symbol->offset,
                 image->encoded.functions[f].bytes, (size_t)symbol->size);
      }
    }
  }
  free(live);
  free(queue);
  return ok;
}

static int layout_sections(BuildImage *image) {
  uint64_t cursor = 0u;
  for (size_t i = 0; i < image->section_count; ++i) {
    int ok = 1;
    BuildSection *section = &image->sections[i];
    cursor = align_up_u64(cursor, section->alignment, &ok);
    if (!ok || section->size > UINT64_MAX - cursor ||
        image->base_address > UINT64_MAX - cursor)
      return 0;
    section->offset = cursor;
    cursor += section->size;
  }
  for (size_t i = 0; i < image->symbol_count; ++i) {
    BuildSymbol *symbol = &image->symbols[i];
    if (!symbol->defined)
      continue;
    if (symbol->section >= image->section_count)
      return 0;
    BuildSection *section = &image->sections[symbol->section];
    if (symbol->offset > section->size ||
        symbol->size > section->size - symbol->offset ||
        image->base_address > UINT64_MAX - section->offset ||
        image->base_address + section->offset > UINT64_MAX - symbol->offset)
      return 0;
    symbol->address = image->base_address + section->offset + symbol->offset;
  }
  return 1;
}

static int resolve_encoder_symbol(void *user, AstName module, AstName name,
                                  uint64_t *address) {
  BuildImage *image = (BuildImage *)user;
  BuildSymbol *symbol = find_symbol_exact(image, module, name);
  if (!symbol || !symbol->defined)
    return 0;
  *address = symbol->address;
  return 1;
}

static int write_address_le(uint8_t *destination, uint64_t value,
                            uint32_t bytes) {
  if (!destination)
    return 0;
  if (bytes == 4u && value > UINT32_MAX)
    return 0;
  for (unsigned i = 0; i < bytes; ++i)
    destination[i] = (uint8_t)(value >> (8u * i));
  return 1;
}

static int apply_data_relocations(BuildImage *image) {
  for (size_t r = 0; r < image->data_reloc_count; ++r) {
    DataRelocation *reloc = &image->data_relocs[r];
    BuildSymbol *symbol =
        find_symbol_exact(image, reloc->module, reloc->symbol);
    if (!symbol || !symbol->defined || reloc->section >= image->section_count) {
      fprintf(stderr, "asterc: unresolved data symbol: %.*s\n",
              (int)reloc->symbol.span.length, reloc->symbol.text);
      return 0;
    }
    BuildSection *section = &image->sections[reloc->section];
    if (section->kind == 4u || reloc->offset > section->size ||
        section->size - reloc->offset < image->pointer_bytes)
      return 0;
    uint64_t value;
    if (reloc->addend >= 0) {
      uint64_t add = (uint64_t)reloc->addend;
      if (symbol->address > UINT64_MAX - add)
        return 0;
      value = symbol->address + add;
    } else {
      uint64_t sub = (uint64_t)(-(reloc->addend + 1)) + 1u;
      if (symbol->address < sub)
        return 0;
      value = symbol->address - sub;
    }
    if (!write_address_le(section->data + (size_t)reloc->offset, value,
                          image->pointer_bytes))
      return 0;
  }
  return 1;
}

static int write_bytes_file(const char *path, const uint8_t *bytes,
                            size_t size) {
  FILE *file = ast_fopen(path, "wb");
  if (!file) {
    char error_text[256];
    fprintf(stderr, "asterc: cannot open %s: %s\n", path,
            ast_strerror(errno, error_text, sizeof(error_text)));
    return 0;
  }
  int ok = !size || fwrite(bytes, 1u, size, file) == size;
  if (fclose(file) != 0)
    ok = 0;
  if (!ok)
    fprintf(stderr, "asterc: cannot write %s\n", path);
  return ok;
}

static int emit_binary(BuildImage *image, const char *path) {
  for (size_t f = 0; f < image->encoded.function_count; ++f) {
    BuildSymbol *symbol = &image->symbols[f];
    if (!symbol->defined)
      continue;
    if (!ast_encoder_apply_fixups_qualified(&image->encoded.functions[f],
                                            symbol->address,
                                            resolve_encoder_symbol, image)) {
      const char *message = image->encoded.functions[f].message;
      fprintf(stderr,
              "asterc: unresolved/invalid code relocation in %.*s%s%s\n",
              (int)symbol->source_name.span.length, symbol->source_name.text,
              message ? ": " : "", message ? message : "");
      return 0;
    }
    if (symbol->size &&
        memcmp(image->sections[0].data + (size_t)symbol->offset,
               image->encoded.functions[f].bytes, (size_t)symbol->size) != 0) {
      memcpy(image->sections[0].data + (size_t)symbol->offset,
             image->encoded.functions[f].bytes, (size_t)symbol->size);
    }
  }
  if (!apply_data_relocations(image))
    return 0;
  uint64_t total = 0u;
  for (size_t i = 0; i < image->section_count; ++i) {
    BuildSection *section = &image->sections[i];
    if (section->offset > UINT64_MAX - section->size)
      return 0;
    uint64_t end = section->offset + section->size;
    if (end > total)
      total = end;
  }
  if (total > SIZE_MAX)
    return 0;
  uint8_t *binary = (uint8_t *)calloc((size_t)(total ? total : 1u), 1u);
  if (!binary)
    return 0;
  for (size_t i = 0; i < image->section_count; ++i) {
    BuildSection *section = &image->sections[i];
    if (section->kind != 4u && section->size)
      memcpy(binary + (size_t)section->offset, section->data,
             (size_t)section->size);
  }
  int ok = write_bytes_file(path, binary, (size_t)total);
  free(binary);
  return ok;
}

static uint32_t object_relocation_kind(AstEncoderFixupKind kind) {
  switch (kind) {
  case AST_ENCODER_FIXUP_PC_REL32:
    return AST_OBJECT_PCREL32;
  case AST_ENCODER_FIXUP_X86_CALL_REL32:
    return AST_OBJECT_X86_CALL32;
  case AST_ENCODER_FIXUP_ABS32:
    return AST_OBJECT_ABS32;
  case AST_ENCODER_FIXUP_ABS64:
    return AST_OBJECT_ABS64;
  case AST_ENCODER_FIXUP_A64_CALL26:
    return AST_OBJECT_A64_CALL26;
  case AST_ENCODER_FIXUP_A64_ADR21:
    return AST_OBJECT_A64_ADR21;
  case AST_ENCODER_FIXUP_A64_ADRP_ADD:
    return AST_OBJECT_A64_ADRP_ADD;
  case AST_ENCODER_FIXUP_RISCV_CALL:
    return AST_OBJECT_RV_CALL;
  case AST_ENCODER_FIXUP_RISCV_PCREL_ADDRESS:
    return AST_OBJECT_RV_ADDRESS;
  default:
    return UINT32_MAX;
  }
}

static uint32_t find_symbol_index(BuildImage *image, BuildSymbol *symbol) {
  if (!symbol || symbol < image->symbols ||
      symbol >= image->symbols + image->symbol_count)
    return UINT32_MAX;
  size_t index = (size_t)(symbol - image->symbols);
  return index <= UINT32_MAX ? (uint32_t)index : UINT32_MAX;
}

static int emit_object(BuildImage *image, const AsterCompilerOptions *options,
                       uint64_t *measured_size) {
  size_t code_reloc_count = 0u;
  for (size_t f = 0; f < image->encoded.function_count; ++f) {
    if (image->encoded.functions[f].fixup_count > SIZE_MAX - code_reloc_count)
      return 0;
    code_reloc_count += image->encoded.functions[f].fixup_count;
  }
  if (image->data_reloc_count > SIZE_MAX - code_reloc_count)
    return 0;
  size_t reloc_count = code_reloc_count + image->data_reloc_count;
  AstObjectSection *sections = (AstObjectSection *)calloc(
      image->section_count ? image->section_count : 1u, sizeof(*sections));
  AstObjectSymbol *symbols = (AstObjectSymbol *)calloc(
      image->symbol_count ? image->symbol_count : 1u, sizeof(*symbols));
  AstObjectRelocation *relocs = (AstObjectRelocation *)calloc(
      reloc_count ? reloc_count : 1u, sizeof(*relocs));
  uint32_t *symbol_map = (uint32_t *)calloc(
      image->symbol_count ? image->symbol_count : 1u, sizeof(*symbol_map));
  if (!sections || !symbols || !relocs || !symbol_map) {
    free(sections);
    free(symbols);
    free(relocs);
    free(symbol_map);
    return 0;
  }
  for (size_t i = 0; i < image->section_count; ++i) {
    BuildSection *source = &image->sections[i];
    sections[i].name =
        (AstObjectBytes){(const uint8_t *)source->name, source->name_length};
    sections[i].data =
        (AstObjectBytes){source->kind == AST_OBJECT_BSS ? NULL : source->data,
                         source->kind == AST_OBJECT_BSS ? 0u : source->size};
    sections[i].size = source->size;
    sections[i].alignment = source->alignment;
    sections[i].kind = source->kind;
  }
  for (size_t i = 0; i < image->symbol_count; ++i) {
    BuildSymbol *source = &image->symbols[i];
    symbols[i].name = (AstObjectBytes){(const uint8_t *)source->emit_name,
                                       source->emit_name_length};
    symbols[i].value = source->defined ? source->offset : 0u;
    symbols[i].size = source->size;
    symbols[i].section =
        source->defined ? source->section : AST_OBJECT_UNDEFINED;
    symbols[i].binding = source->binding ? AST_OBJECT_GLOBAL : AST_OBJECT_LOCAL;
    symbols[i].kind =
        source->is_function ? AST_OBJECT_SYMBOL_CODE : AST_OBJECT_SYMBOL_DATA;
  }
  size_t r = 0u;
  for (size_t f = 0; f < image->encoded.function_count; ++f) {
    BuildSymbol *owner = &image->symbols[f];
    if (!owner->defined)
      continue;
    for (size_t n = 0; n < image->encoded.functions[f].fixup_count; ++n) {
      const AstEncoderFixup *fixup = &image->encoded.functions[f].fixups[n];
      BuildSymbol *target =
          find_symbol_exact(image, fixup->module, fixup->symbol);
      uint32_t target_index = find_symbol_index(image, target);
      uint32_t kind = object_relocation_kind(fixup->kind);
      if (target_index == UINT32_MAX || kind == UINT32_MAX)
        goto fail;
      relocs[r].section = owner->section;
      relocs[r].offset = owner->offset + fixup->offset;
      relocs[r].symbol = target_index;
      relocs[r].kind = kind;
      relocs[r].addend = fixup->addend;
      ++r;
    }
  }
  for (size_t n = 0; n < image->data_reloc_count; ++n) {
    DataRelocation *source = &image->data_relocs[n];
    BuildSymbol *target =
        find_symbol_exact(image, source->module, source->symbol);
    uint32_t target_index = find_symbol_index(image, target);
    if (target_index == UINT32_MAX)
      goto fail;
    relocs[r].section = source->section;
    relocs[r].offset = source->offset;
    relocs[r].symbol = target_index;
    relocs[r].kind =
        image->pointer_bytes == 4u ? AST_OBJECT_ABS32 : AST_OBJECT_ABS64;
    relocs[r].addend = source->addend;
    ++r;
  }
  /* An unused extern declaration is not a link dependency. COFF linkers
   * resolve every undefined symbol, even without a relocation to it. Keep
   * all definitions and only the externs used by code or data relocations. */
  for (size_t i = 0; i < image->symbol_count; ++i)
    if (image->symbols[i].defined)
      symbol_map[i] = 1u;
  for (size_t i = 0; i < r; ++i)
    symbol_map[relocs[i].symbol] = 1u;
  size_t emitted_symbol_count = 0u;
  for (size_t i = 0; i < image->symbol_count; ++i) {
    if (!symbol_map[i])
      continue;
    if (emitted_symbol_count > UINT32_MAX)
      goto fail;
    symbols[emitted_symbol_count] = symbols[i];
    symbol_map[i] = (uint32_t)emitted_symbol_count++;
  }
  for (size_t i = 0; i < r; ++i)
    relocs[i].symbol = symbol_map[relocs[i].symbol];
  AstObjectModule module;
  memset(&module, 0, sizeof(module));
  module.abi_version = AST_OBJECT_ABI_VERSION;
  module.format = options->object_format;
  module.architecture =
      options->target.arch == AST_TARGET_ARCH_X86_32 ||
              options->target.arch == AST_TARGET_ARCH_X86_16
          ? AST_OBJECT_I386
      : options->target.arch == AST_TARGET_ARCH_X86_64  ? AST_OBJECT_X86_64
      : options->target.arch == AST_TARGET_ARCH_AARCH64 ? AST_OBJECT_AARCH64
                                                        : AST_OBJECT_RISCV64;
  module.sections = sections;
  module.section_count = image->section_count;
  module.symbols = symbols;
  module.symbol_count = emitted_symbol_count;
  module.relocations = relocs;
  module.relocation_count = r;
  module.base_address = image->base_address;
  module.image_limit = 0u;
  module.entry_symbol = AST_OBJECT_NO_ENTRY;
  module.riscv_float_abi = options->abi == AST_ABI_RISCV_LP64D ||
                                   options->abi == AST_ABI_RISCV_LP64DV
                               ? 2u
                           : options->abi == AST_ABI_RISCV_LP64F ||
                                   options->abi == AST_ABI_RISCV_LP64FV
                               ? 1u
                               : 0u;
  AstObjectOutput output;
  memset(&output, 0, sizeof(output));
#ifdef ASTERASM_HAS_OBJECT_WRITER
  uint32_t status = ast_object_write(&module, &output);
#else
  uint32_t status = aster_elf64_fallback_write(&module, &output);
#endif
  int ok = status == AST_OBJECT_OK && output.size <= SIZE_MAX;
  if (ok && measured_size)
    *measured_size = output.size;
  else if (ok)
    ok = write_bytes_file(options->output_path, output.data,
                          (size_t)output.size);
  if (status != AST_OBJECT_OK)
    fprintf(stderr, "asterc: object writer: %s\n", output.message);
#ifdef ASTERASM_HAS_OBJECT_WRITER
  ast_object_output_free(&output);
#else
  aster_elf64_fallback_free(&output);
#endif
  free(sections);
  free(symbols);
  free(relocs);
  free(symbol_map);
  return ok;
fail:
  free(sections);
  free(symbols);
  free(relocs);
  free(symbol_map);
  return 0;
}

static void image_release_backend(BuildImage *image) {
  if (image->allocation)
    image->allocation_stats = ast_regalloc_stats(image->allocation);
  ast_regalloc_destroy(image->allocation);
  ast_machine_destroy(image->machine);
  ast_abi_destroy(image->abi);
  ast_target_destroy(image->target);
  image->allocation = NULL;
  image->machine = NULL;
  image->abi = NULL;
  image->target = NULL;
}
static void image_destroy(BuildImage *image) {
  if (!image)
    return;
  ast_encoder_module_output_destroy(&image->encoded);
  for (size_t i = 0; i < image->section_count; ++i)
    free(image->sections[i].data);
  free(image->sections);
  free(image->symbol_index);
  free(image->symbols);
  free(image->data_relocs);
  image_release_backend(image);
  memset(image, 0, sizeof(*image));
}

static void print_hir_diagnostics(const AstHirProgram *hir) {
  size_t count = 0u;
  const AstHirDiagnostic *diags = ast_hir_diagnostics(hir, &count);
  for (size_t i = 0; i < count; ++i)
    fprintf(stderr, "asterc: HIR: %s\n", diags[i].message);
}

static void print_mir_diagnostics(const AstMirProgram *mir) {
  size_t count = 0u;
  const AstMirDiagnostic *diags = ast_mir_diagnostics(mir, &count);
  for (size_t i = 0; i < count; ++i)
    fprintf(stderr, "asterc: MIR: %s\n", diags[i].message);
}

static void print_target_diagnostics(const AstTargetProgram *target) {
  size_t count = 0u;
  const AstTargetDiagnostic *diags = ast_target_diagnostics(target, &count);
  for (size_t i = 0; i < count; ++i)
    fprintf(stderr, "asterc: target: %s\n", diags[i].message);
}

static void print_abi_diagnostics(const AstAbiProgram *abi) {
  size_t count = 0u;
  const AstAbiDiagnostic *diags = ast_abi_diagnostics(abi, &count);
  for (size_t i = 0; i < count; ++i)
    fprintf(stderr, "asterc: ABI: %s\n", diags[i].message);
}

static void print_regalloc_diagnostics(const AstRegAllocProgram *allocation,
                                       const AstMachineProgram *machine) {
  size_t count = 0u;
  const AstRegAllocDiagnostic *diags =
      ast_regalloc_diagnostics(allocation, &count);
  for (size_t i = 0; i < count; ++i) {
    AstMachineFunctionInfo fn = {0};
    AstMachineInstructionInfo inst = {0};
    (void)ast_machine_function_info(machine, diags[i].function_index, &fn);
    (void)ast_machine_function_instruction(machine, diags[i].function_index,
                                           diags[i].instruction, &inst);
    fprintf(stderr, "asterc: regalloc: %.*s instruction %u (%s): %s\n",
            (int)fn.name.span.length, fn.name.text ? fn.name.text : "",
            diags[i].instruction, ast_machine_opcode_name(inst.opcode),
            diags[i].message);
  }
}

static void compiler_dump_stdout(void *user, const char *bytes, size_t length) {
  (void)fwrite(bytes, 1u, length, (FILE *)user);
}

static int compiler_codegen_image(AstHirProgram *hir, AstMirProgram *mir,
                                  const AsterCompilerOptions *options,
                                  BuildImage *image) {
  CompilerBackendProfile *profile = options->backend_profile;
  double phase_begin = compiler_seconds();
  if (profile)
    ++profile->candidates;
  image->hir = hir;
  image->mir = mir;
  image->base_address = options->base_address;
  image->pointer_bytes = options->target.pointer_bits / 8u;
  image->function_alignment =
      options->target.arch == AST_TARGET_ARCH_AARCH64 ||
              options->target.arch == AST_TARGET_ARCH_RISCV64
          ? 4u
          : 1u;
  int legacy = options->target.arch == AST_TARGET_ARCH_X86_32 ||
               options->target.arch == AST_TARGET_ARCH_X86_16;
  int encoded = 0;
  if (legacy && (options->dump_machine || options->dump_regalloc)) {
    fputs("asterc: x86_16/x86_32 use the legacy encoder; machine/regalloc "
          "dumps are unavailable\n",
          stderr);
    return 0;
  }
  if (legacy && options->abi != AST_ABI_I386_CDECL) {
    fputs("asterc: legacy x86 requires --abi cdecl\n", stderr);
    return 0;
  }
  /* All targets, including the dedicated 386+ x86_16/x86_32 encoder,
   * must pass the frozen Target + ABI contracts.  Legacy x86 still consumes
   * generic MIR after validation because it has a separate stack-home
   * encoder, but it no longer bypasses legalization or cdecl verification. */
  AstTargetConfig effective_target = options->target;
  effective_target.optimize_size = options->optimize_size;
  /* The selected MIR was just verified; these source plans are immutable. */
  effective_target.verify_mir_before_build = 0u;
  image->target = ast_target_legalize(mir, &effective_target);
  if (!image->target || !ast_target_verify(image->target)) {
    if (image->target)
      print_target_diagnostics(image->target);
    return 0;
  }
  if (profile)
    profile->target_seconds +=
        compiler_elapsed(phase_begin, compiler_seconds());
  phase_begin = compiler_seconds();
  if (options->dump_target)
    ast_target_dump_program(image->target, compiler_dump_stdout, stdout);

  AstAbiConfig ac = ast_abi_config_default(options->abi);
  ac.verify_target_before_lowering = 0u;
  image->abi = ast_abi_lower(image->target, &ac);
  if (!image->abi || !ast_abi_verify(image->abi)) {
    if (image->abi)
      print_abi_diagnostics(image->abi);
    return 0;
  }
  if (profile)
    profile->abi_seconds += compiler_elapsed(phase_begin, compiler_seconds());
  phase_begin = compiler_seconds();
  if (options->dump_abi)
    ast_abi_dump_program(image->abi, compiler_dump_stdout, stdout);

  if (!legacy) {
    AstBackendConfig bc = ast_backend_config_default();
    bc.machine.verify_sources = 0u;
    image->machine = ast_backend_lower(image->abi, &bc);
    if (!image->machine) {
      fputs("asterc: machine lowering/verification failed\n", stderr);
      return 0;
    }
    if (profile)
      profile->machine_seconds +=
          compiler_elapsed(phase_begin, compiler_seconds());
    phase_begin = compiler_seconds();
    if (options->dump_machine)
      ast_machine_dump_program(image->machine, compiler_dump_stdout, stdout);

    AstRegAllocConfig rc = ast_regalloc_config_default();
    rc.register_limit = options->register_limit;
    rc.enable_red_zone = (uint8_t)!options->disable_red_zone;
    /* Machine IR was verified immediately above.  Re-running the full
     * verifier inside regalloc only duplicates CFG/liveness work without
     * observing any intervening mutation. */
    rc.verify_machine = 0u;
    if (!options->retain_backend) {
      if (!backend_encode_stream(
              image->machine, &rc, &image->encoded, &image->allocation_stats,
              image->backend_diagnostic, sizeof(image->backend_diagnostic))) {
        fprintf(stderr, "asterc: backend function %zu: %s\n",
                image->encoded.failed_function, image->backend_diagnostic);
        return 0;
      }
      encoded = 1;
    } else {
      image->allocation = ast_regalloc_build(image->machine, &rc);
      /* Allocation is verified once by ast_regalloc_build().  Finalization
       * reuses that proof and verifies the resulting Physical IR. */
      if (!image->allocation || !ast_regalloc_finalize(image->allocation)) {
        if (image->allocation)
          print_regalloc_diagnostics(image->allocation, image->machine);
        return 0;
      }
      if (options->dump_regalloc)
        ast_regalloc_dump(image->allocation, compiler_dump_stdout, stdout);
    }
  }
  image->hir = hir;
  image->mir = mir;
  if (add_section(image, ".text", 5u, 1u, 16u) == UINT32_MAX)
    return 0;
  if (!encoded &&
      !(legacy ? ast_encoder_x86_legacy_module(mir, &options->target,
                                               &image->encoded)
               : ast_encoder_module(image->allocation, &image->encoded))) {
    fprintf(stderr, "asterc: encoder function %zu: %s\n",
            image->encoded.failed_function,
            image->encoded.message ? image->encoded.message
                                   : "encoding failed");
    return 0;
  }
  if (profile)
    profile->allocation_encoding_seconds +=
        compiler_elapsed(phase_begin, compiler_seconds());
  phase_begin = compiler_seconds();
  if (!add_functions(image) || !add_globals(image) || !index_symbols(image) ||
      !compact_functions(image) || !layout_sections(image)) {
    fputs("asterc: final image layout failed\n", stderr);
    return 0;
  }

  if (image->allocation)
    image->allocation_stats = ast_regalloc_stats(image->allocation);
  /* Encoded bytes/fixups and the selected MIR own everything emission needs.
     Discard transient plans before building another optimization candidate. */
  if (!options->retain_backend)
    image_release_backend(image);
  if (profile)
    profile->layout_seconds +=
        compiler_elapsed(phase_begin, compiler_seconds());

  return 1;
}

/* Compare complete emitted artifacts, including layout, literals, symbols and
   relocations. Measurement never writes a speculative candidate to disk. */
static int compiler_image_size(BuildImage *image,
                               const AsterCompilerOptions *options,
                               uint64_t *size) {
  if (options->emit == ASTER_COMPILER_EMIT_OBJECT)
    return emit_object(image, options, size);
  *size = 0u;
  for (size_t j = 0; j < image->section_count; ++j) {
    const BuildSection *s = &image->sections[j];
    if (s->offset > UINT64_MAX - s->size)
      return 0;
    uint64_t end = s->offset + s->size;
    if (end > *size)
      *size = end;
  }
  return 1;
}
static int compiler_optimized_image(AstFrontend *frontend, AstHirProgram *hir,
                                    const AstMirConfig *mc,
                                    const AsterCompilerOptions *options,
                                    AstMirProgram **out_mir, BuildImage *image,
                                    AstMirInlineStats *inlined) {
  double mir_begin = compiler_seconds();
  AstMirProgram *mir = ast_mir_build(frontend, hir, mc);
  AstMirProgram *baseline = NULL;
  BuildImage baseline_image = {0};
  int ok = 0;
  memset(inlined, 0, sizeof(*inlined));
  if (!mir || !ast_mir_verify(mir)) {
    if (mir)
      print_mir_diagnostics(mir);
    goto done;
  }
  if (options->optimize_mir) {
    AstMirOptimizeStats optimized = {0};
    if (!ast_mir_optimize(mir, &optimized)) {
      print_mir_diagnostics(mir);
      goto done;
    }
    if (options->print_stats)
      fprintf(stderr,
              "MIR optimization: blocks-removed=%llu instructions-removed=%llu "
              "phis-removed=%llu branches=%llu threaded=%llu rounds=%llu\n",
              (unsigned long long)optimized.blocks_removed,
              (unsigned long long)optimized.instructions_removed,
              (unsigned long long)optimized.phis_removed,
              (unsigned long long)optimized.branch_folded,
              (unsigned long long)optimized.edges_threaded,
              (unsigned long long)optimized.rounds);
  }
  if (options->optimize_mir && !options->disable_inlining) {
    AstMirInlineConfig config = ast_mir_inline_config_default();
    config.closed_world =
        (uint8_t)(options->emit == ASTER_COMPILER_EMIT_BINARY);
    if (options->optimize_size)
      config.mode = AST_MIR_INLINE_SIZE;
    if (!ast_mir_inline(mir, &config, inlined)) {
      print_mir_diagnostics(mir);
      goto done;
    }
  }
  if (options->backend_profile)
    options->backend_profile->mir_seconds +=
        compiler_elapsed(mir_begin, compiler_seconds());
  if (!compiler_codegen_image(hir, mir, options, image))
    goto done;
  if (inlined->inlined && !inlined->forced) {
    mir_begin = compiler_seconds();
    baseline = ast_mir_build(frontend, hir, mc);
    if (!baseline || !ast_mir_optimize(baseline, NULL))
      goto done;
    if (options->backend_profile)
      options->backend_profile->mir_seconds +=
          compiler_elapsed(mir_begin, compiler_seconds());
    if (!compiler_codegen_image(hir, baseline, options, &baseline_image))
      goto done;
    uint64_t candidate_bytes, baseline_bytes;
    if (!compiler_image_size(image, options, &candidate_bytes) ||
        !compiler_image_size(&baseline_image, options, &baseline_bytes))
      goto done;
    int use_baseline =
        candidate_bytes > baseline_bytes ||
        (candidate_bytes == baseline_bytes &&
         image->sections[0].size > baseline_image.sections[0].size);
    if (options->print_stats)
      fprintf(stderr,
              "MIR inline: sites=%llu depth=%u baseline-bytes=%llu "
              "candidate-bytes=%llu selected=%s\n",
              (unsigned long long)inlined->inlined, inlined->max_depth,
              (unsigned long long)baseline_bytes,
              (unsigned long long)candidate_bytes,
              use_baseline ? "baseline" : "inlined");
    if (use_baseline) {
      image_destroy(image);
      *image = baseline_image;
      memset(&baseline_image, 0, sizeof(baseline_image));
      ast_mir_destroy(mir);
      mir = baseline;
      baseline = NULL;
    }
  }
  *out_mir = mir;
  mir = NULL;
  ok = 1;
done:
  if (!ok)
    image_destroy(image);
  image_destroy(&baseline_image);
  ast_mir_destroy(baseline);
  ast_mir_destroy(mir);
  return ok;
}

int aster_compiler_build(AstFrontend *frontend, AstSema *sema,
                         const AsterCompilerOptions *options) {
  if (!frontend || !sema || !options || !options->output_path ||
      options->target.arch == AST_TARGET_ARCH_INVALID)
    return 0;
  int ok = 0;
  double begin = compiler_seconds(), hir_done = begin, lowering_done = begin;
  AstHirProgram *hir = NULL;
  AstMirProgram *mir = NULL;
  AstMirProgram *speed_mir = NULL;
  BuildImage speed_image = {0};
  BuildImage image;
  memset(&image, 0, sizeof(image));
  image.base_address = options->base_address;
  image.pointer_bytes = options->target.pointer_bits / 8u;

  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = options->thread_count;
  /* Most functions are small. Avoid retaining a 128 KiB minimum allocation
     for every HIR/MIR/proof function while larger requests still grow freely.
   */
  hc.arena_block_size = 16u * 1024u;
  hc.verify_after_build =
      0u; /* The checked result is verified immediately below. */
  hir = ast_hir_build(frontend, sema, &hc);
  if (!hir || !ast_hir_verify(hir)) {
    if (hir)
      print_hir_diagnostics(hir);
    goto done;
  }
  if (options->dump_hir)
    ast_hir_dump_program(hir, compiler_dump_stdout, stdout);
  hir_done = compiler_seconds();

  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = options->thread_count;
  mc.arena_block_size = 16u * 1024u;
  mc.verify_hir_before_build =
      0u; /* HIR above is already verified and immutable. */
  mc.verify_after_build =
      0u; /* The caller/optimizer verifies each candidate. */
  mc.promote_locals = options->promote_locals;
  AstMirInlineStats inlined = {0};
  CompilerBackendProfile profile = {0};
  AsterCompilerOptions quiet = *options;
  quiet.backend_profile =
      options->print_stats || options->optimization_report ? &profile : NULL;
  quiet.retain_backend =
      (uint8_t)(options->dump_target || options->dump_abi ||
                options->dump_machine || options->dump_regalloc);
  if ((options->target.arch == AST_TARGET_ARCH_X86_16 ||
       options->target.arch == AST_TARGET_ARCH_X86_32) &&
      (options->dump_machine || options->dump_regalloc)) {
    fputs("asterc: x86_16/x86_32 use the legacy encoder; machine/regalloc "
          "dumps are unavailable\n",
          stderr);
    goto done;
  }
  quiet.dump_target = quiet.dump_abi = quiet.dump_machine =
      quiet.dump_regalloc = 0u;
  if (!compiler_optimized_image(frontend, hir, &mc, &quiet, &mir, &image,
                                &inlined))
    goto done;
  if (options->optimize_size) {
    AsterCompilerOptions speed = quiet;
    speed.optimize_size = 0u;
    speed.print_stats = 0u;
    AstMirInlineStats speed_inlined = {0};
    if (!compiler_optimized_image(frontend, hir, &mc, &speed, &speed_mir,
                                  &speed_image, &speed_inlined))
      goto done;
    uint64_t size_bytes, speed_bytes;
    if (!compiler_image_size(&image, &quiet, &size_bytes) ||
        !compiler_image_size(&speed_image, &speed, &speed_bytes))
      goto done;
    int use_speed = speed_bytes < size_bytes ||
                    (speed_bytes == size_bytes &&
                     speed_image.sections[0].size < image.sections[0].size);
    if (options->print_stats)
      fprintf(stderr,
              "size policy: size-bytes=%llu speed-bytes=%llu selected=%s\n",
              (unsigned long long)size_bytes, (unsigned long long)speed_bytes,
              use_speed ? "balanced" : "size");
    if (use_speed) {
      image_destroy(&image);
      image = speed_image;
      memset(&speed_image, 0, sizeof(speed_image));
      ast_mir_destroy(mir);
      mir = speed_mir;
      speed_mir = NULL;
      inlined = speed_inlined;
    }
  }
  if (options->print_stats && inlined.forced)
    fprintf(stderr,
            "MIR inline: sites=%llu depth=%u forced=%llu "
            "selected=explicit-policy\n",
            (unsigned long long)inlined.inlined, inlined.max_depth,
            (unsigned long long)inlined.forced);
  if (options->dump_mir)
    ast_mir_dump_program(mir, compiler_dump_stdout, stdout);
  if (options->dump_target)
    ast_target_dump_program(image.target, compiler_dump_stdout, stdout);
  if (options->dump_abi)
    ast_abi_dump_program(image.abi, compiler_dump_stdout, stdout);
  if (options->dump_machine)
    ast_machine_dump_program(image.machine, compiler_dump_stdout, stdout);
  if (options->dump_regalloc)
    ast_regalloc_dump(image.allocation, compiler_dump_stdout, stdout);

  lowering_done = compiler_seconds();
  if (options->optimization_report) {
    for (size_t j = 0; j < ast_frontend_file_count(frontend); ++j) {
      AstFrontendFile file;
      if (!ast_frontend_get_file(frontend, j, &file) ||
          compiler_paths_alias(options->optimization_report, file.path)) {
        fputs("asterc: optimization report must not overwrite source files\n",
              stderr);
        goto done;
      }
    }
  }
  if (options->emit == ASTER_COMPILER_EMIT_OBJECT) {
    ok = emit_object(&image, options, NULL);
  } else {
    ok = emit_binary(&image, options->output_path);
  }

  if (ok && options->optimization_report) {
    if (compiler_paths_alias(options->optimization_report,
                             options->output_path)) {
      fputs("asterc: optimization report and artifact paths must differ\n",
            stderr);
      ok = 0;
      goto done;
    }
    AstHirStats hs = ast_hir_stats(hir);
    AstMirStats ms = ast_mir_stats(mir);
    AstRegAllocStats rs = image.allocation_stats;
    uint64_t artifact_bytes;
    if (!compiler_image_size(&image, options, &artifact_bytes)) {
      ok = 0;
      goto done;
    }
    char report[4096];
    int length = snprintf(
        report, sizeof(report),
        "{\n  \"schema\":1,\n  \"target\":\"%s\",\n  "
        "\"features\":\"0x%016" PRIx64 "\",\n"
        "  \"optimization_requested\":\"%s\",\n  \"inlining_disabled\":%s,\n"
        "  \"artifact_bytes\":%" PRIu64
        ",\n  \"text_bytes\":%zu,\n  \"functions_removed\":%zu,\n"
        "  \"hir_functions\":%zu,\n  \"mir_instructions\":%zu,\n  "
        "\"physical_instructions\":%zu,\n"
        "  \"frame_bytes\":%" PRIu64 ",\n  \"inline_sites\":%" PRIu64
        ",\n  \"forced_inline_sites\":%" PRIu64 ",\n"
        "  \"backend_candidates\":%zu,\n"
        "  "
        "\"backend_timing_seconds\":{\"mir\":%.9f,\"target\":%.9f,\"abi\":%.9f,"
        "\"machine\":%.9f,\"allocation_encoding\":%.9f,\"layout\":%.9f},\n"
        "  "
        "\"timing_seconds\":{\"frontend\":%.9f,\"sema\":%.9f,\"hir\":%.9f,"
        "\"lowering\":%.9f,\"emit\":%.9f}\n}\n",
        ast_target_arch_name(options->target.arch), options->target.features,
        options->optimize_size  ? "size"
        : options->optimize_mir ? "speed"
                                : "none",
        options->disable_inlining ? "true" : "false", artifact_bytes,
        image.sections[0].size, image.omitted_functions, hs.function_count,
        ms.instruction_count, rs.physical_instructions,
        (uint64_t)rs.frame_bytes, inlined.inlined, inlined.forced,
        profile.candidates, profile.mir_seconds, profile.target_seconds,
        profile.abi_seconds, profile.machine_seconds,
        profile.allocation_encoding_seconds, profile.layout_seconds,
        options->frontend_seconds, options->sema_seconds,
        compiler_elapsed(begin, hir_done),
        compiler_elapsed(hir_done, lowering_done),
        compiler_elapsed(lowering_done, compiler_seconds()));
    ok = length > 0 && (size_t)length < sizeof(report) &&
         write_bytes_file(options->optimization_report, (const uint8_t *)report,
                          (size_t)length);
  }

  if (ok && options->print_stats) {
    fprintf(stderr,
            "timing-seconds: frontend=%.6f sema=%.6f hir=%.6f lowering=%.6f "
            "emit=%.6f\n",
            options->frontend_seconds, options->sema_seconds,
            compiler_elapsed(begin, hir_done),
            compiler_elapsed(hir_done, lowering_done),
            compiler_elapsed(lowering_done, compiler_seconds()));
    fprintf(stderr, "image: text-bytes=%llu functions-removed=%zu\n",
            (unsigned long long)image.sections[0].size,
            image.omitted_functions);
    AstHirStats hs = ast_hir_stats(hir);
    AstMirStats ms = ast_mir_stats(mir);
    AstRegAllocStats rs = image.allocation_stats;
    fprintf(stderr,
            "hir_functions=%zu globals=%zu mir_instructions=%zu "
            "physical_instructions=%zu frame_bytes=%" PRIu64
            " sections=%zu output=%s\n",
            hs.function_count, hs.global_count, ms.instruction_count,
            rs.physical_instructions, (uint64_t)rs.frame_bytes,
            image.section_count, options->output_path);
  }

done:
  image_destroy(&image);
  image_destroy(&speed_image);
  ast_mir_destroy(speed_mir);
  ast_mir_destroy(mir);
  ast_hir_destroy(hir);
  return ok;
}
