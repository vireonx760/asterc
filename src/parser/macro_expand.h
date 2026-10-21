#ifndef ASTERASM_MACRO_EXPAND_H
#define ASTERASM_MACRO_EXPAND_H

#include "asterasm/lexer.h"

/*
 * Expand the hygienic, token-tree macro syntax used by Core before the
 * recursive-descent parser sees the file.  A return value of 1 means that
 * `out` owns a newly allocated source buffer and lexed file.  A return value
 * of 0 means that no invocation was found and `out` is untouched.  The
 * caller owns the returned source buffer separately from the lexed file.
 */
int ast_macro_expand_file(const AstLexedFile *input, AstLexedFile *out,
                          uint8_t **source_out);

#endif
