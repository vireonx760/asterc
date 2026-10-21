#include "asterasm/asterasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #x);  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int parse_text(const char *text, AstLexedFile *lexed,
                      AstParseResult *parsed) {
  AstSourceFile source = {"test.aa", (const uint8_t *)text, strlen(text)};
  if (ast_lex_source(&source, NULL, lexed) != 0)
    return -1;
  if (ast_parse_file(lexed, 1u, NULL, parsed) != 0)
    return -1;
  return 0;
}

static int test_full_core(void) {
  static const char src[] =
      "@file_attr(1) module demo.core;\n"
      "target \"x86_64-unknown-linux-gnu\" { cpu = \"baseline\"; feature + "
      "\"avx2\"; }\n"
      "import core.bits as bits;\n"
      "export type Pair<T:type> = struct @layout(c) { x: u64 @align(8); y: "
      "u64; };\n"
      "export type Bytes<N:const usize> = [u8; N];\n"
      "export const answer: u64 = 42;\n"
      "section \".data\" @writable { export global counter: u64 = 0 @align(8); "
      "const table: [u8; 4] = {1,2,3,4}; }\n"
      "extern fn malloc(n: usize) -> ptr<void> @abi(c);\n"
      "dispatch fn pick(x: u64) -> u64 { when feature(\"avx2\") => fast; else "
      "=> slow; }\n"
      "macro repeat_load($dst:ident, $ptr:expr, $count:const usize) { comptime "
      "for i in 0..$count { let $dst##i = load.u32 [$ptr + i * 4]; } }\n"
      "static_assert(sizeof(Pair) == 16);\n"
      "export fn work(p: ptr<const u8>, q: ptr<volatile u32>, n: usize) -> u64 "
      "@abi(c) {\n"
      "  let acc: u64 = 0; let out: u64; const one: u64 = 1;\n"
      "  bind acc to reg(\"arg0\");\n"
      "  store.u32 [q] = load.u32 [p + 4] @align(4);\n"
      "  if n > 0 { acc = acc + 1; } else if n == 0 { return 0; } else { "
      "return 1; }\n"
      "  while acc < n { acc = acc + one; }\n"
      "  loop { break; }\n"
      "  block done: br done;\n"
      "  unsafe control { let raw = ptrcast<ptr<u8>>(p); }\n"
      "  comptime if target.arch == \"x86_64\" { const line: usize = 64; } "
      "else { const line: usize = 32; }\n"
      "  comptime for i in 0..4 { let z = i; }\n"
      "  requires feature(\"avx2\") { let v: vec<8, u32> = "
      "cast<vec<8,u32>>(0); }\n"
      "  native x86_64 inputs(acc) outputs(out) { add { dst: out, src: acc }; "
      "mov { dst: out, src: mem<u64>[p + 8] } @nf; }\n"
      "  exact x86_64 { bytes { 0x90, 0x90 }; ret; }\n"
      "  static_assert(1 == 1);\n"
      "  return atomic.load.u64 [q] @order(acquire) ? out : acc;\n"
      "}\n";
  AstLexedFile lexed;
  AstParseResult parsed;
  CHECK(parse_text(src, &lexed, &parsed) == 0);
  if (lexed.error_count || parsed.error_count) {
    ast_print_lexer_diagnostics(stderr, &lexed, NULL);
    ast_print_parser_diagnostics(stderr, &lexed.source, &parsed, NULL);
  }
  CHECK(lexed.error_count == 0u);
  CHECK(parsed.error_count == 0u);
  CHECK(parsed.module != NULL);
  CHECK(ast_name_equal_cstr(parsed.module->name, "demo.core"));
  CHECK(parsed.module->declarations.count >= 10u);
  CHECK(parsed.stats.ast_nodes > 80u);
  ast_parse_result_destroy(&parsed);
  ast_lexed_file_destroy(&lexed);
  return 0;
}

static int test_recovery(void) {
  static const char src[] = "module broken;\n"
                            "const x: u64 = ;\n"
                            "type Bad = ptr<u64;\n"
                            "export fn good(a: u64) -> u64 { return a + 1; }\n";
  AstLexedFile lexed;
  AstParseResult parsed;
  CHECK(parse_text(src, &lexed, &parsed) == 0);
  CHECK(parsed.error_count > 0u);
  CHECK(parsed.module != NULL);
  int found_good = 0;
  for (uint32_t i = 0; i < parsed.module->declarations.count; ++i) {
    AstDecl *d = parsed.module->declarations.items[i];
    if (d && d->kind == AST_DECL_FN && d->as.fn_decl.name.text &&
        ast_name_equal_cstr(d->as.fn_decl.name, "good"))
      found_good = 1;
  }
  CHECK(found_good);
  ast_parse_result_destroy(&parsed);
  ast_lexed_file_destroy(&lexed);
  return 0;
}

static int test_parallel(void) {
  enum { N = 64 };
  AstSourceFile sources[N];
  char *texts[N];
  for (int i = 0; i < N; ++i) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer),
             "module parallel.m%d; export fn func%d(x:u64)->u64 { return x * 3 "
             "+ %d; }",
             i, i, i);
    const size_t text_len = strlen(buffer);
    texts[i] = (char *)malloc(text_len + 1u);
    CHECK(texts[i] != NULL);
    memcpy(texts[i], buffer, text_len + 1u);
    sources[i].path = "parallel.aa";
    sources[i].data = (const uint8_t *)texts[i];
    sources[i].size = text_len;
  }
  AstLexBatch lexed;
  CHECK(ast_lex_sources_parallel(sources, N, NULL, 8u, &lexed) == 0);
  AstParseBatch parsed;
  CHECK(ast_parse_files_parallel(lexed.files, N, 1u, NULL, 8u, &parsed) == 0);
  for (size_t i = 0; i < N; ++i) {
    CHECK(lexed.files[i].error_count == 0u);
    if (parsed.files[i].error_count != 0u) {
      fprintf(stderr, "parallel parser failure at index %zu\n", i);
      ast_print_parser_diagnostics(stderr, &lexed.files[i].source,
                                   &parsed.files[i], NULL);
    }
    CHECK(parsed.files[i].error_count == 0u);
    CHECK(parsed.files[i].module != NULL);
  }
  ast_parse_batch_destroy(&parsed);
  ast_lex_batch_destroy(&lexed);
  for (int i = 0; i < N; ++i)
    free(texts[i]);
  return 0;
}

static int test_hygienic_macro_expansion(void) {
  static const char src[] =
      "module hygienic;"
      "macro make($x:expr) { let tmp: u64 = $x; tmp = tmp + 1; }"
      "macro paste($x:ident) { let $x##value: u64 = 1; }"
      "fn f(tmp: u64) -> u64 { make!(1); return tmp; }"
      "fn g() -> u64 { paste!(local); return localvalue; }";
  AstLexedFile lexed;
  AstParseResult parsed;
  CHECK(parse_text(src, &lexed, &parsed) == 0);
  CHECK(lexed.error_count == 0u);
  if (parsed.error_count)
    ast_print_parser_diagnostics(stderr, &lexed.source, &parsed, NULL);
  CHECK(parsed.error_count == 0u);
  CHECK(parsed.module && parsed.module->declarations.count == 4u);
  AstDecl *fn = parsed.module->declarations.items[2];
  CHECK(fn && fn->kind == AST_DECL_FN && fn->as.fn_decl.body);
  CHECK(fn->as.fn_decl.body->statements.count == 3u);
  AstStmt *decl = fn->as.fn_decl.body->statements.items[0];
  AstStmt *assign = fn->as.fn_decl.body->statements.items[1];
  CHECK(decl && decl->kind == AST_STMT_LET);
  CHECK(assign && assign->kind == AST_STMT_ASSIGN);
  CHECK(!ast_name_equal(decl->as.let_stmt.name,
                        fn->as.fn_decl.params.items[0].name));
  CHECK(ast_name_equal(assign->as.assign_stmt.name, decl->as.let_stmt.name));
  AstDecl *paste_fn = parsed.module->declarations.items[3];
  CHECK(paste_fn && paste_fn->kind == AST_DECL_FN && paste_fn->as.fn_decl.body);
  CHECK(paste_fn->as.fn_decl.body->statements.count == 2u);
  CHECK(ast_name_equal_cstr(
      paste_fn->as.fn_decl.body->statements.items[0]->as.let_stmt.name,
      "localvalue"));
  ast_parse_result_destroy(&parsed);
  ast_lexed_file_destroy(&lexed);
  return 0;
}

int main(void) {
  CHECK(test_full_core() == 0);
  CHECK(test_recovery() == 0);
  CHECK(test_parallel() == 0);
  CHECK(test_hygienic_macro_expansion() == 0);
  puts("parser tests: OK");
  return 0;
}
