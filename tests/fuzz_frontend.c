/* In-process lexer, macro expansion and parser recovery target. */
#include "asterasm/parser.h"
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 65536u)
    return 0;
  AstSourceFile source = {"fuzz.aa", data, size};
  AstLexedFile lexed;
  if (ast_lex_source(&source, NULL, &lexed) != 0)
    return 0;
  AstTokenCursor cursor = ast_token_cursor_begin(&lexed);
  for (const AstToken *token = ast_token_cursor_get(&cursor); token;
       ast_token_cursor_next(&cursor), token = ast_token_cursor_get(&cursor)) {
    if (token->offset > size || token->length > size - token->offset)
      abort();
  }
  AstParseConfig config = ast_parse_config_default();
  config.recursion_limit = 64u;
  config.max_diagnostics = 16u;
  AstParseResult parsed;
  if (ast_parse_file(&lexed, 1u, &config, &parsed) == 0) {
    const AstLexedFile *expanded = ast_parse_expanded_lexed(&parsed);
    size_t length = expanded ? expanded->source.size : size;
    for (size_t i = 0; i < parsed.diagnostic_count; ++i) {
      const AstSpan *span = &parsed.diagnostics[i].span;
      if (span->offset > length || span->length > length - span->offset)
        abort();
    }
    ast_parse_result_destroy(&parsed);
  }
  ast_lexed_file_destroy(&lexed);
  return 0;
}
