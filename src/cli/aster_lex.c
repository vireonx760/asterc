/* SPDX-License-Identifier: MPL-2.0 */
#include "asterasm/lexer.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fputs("usage: aster-lex <file.aa>...\n", stderr);
    return 2;
  }
  AstLexBatch batch;
  if (ast_lex_paths_parallel((const char *const *)(argv + 1),
                             (size_t)(argc - 1), NULL, 0u, &batch) != 0) {
    fputs("aster-lex: failed to read or lex input\n", stderr);
    return 2;
  }
  int status = 0;
  for (size_t f = 0; f < batch.count; ++f) {
    const AstLexedFile *file = &batch.files[f];
    printf("== %s ==\n", file->source.path ? file->source.path : "<source>");
    for (AstTokenCursor c = ast_token_cursor_begin(file);
         ast_token_cursor_get(&c); ast_token_cursor_next(&c)) {
      const AstToken *t = ast_token_cursor_get(&c);
      printf("%6u:%-4u %-22s off=%llu len=%u\n", t->line, t->column,
             ast_token_kind_name((AstTokenKind)t->kind),
             (unsigned long long)t->offset, t->length);
      if ((AstTokenKind)t->kind == AST_TOK_EOF)
        break;
    }
    if (file->error_count)
      status = 1;
  }
  ast_lex_batch_destroy(&batch);
  return status;
}
