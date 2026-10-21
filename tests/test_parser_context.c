#include "asterasm/asterasm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x)                                                             \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "parser context check at %d: %s\n", __LINE__, #x);       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int spans(const char *text, size_t length, uint32_t max_errors,
                 size_t wanted) {
  AstSourceFile source = {"context.aa", (const uint8_t *)text, length};
  AstLexedFile lexed;
  AstParseResult parsed;
  REQUIRE(ast_lex_source(&source, NULL, &lexed) == 0);
  AstParseConfig config = ast_parse_config_default();
  config.max_diagnostics = max_errors;
  REQUIRE(ast_parse_file(&lexed, 7u, &config, &parsed) == 0);
  REQUIRE(parsed.error_count > 0u &&
          parsed.diagnostic_count <= (size_t)max_errors + 1u);
  for (size_t j = 0u; j < parsed.diagnostic_count; ++j) {
    const AstParseDiagnostic *d = &parsed.diagnostics[j];
    REQUIRE(d->span.file_id == 7u && d->span.offset <= length &&
            d->span.length <= length - d->span.offset);
    REQUIRE(d->message && d->message[0]);
  }
  if (wanted != SIZE_MAX)
    REQUIRE(parsed.diagnostics[0].span.offset == wanted);
  ast_parse_result_destroy(&parsed);
  ast_lexed_file_destroy(&lexed);
  return 0;
}
int main(void) {
  static const char split[] =
      "module context; type X=ptr<u64>>; fn recovered(){}";
  REQUIRE(spans(split, sizeof(split) - 1u, 8u,
                (size_t)(strstr(split, ">>") - split) + 1u) == 0);
  static const char base[] =
      "module context; type B<N:const usize>=[u8;N]; fn "
      "f(x:ptr<ptr<u64>>)->u64 {if !(x!=0) {return 1;} return 2;}";
  /* Every truncated prefix must terminate within the configured error bound. */
  for (size_t j = 1u; j < sizeof(base) - 1u; ++j) {
    AstSourceFile source = {"prefix.aa", (const uint8_t *)base, j};
    AstLexedFile lexed;
    AstParseResult parsed;
    REQUIRE(ast_lex_source(&source, NULL, &lexed) == 0);
    AstParseConfig config = ast_parse_config_default();
    config.max_diagnostics = 8u;
    REQUIRE(ast_parse_file(&lexed, 7u, &config, &parsed) == 0);
    REQUIRE(parsed.diagnostic_count <= 9u);
    for (size_t k = 0u; k < parsed.diagnostic_count; ++k)
      REQUIRE(parsed.diagnostics[k].span.offset <= j &&
              parsed.diagnostics[k].span.length <=
                  j - parsed.diagnostics[k].span.offset);
    ast_parse_result_destroy(&parsed);
    ast_lexed_file_destroy(&lexed);
  }
  char broken[2048];
  size_t used = 0u;
  used +=
      (size_t)snprintf(broken + used, sizeof(broken) - used, "module context;");
  for (unsigned j = 0u; j < 50u; ++j)
    used += (size_t)snprintf(broken + used, sizeof(broken) - used,
                             "const x%u:u64=;", j);
  REQUIRE(spans(broken, used, 4u, SIZE_MAX) == 0);
  puts("PASS: generic split spans, every source truncation, bounded recovery");
  return 0;
}
