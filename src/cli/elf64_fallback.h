#ifndef ASTERASM_ELF64_FALLBACK_H
#define ASTERASM_ELF64_FALLBACK_H
#include "asterasm/object_writer.h"
uint32_t aster_elf64_fallback_write(const AstObjectModule *module,
                                    AstObjectOutput *output);
void aster_elf64_fallback_free(AstObjectOutput *output);
#endif
