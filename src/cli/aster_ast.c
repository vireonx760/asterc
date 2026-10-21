/* SPDX-License-Identifier: MPL-2.0 */
#include "asterasm/asterasm.h"

#include <stdio.h>

static void dump_write(void *user, const char *text, size_t length) {
  fwrite(text, 1u, length, (FILE *)user);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fputs("usage: aster-ast <file.aa>\n", stderr);
    return 2;
  }
  AstMappedSource source;
  if (ast_source_map_file(argv[1], &source) != 0) {
    perror(argv[1]);
    return 2;
  }
  AstLexedFile lexed;
  if (ast_lex_source(&source.source, NULL, &lexed) != 0) {
    ast_source_unmap(&source);
    return 2;
  }
  AstParseResult parsed;
  int rc = ast_parse_file(&lexed, 1u, NULL, &parsed);
  ast_print_lexer_diagnostics(stderr, &lexed, NULL);
  ast_print_parser_diagnostics(stderr, &source.source, &parsed, NULL);
  if (parsed.module)
    ast_dump_module(parsed.module, 0, dump_write, stdout);
  int status = rc != 0 || lexed.error_count || parsed.error_count;
  ast_parse_result_destroy(&parsed);
  ast_lexed_file_destroy(&lexed);
  ast_source_unmap(&source);
  return status ? 1 : 0;
}
