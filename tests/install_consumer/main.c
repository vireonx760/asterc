#include <asterasm/asterasm.h>
#include <string.h>

int main(void) {
  static const char text[] =
      "module consumer;fn f(inputs:u64)->u64{return inputs+1;}";
  AstSourceFile source = {"consumer.aa", (const uint8_t *)text,
                          sizeof(text) - 1u};
  AstLexedFile lexed;
  AstParseResult parsed;
  if (ast_lex_source(&source, NULL, &lexed)) {
    fputs("Installed lexer failed\n", stderr);
    return 1;
  }
  int status = ast_parse_file(&lexed, 1u, NULL, &parsed);
  if (!status) {
    status = parsed.error_count != 0u || parsed.module == NULL;
    if (status)
      fprintf(stderr, "Installed parser reported %zu errors\n",
              parsed.error_count);
    ast_parse_result_destroy(&parsed);
  }
  ast_lexed_file_destroy(&lexed);
  AstTargetConfig target = ast_target_config_default(AST_TARGET_ARCH_AARCH64);
  if (target.arch != AST_TARGET_ARCH_AARCH64 ||
      strlen(ASTERASM_VERSION_STRING) == 0u) {
    fputs("Installed target configuration mismatch\n", stderr);
    return 1;
  }
  return status ? 1 : 0;
}
