/* Exercise owned object readers and raw instruction decoders without I/O. */
/* Compile the private raw A64/RV decoder in this test translation unit. Keeping
   its CLI entry under another name avoids adding a production-only test API. */
int asterdump_fuzz_cli_main(int argc, char **argv);
#define main asterdump_fuzz_cli_main
#include "../src/cli/asterdump.c"
#undef main

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 65536u)
    return 0;
  char text[512], error[256];
  DumpElfView elf;
  /* Inspect every machine rather than hiding parsing behind a magic prefix. */
  const uint16_t machines[] = {62u, 183u, 243u};
  for (size_t i = 0; i < 3u; ++i) {
    if (dump_elf_parse(data, size, machines[i], &elf, error, sizeof(error)))
      dump_elf_destroy(&elf);
  }
  for (size_t i = 0; i < 2u; ++i) {
    DumpCoffView coff;
    if (dump_coff_parse(data, size, i ? 0xaa64u : 0x8664u, &coff, error,
                        sizeof(error)))
      dump_coff_destroy(&coff);
  }
  for (size_t offset = 0u; offset < size;) {
    size_t consumed = 0u;
    (void)dump_x86_decode(data + offset, size - offset, UINT64_MAX - offset,
                          text, sizeof(text), &consumed);
    if (consumed == 0u || consumed > size - offset || consumed > 15u)
      abort();
    offset += consumed;
  }
  for (size_t offset = 0u; offset + 4u <= size; offset += 4u) {
    uint32_t word = (uint32_t)data[offset] |
                    ((uint32_t)data[offset + 1u] << 8u) |
                    ((uint32_t)data[offset + 2u] << 16u) |
                    ((uint32_t)data[offset + 3u] << 24u);
    (void)decode_a64(word, UINT64_MAX - offset, text, sizeof(text));
    (void)decode_rv64(word, UINT64_MAX - offset, text, sizeof(text));
  }
  return 0;
}
