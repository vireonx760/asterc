#include "macro_expand.h"
#include "parser_internal.h"

typedef struct AstMacroParseOwner {
  AstLexedFile lexed;
  uint8_t *source;
} AstMacroParseOwner;

AstParseConfig ast_parse_config_default(void) {
  AstParseConfig c;
  c.recursion_limit = ASTER_DEFAULT_PARSE_RECURSION_LIMIT;
  c.max_diagnostics = ASTER_DEFAULT_MAX_PARSE_DIAGNOSTICS;
  c.arena_block_size = 128u * 1024u;
  c.allow_missing_module = 0u;
  return c;
}

const char *ast_parse_diag_code_name(AstParseDiagCode code) {
  switch (code) {
  case AST_PARSE_DIAG_NONE:
    return "none";
  case AST_PARSE_DIAG_EXPECTED_TOKEN:
    return "expected_token";
  case AST_PARSE_DIAG_EXPECTED_IDENTIFIER:
    return "expected_identifier";
  case AST_PARSE_DIAG_EXPECTED_EXPRESSION:
    return "expected_expression";
  case AST_PARSE_DIAG_EXPECTED_TYPE:
    return "expected_type";
  case AST_PARSE_DIAG_EXPECTED_DECLARATION:
    return "expected_declaration";
  case AST_PARSE_DIAG_EXPECTED_STATEMENT:
    return "expected_statement";
  case AST_PARSE_DIAG_INVALID_TOP_LEVEL:
    return "invalid_top_level";
  case AST_PARSE_DIAG_DUPLICATE_MODULE_DECL:
    return "duplicate_module_decl";
  case AST_PARSE_DIAG_MISSING_MODULE_DECL:
    return "missing_module_decl";
  case AST_PARSE_DIAG_INVALID_ASSIGNMENT_TARGET:
    return "invalid_assignment_target";
  case AST_PARSE_DIAG_INVALID_NATIVE_OPERAND:
    return "invalid_native_operand";
  case AST_PARSE_DIAG_INVALID_MACRO_PARAMETER:
    return "invalid_macro_parameter";
  case AST_PARSE_DIAG_RECURSION_LIMIT:
    return "recursion_limit";
  case AST_PARSE_DIAG_TOO_MANY_DIAGNOSTICS:
    return "too_many_diagnostics";
  case AST_PARSE_DIAG_OUT_OF_MEMORY:
    return "out_of_memory";
  default:
    return "unknown";
  }
}

int ast_parse_file(const AstLexedFile *lexed, uint32_t file_id,
                   const AstParseConfig *config, AstParseResult *out) {
  if (!lexed || !out)
    return -1;
  memset(out, 0, sizeof(*out));
  AstParseConfig cfg = config ? *config : ast_parse_config_default();
  if (!cfg.arena_block_size)
    cfg.arena_block_size = 128u * 1024u;

  AstLexedFile expanded;
  uint8_t *expanded_source = NULL;
  int expansion = ast_macro_expand_file(lexed, &expanded, &expanded_source);
  const AstLexedFile *parse_lexed = expansion > 0 ? &expanded : lexed;
  AstMacroParseOwner *owner = NULL;
  if (expansion > 0) {
    owner = (AstMacroParseOwner *)calloc(1u, sizeof(*owner));
    if (!owner) {
      ast_lexed_file_destroy(&expanded);
      free(expanded_source);
      return -1;
    }
    owner->lexed = expanded;
    owner->source = expanded_source;
    out->internal = owner;
  }
  ast_arena_init(&out->arena, cfg.arena_block_size);

  AstParser p;
  ast_parser_init(&p, parse_lexed, file_id, &cfg, out);
  AstBuildVec file_attrs;
  ast_vec_init(&file_attrs, sizeof(AstAttribute), ASTER_ALIGNOF(AstAttribute));
  AstAttributeArray attrs = ast_parse_attributes(&p);
  ASTER_UNUSED(file_attrs);

  AstModule *module = AST_ARENA_NEW(&out->arena, AstModule);
  if (!module)
    p.out_of_memory = 1u;
  else {
    p.ast_nodes++;
    module->file_id = file_id;
    module->attributes = attrs;
  }

  if (ast_p_kind(&p) == AST_TOK_KW_MODULE) {
    const AstToken *start = ast_p_advance(&p);
    AstName name;
    memset(&name, 0, sizeof(name));
    ast_parse_qualified_name(&p, &name);
    const AstToken *semi = ast_p_expect(&p, AST_TOK_SEMICOLON, "';'");
    if (module) {
      module->name = name;
      module->span = semi ? ast_span_join(ast_span_token(&p, start),
                                          ast_span_token(&p, semi))
                          : ast_span_token(&p, start);
    }
  } else if (!cfg.allow_missing_module) {
    ast_p_diag(&p, AST_PARSE_DIAG_MISSING_MODULE_DECL, AST_DIAG_ERROR,
               ast_span_current(&p),
               "source file must begin with a module declaration");
  }

  AstBuildVec decls;
  ast_vec_init(&decls, sizeof(AstDecl *), ASTER_ALIGNOF(AstDecl *));
  while (ast_p_kind(&p) != AST_TOK_EOF) {
    size_t before = p.token_index;
    AstDecl *d = ast_parse_top_decl(&p);
    if (d)
      ast_vec_push(&p, &decls, &d);
    if (p.panic_mode)
      ast_p_sync_top_level(&p);
    if (p.token_index == before && ast_p_kind(&p) != AST_TOK_EOF)
      ast_p_advance(&p);
  }
  if (module) {
    module->declarations.items = (AstDecl **)ast_vec_finish(&p, &decls);
    module->declarations.count = decls.count;
    if (p.previous)
      module->span =
          ast_span_join(module->span, ast_span_token(&p, p.previous));
  }
  out->module = module;

  if (p.out_of_memory) {
    ast_p_diag(&p, AST_PARSE_DIAG_OUT_OF_MEMORY, AST_DIAG_ERROR,
               ast_span_current(&p), "parser arena allocation failed");
  }
  ast_parser_finish(&p);
  return p.out_of_memory ? -1 : 0;
}

const AstLexedFile *ast_parse_expanded_lexed(const AstParseResult *result) {
  const AstMacroParseOwner *owner =
      result ? (const AstMacroParseOwner *)result->internal : NULL;
  return owner ? &owner->lexed : NULL;
}

void ast_parse_result_destroy(AstParseResult *result) {
  if (!result)
    return;
  AstMacroParseOwner *owner = (AstMacroParseOwner *)result->internal;
  if (owner) {
    ast_lexed_file_destroy(&owner->lexed);
    free(owner->source);
    free(owner);
  }
  ast_arena_destroy(&result->arena);
  memset(result, 0, sizeof(*result));
}
