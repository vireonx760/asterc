#include "asterasm/object_writer.h"
#include "test_process.h"
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(void *) != 8 || sizeof(AstObjectModule) == 88,
               "C/Rust module ABI layout");
_Static_assert(sizeof(void *) != 8 || sizeof(AstObjectOutput) == 288,
               "C/Rust output ABI layout");
static int require(int ok, const char *message) {
  if (!ok)
    fprintf(stderr, "%s\n", message);
  return ok;
}
int main(void) {
  ast_test_process_setup();
  uint8_t data[32] = {0};
  AstObjectSection sections[] = {
      {{(const uint8_t *)".text", 5},
       {data, sizeof(data)},
       sizeof(data),
       16,
       AST_OBJECT_TEXT,
       0},
      {{(const uint8_t *)".bss", 4}, {NULL, 0}, 40, 64, AST_OBJECT_BSS, 0}};
  AstObjectSymbol symbol = {
      {(const uint8_t *)"entry", 5}, 0, 8, 0, AST_OBJECT_GLOBAL,
      AST_OBJECT_SYMBOL_CODE,        0};
  AstObjectRelocation reloc = {8, 0, 0, 0, AST_OBJECT_ABS64, 0};
  AstObjectModule m = {0};
  m.abi_version = AST_OBJECT_ABI_VERSION;
  m.architecture = AST_OBJECT_X86_64;
  m.sections = sections;
  m.section_count = 2;
  m.symbols = &symbol;
  m.symbol_count = 1;
  m.relocations = &reloc;
  m.relocation_count = 1;
  m.base_address = 0x100000;
  m.image_limit = 104;
  int ok = 1;
  for (uint32_t format = AST_OBJECT_ELF; format <= AST_OBJECT_FLAT; ++format) {
    m.format = format;
    AstObjectOutput out;
    if (!require(ast_object_write(&m, &out) == AST_OBJECT_OK,
                 "C API serialization failed")) {
      ast_object_output_free(&out);
      return 1;
    }
    if (format == AST_OBJECT_ELF)
      ok &=
          require(out.size > 4 && !memcmp(out.data, "\177ELF", 4), "ELF magic");
    if (format == AST_OBJECT_FLAT) {
      const uint8_t expected[] = {0, 0, 0x10, 0, 0, 0, 0, 0};
      ok &= require(out.size == 104 && out.entry_address == 0x100000,
                    "Flat layout/entry");
      ok &= require(!memcmp(out.data + 8, expected, 8), "Flat ABS64");
      for (uint64_t b = 32; b < out.size; ++b)
        ok &= require(!out.data[b], "Flat BSS/padding must be zero");
      data[0] = 42;
      ok &= require(out.data[0] == 0, "Output must own its bytes");
      data[0] = 0;
    }
    ast_object_output_free(&out);
    ast_object_output_free(&out);
    ok &= require(!out.data && !out.size, "Repeated release must be safe");
  }
  AstObjectOutput out;
  reloc.symbol = 1;
  ok &= require(ast_object_write(&m, &out) == AST_OBJECT_INVALID && !out.data &&
                    !out.size,
                "Invalid symbol must fail without partial output");
  ast_object_output_free(&out);
  ok &= require(ast_object_write(NULL, &out) == AST_OBJECT_INVALID && !out.data,
                "Null module must fail");
  ast_object_output_free(&out);
  return ok ? 0 : 1;
}
