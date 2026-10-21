#include "asterasm/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "CHECK failed: %s at %s:%d\n", #expr, __FILE__,          \
              __LINE__);                                                       \
      return 0;                                                                \
    }                                                                          \
  } while (0)

static int encode_utf8(uint32_t cp, uint8_t out[4]) {
  if (cp <= 0x7Fu) {
    out[0] = (uint8_t)cp;
    return 1;
  }
  if (cp <= 0x7FFu) {
    out[0] = (uint8_t)(0xC0u | (cp >> 6));
    out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 2;
  }
  if (cp <= 0xFFFFu) {
    if (cp >= 0xD800u && cp <= 0xDFFFu)
      return 0;
    out[0] = (uint8_t)(0xE0u | (cp >> 12));
    out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 3;
  }
  if (cp <= 0x10FFFFu) {
    out[0] = (uint8_t)(0xF0u | (cp >> 18));
    out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 4;
  }
  return 0;
}

static int test_utf8_all_scalars(void) {
  uint8_t bytes[4];
  for (uint32_t cp = 0; cp <= 0x10FFFFu; ++cp) {
    if (cp >= 0xD800u && cp <= 0xDFFFu)
      continue;
    int n = encode_utf8(cp, bytes);
    CHECK(n >= 1 && n <= 4);
    uint32_t decoded = 0, width = 0;
    CHECK(ast_utf8_decode_one(bytes, (size_t)n, &decoded, &width));
    CHECK(decoded == cp);
    CHECK(width == (uint32_t)n);
  }
  return 1;
}

static int test_unicode_properties(void) {
  CHECK(strcmp(ast_unicode_version(), "17.0.0") == 0);
  CHECK(ast_unicode_is_xid_start('A'));
  CHECK(ast_unicode_is_xid_start(0x03B1u));
  CHECK(ast_unicode_is_xid_start(0x4E2Du));
  CHECK(!ast_unicode_is_xid_start(0x1F600u));
  CHECK(!ast_unicode_is_xid_start(0x0301u));
  CHECK(ast_unicode_is_xid_continue(0x0301u));
  return 1;
}

static int test_core_sample(void) {
  static const char source_text[] =
      "module demo.core;\n"
      "const число: u64 = 0xff00_ff00;\n"
      "export fn add(α: u64, β: u64) -> u64 @abi(c) {\n"
      "    /* nested /* comment */ still comment */\n"
      "    let строка = \"Привет, 世界 🌍\";\n"
      "    let символ = 'λ';\n"
      "    return α + β;\n"
      "}\n";

  AstSourceFile source = {"sample.aa", (const uint8_t *)source_text,
                          sizeof(source_text) - 1u};
  AstLexedFile file;
  CHECK(ast_lex_source(&source, NULL, &file) == 0);
  CHECK(file.error_count == 0);
  CHECK(file.token_count > 20);

  int saw_unicode_ident = 0;
  int saw_string = 0;
  int saw_char = 0;
  int saw_return = 0;
  AstTokenCursor cursor = ast_token_cursor_begin(&file);
  for (;;) {
    const AstToken *tok = ast_token_cursor_get(&cursor);
    if (!tok)
      break;
    if (tok->kind == AST_TOK_IDENTIFIER && (tok->flags & AST_TOKENF_UNICODE))
      saw_unicode_ident = 1;
    if (tok->kind == AST_TOK_STRING && (tok->flags & AST_TOKENF_UNICODE))
      saw_string = 1;
    if (tok->kind == AST_TOK_CHAR && (tok->flags & AST_TOKENF_UNICODE))
      saw_char = 1;
    if (tok->kind == AST_TOK_KW_RETURN)
      saw_return = 1;
    if (tok->kind == AST_TOK_EOF)
      break;
    ast_token_cursor_next(&cursor);
  }
  CHECK(saw_unicode_ident && saw_string && saw_char && saw_return);
  ast_lexed_file_destroy(&file);
  return 1;
}

static int test_trivia(void) {
  static const char source_text[] =
      "let x = 1; // hello\n/* world */ return x;\n";
  AstLexConfig config = ast_lex_config_default();
  config.flags |= AST_LEX_EMIT_TRIVIA;
  AstSourceFile source = {"trivia.aa", (const uint8_t *)source_text,
                          sizeof(source_text) - 1u};
  AstLexedFile file;
  CHECK(ast_lex_source(&source, &config, &file) == 0);
  CHECK(file.error_count == 0);
  int line_comment = 0, block_comment = 0, newline = 0, whitespace = 0;
  for (AstTokenCursor c = ast_token_cursor_begin(&file);
       ast_token_cursor_get(&c); ast_token_cursor_next(&c)) {
    const AstToken *t = ast_token_cursor_get(&c);
    line_comment += t->kind == AST_TOK_LINE_COMMENT;
    block_comment += t->kind == AST_TOK_BLOCK_COMMENT;
    newline += t->kind == AST_TOK_NEWLINE;
    whitespace += t->kind == AST_TOK_WHITESPACE;
    if (t->kind == AST_TOK_EOF)
      break;
  }
  CHECK(line_comment == 1 && block_comment == 1 && newline == 2 &&
        whitespace > 0);
  ast_lexed_file_destroy(&file);
  return 1;
}

static int test_comment_depth_recovery(void) {
  size_t cap = 512;
  char *text = (char *)malloc(cap);
  CHECK(text != NULL);
  size_t n = 0;
  for (int i = 0; i < 33; ++i) {
    memcpy(text + n, "/*", 2);
    n += 2;
  }
  for (int i = 0; i < 33; ++i) {
    memcpy(text + n, "*/", 2);
    n += 2;
  }
  memcpy(text + n, " let x = 1;", 11);
  n += 11;

  AstSourceFile source = {"depth.aa", (const uint8_t *)text, n};
  AstLexedFile file;
  CHECK(ast_lex_source(&source, NULL, &file) == 0);
  CHECK(file.error_count == 1);
  CHECK(file.diagnostic_head != NULL);
  CHECK(file.diagnostic_head->diagnostics[0].code ==
        AST_DIAG_BLOCK_COMMENT_NESTING);
  const AstToken *first = ast_token_at(&file, 0);
  CHECK(first && first->kind == AST_TOK_KW_LET);
  ast_lexed_file_destroy(&file);
  free(text);
  return 1;
}

static int test_invalid_utf8(void) {
  static const uint8_t data[] = {'l',  'e', 't', ' ', 0xF0, 0x28, 0x8C,
                                 0x28, ' ', '=', ' ', '1',  ';'};
  AstSourceFile source = {"invalid.aa", data, sizeof(data)};
  AstLexedFile file;
  CHECK(ast_lex_source(&source, NULL, &file) == 0);
  CHECK(file.error_count >= 1);
  int found = 0;
  for (const AstDiagnosticChunk *c = file.diagnostic_head; c; c = c->next) {
    for (uint32_t i = 0; i < c->count; ++i)
      found |= c->diagnostics[i].code == AST_DIAG_INVALID_UTF8;
  }
  CHECK(found);
  ast_lexed_file_destroy(&file);
  return 1;
}

static int test_numbers_and_operators(void) {
  static const char text[] =
      "10 0xff 0b1010 0o755 3.14 2e10 1..4 -> => == != <= >= << >> && || ##";
  AstSourceFile source = {"tokens.aa", (const uint8_t *)text,
                          sizeof(text) - 1u};
  AstLexedFile file;
  CHECK(ast_lex_source(&source, NULL, &file) == 0);
  CHECK(file.error_count == 0);
  AstTokenKind expected[] = {
      AST_TOK_INTEGER,    AST_TOK_INTEGER,     AST_TOK_INTEGER,
      AST_TOK_INTEGER,    AST_TOK_FLOAT,       AST_TOK_FLOAT,
      AST_TOK_INTEGER,    AST_TOK_DOT_DOT,     AST_TOK_INTEGER,
      AST_TOK_ARROW,      AST_TOK_FAT_ARROW,   AST_TOK_EQ_EQ,
      AST_TOK_BANG_EQ,    AST_TOK_LT_EQ,       AST_TOK_GT_EQ,
      AST_TOK_SHIFT_LEFT, AST_TOK_SHIFT_RIGHT, AST_TOK_AMP_AMP,
      AST_TOK_PIPE_PIPE,  AST_TOK_HASH_HASH,   AST_TOK_EOF};
  CHECK(file.token_count == sizeof(expected) / sizeof(expected[0]));
  for (size_t i = 0; i < file.token_count; ++i)
    CHECK(ast_token_at(&file, i)->kind == (uint32_t)expected[i]);
  ast_lexed_file_destroy(&file);
  return 1;
}

static int test_parallel_sources(void) {
  static const char text[] = "module параллель; export fn sum(a: u64, b: u64) "
                             "-> u64 { return a + b; }\n";
  enum { N = 64 };
  AstSourceFile sources[N];
  for (int i = 0; i < N; ++i) {
    sources[i].path = "parallel.aa";
    sources[i].data = (const uint8_t *)text;
    sources[i].size = sizeof(text) - 1u;
  }
  AstLexBatch batch;
  CHECK(ast_lex_sources_parallel(sources, N, NULL, 8, &batch) == 0);
  CHECK(batch.count == N);
  for (size_t i = 0; i < batch.count; ++i) {
    CHECK(batch.files[i].error_count == 0);
    CHECK(batch.files[i].token_count > 10);
  }
  ast_lex_batch_destroy(&batch);
  return 1;
}

static int test_ascii_identifier_mode(void) {
  static const char text[] = "let α = 1;";
  AstLexConfig config = ast_lex_config_default();
  config.flags |= AST_LEX_ASCII_IDENTIFIERS_ONLY;
  AstSourceFile source = {"ascii.aa", (const uint8_t *)text, sizeof(text) - 1u};
  AstLexedFile file;
  CHECK(ast_lex_source(&source, &config, &file) == 0);
  CHECK(file.error_count >= 1);
  ast_lexed_file_destroy(&file);
  return 1;
}

static int test_all_keywords(void) {
  struct KeywordCase {
    const char *text;
    AstTokenKind kind;
  } cases[] = {{"module", AST_TOK_KW_MODULE},
               {"target", AST_TOK_KW_TARGET},
               {"feature", AST_TOK_KW_FEATURE},
               {"import", AST_TOK_KW_IMPORT},
               {"as", AST_TOK_KW_AS},
               {"export", AST_TOK_KW_EXPORT},
               {"type", AST_TOK_KW_TYPE},
               {"struct", AST_TOK_KW_STRUCT},
               {"const", AST_TOK_KW_CONST},
               {"static_assert", AST_TOK_KW_STATIC_ASSERT},
               {"section", AST_TOK_KW_SECTION},
               {"global", AST_TOK_KW_GLOBAL},
               {"extern", AST_TOK_KW_EXTERN},
               {"fn", AST_TOK_KW_FN},
               {"dispatch", AST_TOK_KW_DISPATCH},
               {"when", AST_TOK_KW_WHEN},
               {"else", AST_TOK_KW_ELSE},
               {"let", AST_TOK_KW_LET},
               {"store", AST_TOK_KW_STORE},
               {"if", AST_TOK_KW_IF},
               {"while", AST_TOK_KW_WHILE},
               {"loop", AST_TOK_KW_LOOP},
               {"return", AST_TOK_KW_RETURN},
               {"break", AST_TOK_KW_BREAK},
               {"continue", AST_TOK_KW_CONTINUE},
               {"block", AST_TOK_KW_BLOCK},
               {"br", AST_TOK_KW_BR},
               {"unsafe", AST_TOK_KW_UNSAFE},
               {"control", AST_TOK_KW_CONTROL},
               {"comptime", AST_TOK_KW_COMPTIME},
               {"for", AST_TOK_KW_FOR},
               {"in", AST_TOK_KW_IN},
               {"requires", AST_TOK_KW_REQUIRES},
               {"bind", AST_TOK_KW_BIND},
               {"to", AST_TOK_KW_TO},
               {"reg", AST_TOK_KW_REG},
               {"macro", AST_TOK_KW_MACRO},
               {"native", AST_TOK_KW_NATIVE},
               {"inputs", AST_TOK_KW_INPUTS},
               {"outputs", AST_TOK_KW_OUTPUTS},
               {"exact", AST_TOK_KW_EXACT},
               {"bytes", AST_TOK_KW_BYTES},
               {"load", AST_TOK_KW_LOAD},
               {"cast", AST_TOK_KW_CAST},
               {"trunc", AST_TOK_KW_TRUNC},
               {"zext", AST_TOK_KW_ZEXT},
               {"sext", AST_TOK_KW_SEXT},
               {"bitcast", AST_TOK_KW_BITCAST},
               {"ptrcast", AST_TOK_KW_PTRCAST},
               {"atomic", AST_TOK_KW_ATOMIC},
               {"true", AST_TOK_KW_TRUE},
               {"false", AST_TOK_KW_FALSE},
               {"void", AST_TOK_KW_VOID},
               {"bool", AST_TOK_KW_BOOL},
               {"i8", AST_TOK_KW_I8},
               {"u8", AST_TOK_KW_U8},
               {"i16", AST_TOK_KW_I16},
               {"u16", AST_TOK_KW_U16},
               {"i32", AST_TOK_KW_I32},
               {"u32", AST_TOK_KW_U32},
               {"i64", AST_TOK_KW_I64},
               {"u64", AST_TOK_KW_U64},
               {"i128", AST_TOK_KW_I128},
               {"u128", AST_TOK_KW_U128},
               {"isize", AST_TOK_KW_ISIZE},
               {"usize", AST_TOK_KW_USIZE},
               {"f16", AST_TOK_KW_F16},
               {"bf16", AST_TOK_KW_BF16},
               {"f32", AST_TOK_KW_F32},
               {"f64", AST_TOK_KW_F64},
               {"ptr", AST_TOK_KW_PTR},
               {"cap", AST_TOK_KW_CAP},
               {"bits", AST_TOK_KW_BITS},
               {"vec", AST_TOK_KW_VEC},
               {"mask", AST_TOK_KW_MASK},
               {"svec", AST_TOK_KW_SVEC},
               {"pred", AST_TOK_KW_PRED},
               {"volatile", AST_TOK_KW_VOLATILE}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    AstSourceFile source = {"keyword.aa", (const uint8_t *)cases[i].text,
                            strlen(cases[i].text)};
    AstLexedFile file;
    CHECK(ast_lex_source(&source, NULL, &file) == 0);
    CHECK(file.error_count == 0);
    CHECK(file.token_count == 2);
    CHECK(ast_token_at(&file, 0)->kind == cases[i].kind);
    ast_lexed_file_destroy(&file);
  }
  return 1;
}

static uint32_t xorshift32(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static int test_random_bytes_no_crash(void) {
  uint32_t rng = 0xA57E1234u;
  for (int iteration = 0; iteration < 2000; ++iteration) {
    size_t size = (size_t)(xorshift32(&rng) & 2047u);
    uint8_t *data = size ? (uint8_t *)malloc(size) : NULL;
    CHECK(size == 0 || data != NULL);
    for (size_t i = 0; i < size; ++i)
      data[i] = (uint8_t)xorshift32(&rng);
    AstSourceFile source = {"random.bin", data, size};
    AstLexedFile file;
    CHECK(ast_lex_source(&source, NULL, &file) == 0);
    CHECK(file.token_count >= 1);
    const AstToken *last = ast_token_at(&file, file.token_count - 1u);
    CHECK(last && last->kind == AST_TOK_EOF);
    ast_lexed_file_destroy(&file);
    free(data);
  }
  return 1;
}

static int test_parallel_paths(void) {
  const char *paths[3] = {"aster_lexer_path_test_1.aa",
                          "aster_lexer_path_test_2.aa",
                          "aster_lexer_path_test_3.aa"};
  static const char text[] =
      "module файл; export fn f(x: u64) -> u64 { return x + 1; }\n";
  for (size_t i = 0; i < 3; ++i) {
    FILE *f = ast_fopen(paths[i], "wb");
    CHECK(f != NULL);
    CHECK(fwrite(text, 1, sizeof(text) - 1u, f) == sizeof(text) - 1u);
    CHECK(fclose(f) == 0);
  }
  AstLexBatch batch;
  int rc = ast_lex_paths_parallel(paths, 3, NULL, 3, &batch);
  for (size_t i = 0; i < 3; ++i)
    remove(paths[i]);
  CHECK(rc == 0);
  CHECK(batch.count == 3);
  for (size_t i = 0; i < batch.count; ++i)
    CHECK(batch.files[i].error_count == 0);
  ast_lex_batch_destroy(&batch);
  return 1;
}

static int test_bom_policy(void) {
  static const uint8_t text[] = {0xEF, 0xBB, 0xBF, 'l', 'e', 't',
                                 ' ',  'x',  '=',  '1', ';'};
  AstSourceFile source = {"bom.aa", text, sizeof(text)};
  AstLexedFile file;
  CHECK(ast_lex_source(&source, NULL, &file) == 0);
  CHECK(file.error_count == 1);
  ast_lexed_file_destroy(&file);

  AstLexConfig config = ast_lex_config_default();
  config.flags |= AST_LEX_ACCEPT_BOM;
  CHECK(ast_lex_source(&source, &config, &file) == 0);
  CHECK(file.error_count == 0);
  ast_lexed_file_destroy(&file);
  return 1;
}

int main(void) {
  struct Test {
    const char *name;
    int (*fn)(void);
  } tests[] = {{"utf8_all_scalars", test_utf8_all_scalars},
               {"unicode_properties", test_unicode_properties},
               {"core_sample", test_core_sample},
               {"trivia", test_trivia},
               {"comment_depth_recovery", test_comment_depth_recovery},
               {"invalid_utf8", test_invalid_utf8},
               {"numbers_and_operators", test_numbers_and_operators},
               {"parallel_sources", test_parallel_sources},
               {"ascii_identifier_mode", test_ascii_identifier_mode},
               {"all_keywords", test_all_keywords},
               {"random_bytes_no_crash", test_random_bytes_no_crash},
               {"parallel_paths", test_parallel_paths},
               {"bom_policy", test_bom_policy}};

  size_t passed = 0;
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    int ok = tests[i].fn();
    printf("%-28s %s\n", tests[i].name, ok ? "OK" : "FAIL");
    if (!ok)
      return 1;
    passed++;
  }
  printf("Passed %zu tests. Unicode %s.\n", passed, ast_unicode_version());
  return 0;
}
