/* Portable deterministic replay driver; libFuzzer supplies its own main. */
#include "asterasm/common.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
  static const char seed[] =
      "module fuzz; type B<N:const usize>=[u8;N]; "
      "export fn f(inputs:u64)->u64 {if inputs>3 {return inputs/7;}return 0;}";
  uint8_t data[1024];
  uint32_t state = UINT32_C(0x517a37);
  for (size_t i = 0u; i < sizeof(seed); ++i)
    LLVMFuzzerTestOneInput((const uint8_t *)seed, i);
  for (unsigned i = 0u; i < 2048u; ++i) {
    size_t length = i % sizeof(data);
    for (size_t j = 0u; j < length; ++j) {
      state ^= state << 13u;
      state ^= state >> 17u;
      state ^= state << 5u;
      data[j] = (uint8_t)state;
    }
    LLVMFuzzerTestOneInput(data, length);
  }
  for (int i = 1; i < argc; ++i) {
    FILE *file = ast_fopen(argv[i], "rb");
    if (!file)
      return 1;
    uint8_t *bytes = (uint8_t *)malloc(65537u);
    if (!bytes) {
      fclose(file);
      return 1;
    }
    size_t length = fread(bytes, 1u, 65537u, file);
    int failed = ferror(file) || length > 65536u;
    fclose(file);
    if (!failed)
      LLVMFuzzerTestOneInput(bytes, length);
    free(bytes);
    if (failed)
      return 1;
  }
  puts("PASS: deterministic fuzz smoke and supplied regression corpus");
  return 0;
}
