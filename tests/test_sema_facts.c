#include "asterasm/diagnostics.h"
#include "asterasm/sema.h"

#include <stdio.h>
#include <string.h>

#ifndef ASTERASM_SOURCE_DIR
#define ASTERASM_SOURCE_DIR "."
#endif

static const AstDecl *find_function(const AstModule *module, const char *name) {
  if (!module)
    return NULL;
  for (uint32_t i = 0u; i < module->declarations.count; ++i) {
    const AstDecl *decl = module->declarations.items[i];
    if (decl && decl->kind == AST_DECL_FN &&
        ast_name_equal_cstr(decl->as.fn_decl.name, name))
      return decl;
  }
  return NULL;
}

int main(void) {
  char root[4096];
  char entry[4096];
  int rn = snprintf(root, sizeof(root), "%s/tests/sema_cases/facts_success",
                    ASTERASM_SOURCE_DIR);
  int en = snprintf(entry, sizeof(entry), "%s/main.aa", root);
  if (rn <= 0 || en <= 0 || (size_t)rn >= sizeof(root) ||
      (size_t)en >= sizeof(entry))
    return 2;

  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 4u;
  AstFrontend *frontend = ast_frontend_create(&fc);
  if (!frontend)
    return 2;
  if (ast_frontend_add_import_root(frontend, root) != 0)
    return 2;
  const char *entries[] = {entry};
  if (ast_frontend_load(frontend, entries, 1u) != 0)
    return 1;

  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 5u;
  AstSema *sema = ast_sema_create(frontend, &sc);
  if (!sema)
    return 2;
  if (ast_sema_check(sema) != 0) {
    ast_print_sema_diagnostics(stderr, sema, NULL);
    return 1;
  }

  AstFrontendFile file;
  if (!ast_frontend_find_module(frontend, "sema.facts_success", &file) ||
      !file.parsed || !file.parsed->module)
    return 1;
  const AstDecl *fn = find_function(file.parsed->module, "f");
  if (!fn || !fn->as.fn_decl.body ||
      fn->as.fn_decl.body->statements.count != 3u)
    return 1;
  const AstStmt *let_stmt = fn->as.fn_decl.body->statements.items[0];
  const AstStmt *spare_stmt = fn->as.fn_decl.body->statements.items[1];
  const AstStmt *return_stmt = fn->as.fn_decl.body->statements.items[2];
  if (!let_stmt || let_stmt->kind != AST_STMT_LET ||
      !let_stmt->as.let_stmt.value ||
      let_stmt->as.let_stmt.value->kind != AST_EXPR_BINARY || !return_stmt ||
      return_stmt->kind != AST_STMT_RETURN)
    return 1;

  const AstExpr *sum = let_stmt->as.let_stmt.value;
  const AstExpr *param = sum->as.binary.left;
  const AstExpr *constant = sum->as.binary.right;
  const AstExpr *local = return_stmt->as.return_stmt.value;
  AstSemaExprFact pf, kf, sf, lf;
  if (!ast_sema_expr_fact(sema, param, &pf) ||
      !ast_sema_expr_fact(sema, constant, &kf) ||
      !ast_sema_expr_fact(sema, sum, &sf) ||
      !ast_sema_expr_fact(sema, local, &lf))
    return 1;

  if (pf.resolution_kind != AST_SEMA_RESOLUTION_PARAMETER ||
      pf.declaration_span.offset !=
          fn->as.fn_decl.params.items[0].name.span.offset ||
      ast_sema_type_kind(pf.type) != AST_SEMA_TYPE_INT ||
      ast_sema_type_bit_width(pf.type) != 32u)
    return 1;
  if (kf.resolution_kind != AST_SEMA_RESOLUTION_TOP_LEVEL ||
      kf.constant.kind != AST_SEMA_CONST_INTEGER || kf.constant.is_negative ||
      kf.constant.hi != 0u || kf.constant.lo != 7u ||
      ast_sema_type_bit_width(kf.type) != 32u)
    return 1;
  if (ast_sema_type_kind(sf.type) != AST_SEMA_TYPE_INT ||
      ast_sema_type_bit_width(sf.type) != 32u)
    return 1;
  if (lf.resolution_kind != AST_SEMA_RESOLUTION_LOCAL ||
      lf.declaration_span.offset != let_stmt->as.let_stmt.name.span.offset ||
      ast_sema_type_bit_width(lf.type) != 32u)
    return 1;

  if (!spare_stmt || spare_stmt->kind != AST_STMT_LET ||
      !spare_stmt->as.let_stmt.type)
    return 1;
  AstSemaTypeFact spare_type_fact;
  if (!ast_sema_type_fact(sema, spare_stmt->as.let_stmt.type,
                          &spare_type_fact) ||
      spare_type_fact.context_dependent ||
      ast_sema_type_kind(spare_type_fact.type) != AST_SEMA_TYPE_PTR ||
      (ast_sema_type_pointer_qualifiers(spare_type_fact.type) &
       AST_TYPE_QUAL_CONST) == 0u)
    return 1;
  const AstSemaType *spare_element =
      ast_sema_type_element(spare_type_fact.type);
  if (!spare_element ||
      ast_sema_type_kind(spare_element) != AST_SEMA_TYPE_INT ||
      ast_sema_type_bit_width(spare_element) != 16u ||
      ast_sema_type_is_signed(spare_element))
    return 1;

  const AstDecl *generic_call =
      find_function(file.parsed->module, "generic_call");
  if (!generic_call || !generic_call->as.fn_decl.body ||
      generic_call->as.fn_decl.body->statements.count != 1u)
    return 1;
  const AstStmt *generic_return =
      generic_call->as.fn_decl.body->statements.items[0];
  if (!generic_return || generic_return->kind != AST_STMT_RETURN ||
      !generic_return->as.return_stmt.value ||
      generic_return->as.return_stmt.value->kind != AST_EXPR_CALL)
    return 1;
  const AstExpr *generic_call_expr = generic_return->as.return_stmt.value;
  AstSemaExprFact call_fact, callee_fact;
  if (!ast_sema_expr_fact(sema, generic_call_expr, &call_fact) ||
      !call_fact.call_signature ||
      ast_sema_type_kind(call_fact.call_signature) != AST_SEMA_TYPE_FUNCTION ||
      ast_sema_type_function_param_count(call_fact.call_signature) != 1u ||
      ast_sema_type_bit_width(
          ast_sema_type_function_param(call_fact.call_signature, 0u)) != 16u ||
      ast_sema_type_bit_width(
          ast_sema_type_function_result(call_fact.call_signature)) != 16u)
    return 1;
  if (!ast_sema_expr_fact(sema, generic_call_expr->as.call.callee,
                          &callee_fact) ||
      callee_fact.resolution_kind != AST_SEMA_RESOLUTION_TOP_LEVEL)
    return 1;

  const AstDecl *effects_fn =
      find_function(file.parsed->module, "effect_probe");
  if (!effects_fn || !effects_fn->as.fn_decl.body ||
      effects_fn->as.fn_decl.body->statements.count != 5u)
    return 1;
  const AstStmt *load_stmt = effects_fn->as.fn_decl.body->statements.items[0];
  const AstStmt *atomic_stmt = effects_fn->as.fn_decl.body->statements.items[1];
  const AstStmt *call_stmt = effects_fn->as.fn_decl.body->statements.items[2];
  const AstStmt *div_stmt = effects_fn->as.fn_decl.body->statements.items[3];
  if (!load_stmt || !atomic_stmt || !call_stmt || !div_stmt ||
      load_stmt->kind != AST_STMT_LET || atomic_stmt->kind != AST_STMT_LET ||
      call_stmt->kind != AST_STMT_LET || div_stmt->kind != AST_STMT_LET)
    return 1;
  AstSemaExprFact load_effect, atomic_effect, call_effect, div_effect;
  if (!ast_sema_expr_fact(sema, load_stmt->as.let_stmt.value, &load_effect) ||
      !ast_sema_expr_fact(sema, atomic_stmt->as.let_stmt.value,
                          &atomic_effect) ||
      !ast_sema_expr_fact(sema, call_stmt->as.let_stmt.value, &call_effect) ||
      !ast_sema_expr_fact(sema, div_stmt->as.let_stmt.value, &div_effect))
    return 1;
  if ((load_effect.effects &
       (AST_SEMA_EFFECT_READ_MEMORY | AST_SEMA_EFFECT_VOLATILE)) !=
      (AST_SEMA_EFFECT_READ_MEMORY | AST_SEMA_EFFECT_VOLATILE))
    return 1;
  if ((atomic_effect.effects &
       (AST_SEMA_EFFECT_READ_MEMORY | AST_SEMA_EFFECT_ATOMIC)) !=
      (AST_SEMA_EFFECT_READ_MEMORY | AST_SEMA_EFFECT_ATOMIC))
    return 1;
  if ((call_effect.effects & AST_SEMA_EFFECT_CALL) == 0u)
    return 1;
  if ((div_effect.effects & AST_SEMA_EFFECT_MAY_TRAP) == 0u)
    return 1;

  const AstDecl *ct = find_function(file.parsed->module, "compile_time_facts");
  if (!ct || !ct->as.fn_decl.body ||
      ct->as.fn_decl.body->statements.count != 1u)
    return 1;
  const AstStmt *comptime = ct->as.fn_decl.body->statements.items[0];
  if (!comptime || comptime->kind != AST_STMT_COMPTIME ||
      !comptime->as.comptime_stmt.inner ||
      comptime->as.comptime_stmt.inner->kind != AST_STMT_FOR)
    return 1;
  const AstStmt *for_stmt = comptime->as.comptime_stmt.inner;
  if (!for_stmt->as.for_stmt.body ||
      for_stmt->as.for_stmt.body->statements.count != 1u)
    return 1;
  const AstStmt *assert_stmt = for_stmt->as.for_stmt.body->statements.items[0];
  if (!assert_stmt || assert_stmt->kind != AST_STMT_STATIC_ASSERT ||
      !assert_stmt->as.static_assert_stmt.condition ||
      assert_stmt->as.static_assert_stmt.condition->kind != AST_EXPR_BINARY)
    return 1;
  const AstExpr *iter_name =
      assert_stmt->as.static_assert_stmt.condition->as.binary.left;
  AstSemaExprFact iter_fact;
  if (!ast_sema_expr_fact(sema, iter_name, &iter_fact) ||
      !iter_fact.context_dependent ||
      iter_fact.constant.kind != AST_SEMA_CONST_NONE ||
      iter_fact.resolution_kind != AST_SEMA_RESOLUTION_LOCAL_CONST)
    return 1;

  ast_sema_destroy(sema);
  ast_frontend_destroy(frontend);
  puts("sema facts test passed");
  return 0;
}
