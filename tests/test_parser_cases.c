#include "asterasm/asterasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ParserCase {
  const char *name;
  const char *source;
  int expect_errors;
  uint32_t recursion_limit;
} ParserCase;

static const ParserCase cases[] = {
    {"nested_generics", "module t; type T = ptr<ptr<ptr<const u8>>>;", 0, 0},
    {"shift_expression", "module t; fn f(x:u64)->u64 { return x >> 3; }", 0, 0},
    {"atomic_load",
     "module t; fn f(p:ptr<u32>)->u32 { return atomic.load.u32 [p] @acquire; }",
     0, 0},
    {"atomic_store",
     "module t; fn f(p:ptr<u32>) { atomic.store.u32 [p] = 1 @release; }", 0, 0},
    {"atomic_rmw",
     "module t; fn f(p:ptr<u32>)->u32 { return atomic.rmw.add.u32 [p], 1 "
     "@acq_rel; }",
     0, 0},
    {"atomic_cas",
     "module t; fn f(p:ptr<u64>,a:u64,b:u64)->bool { return atomic.cas.u64 "
     "[p], a, b @seq_cst; }",
     0, 0},
    {"native_bridge",
     "module t; fn f(x:u64)->u64 { let y:u64; native x86_64 inputs(x) "
     "outputs(y) { mov { dst:y, src:x }; } return y; }",
     0, 0},
    {"unsafe_exact_bytes",
     "module t; fn f() @naked { unsafe exact x86_64 { bytes {0x90,0x90}; ret; "
     "} }",
     0, 0},
    {"macro_tokens",
     "module t; macro m($x:expr,$n:const usize){ comptime for i in 0..$n { let "
     "y = $x + i; } }",
     0, 0},
    {"unicode_names",
     "module пример.ядро; const число:u64=1; fn сумма(арг:u64)->u64{return "
     "арг+число;}",
     0, 0},
    {"struct_attributes",
     "module t; export type S = struct @layout(c) { x:u32 @align(4); y:u16; };",
     0, 0},
    {"dispatch",
     "module t; dispatch fn pick(x:u64)->u64 { when feature(\"avx2\") => fast; "
     "else => slow; }",
     0, 0},
    {"comptime_constructs",
     "module t; fn f(){ comptime if true { const x:u64=1; } else { const "
     "x:u64=2; } comptime for i in 0..4 { let y=i; } }",
     0, 0},
    {"nested_initializer",
     "module t; section \".data\" { global a:[[u8;2];2]={{1,2},{3,4}}; }", 0,
     0},
    {"target_features",
     "module t; target \"x86_64-unknown-linux-gnu\" { cpu=\"baseline\"; "
     "feature + \"avx2\"; feature - \"avx512f\"; }",
     0, 0},
    {"qualified_keyword_import",
     "module t; import core.bits as b; fn f()->u64 { return b.word_bits; }", 0,
     0},
    {"missing_module_semicolon", "module broken fn f(){}", 1, 0},
    {"missing_generic_close", "module t; type T=ptr<ptr<u64>; fn good(){}", 1,
     0},
    {"malformed_parameter", "module t; fn bad(x u64){} fn good(){}", 1, 0},
    {"expression_recovery", "module t; const x:u64=; fn good()->u64{return 1;}",
     1, 0},
    {"native_positional_operand",
     "module t; fn f(){ native x86_64 { mov rax, rbx; } }", 1, 0},
    {"malformed_import_alias", "module t; import core.bits as ; fn good(){}", 1,
     0},
    {"missing_block_close",
     "module t; fn bad(){ if true { let x=1; } fn good(){}", 1, 0},
    {"invalid_generic_param", "module t; type T<X:banana>=X;", 1, 0},
    {"atomic_missing_dot",
     "module t; fn f(p:ptr<u32>)->u32{return atomic load.u32[p];}", 1, 0},
    {"bad_array_type", "module t; type T=[u8 4]; fn good(){}", 1, 0},
    {"top_level_recovery",
     "module t; const a:u64=1 + ; export fn recovered()->u64{return 9;}", 1, 0},
    {"recursion_limit",
     "module t; fn f()->u64{return "
     "(((((((((((((((((((((((((((((((((((((1)))))))))))))))))))))))))))))))))))"
     ")));}",
     1, 8},
    {"contextual_params_locals",
     "module t; fn f(target:u64, feature:u64, reg:u64)->u64 { let result:u64 = "
     "target + feature + reg; return result; }",
     0, 0},
    {"contextual_assign",
     "module t; fn f()->u64 { let reg:u64 = 1; reg = reg + 1; return reg; }", 0,
     0},
    {"contextual_types",
     "module t; type feature=u64; type target=ptr<feature>; fn "
     "f(reg:target)->feature { return 1; }",
     0, 0},
    {"contextual_generic_types",
     "module t; type feature=u64; type Box<T:type>=ptr<T>; type "
     "Some=Box<feature>;",
     0, 0},
    {"contextual_intrinsics",
     "module t; fn f()->bool { return feature(\"avx2\") && target(\"x86_64\"); "
     "}",
     0, 0},
    {"section_keyword_name_recovery",
     "module t; section \".data\" { global ptr:ptr<u64>=0; } fn good(){}", 1,
     0},
    {"section_integer_recovery",
     "module t; section \".data\" { 42; global good:u64=1; }", 1, 0},
    {"section_statement_recovery",
     "module t; section \".data\" { let x:u64=1; global good:u64=2; }", 1, 0},
    {"section_export_recovery",
     "module t; section \".data\" { export 42; global good:u64=2; }", 1, 0},
    {"struct_statement_recovery", "module t; type T=struct { let; good:u64; };",
     1, 0},
    {"target_expression_recovery",
     "module t; target \"aarch64-linux-gnu\" { 42; cpu=\"baseline\"; }", 1, 0}};

static int run_case(size_t index) {
  if (index >= sizeof(cases) / sizeof(cases[0]))
    return 2;
  const ParserCase *c = &cases[index];
  AstSourceFile source = {c->name, (const uint8_t *)c->source,
                          strlen(c->source)};
  AstLexedFile lexed;
  if (ast_lex_source(&source, NULL, &lexed) != 0)
    return 2;
  AstParseConfig cfg = ast_parse_config_default();
  if (c->recursion_limit)
    cfg.recursion_limit = c->recursion_limit;
  AstParseResult parsed;
  int rc = ast_parse_file(&lexed, 1u, &cfg, &parsed);
  if (rc != 0) {
    ast_lexed_file_destroy(&lexed);
    return 2;
  }
  size_t errors = lexed.error_count + parsed.error_count;
  int ok = c->expect_errors ? errors > 0u : errors == 0u;
  if (!ok) {
    fprintf(stderr, "parser case '%s' expected %s errors, got %zu\n", c->name,
            c->expect_errors ? "one or more" : "zero", errors);
    ast_print_lexer_diagnostics(stderr, &lexed, NULL);
    ast_print_parser_diagnostics(stderr, &lexed.source, &parsed, NULL);
  }
  ast_parse_result_destroy(&parsed);
  ast_lexed_file_destroy(&lexed);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <case-index>\n", argv[0]);
    return 2;
  }
  char *end = NULL;
  unsigned long value = strtoul(argv[1], &end, 10);
  if (!end || *end != '\0')
    return 2;
  return run_case((size_t)value);
}
