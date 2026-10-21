#include "elf64_fallback.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BYTES(s) {(const uint8_t *)(s), sizeof(s) - 1u}
static int check(const AstObjectModule *module, int accepted) {
  AstObjectOutput out = {0};
  uint32_t status = aster_elf64_fallback_write(module, &out);
  int ok = (status == AST_OBJECT_OK) == accepted;
  if (accepted &&
      (out.size < 64u || !out.data || memcmp(out.data, "\177ELF", 4u)))
    ok = 0;
  if (!accepted && (out.size || out.data))
    ok = 0;
  aster_elf64_fallback_free(&out);
  return ok;
}
int main(void) {
  uint8_t arm_call[16] = {0, 0, 0, 0x94, 0, 0, 0, 0x94, 0xc0, 0x03, 0x5f, 0xd6};
  uint8_t rv_call[16] = {0x97, 0, 0, 0, 0xe7, 0x80, 0, 0, 0x67, 0x80, 0, 0};
  AstObjectSection section = {0};
  section.name = (AstObjectBytes)BYTES(".text");
  section.kind = AST_OBJECT_TEXT;
  section.data = (AstObjectBytes){arm_call, 12u};
  section.size = 12u;
  section.alignment = 4u;
  AstObjectSymbol symbol = {0};
  symbol.name = (AstObjectBytes)BYTES("foreign");
  symbol.section = AST_OBJECT_UNDEFINED;
  symbol.binding = AST_OBJECT_GLOBAL;
  symbol.kind = AST_OBJECT_SYMBOL_CODE;
  AstObjectRelocation fixup = {0};
  fixup.section = 0u;
  fixup.symbol = 0u;
  fixup.offset = 0u;
  fixup.kind = AST_OBJECT_A64_CALL26;
  AstObjectModule module = {0};
  module.abi_version = AST_OBJECT_ABI_VERSION;
  module.format = AST_OBJECT_ELF;
  module.architecture = AST_OBJECT_AARCH64;
  module.sections = &section;
  module.section_count = 1u;
  module.symbols = &symbol;
  module.symbol_count = 1u;
  module.relocations = &fixup;
  module.relocation_count = 1u;
  int count = 0;
#define REQUIRE(expr)                                                          \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "FAILED line %d\n", __LINE__);                           \
      return 1;                                                                \
    }                                                                          \
    ++count;                                                                   \
  } while (0)
  REQUIRE(check(&module, 1));
  fixup.offset = 4u;
  REQUIRE(check(&module, 1));
  fixup.offset = 0u;
  fixup.symbol = 4u;
  REQUIRE(check(&module, 0));
  fixup.symbol = 0u;
  fixup.offset = 1u;
  REQUIRE(check(&module, 0));
  fixup.offset = 0u;
  fixup.addend = 1;
  REQUIRE(check(&module, 0));
  fixup.addend = 0;
  fixup.kind = AST_OBJECT_RV_CALL;
  REQUIRE(check(&module, 0));
  fixup.kind = AST_OBJECT_A64_CALL26;
  arm_call[3] = 0;
  REQUIRE(check(&module, 0));
  arm_call[3] = 0x94;
  section.alignment = 3u;
  REQUIRE(check(&module, 0));
  section.alignment = 4u;
  section.kind = 99u;
  REQUIRE(check(&module, 0));
  section.kind = AST_OBJECT_TEXT;
  section.data.size = 11u;
  REQUIRE(check(&module, 0));
  section.data.size = 12u;
  section.data.data = NULL;
  REQUIRE(check(&module, 0));
  section.data.data = arm_call;
  section.name.data = NULL;
  REQUIRE(check(&module, 0));
  section.name = (AstObjectBytes)BYTES(".text");
  module.format = AST_OBJECT_COFF;
  REQUIRE(check(&module, 0));
  module.format = AST_OBJECT_ELF;
  module.architecture = AST_OBJECT_X86_64;
  REQUIRE(check(&module, 0));
  module.architecture = AST_OBJECT_AARCH64;
  module.abi_version = 3u;
  REQUIRE(check(&module, 0));
  module.abi_version = AST_OBJECT_ABI_VERSION;
  AstObjectRelocation twins[2] = {fixup, fixup};
  module.relocations = twins;
  module.relocation_count = 2u;
  REQUIRE(check(&module, 0));
  twins[1].offset = 4u;
  REQUIRE(check(&module, 1));
  module.relocations = &fixup;
  module.relocation_count = 1u;
  module.architecture = AST_OBJECT_RISCV64;
  section.data.data = rv_call;
  fixup.kind = AST_OBJECT_RV_CALL;
  REQUIRE(check(&module, 1));
  rv_call[4] = 0;
  REQUIRE(check(&module, 0));
  rv_call[4] = 0xe7;
  fixup.addend = 1;
  REQUIRE(check(&module, 0));
  fixup.addend = 0;
  section.size = 7u;
  section.data.size = 7u;
  REQUIRE(check(&module, 0));
  section.size = 12u;
  section.data.size = 12u;
  module.riscv_float_abi = 2u;
  REQUIRE(check(&module, 1));
  fprintf(stdout, "elf64 fallback: %d C ABI/bounds/relocation cases passed\n",
          count);
  return 0;
}
