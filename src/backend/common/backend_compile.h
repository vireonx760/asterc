#ifndef ASTERASM_BACKEND_COMPILE_H
#define ASTERASM_BACKEND_COMPILE_H
#include "asterasm/encoder.h"
/* CLI path: verify/encode/release each independent allocation/physical plan.
   Machine IR remains borrowed. Error text borrows the caller's buffer. */
int backend_encode_stream(const AstMachineProgram *machine,
                          const AstRegAllocConfig *config,
                          AstEncoderModuleOutput *output,
                          AstRegAllocStats *stats, char *error,
                          size_t error_capacity);
#endif
