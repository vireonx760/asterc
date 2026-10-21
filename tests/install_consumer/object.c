#include <asterasm/object_writer.h>
#include <string.h>

int main(void) {
  static const uint8_t bytes[] = {0xc3};
  AstObjectSection section = {0};
  section.name.data = (const uint8_t *)".text";
  section.name.size = 5u;
  section.data.data = bytes;
  section.data.size = sizeof(bytes);
  section.size = sizeof(bytes);
  section.alignment = 1u;
  section.kind = AST_OBJECT_TEXT;
  AstObjectModule module = {0};
  module.abi_version = AST_OBJECT_ABI_VERSION;
  module.format = AST_OBJECT_ELF;
  module.architecture = AST_OBJECT_X86_64;
  module.sections = &section;
  module.section_count = 1u;
  module.entry_symbol = AST_OBJECT_NO_ENTRY;
  AstObjectOutput output;
  int result = ast_object_write(&module, &output);
  if (result != AST_OBJECT_OK)
    return 1;
  result = output.size < 4u || memcmp(output.data, "\177ELF", 4u) != 0;
  ast_object_output_free(&output);
  return result;
}
