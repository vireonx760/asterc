#ifndef ASTERASM_OBJECT_WRITER_H
#define ASTERASM_OBJECT_WRITER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* This ABI belongs to the separate Rust library, not asterasm_shared. All
   pointers must refer to valid, aligned memory for their declared lengths.
   Inputs are borrowed for one synchronous call. Names are bytes without NUL.
   No structs may overlap the output record. Independent calls are thread safe.
 */
#define AST_OBJECT_ABI_VERSION 1u
#define AST_OBJECT_UNDEFINED UINT32_MAX
#define AST_OBJECT_ABSOLUTE (UINT32_MAX - 1u)
#define AST_OBJECT_NO_ENTRY UINT32_MAX
enum {
  AST_OBJECT_ELF = 1,
  AST_OBJECT_COFF = 2,
  AST_OBJECT_MACHO = 3,
  AST_OBJECT_FLAT = 4
};
enum {
  AST_OBJECT_X86_64 = 1,
  AST_OBJECT_AARCH64 = 2,
  AST_OBJECT_RISCV64 = 3,
  AST_OBJECT_I386 = 4
};
enum {
  AST_OBJECT_TEXT = 1,
  AST_OBJECT_RODATA = 2,
  AST_OBJECT_DATA = 3,
  AST_OBJECT_BSS = 4
};
enum { AST_OBJECT_LOCAL = 0, AST_OBJECT_GLOBAL = 1, AST_OBJECT_WEAK = 2 };
enum { AST_OBJECT_SYMBOL_CODE = 1, AST_OBJECT_SYMBOL_DATA = 2 };
enum {
  AST_OBJECT_PCREL32 = 0,
  AST_OBJECT_ABS64 = 1,
  AST_OBJECT_A64_CALL26 = 2,
  AST_OBJECT_A64_ADR21 = 3,
  AST_OBJECT_RV_CALL = 4,
  AST_OBJECT_A64_ADRP_ADD = 5,
  AST_OBJECT_RV_ADDRESS = 6,
  AST_OBJECT_X86_CALL32 = 7,
  AST_OBJECT_ABS32 = 8
};
enum {
  AST_OBJECT_OK = 0,
  AST_OBJECT_INVALID = 1,
  AST_OBJECT_UNSUPPORTED = 2,
  AST_OBJECT_RANGE = 3,
  AST_OBJECT_UNRESOLVED = 4,
  AST_OBJECT_WRITE_ERROR = 5,
  AST_OBJECT_INTERNAL = 6
};
typedef struct AstObjectBytes {
  const uint8_t *data;
  uint64_t size;
} AstObjectBytes;
typedef struct AstObjectSection {
  AstObjectBytes name, data;
  uint64_t size, alignment; /* size == data.size except BSS (data.size == 0) */
  uint32_t kind, reserved;
} AstObjectSection;
typedef struct AstObjectSymbol {
  AstObjectBytes name;
  uint64_t value, size; /* section offset, or absolute value */
  uint32_t section, binding, kind, reserved;
} AstObjectSymbol;
typedef struct AstObjectRelocation {
  uint64_t offset;
  int64_t
      addend; /* PCREL32/CALL32 are relative to the END of the 4-byte field. */
  uint32_t section, symbol, kind, reserved;
} AstObjectRelocation;
typedef struct AstObjectModule {
  uint32_t abi_version, format, architecture, reserved;
  const AstObjectSection *sections;
  uint64_t section_count;
  const AstObjectSymbol *symbols;
  uint64_t symbol_count;
  const AstObjectRelocation *relocations;
  uint64_t relocation_count;
  uint64_t base_address,
      image_limit; /* FLAT: input order, aligned VAs, BSS filled with zero. */
  uint32_t entry_symbol, riscv_float_abi; /* NO_ENTRY or defined code symbol;
                                             RV: 0/1/2 = soft/F/D */
} AstObjectModule;
typedef struct AstObjectOutput {
  uint8_t *data;
  uint64_t size, entry_address; /* entry_address only meaningful for FLAT */
  uint32_t status;
  char message[256];
} AstObjectOutput;

/* Output must be fresh or previously freed. On failure data/size are zero.
   Allocation exhaustion may terminate the process under Rust's allocator
   policy; Rust panics are caught and reported, never unwound into C. */
uint32_t ast_object_write(const AstObjectModule *module,
                          AstObjectOutput *output);
/* Only release a record returned by ast_object_write; resets it, so repeat free
   is safe. Do not free a copied record or change its data/size fields. */
void ast_object_output_free(AstObjectOutput *output);
#ifdef __cplusplus
}
#endif
#endif
