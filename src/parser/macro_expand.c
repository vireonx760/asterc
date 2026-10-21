#include "macro_expand.h"

#include "asterasm/common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MacroToken {
  AstTokenKind kind;
  const char *text;
  size_t length;
  char *owned;
} MacroToken;

typedef struct MacroTokenVec {
  MacroToken *items;
  size_t count;
  size_t capacity;
} MacroTokenVec;

typedef struct MacroParam {
  const char *name;
  size_t length;
  AstTokenKind category;
  uint8_t is_const;
} MacroParam;

typedef struct MacroDef {
  const char *name;
  size_t name_length;
  size_t body_first;
  size_t body_last;
  MacroParam *params;
  size_t param_count;
} MacroDef;

typedef struct MacroDefVec {
  MacroDef *items;
  size_t count;
  size_t capacity;
} MacroDefVec;

typedef struct MacroRename {
  const char *name;
  size_t length;
  char *replacement;
} MacroRename;

typedef struct MacroRenameVec {
  MacroRename *items;
  size_t count;
  size_t capacity;
} MacroRenameVec;

typedef struct MacroBuffer {
  unsigned char *data;
  size_t count;
  size_t capacity;
} MacroBuffer;

typedef struct MacroContext {
  const AstLexedFile *input;
  const uint8_t *source;
  MacroTokenVec tokens;
  MacroDefVec defs;
  MacroBuffer source_out;
  uint64_t expansion_id;
  uint32_t depth;
  uint8_t found_invocation;
} MacroContext;

static void macro_token_vec_destroy(MacroTokenVec *v) {
  if (!v)
    return;
  for (size_t i = 0u; i < v->count; ++i)
    free(v->items[i].owned);
  free(v->items);
  memset(v, 0, sizeof(*v));
}

static void macro_defs_destroy(MacroDefVec *v) {
  if (!v)
    return;
  for (size_t i = 0u; i < v->count; ++i)
    free(v->items[i].params);
  free(v->items);
  memset(v, 0, sizeof(*v));
}

static void macro_renames_destroy(MacroRenameVec *v) {
  if (!v)
    return;
  for (size_t i = 0u; i < v->count; ++i)
    free(v->items[i].replacement);
  free(v->items);
  memset(v, 0, sizeof(*v));
}

static int macro_reserve(void **data, size_t element_size, size_t *capacity,
                         size_t need) {
  if (*capacity >= need)
    return 0;
  size_t cap = *capacity ? *capacity : 16u;
  while (cap < need) {
    if (cap > SIZE_MAX / 2u)
      return -1;
    cap *= 2u;
  }
  size_t bytes;
  if (ast_size_mul_overflow(cap, element_size, &bytes))
    return -1;
  void *fresh = realloc(*data, bytes);
  if (!fresh)
    return -1;
  *data = fresh;
  *capacity = cap;
  return 0;
}

static int macro_token_push(MacroTokenVec *v, MacroToken token) {
  if (!v || (!token.text && token.length != 0u))
    return -1;
  if (macro_reserve((void **)&v->items, sizeof(*v->items), &v->capacity,
                    v->count + 1u) != 0)
    return -1;
  v->items[v->count++] = token;
  return 0;
}

static int macro_buffer_push(MacroBuffer *b, const void *data, size_t length) {
  if (!length)
    return 0;
  if (b->count > SIZE_MAX - length)
    return -1;
  size_t need = b->count + length;
  if (macro_reserve((void **)&b->data, sizeof(*b->data), &b->capacity, need) !=
      0)
    return -1;
  memcpy(b->data + b->count, data, length);
  b->count = need;
  return 0;
}

static int macro_buffer_char(MacroBuffer *b, char c) {
  return macro_buffer_push(b, &c, 1u);
}

static int macro_name_equal(const MacroDef *def, const MacroToken *token) {
  return def && token && def->name_length == token->length &&
         memcmp(def->name, token->text, token->length) == 0;
}

static MacroToken macro_token_ref(const MacroToken *token) {
  MacroToken copy = *token;
  if (token && token->owned) {
    char *owned = (char *)malloc(token->length + 1u);
    if (!owned) {
      memset(&copy, 0, sizeof(copy));
      copy.length = SIZE_MAX;
      return copy;
    }
    memcpy(owned, token->text, token->length);
    owned[token->length] = '\0';
    copy.text = owned;
    copy.owned = owned;
  }
  return copy;
}

static MacroToken macro_token_owned(AstTokenKind kind, char *text,
                                    size_t length) {
  MacroToken token;
  memset(&token, 0, sizeof(token));
  token.kind = kind;
  token.text = text;
  token.length = length;
  token.owned = text;
  return token;
}

static int macro_token_concat(MacroToken *left, const MacroToken *right) {
  if (!left || !right || left->length > SIZE_MAX - right->length)
    return -1;
  size_t length = left->length + right->length;
  char *text = (char *)malloc(length + 1u);
  if (!text)
    return -1;
  memcpy(text, left->text, left->length);
  memcpy(text + left->length, right->text, right->length);
  text[length] = '\0';
  free(left->owned);
  left->owned = text;
  left->text = text;
  left->length = length;
  left->kind = AST_TOK_IDENTIFIER;
  return 0;
}

static int macro_rename_push(MacroRenameVec *v, const char *name, size_t length,
                             char *replacement) {
  if (macro_reserve((void **)&v->items, sizeof(*v->items), &v->capacity,
                    v->count + 1u) != 0) {
    free(replacement);
    return -1;
  }
  v->items[v->count].name = name;
  v->items[v->count].length = length;
  v->items[v->count].replacement = replacement;
  v->count++;
  return 0;
}

static const MacroRename *macro_find_rename(const MacroRenameVec *v,
                                            const MacroToken *token) {
  if (!v || !token)
    return NULL;
  for (size_t i = 0u; i < v->count; ++i) {
    if (v->items[i].length == token->length &&
        memcmp(v->items[i].name, token->text, token->length) == 0)
      return &v->items[i];
  }
  return NULL;
}

static const MacroParam *macro_find_param(const MacroDef *def,
                                          const MacroToken *token) {
  if (!def || !token)
    return NULL;
  for (size_t i = 0u; i < def->param_count; ++i) {
    if (def->params[i].length == token->length &&
        memcmp(def->params[i].name, token->text, token->length) == 0)
      return &def->params[i];
  }
  return NULL;
}

static int macro_collect_tokens(MacroContext *ctx) {
  if (!ctx || !ctx->input)
    return -1;
  for (size_t i = 0u; i < ctx->input->token_count; ++i) {
    const AstToken *token = ast_token_at(ctx->input, i);
    if (!token || token->kind == AST_TOK_EOF ||
        (token->flags & AST_TOKENF_TRIVIA))
      continue;
    if ((size_t)token->offset > ctx->input->source.size ||
        (size_t)token->length > ctx->input->source.size - (size_t)token->offset)
      return -1;
    MacroToken copy;
    memset(&copy, 0, sizeof(copy));
    copy.kind = (AstTokenKind)token->kind;
    copy.text = (const char *)ctx->input->source.data + token->offset;
    copy.length = token->length;
    if (macro_token_push(&ctx->tokens, copy) != 0)
      return -1;
  }
  return 0;
}

static size_t macro_find_matching(const MacroTokenVec *tokens, size_t open,
                                  AstTokenKind left, AstTokenKind right) {
  if (!tokens || open >= tokens->count || tokens->items[open].kind != left)
    return SIZE_MAX;
  uint32_t depth = 0u;
  for (size_t i = open; i < tokens->count; ++i) {
    AstTokenKind kind = tokens->items[i].kind;
    if (kind == left) {
      if (depth == UINT32_MAX)
        return SIZE_MAX;
      depth++;
    } else if (kind == right) {
      if (!depth)
        return SIZE_MAX;
      depth--;
      if (!depth)
        return i;
    }
  }
  return SIZE_MAX;
}

static int macro_parse_definitions(MacroContext *ctx) {
  for (size_t i = 0u; i < ctx->tokens.count; ++i) {
    if (ctx->tokens.items[i].kind != AST_TOK_KW_MACRO)
      continue;
    if (i + 2u >= ctx->tokens.count ||
        ctx->tokens.items[i + 1u].kind != AST_TOK_IDENTIFIER ||
        ctx->tokens.items[i + 2u].kind != AST_TOK_LPAREN)
      continue;
    size_t param_close = macro_find_matching(&ctx->tokens, i + 2u,
                                             AST_TOK_LPAREN, AST_TOK_RPAREN);
    if (param_close == SIZE_MAX || param_close + 1u >= ctx->tokens.count ||
        ctx->tokens.items[param_close + 1u].kind != AST_TOK_LBRACE)
      continue;
    size_t body_close = macro_find_matching(&ctx->tokens, param_close + 1u,
                                            AST_TOK_LBRACE, AST_TOK_RBRACE);
    if (body_close == SIZE_MAX)
      continue;

    MacroParam *params = NULL;
    size_t param_count = 0u, param_capacity = 0u;
    size_t p = i + 3u;
    while (p < param_close) {
      if (ctx->tokens.items[p].kind != AST_TOK_DOLLAR ||
          p + 2u >= param_close ||
          ctx->tokens.items[p + 1u].kind != AST_TOK_IDENTIFIER ||
          ctx->tokens.items[p + 2u].kind != AST_TOK_COLON) {
        free(params);
        params = NULL;
        param_count = 0u;
        break;
      }
      size_t q = p + 3u;
      uint8_t is_const = 0u;
      AstTokenKind category = AST_TOK_IDENTIFIER;
      if (q < param_close && ctx->tokens.items[q].kind == AST_TOK_KW_CONST) {
        is_const = 1u;
        q++;
        category = AST_TOK_KW_CONST;
        uint32_t angle = 0u, paren = 0u, bracket = 0u;
        while (q < param_close) {
          AstTokenKind kind = ctx->tokens.items[q].kind;
          if (kind == AST_TOK_LT)
            angle++;
          else if (kind == AST_TOK_GT && angle)
            angle--;
          else if (kind == AST_TOK_SHIFT_RIGHT && angle)
            angle = angle > 1u ? angle - 2u : 0u;
          else if (kind == AST_TOK_LPAREN)
            paren++;
          else if (kind == AST_TOK_RPAREN && paren)
            paren--;
          else if (kind == AST_TOK_LBRACKET)
            bracket++;
          else if (kind == AST_TOK_RBRACKET && bracket)
            bracket--;
          if (kind == AST_TOK_COMMA && !angle && !paren && !bracket)
            break;
          q++;
        }
      } else if (q < param_close) {
        category = ctx->tokens.items[q].kind;
        q++;
      }
      if (macro_reserve((void **)&params, sizeof(*params), &param_capacity,
                        param_count + 1u) != 0) {
        free(params);
        return -1;
      }
      params[param_count].name = ctx->tokens.items[p + 1u].text;
      params[param_count].length = ctx->tokens.items[p + 1u].length;
      params[param_count].category = category;
      params[param_count].is_const = is_const;
      param_count++;
      p = q;
      if (p < param_close && ctx->tokens.items[p].kind == AST_TOK_COMMA)
        p++;
    }
    if (macro_reserve((void **)&ctx->defs.items, sizeof(*ctx->defs.items),
                      &ctx->defs.capacity, ctx->defs.count + 1u) != 0) {
      free(params);
      return -1;
    }
    MacroDef *def = &ctx->defs.items[ctx->defs.count++];
    def->name = ctx->tokens.items[i + 1u].text;
    def->name_length = ctx->tokens.items[i + 1u].length;
    def->body_first = param_close + 2u;
    def->body_last = body_close;
    def->params = params;
    def->param_count = param_count;
    i = body_close;
  }
  return 0;
}

static const MacroDef *macro_find_def(const MacroDefVec *defs,
                                      const MacroToken *token) {
  if (!defs || !token)
    return NULL;
  for (size_t i = 0u; i < defs->count; ++i)
    if (macro_name_equal(&defs->items[i], token))
      return &defs->items[i];
  return NULL;
}

static int macro_is_binding_keyword(AstTokenKind kind) {
  return kind == AST_TOK_KW_LET || kind == AST_TOK_KW_CONST ||
         kind == AST_TOK_KW_FOR || kind == AST_TOK_KW_BLOCK;
}

static int macro_build_renames(MacroContext *ctx, const MacroDef *def,
                               MacroRenameVec *renames) {
  for (size_t i = def->body_first; i < def->body_last; ++i) {
    const MacroToken *token = &ctx->tokens.items[i];
    if (token->kind != AST_TOK_IDENTIFIER || i == def->body_first ||
        !macro_is_binding_keyword(ctx->tokens.items[i - 1u].kind))
      continue;
    if (macro_find_param(def, token) || macro_find_rename(renames, token))
      continue;
    char suffix[64];
    int n = snprintf(suffix, sizeof(suffix), "__aster_macro_%llu_",
                     (unsigned long long)ctx->expansion_id++);
    if (n < 0 || (size_t)n >= sizeof(suffix) ||
        token->length > SIZE_MAX - (size_t)n - 1u)
      return -1;
    size_t length = (size_t)n + token->length;
    char *replacement = (char *)malloc(length + 1u);
    if (!replacement)
      return -1;
    memcpy(replacement, suffix, (size_t)n);
    memcpy(replacement + (size_t)n, token->text, token->length);
    replacement[length] = '\0';
    if (macro_rename_push(renames, token->text, token->length, replacement) !=
        0)
      return -1;
  }
  return 0;
}

static int macro_make_identifier(const MacroToken *token,
                                 const MacroRenameVec *renames,
                                 MacroToken *out) {
  const MacroRename *rename = macro_find_rename(renames, token);
  if (!rename) {
    *out = macro_token_ref(token);
    return 0;
  }
  size_t length = strlen(rename->replacement);
  char *text = (char *)malloc(length + 1u);
  if (!text)
    return -1;
  memcpy(text, rename->replacement, length + 1u);
  *out = macro_token_owned(AST_TOK_IDENTIFIER, text, length);
  return 0;
}

static int macro_push_transformed(MacroTokenVec *out, const MacroToken *token,
                                  const MacroRenameVec *renames) {
  MacroToken transformed;
  if (token->kind == AST_TOK_IDENTIFIER) {
    if (macro_make_identifier(token, renames, &transformed) != 0)
      return -1;
  } else {
    transformed = macro_token_ref(token);
  }
  if (macro_token_push(out, transformed) != 0) {
    free(transformed.owned);
    return -1;
  }
  return 0;
}

static int macro_find_arg(const MacroDef *def, const MacroToken *name,
                          size_t *index_out) {
  for (size_t i = 0u; i < def->param_count; ++i) {
    if (def->params[i].length == name->length &&
        memcmp(def->params[i].name, name->text, name->length) == 0) {
      if (index_out)
        *index_out = i;
      return 1;
    }
  }
  return 0;
}

static int macro_expand_body(MacroContext *ctx, const MacroDef *def,
                             MacroTokenVec *args, size_t arg_count,
                             MacroTokenVec *out) {
  if (arg_count != def->param_count)
    return -1;
  MacroRenameVec renames;
  memset(&renames, 0, sizeof(renames));
  if (macro_build_renames(ctx, def, &renames) != 0) {
    macro_renames_destroy(&renames);
    return -1;
  }
  for (size_t i = def->body_first; i < def->body_last;) {
    const MacroToken *token = &ctx->tokens.items[i];
    if (token->kind == AST_TOK_DOLLAR && i + 1u < def->body_last &&
        ctx->tokens.items[i + 1u].kind == AST_TOK_IDENTIFIER) {
      size_t argument_index;
      if (!macro_find_arg(def, &ctx->tokens.items[i + 1u], &argument_index)) {
        if (macro_token_push(out, macro_token_ref(token)) != 0 ||
            macro_token_push(
                out, macro_token_ref(&ctx->tokens.items[i + 1u])) != 0) {
          macro_renames_destroy(&renames);
          return -1;
        }
        i += 2u;
        continue;
      }
      MacroTokenVec *argument = &args[argument_index];
      for (size_t a = 0u; a < argument->count; ++a)
        if (macro_token_push(out, macro_token_ref(&argument->items[a])) != 0) {
          macro_renames_destroy(&renames);
          return -1;
        }
      i += 2u;
    } else {
      if (macro_push_transformed(out, token, &renames) != 0) {
        macro_renames_destroy(&renames);
        return -1;
      }
      i++;
    }
    if (i < def->body_last && ctx->tokens.items[i].kind == AST_TOK_HASH_HASH) {
      i++;
      if (!out->count || i >= def->body_last) {
        macro_renames_destroy(&renames);
        return -1;
      }
      MacroToken pasted;
      if (macro_push_transformed(out, &ctx->tokens.items[i], &renames) != 0) {
        macro_renames_destroy(&renames);
        return -1;
      }
      pasted = out->items[out->count - 1u];
      out->count--;
      if (macro_token_concat(&out->items[out->count - 1u], &pasted) != 0) {
        free(pasted.owned);
        macro_renames_destroy(&renames);
        return -1;
      }
      free(pasted.owned);
      i++;
    }
  }
  macro_renames_destroy(&renames);
  return 0;
}

static int macro_parse_args(const MacroTokenVec *tokens, size_t open,
                            size_t close, MacroTokenVec **args_out,
                            size_t *count_out) {
  MacroTokenVec *args = NULL;
  size_t count = 0u, capacity = 0u;
  size_t begin = open + 1u;
  uint32_t paren = 0u, bracket = 0u, brace = 0u;
  for (size_t i = begin; i <= close; ++i) {
    AstTokenKind kind = i == close ? AST_TOK_COMMA : tokens->items[i].kind;
    if (kind == AST_TOK_LPAREN)
      paren++;
    else if (kind == AST_TOK_RPAREN && paren)
      paren--;
    else if (kind == AST_TOK_LBRACKET)
      bracket++;
    else if (kind == AST_TOK_RBRACKET && bracket)
      bracket--;
    else if (kind == AST_TOK_LBRACE)
      brace++;
    else if (kind == AST_TOK_RBRACE && brace)
      brace--;
    if (kind == AST_TOK_COMMA && !paren && !bracket && !brace) {
      if (macro_reserve((void **)&args, sizeof(*args), &capacity, count + 1u) !=
          0)
        goto fail;
      memset(&args[count], 0, sizeof(args[count]));
      for (size_t j = begin; j < i; ++j)
        if (macro_token_push(&args[count],
                             macro_token_ref(&tokens->items[j])) != 0)
          goto fail;
      count++;
      begin = i + 1u;
    }
  }
  if (count == 1u && args[0].count == 0u) {
    macro_token_vec_destroy(&args[0]);
    count = 0u;
  }
  *args_out = args;
  *count_out = count;
  return 0;
fail:
  for (size_t i = 0u; i < count; ++i)
    macro_token_vec_destroy(&args[i]);
  free(args);
  return -1;
}

static void macro_args_destroy(MacroTokenVec *args, size_t count) {
  if (!args)
    return;
  for (size_t i = 0u; i < count; ++i)
    macro_token_vec_destroy(&args[i]);
  free(args);
}

static int macro_expand_stream(MacroContext *ctx, const MacroTokenVec *input,
                               MacroTokenVec *output, uint32_t depth) {
  if (depth > 128u)
    return -1;
  for (size_t i = 0u; i < input->count;) {
    const MacroToken *token = &input->items[i];
    if (token->kind == AST_TOK_KW_MACRO) {
      size_t end = i + 1u;
      while (end < input->count && input->items[end].kind != AST_TOK_LBRACE)
        end++;
      if (end < input->count) {
        size_t close =
            macro_find_matching(input, end, AST_TOK_LBRACE, AST_TOK_RBRACE);
        if (close != SIZE_MAX)
          end = close + 1u;
      }
      for (; i < end && i < input->count; ++i)
        if (macro_token_push(output, macro_token_ref(&input->items[i])) != 0)
          return -1;
      continue;
    }
    if (token->kind == AST_TOK_IDENTIFIER && i + 2u < input->count &&
        input->items[i + 1u].kind == AST_TOK_BANG &&
        (input->items[i + 2u].kind == AST_TOK_LPAREN ||
         input->items[i + 2u].kind == AST_TOK_LBRACE)) {
      const MacroDef *def = macro_find_def(&ctx->defs, token);
      AstTokenKind close_kind = input->items[i + 2u].kind == AST_TOK_LPAREN
                                    ? AST_TOK_RPAREN
                                    : AST_TOK_RBRACE;
      size_t close = macro_find_matching(input, i + 2u,
                                         input->items[i + 2u].kind, close_kind);
      if (def && close != SIZE_MAX) {
        MacroTokenVec *args = NULL;
        size_t arg_count = 0u;
        if (macro_parse_args(input, i + 2u, close, &args, &arg_count) != 0)
          return -1;
        MacroTokenVec expanded;
        memset(&expanded, 0, sizeof(expanded));
        if (macro_expand_body(ctx, def, args, arg_count, &expanded) != 0) {
          macro_args_destroy(args, arg_count);
          macro_token_vec_destroy(&expanded);
          return -1;
        }
        macro_args_destroy(args, arg_count);
        ctx->found_invocation = 1u;
        int result = macro_expand_stream(ctx, &expanded, output, depth + 1u);
        int body_ends_in_statement =
            expanded.count != 0u &&
            expanded.items[expanded.count - 1u].kind == AST_TOK_SEMICOLON;
        macro_token_vec_destroy(&expanded);
        if (result != 0)
          return result;
        i = close + 1u;
        if (i < input->count && input->items[i].kind == AST_TOK_SEMICOLON &&
            body_ends_in_statement)
          i++;
        continue;
      }
    }
    if (macro_token_push(output, macro_token_ref(token)) != 0)
      return -1;
    i++;
  }
  return 0;
}

static int macro_emit_source(MacroContext *ctx, const MacroTokenVec *tokens) {
  for (size_t i = 0u; i < tokens->count; ++i) {
    /* Dotted module/intrinsic names are represented by a contiguous span.
       Keep their spelling intact while separating all other tokens. */
    int dotted = i && (tokens->items[i].kind == AST_TOK_DOT ||
                       tokens->items[i - 1u].kind == AST_TOK_DOT);
    if (i && !dotted && macro_buffer_char(&ctx->source_out, ' ') != 0)
      return -1;
    if (macro_buffer_push(&ctx->source_out, tokens->items[i].text,
                          tokens->items[i].length) != 0)
      return -1;
  }
  return macro_buffer_char(&ctx->source_out, '\0');
}

int ast_macro_expand_file(const AstLexedFile *input, AstLexedFile *out,
                          uint8_t **source_out) {
  if (!input || !out || !source_out)
    return -1;
  *source_out = NULL;
  memset(out, 0, sizeof(*out));
  /* The overwhelmingly common path has no macro definitions at all.
     Previously we duplicated every non-trivia token into heap-backed
     MacroToken vectors only to discover that expansion was unnecessary.
     A lexical presence check keeps ordinary parsing allocation-free here. */
  int has_macro_definition = 0;
  for (size_t i = 0u; i < input->token_count; ++i) {
    const AstToken *token = ast_token_at(input, i);
    if (token && token->kind == AST_TOK_KW_MACRO) {
      has_macro_definition = 1;
      break;
    }
  }
  if (!has_macro_definition)
    return 0;
  MacroContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.input = input;
  if (macro_collect_tokens(&ctx) != 0 || macro_parse_definitions(&ctx) != 0) {
    macro_token_vec_destroy(&ctx.tokens);
    macro_defs_destroy(&ctx.defs);
    return -1;
  }
  MacroTokenVec expanded;
  memset(&expanded, 0, sizeof(expanded));
  int result = macro_expand_stream(&ctx, &ctx.tokens, &expanded, 0u);
  if (result != 0 || !ctx.found_invocation) {
    macro_token_vec_destroy(&expanded);
    macro_token_vec_destroy(&ctx.tokens);
    macro_defs_destroy(&ctx.defs);
    return result == 0 ? 0 : -1;
  }
  result = macro_emit_source(&ctx, &expanded);
  macro_token_vec_destroy(&expanded);
  macro_token_vec_destroy(&ctx.tokens);
  macro_defs_destroy(&ctx.defs);
  if (result != 0) {
    free(ctx.source_out.data);
    return -1;
  }
  AstSourceFile source = {0};
  source.path = input->source.path;
  source.data = ctx.source_out.data;
  source.size = ctx.source_out.count - 1u;
  if (ast_lex_source(&source, NULL, out) != 0) {
    free(ctx.source_out.data);
    return -1;
  }
  *source_out = ctx.source_out.data;
  return 1;
}
