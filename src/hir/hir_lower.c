#include "../sema/sema_internal.h"
#include "hir_internal.h"

static int builder_reserve_bindings(HirBuilder *b, size_t n) {
  return hir_reserve((void **)&b->bindings, sizeof(*b->bindings),
                     &b->binding_capacity, n);
}
static int builder_reserve_labels(HirBuilder *b, size_t n) {
  return hir_reserve((void **)&b->labels, sizeof(*b->labels),
                     &b->label_capacity, n);
}
static int builder_reserve_loops(HirBuilder *b, size_t n) {
  return hir_reserve((void **)&b->loops, sizeof(*b->loops), &b->loop_capacity,
                     n);
}

static const AstSemaType *
builder_specialize_type(HirBuilder *b, const AstSemaType *type, AstSpan span) {
  if (!b || !type || !b->template_bindings || !b->specialization_values ||
      b->specialization_count == 0u)
    return type;
  return sema_substitute_type((AstSema *)b->program->sema, type,
                              b->template_bindings, b->specialization_values,
                              b->specialization_count, span,
                              b->function->file_id, 0u);
}

static int builder_expr_fact(HirBuilder *b, const AstExpr *expr,
                             AstSemaExprFact *out) {
  if (!b || !expr || !out || !hir_get_expr_fact(b->program, expr, out))
    return 0;
  out->type = builder_specialize_type(b, out->type, expr->span);
  out->call_signature =
      builder_specialize_type(b, out->call_signature, expr->span);
  return 1;
}

static const AstSemaType *builder_expr_type(HirBuilder *b,
                                            const AstExpr *expr) {
  AstSemaExprFact fact;
  memset(&fact, 0, sizeof(fact));
  return builder_expr_fact(b, expr, &fact) ? fact.type : NULL;
}

static AstSemaConstInfo builder_public_constant(AstSemaConstValue value) {
  AstSemaConstInfo out;
  memset(&out, 0, sizeof(out));
  if (!sema_const_is_valid(value))
    return out;
  out.kind = (value.flags & AST_SEMA_CONSTF_FLOAT)  ? AST_SEMA_CONST_FLOAT
             : (value.flags & AST_SEMA_CONSTF_BOOL) ? AST_SEMA_CONST_BOOL
                                                    : AST_SEMA_CONST_INTEGER;
  out.lo = value.lo;
  out.hi = value.hi;
  out.floating = value.floating;
  out.is_negative = (uint8_t)((value.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u);
  out.is_signed = (uint8_t)((value.flags & AST_SEMA_CONSTF_SIGNED) != 0u);
  return out;
}

static const AstSemaGenericBinding *
builder_find_specialization_binding(HirBuilder *b, AstName name) {
  if (!b || !b->template_bindings || !b->specialization_values)
    return NULL;
  for (size_t i = 0u; i < b->specialization_count; ++i)
    if (sema_name_eq(b->template_bindings[i].name, name))
      return &b->specialization_values[i];
  return NULL;
}

static int hir_preallocate_function_storage(AstHirFunction *fn,
                                            const AstDecl *decl) {
  if (!fn || !decl || decl->kind != AST_DECL_FN)
    return 0;
  size_t statements =
      decl->as.fn_decl.body ? decl->as.fn_decl.body->statements.count : 0u;
  size_t params = decl->as.fn_decl.params.count;
  size_t blocks = 4u;
  size_t instructions = 8u;
  size_t values = 8u;
  size_t locals = 4u;
  if (statements > (SIZE_MAX - blocks) / 2u ||
      statements > (SIZE_MAX - instructions) / 4u ||
      params > (SIZE_MAX - instructions - statements * 4u) / 2u)
    return 0;
  blocks += statements * 2u;
  instructions += statements * 4u + params * 2u;
  if (instructions > SIZE_MAX - params - 4u)
    return 0;
  values = instructions + params + 4u;
  if (statements > SIZE_MAX - locals - params)
    return 0;
  locals += params + statements;

  size_t cap = fn->block_capacity;
  if (!hir_reserve((void **)&fn->blocks, sizeof(*fn->blocks), &cap, blocks) ||
      cap > UINT32_MAX)
    return 0;
  fn->block_capacity = (uint32_t)cap;
  cap = fn->instruction_capacity;
  if (!hir_reserve((void **)&fn->instructions, sizeof(*fn->instructions), &cap,
                   instructions) ||
      cap > UINT32_MAX)
    return 0;
  fn->instruction_capacity = (uint32_t)cap;
  cap = fn->value_capacity;
  if (!hir_reserve((void **)&fn->values, sizeof(*fn->values), &cap, values) ||
      cap > UINT32_MAX)
    return 0;
  fn->value_capacity = (uint32_t)cap;
  cap = fn->local_capacity;
  if (!hir_reserve((void **)&fn->locals, sizeof(*fn->locals), &cap, locals) ||
      cap > UINT32_MAX)
    return 0;
  fn->local_capacity = (uint32_t)cap;
  return 1;
}

static HirBinding *find_binding(HirBuilder *b, AstName name) {
  if (!b)
    return NULL;
  for (size_t i = b->binding_count; i > 0u; --i) {
    if (hir_name_eq(b->bindings[i - 1u].name, name))
      return &b->bindings[i - 1u];
  }
  return NULL;
}

static int push_local_binding(HirBuilder *b, AstName name, AstHirLocalId local,
                              const AstSemaType *type) {
  if (!builder_reserve_bindings(b, b->binding_count + 1u)) {
    b->function->oom = 1u;
    return 0;
  }
  HirBinding *binding = &b->bindings[b->binding_count++];
  memset(binding, 0, sizeof(*binding));
  binding->name = name;
  binding->local = local;
  binding->direct_value = AST_HIR_INVALID_ID;
  binding->type = type;
  binding->scope_depth = b->scope_depth;
  return 1;
}

static int push_const_binding(HirBuilder *b, AstName name,
                              AstSemaConstInfo constant) {
  if (!builder_reserve_bindings(b, b->binding_count + 1u)) {
    b->function->oom = 1u;
    return 0;
  }
  HirBinding *binding = &b->bindings[b->binding_count++];
  memset(binding, 0, sizeof(*binding));
  binding->name = name;
  binding->local = AST_HIR_INVALID_ID;
  binding->direct_value = AST_HIR_INVALID_ID;
  binding->direct_constant = constant;
  binding->has_direct_constant = 1u;
  binding->scope_depth = b->scope_depth;
  return 1;
}

static HirLabel *find_label(HirBuilder *b, AstName name) {
  for (size_t i = 0u; i < b->label_count; ++i)
    if (hir_name_eq(b->labels[i].name, name))
      return &b->labels[i];
  return NULL;
}

static AstHirBlockId new_block(HirBuilder *b, AstSpan span) {
  AstHirBlockId id = hir_add_block(b->function, span);
  if (id == AST_HIR_INVALID_ID)
    b->function->oom = 1u;
  return id;
}

static void set_current(HirBuilder *b, AstHirBlockId block) {
  b->current = block;
}

static int emit_br(HirBuilder *b, AstHirBlockId target, AstSpan span,
                   const AstHirValueId *args, uint32_t arg_count) {
  AstHirTerminator t;
  memset(&t, 0, sizeof(t));
  t.kind = AST_HIR_TERM_BR;
  t.span = span;
  t.condition_or_value = AST_HIR_INVALID_ID;
  t.target_true = target;
  t.target_false = AST_HIR_INVALID_ID;
  return hir_set_terminator(b->function, b->current, t, args, arg_count, NULL,
                            0u);
}

static int emit_cond_br(HirBuilder *b, AstHirValueId condition,
                        AstHirBlockId tblock, AstHirBlockId fblock,
                        AstSpan span) {
  AstHirTerminator t;
  memset(&t, 0, sizeof(t));
  t.kind = AST_HIR_TERM_COND_BR;
  t.span = span;
  t.condition_or_value = condition;
  t.target_true = tblock;
  t.target_false = fblock;
  return hir_set_terminator(b->function, b->current, t, NULL, 0u, NULL, 0u);
}

static int emit_return(HirBuilder *b, AstHirValueId value, AstSpan span) {
  AstHirTerminator t;
  memset(&t, 0, sizeof(t));
  t.kind = AST_HIR_TERM_RETURN;
  t.span = span;
  t.condition_or_value = value;
  t.target_true = AST_HIR_INVALID_ID;
  t.target_false = AST_HIR_INVALID_ID;
  return hir_set_terminator(b->function, b->current, t, NULL, 0u, NULL, 0u);
}

static int emit_unreachable(HirBuilder *b, AstSpan span) {
  AstHirTerminator t;
  memset(&t, 0, sizeof(t));
  t.kind = AST_HIR_TERM_UNREACHABLE;
  t.span = span;
  t.condition_or_value = AST_HIR_INVALID_ID;
  t.target_true = AST_HIR_INVALID_ID;
  t.target_false = AST_HIR_INVALID_ID;
  return hir_set_terminator(b->function, b->current, t, NULL, 0u, NULL, 0u);
}

static AstHirValueId emit_undef(HirBuilder *b, const AstSemaType *type,
                                AstSpan span, const AstExpr *source) {
  AstHirInstruction i;
  memset(&i, 0, sizeof(i));
  i.op = AST_HIR_OP_UNDEF;
  i.result = AST_HIR_INVALID_ID;
  i.local = AST_HIR_INVALID_ID;
  i.span = span;
  i.source_expr = source;
  return hir_emit_value_instruction(b->function, b->current, i, type, NULL, 0u);
}

static AstHirValueId emit_constant(HirBuilder *b, const AstSemaType *type,
                                   AstSpan span, AstSemaConstInfo value,
                                   const AstExpr *source) {
  AstHirInstruction i;
  memset(&i, 0, sizeof(i));
  i.op = AST_HIR_OP_CONSTANT;
  i.result = AST_HIR_INVALID_ID;
  i.local = AST_HIR_INVALID_ID;
  i.span = span;
  i.constant = value;
  i.source_expr = source;
  return hir_emit_value_instruction(b->function, b->current, i, type, NULL, 0u);
}

static AstHirValueId emit_string_literal(HirBuilder *b, const AstExpr *expr,
                                         const AstSemaType *type) {
  AstHirFunction *fn = b->function;
  AstHirText bytes;
  if (!hir_decode_literal_text(b->program, &fn->arena, expr->span,
                               expr->as.literal.token_kind, &bytes)) {
    hir_function_diag(fn, AST_HIR_DIAG_INVALID_INITIALIZER, AST_DIAG_ERROR,
                      expr->span, "cannot decode runtime string literal");
    return AST_HIR_INVALID_ID;
  }
  if (!hir_reserve((void **)&fn->literal_globals, sizeof(*fn->literal_globals),
                   &fn->literal_global_capacity,
                   fn->literal_global_count + 1u)) {
    fn->oom = 1u;
    return AST_HIR_INVALID_ID;
  }
  char suffix[64];
  int n =
      snprintf(suffix, sizeof(suffix), "$str$%zu", fn->literal_global_count);
  size_t base = fn->name.span.length;
  if (n < 0 || (size_t)n >= sizeof(suffix) || base > UINT32_MAX - (size_t)n) {
    fn->oom = 1u;
    return AST_HIR_INVALID_ID;
  }
  char *name = ast_arena_alloc(&fn->arena, base + (size_t)n + 1u, 1u);
  AstSemaType *storage = AST_ARENA_NEW(&fn->arena, AstSemaType);
  AstHirInitializer *init = AST_ARENA_NEW(&fn->arena, AstHirInitializer);
  if (!name || !storage || !init) {
    fn->oom = 1u;
    return AST_HIR_INVALID_ID;
  }
  memcpy(name, fn->name.text, base);
  memcpy(name + base, suffix, (size_t)n + 1u);
  memset(storage, 0, sizeof(*storage));
  storage->kind = AST_SEMA_TYPE_ARRAY;
  storage->flags = AST_SEMA_TYPEF_COMPLETE;
  /* Even an empty non-C literal has an addressable read-only object. */
  storage->size = bytes.length ? bytes.length : 1u;
  storage->alignment = 1u;
  storage->as.array.count = storage->size;
  storage->as.array.element =
      sema_builtin_for_token((AstSema *)b->program->sema, AST_TOK_KW_U8);
  memset(init, 0, sizeof(*init));
  init->kind = AST_HIR_INIT_LITERAL;
  init->type = storage;
  init->span = init->literal_span = expr->span;
  init->literal_kind = expr->as.literal.token_kind;
  init->literal = bytes;
  AstHirGlobal *global = &fn->literal_globals[fn->literal_global_count++];
  memset(global, 0, sizeof(*global));
  global->module_name = fn->module_name;
  global->name.text = name;
  global->name.span = expr->span;
  global->name.span.length = (uint32_t)(base + (size_t)n);
  global->type = storage;
  global->span = expr->span;
  global->section_name = (AstHirText){".rodata", 7u};
  global->flags = AST_HIR_GLOBAL_CONST | AST_HIR_GLOBAL_READONLY;
  global->alignment = 1u;
  global->initializer = init;
  AstHirInstruction inst;
  memset(&inst, 0, sizeof(inst));
  inst.op = AST_HIR_OP_GLOBAL_ADDR;
  inst.result = inst.local = AST_HIR_INVALID_ID;
  inst.span = expr->span;
  inst.module = global->module_name;
  inst.symbol = global->name;
  inst.source_expr = expr;
  return hir_emit_value_instruction(fn, b->current, inst, type, NULL, 0u);
}

static AstHirValueId emit_local_load(HirBuilder *b, HirBinding *binding,
                                     AstSpan span, const AstExpr *source) {
  if (!binding)
    return AST_HIR_INVALID_ID;
  if (binding->has_direct_constant) {
    const AstSemaType *type =
        source ? builder_expr_type(b, source) : binding->type;
    return emit_constant(b, type, span, binding->direct_constant, source);
  }
  if (binding->direct_value != AST_HIR_INVALID_ID)
    return binding->direct_value;
  AstHirInstruction i;
  memset(&i, 0, sizeof(i));
  i.op = AST_HIR_OP_LOCAL_LOAD;
  i.result = AST_HIR_INVALID_ID;
  i.local = binding->local;
  i.span = span;
  i.source_expr = source;
  return hir_emit_value_instruction(b->function, b->current, i, binding->type,
                                    NULL, 0u);
}

static AstHirValueId coerce_value(HirBuilder *b, AstHirValueId value,
                                  const AstSemaType *target, AstSpan span,
                                  const AstExpr *source) {
  if (!b || value == AST_HIR_INVALID_ID || value >= b->function->value_count ||
      !target)
    return value;
  const AstSemaType *source_type = b->function->values[value].type;
  if (hir_type_equal(source_type, target))
    return value;
  AstHirInstruction cast;
  memset(&cast, 0, sizeof(cast));
  cast.op = AST_HIR_OP_CAST;
  cast.result = AST_HIR_INVALID_ID;
  cast.local = AST_HIR_INVALID_ID;
  cast.span = span;
  cast.token_op = AST_TOK_KW_CAST;
  cast.source_expr = source;
  return hir_emit_value_instruction(b->function, b->current, cast, target,
                                    &value, 1u);
}

static uint32_t find_generic_const_index(const AstHirFunction *fn,
                                         const AstSemaExprFact *fact,
                                         AstName name) {
  if (!fn)
    return UINT32_MAX;
  for (uint32_t i = 0u; i < fn->generic_param_count; ++i) {
    const AstHirGenericParam *param = &fn->generic_params[i];
    if (param->kind != AST_HIR_GENERIC_CONST)
      continue;
    if (fact && fact->declaration_span.length &&
        param->span.file_id == fact->declaration_span.file_id &&
        param->span.offset <= fact->declaration_span.offset &&
        fact->declaration_span.offset < param->span.offset + param->span.length)
      return i;
    if (hir_name_eq(param->name, name))
      return i;
  }
  return UINT32_MAX;
}

static AstHirValueId emit_generic_const(HirBuilder *b, const AstExpr *expr,
                                        const AstSemaType *type,
                                        const AstSemaExprFact *fact) {
  const AstSemaGenericBinding *specialized =
      builder_find_specialization_binding(b, expr->as.name.name);
  if (specialized && specialized->is_const &&
      sema_const_is_valid(specialized->const_value))
    return emit_constant(b, type, expr->span,
                         builder_public_constant(specialized->const_value),
                         expr);
  uint32_t index =
      find_generic_const_index(b->function, fact, expr->as.name.name);
  if (index == UINT32_MAX) {
    hir_function_diag(b->function, AST_HIR_DIAG_UNKNOWN_BINDING, AST_DIAG_ERROR,
                      expr->span,
                      "cannot resolve const generic parameter in HIR");
    return emit_undef(b, type, expr->span, expr);
  }
  AstHirInstruction inst;
  memset(&inst, 0, sizeof(inst));
  inst.op = AST_HIR_OP_GENERIC_CONST;
  inst.result = AST_HIR_INVALID_ID;
  inst.local = AST_HIR_INVALID_ID;
  inst.span = expr->span;
  inst.symbol = expr->as.name.name;
  inst.aux_index = index;
  inst.source_expr = expr;
  return hir_emit_value_instruction(b->function, b->current, inst, type, NULL,
                                    0u);
}

static int emit_local_store(HirBuilder *b, AstHirLocalId local,
                            AstHirValueId value, AstSpan span,
                            const AstStmt *source) {
  AstHirInstruction i;
  memset(&i, 0, sizeof(i));
  i.op = AST_HIR_OP_LOCAL_STORE;
  i.result = AST_HIR_INVALID_ID;
  i.local = local;
  i.span = span;
  i.source_stmt = source;
  AstHirValueId ops[1] = {value};
  return hir_emit_instruction(b->function, b->current, i, ops, 1u) !=
         AST_HIR_INVALID_ID;
}

static int resolve_simple_symbol(HirBuilder *b, AstName module, AstName symbol,
                                 AstSemaSymbolInfo *out) {
  char *mc = (char *)malloc((size_t)module.span.length + 1u);
  char *sc = (char *)malloc((size_t)symbol.span.length + 1u);
  if (!mc || !sc) {
    free(mc);
    free(sc);
    b->function->oom = 1u;
    return 0;
  }
  memcpy(mc, module.text, module.span.length);
  mc[module.span.length] = '\0';
  memcpy(sc, symbol.text, symbol.span.length);
  sc[symbol.span.length] = '\0';
  int ok = ast_sema_find_symbol(b->program->sema, mc, sc, out);
  free(mc);
  free(sc);
  return ok;
}

#define HIR_MAX_QUALIFIED_SEGMENTS 64u
static uint32_t flatten_member(const AstExpr *expr, AstName *segments,
                               uint32_t capacity) {
  AstName reverse[HIR_MAX_QUALIFIED_SEGMENTS];
  uint32_t rc = 0u;
  const AstExpr *cur = expr;
  while (cur && cur->kind == AST_EXPR_MEMBER) {
    if (rc >= HIR_MAX_QUALIFIED_SEGMENTS)
      return 0u;
    reverse[rc++] = cur->as.member.member;
    cur = cur->as.member.base;
  }
  if (!cur || cur->kind != AST_EXPR_NAME || rc + 1u > capacity)
    return 0u;
  segments[0] = cur->as.name.name;
  for (uint32_t i = 0u; i < rc; ++i)
    segments[i + 1u] = reverse[rc - 1u - i];
  return rc + 1u;
}

static int module_name_matches_segments(AstName module, const AstName *segments,
                                        uint32_t count) {
  if (!module.text || count == 0u)
    return 0;
  uint32_t seg = 0u;
  uint32_t begin = 0u;
  for (uint32_t i = 0u; i <= module.span.length; ++i) {
    if (i != module.span.length && module.text[i] != '.')
      continue;
    if (seg >= count)
      return 0;
    uint32_t len = i - begin;
    if (segments[seg].span.length != len ||
        memcmp(segments[seg].text, module.text + begin, len) != 0)
      return 0;
    seg++;
    begin = i + 1u;
  }
  return seg == count;
}

static uint32_t module_segment_count(AstName module) {
  if (!module.text || module.span.length == 0u)
    return 0u;
  uint32_t n = 1u;
  for (uint32_t i = 0u; i < module.span.length; ++i)
    if (module.text[i] == '.')
      n++;
  return n;
}

/* Sema exposes an unaliased import through its last module segment, e.g.
 * `import lib.hw; hw.init()`. HIR must accept the same spelling; otherwise
 * semantically valid qualified calls degrade to g.undef during lowering. */
static int module_last_segment_matches(AstName module, AstName segment) {
  if (!module.text || !segment.text || !module.span.length)
    return 0;
  uint32_t begin = module.span.length;
  while (begin > 0u && module.text[begin - 1u] != '.')
    begin--;
  uint32_t len = module.span.length - begin;
  return len == segment.span.length &&
         memcmp(module.text + begin, segment.text, len) == 0;
}

static int resolve_expr_symbol(HirBuilder *b, const AstExpr *expr,
                               AstName *module_out, AstName *symbol_out,
                               AstSemaSymbolInfo *info_out) {
  if (!expr)
    return 0;
  if (expr->kind == AST_EXPR_NAME) {
    *module_out = b->module->name;
    *symbol_out = expr->as.name.name;
    return resolve_simple_symbol(b, *module_out, *symbol_out, info_out);
  }
  if (expr->kind != AST_EXPR_MEMBER)
    return 0;
  AstName segments[HIR_MAX_QUALIFIED_SEGMENTS];
  uint32_t count = flatten_member(expr, segments, HIR_MAX_QUALIFIED_SEGMENTS);
  if (count < 2u)
    return 0;
  for (uint32_t di = 0u; di < b->module->declarations.count; ++di) {
    const AstDecl *d = b->module->declarations.items[di];
    if (!d || d->kind != AST_DECL_IMPORT)
      continue;
    uint32_t prefix = module_segment_count(d->as.import_decl.module);
    int match = 0;
    if (count == 2u &&
        ((d->as.import_decl.has_alias &&
          hir_name_eq(d->as.import_decl.alias, segments[0])) ||
         (!d->as.import_decl.has_alias &&
          module_last_segment_matches(d->as.import_decl.module, segments[0]))))
      match = 1;
    else if (prefix + 1u == count &&
             module_name_matches_segments(d->as.import_decl.module, segments,
                                          prefix))
      match = 1;
    if (!match)
      continue;
    *module_out = d->as.import_decl.module;
    *symbol_out = segments[count - 1u];
    return resolve_simple_symbol(b, *module_out, *symbol_out, info_out);
  }
  return 0;
}

static AstHirValueId lower_expr(HirBuilder *b, const AstExpr *expr);

static int is_fence_expr(const AstExpr *expr) {
  const AstExpr *base = expr;
  while (base && base->kind == AST_EXPR_ATTR)
    base = base->as.attributed.base;
  return base && base->kind == AST_EXPR_NAME &&
         hir_name_eq_text(base->as.name.name, "fence");
}

static AstMemoryOrder hir_memory_order_from_name(AstName name) {
  if (hir_name_eq_text(name, "relaxed"))
    return AST_MEMORY_ORDER_RELAXED;
  if (hir_name_eq_text(name, "acquire"))
    return AST_MEMORY_ORDER_ACQUIRE;
  if (hir_name_eq_text(name, "release"))
    return AST_MEMORY_ORDER_RELEASE;
  if (hir_name_eq_text(name, "acq_rel"))
    return AST_MEMORY_ORDER_ACQ_REL;
  if (hir_name_eq_text(name, "seq_cst"))
    return AST_MEMORY_ORDER_SEQ_CST;
  return AST_MEMORY_ORDER_NONE;
}

static AstMemoryOrder
hir_memory_order_from_attribute(const AstAttribute *attr) {
  if (!attr)
    return AST_MEMORY_ORDER_NONE;
  AstMemoryOrder direct = hir_memory_order_from_name(attr->name);
  if (direct != AST_MEMORY_ORDER_NONE && attr->args.count == 0u)
    return direct;
  if ((hir_name_eq_text(attr->name, "order") ||
       hir_name_eq_text(attr->name, "success") ||
       hir_name_eq_text(attr->name, "failure")) &&
      attr->args.count == 1u && attr->args.items[0] &&
      attr->args.items[0]->kind == AST_EXPR_NAME)
    return hir_memory_order_from_name(attr->args.items[0]->as.name.name);
  return AST_MEMORY_ORDER_NONE;
}

static void hir_atomic_memory_orders(const AstExpr *expr,
                                     AstMemoryOrder *success_out,
                                     AstMemoryOrder *failure_out) {
  AstMemoryOrder order = AST_MEMORY_ORDER_NONE;
  AstMemoryOrder success = AST_MEMORY_ORDER_NONE;
  AstMemoryOrder failure = AST_MEMORY_ORDER_NONE;
  if (expr && expr->kind == AST_EXPR_ATOMIC) {
    for (uint32_t i = 0u; i < expr->as.atomic.attributes.count; ++i) {
      const AstAttribute *attr = &expr->as.atomic.attributes.items[i];
      AstMemoryOrder parsed = hir_memory_order_from_attribute(attr);
      if (hir_name_eq_text(attr->name, "success"))
        success = parsed;
      else if (hir_name_eq_text(attr->name, "failure"))
        failure = parsed;
      else
        order = parsed;
    }
    if (hir_name_eq_text(expr->as.atomic.op, "cas")) {
      if (order != AST_MEMORY_ORDER_NONE && success == AST_MEMORY_ORDER_NONE &&
          failure == AST_MEMORY_ORDER_NONE) {
        success = order;
        if (order == AST_MEMORY_ORDER_RELEASE)
          failure = AST_MEMORY_ORDER_RELAXED;
        else if (order == AST_MEMORY_ORDER_ACQ_REL)
          failure = AST_MEMORY_ORDER_ACQUIRE;
        else
          failure = order;
      }
    } else {
      success = order;
      failure = AST_MEMORY_ORDER_NONE;
    }
  }
  if (success_out)
    *success_out = success;
  if (failure_out)
    *failure_out = failure;
}

static AstMemoryOrder hir_fence_memory_order(const AstExpr *expr) {
  const AstExpr *cur = expr;
  AstMemoryOrder order = AST_MEMORY_ORDER_NONE;
  while (cur && cur->kind == AST_EXPR_ATTR) {
    AstMemoryOrder parsed =
        hir_memory_order_from_attribute(cur->as.attributed.attribute);
    if (parsed != AST_MEMORY_ORDER_NONE)
      order = parsed;
    cur = cur->as.attributed.base;
  }
  return order;
}

static AstHirIntrinsicKind intrinsic_from_call(const AstExpr *callee) {
  if (!callee)
    return AST_HIR_INTRINSIC_NONE;
  if (callee->kind == AST_EXPR_NAME) {
    AstName n = callee->as.name.name;
    if (hir_name_eq_text(n, "select"))
      return AST_HIR_INTRINSIC_SELECT;
    if (hir_name_eq_text(n, "clz"))
      return AST_HIR_INTRINSIC_CLZ;
    if (hir_name_eq_text(n, "ctz"))
      return AST_HIR_INTRINSIC_CTZ;
    if (hir_name_eq_text(n, "popcnt"))
      return AST_HIR_INTRINSIC_POPCNT;
    if (hir_name_eq_text(n, "bswap"))
      return AST_HIR_INTRINSIC_BSWAP;
    if (hir_name_eq_text(n, "bitrev"))
      return AST_HIR_INTRINSIC_BITREV;
    if (hir_name_eq_text(n, "rol"))
      return AST_HIR_INTRINSIC_ROL;
    if (hir_name_eq_text(n, "ror"))
      return AST_HIR_INTRINSIC_ROR;
    if (hir_name_eq_text(n, "ptr_to_int"))
      return AST_HIR_INTRINSIC_PTR_TO_INT;
    if (hir_name_eq_text(n, "int_to_ptr"))
      return AST_HIR_INTRINSIC_INT_TO_PTR;
    if (hir_name_eq_text(n, "sizeof"))
      return AST_HIR_INTRINSIC_SIZEOF;
    if (hir_name_eq_text(n, "offsetof"))
      return AST_HIR_INTRINSIC_OFFSETOF;
    if (hir_name_eq_text(n, "symbol_addr"))
      return AST_HIR_INTRINSIC_SYMBOL_ADDR;
    if (hir_name_eq_text(n, "feature"))
      return AST_HIR_INTRINSIC_FEATURE;
  }
  if (callee->kind == AST_EXPR_MEMBER && callee->as.member.base &&
      callee->as.member.base->kind == AST_EXPR_NAME) {
    AstName op = callee->as.member.base->as.name.name;
    AstName mode = callee->as.member.member;
    if (hir_name_eq_text(mode, "checked")) {
      if (hir_name_eq_text(op, "add"))
        return AST_HIR_INTRINSIC_ADD_CHECKED;
      if (hir_name_eq_text(op, "sub"))
        return AST_HIR_INTRINSIC_SUB_CHECKED;
      if (hir_name_eq_text(op, "mul"))
        return AST_HIR_INTRINSIC_MUL_CHECKED;
    }
    if (hir_name_eq_text(mode, "wrap")) {
      if (hir_name_eq_text(op, "add"))
        return AST_HIR_INTRINSIC_ADD_WRAP;
      if (hir_name_eq_text(op, "sub"))
        return AST_HIR_INTRINSIC_SUB_WRAP;
      if (hir_name_eq_text(op, "mul"))
        return AST_HIR_INTRINSIC_MUL_WRAP;
    }
    if (hir_name_eq_text(mode, "sat")) {
      if (hir_name_eq_text(op, "add"))
        return AST_HIR_INTRINSIC_ADD_SAT;
      if (hir_name_eq_text(op, "sub"))
        return AST_HIR_INTRINSIC_SUB_SAT;
    }
  }
  return AST_HIR_INTRINSIC_NONE;
}

static AstHirValueId lower_symbol_value(HirBuilder *b, const AstExpr *expr,
                                        const AstSemaType *type,
                                        AstSemaExprFact fact) {
  AstName module = {0};
  AstName symbol = {0};
  AstSemaSymbolInfo info;
  memset(&info, 0, sizeof(info));
  if (!resolve_expr_symbol(b, expr, &module, &symbol, &info)) {
    hir_function_diag(b->function, AST_HIR_DIAG_UNKNOWN_BINDING, AST_DIAG_ERROR,
                      expr->span, "cannot resolve top-level HIR symbol");
    return emit_undef(b, type, expr->span, expr);
  }
  if ((info.kind == AST_SEMA_SYMBOL_CONST) &&
      fact.constant.kind != AST_SEMA_CONST_NONE)
    return emit_constant(b, type, expr->span, fact.constant, expr);
  AstHirInstruction i;
  memset(&i, 0, sizeof(i));
  i.result = AST_HIR_INVALID_ID;
  i.local = AST_HIR_INVALID_ID;
  i.span = expr->span;
  i.symbol = symbol;
  i.module = module;
  i.source_expr = expr;
  if (info.kind == AST_SEMA_SYMBOL_FUNCTION ||
      info.kind == AST_SEMA_SYMBOL_EXTERN_FUNCTION)
    i.op = AST_HIR_OP_FUNCTION_REF;
  else
    i.op = AST_HIR_OP_GLOBAL_LOAD;
  i.effects = i.op == AST_HIR_OP_GLOBAL_LOAD ? AST_SEMA_EFFECT_READ_MEMORY : 0u;
  return hir_emit_value_instruction(b->function, b->current, i, type, NULL, 0u);
}

/* Keep derived addresses explicit before target legalization.  Aggregate
 * lvalues are lowered to pointer-sized arithmetic only after generic
 * specialization, when field offsets and element sizes are concrete. */
static AstHirValueId emit_address_intrinsic(HirBuilder *b, AstHirValueId value,
                                            AstHirIntrinsicKind intrinsic,
                                            const AstSemaType *type,
                                            AstSpan span) {
  AstHirInstruction i = {0};
  i.op = AST_HIR_OP_INTRINSIC;
  i.intrinsic = intrinsic;
  i.result = AST_HIR_INVALID_ID;
  i.local = AST_HIR_INVALID_ID;
  i.span = span;
  return hir_emit_value_instruction(b->function, b->current, i, type, &value,
                                    1u);
}

static AstHirValueId emit_usize_constant(HirBuilder *b, uint64_t value,
                                         AstSpan span) {
  AstSemaConstInfo constant = {0};
  constant.kind = AST_SEMA_CONST_INTEGER;
  constant.lo = value;
  return emit_constant(b, sema_builtin_usize(b->program->sema), span, constant,
                       NULL);
}

static AstHirValueId emit_pointer_offset(HirBuilder *b, AstHirValueId pointer,
                                         AstHirValueId offset,
                                         const AstSemaType *result_type,
                                         AstSpan span) {
  const AstSemaType *integer = sema_builtin_usize(b->program->sema);
  AstHirValueId address = emit_address_intrinsic(
      b, pointer, AST_HIR_INTRINSIC_PTR_TO_INT, integer, span);
  if (address == AST_HIR_INVALID_ID || offset == AST_HIR_INVALID_ID)
    return AST_HIR_INVALID_ID;
  AstHirInstruction add = {0};
  add.op = AST_HIR_OP_BINARY;
  add.token_op = AST_TOK_PLUS;
  add.result = AST_HIR_INVALID_ID;
  add.local = AST_HIR_INVALID_ID;
  add.span = span;
  AstHirValueId values[] = {address, offset};
  address = hir_emit_value_instruction(b->function, b->current, add, integer,
                                       values, 2u);
  if (address == AST_HIR_INVALID_ID)
    return address;
  return emit_address_intrinsic(b, address, AST_HIR_INTRINSIC_INT_TO_PTR,
                                result_type, span);
}

static AstHirValueId emit_plain_address_of(HirBuilder *b,
                                           const AstExpr *operand,
                                           const AstSemaType *type,
                                           AstSpan span) {
  AstHirValueId value = lower_expr(b, operand);
  AstHirInstruction i;
  memset(&i, 0, sizeof(i));
  i.op = AST_HIR_OP_ADDRESS_OF;
  i.result = AST_HIR_INVALID_ID;
  i.local = AST_HIR_INVALID_ID;
  i.span = span;
  i.source_expr = operand;
  return hir_emit_value_instruction(b->function, b->current, i, type, &value,
                                    1u);
}

static AstHirValueId lower_address_of(HirBuilder *b, const AstExpr *operand,
                                      const AstSemaType *type, AstSpan span) {
  if (!operand)
    return AST_HIR_INVALID_ID;
  if (operand->kind == AST_EXPR_NAME) {
    HirBinding *binding = find_binding(b, operand->as.name.name);
    if (binding && binding->local != AST_HIR_INVALID_ID) {
      AstHirInstruction i;
      memset(&i, 0, sizeof(i));
      i.op = AST_HIR_OP_LOCAL_ADDR;
      i.result = AST_HIR_INVALID_ID;
      i.local = binding->local;
      i.span = span;
      i.source_expr = operand;
      return hir_emit_value_instruction(b->function, b->current, i, type, NULL,
                                        0u);
    }
  }
  AstName module = {0}, symbol = {0};
  AstSemaSymbolInfo info;
  memset(&info, 0, sizeof(info));
  if (resolve_expr_symbol(b, operand, &module, &symbol, &info) &&
      (info.kind == AST_SEMA_SYMBOL_GLOBAL ||
       info.kind == AST_SEMA_SYMBOL_CONST ||
       info.kind == AST_SEMA_SYMBOL_FUNCTION ||
       info.kind == AST_SEMA_SYMBOL_EXTERN_FUNCTION)) {
    AstHirInstruction i;
    memset(&i, 0, sizeof(i));
    i.op = (info.kind == AST_SEMA_SYMBOL_FUNCTION ||
            info.kind == AST_SEMA_SYMBOL_EXTERN_FUNCTION)
               ? AST_HIR_OP_FUNCTION_REF
               : AST_HIR_OP_GLOBAL_ADDR;
    i.result = AST_HIR_INVALID_ID;
    i.local = AST_HIR_INVALID_ID;
    i.span = span;
    i.module = module;
    i.symbol = symbol;
    i.source_expr = operand;
    return hir_emit_value_instruction(b->function, b->current, i, type, NULL,
                                      0u);
  }
  if (operand->kind == AST_EXPR_UNARY && operand->as.unary.op == AST_TOK_STAR)
    return lower_expr(b, operand->as.unary.operand);

  /* Generic templates are not machine-lowered.  Keep one legal abstract
   * ADDRESS_OF until monomorphization re-lowers the source expression with
   * concrete type/const bindings.  This avoids freezing an unknown scale. */
  const AstSemaType *operand_type = builder_expr_type(b, operand);
  if (b->template_bindings && !b->specialization_values && operand_type &&
      sema_type_is_dependent(operand_type, 0u))
    return emit_plain_address_of(b, operand, type, span);

  if (operand->kind == AST_EXPR_MEMBER) {
    const AstExpr *base = operand->as.member.base;
    const AstSemaType *base_type = builder_expr_type(b, base);
    const AstSemaType *record = base_type;
    AstHirValueId pointer = AST_HIR_INVALID_ID;
    if (base_type && (ast_sema_type_kind(base_type) == AST_SEMA_TYPE_PTR ||
                      ast_sema_type_kind(base_type) == AST_SEMA_TYPE_CAP)) {
      record = ast_sema_type_element(base_type);
      pointer = lower_expr(b, base);
    } else if (base_type &&
               ast_sema_type_kind(base_type) == AST_SEMA_TYPE_STRUCT) {
      const AstSemaType *base_ptr = sema_make_pointer(
          (AstSema *)b->program->sema, AST_SEMA_TYPE_PTR, base_type, 0u);
      pointer = lower_address_of(b, base, base_ptr, span);
    }
    if (pointer != AST_HIR_INVALID_ID && record &&
        ast_sema_type_kind(record) == AST_SEMA_TYPE_STRUCT) {
      AstSemaFieldInfo field = {0};
      uint32_t count = ast_sema_type_struct_field_count(record), n;
      for (n = 0u; n < count; ++n) {
        if (!ast_sema_type_struct_field(record, n, &field))
          return AST_HIR_INVALID_ID;
        if (hir_name_eq(field.name, operand->as.member.member))
          break;
      }
      if (n < count) {
        AstHirValueId offset = emit_usize_constant(b, field.offset, span);
        return emit_pointer_offset(b, pointer, offset, type, span);
      }
    }
  } else if (operand->kind == AST_EXPR_INDEX) {
    const AstExpr *base = operand->as.index.base;
    const AstSemaType *base_type = builder_expr_type(b, base);
    const AstSemaType *element = NULL;
    AstHirValueId pointer = AST_HIR_INVALID_ID;
    if (base_type && (ast_sema_type_kind(base_type) == AST_SEMA_TYPE_PTR ||
                      ast_sema_type_kind(base_type) == AST_SEMA_TYPE_CAP)) {
      element = ast_sema_type_element(base_type);
      pointer = lower_expr(b, base);
    } else if (base_type &&
               ast_sema_type_kind(base_type) == AST_SEMA_TYPE_ARRAY) {
      element = ast_sema_type_element(base_type);
      const AstSemaType *base_ptr = sema_make_pointer(
          (AstSema *)b->program->sema, AST_SEMA_TYPE_PTR, base_type, 0u);
      pointer = lower_address_of(b, base, base_ptr, span);
    }
    uint64_t element_size = ast_sema_type_size(element);
    if (pointer != AST_HIR_INVALID_ID && element &&
        element_size != AST_SEMA_UNKNOWN_SIZE) {
      const AstSemaType *integer = sema_builtin_usize(b->program->sema);
      AstSemaExprFact index_fact = {0};
      (void)builder_expr_fact(b, operand->as.index.index, &index_fact);
      AstHirValueId index;
      if (index_fact.constant.kind != AST_SEMA_CONST_NONE) {
        index = emit_constant(b, integer, span, index_fact.constant,
                              operand->as.index.index);
      } else {
        index = lower_expr(b, operand->as.index.index);
        const AstSemaType *index_type = index < b->function->value_count
                                            ? b->function->values[index].type
                                            : NULL;
        if (!index_type || !sema_type_equal(index_type, integer)) {
          AstHirInstruction cast = {0};
          cast.op = AST_HIR_OP_CAST;
          cast.token_op = AST_TOK_KW_CAST;
          cast.result = AST_HIR_INVALID_ID;
          cast.local = AST_HIR_INVALID_ID;
          cast.span = span;
          index = hir_emit_value_instruction(b->function, b->current, cast,
                                             integer, &index, 1u);
        }
      }
      AstHirValueId scale = emit_usize_constant(b, element_size, span);
      AstHirInstruction mul = {0};
      mul.op = AST_HIR_OP_BINARY;
      mul.token_op = AST_TOK_STAR;
      mul.result = AST_HIR_INVALID_ID;
      mul.local = AST_HIR_INVALID_ID;
      mul.span = span;
      AstHirValueId values[] = {index, scale};
      AstHirValueId offset = hir_emit_value_instruction(
          b->function, b->current, mul, integer, values, 2u);
      return emit_pointer_offset(b, pointer, offset, type, span);
    }
  }

  return emit_plain_address_of(b, operand, type, span);
}

static int hir_attribute_has(AstAttributeArray attributes, const char *name) {
  for (uint32_t i = 0u; i < attributes.count; ++i) {
    if (hir_name_eq_text(attributes.items[i].name, name))
      return 1;
  }
  return 0;
}

static int hir_type_is_integer_like(const AstSemaType *type) {
  if (!type)
    return 0;
  AstSemaTypeKind kind = ast_sema_type_kind(type);
  return kind == AST_SEMA_TYPE_INT || kind == AST_SEMA_TYPE_BITS ||
         kind == AST_SEMA_TYPE_UNTYPED_INT;
}

/*
 * Sema expression facts intentionally aggregate effects from child expressions.
 * HIR instructions must not copy that transitive summary: children have already
 * become independent HIR instructions and would otherwise duplicate barriers.
 * This helper describes only the effect owned by the instruction emitted for
 * the current expression.
 */
static uint32_t hir_expr_instruction_effects(const AstExpr *expr,
                                             const AstSemaType *type,
                                             AstHirIntrinsicKind intrinsic) {
  if (!expr)
    return 0u;
  switch (expr->kind) {
  case AST_EXPR_UNARY:
    return expr->as.unary.op == AST_TOK_STAR ? AST_SEMA_EFFECT_READ_MEMORY : 0u;
  case AST_EXPR_BINARY:
    if ((expr->as.binary.op == AST_TOK_SLASH ||
         expr->as.binary.op == AST_TOK_PERCENT) &&
        hir_type_is_integer_like(type))
      return AST_SEMA_EFFECT_MAY_TRAP;
    return 0u;
  case AST_EXPR_CALL:
    if (intrinsic == AST_HIR_INTRINSIC_PTR_TO_INT ||
        intrinsic == AST_HIR_INTRINSIC_INT_TO_PTR)
      return AST_SEMA_EFFECT_UNSAFE;
    return intrinsic == AST_HIR_INTRINSIC_NONE ? AST_SEMA_EFFECT_CALL : 0u;
  case AST_EXPR_INDEX: {
    const AstSemaType *base = type;
    ASTER_UNUSED(base);
    return 0u;
  }
  case AST_EXPR_MEMBER:
    return 0u;
  case AST_EXPR_LOAD: {
    uint32_t effects = AST_SEMA_EFFECT_READ_MEMORY;
    if (hir_attribute_has(expr->as.load.attributes, "volatile"))
      effects |= AST_SEMA_EFFECT_VOLATILE;
    if (hir_attribute_has(expr->as.load.attributes, "nontemporal"))
      effects |= AST_SEMA_EFFECT_NONTEMPORAL;
    return effects;
  }
  case AST_EXPR_CAST:
    if (expr->as.cast.op == AST_TOK_KW_CAST)
      return AST_SEMA_EFFECT_MAY_TRAP;
    if (expr->as.cast.op == AST_TOK_KW_PTRCAST)
      return AST_SEMA_EFFECT_UNSAFE;
    return 0u;
  case AST_EXPR_ATOMIC:
    if (hir_name_eq_text(expr->as.atomic.op, "load"))
      return AST_SEMA_EFFECT_ATOMIC | AST_SEMA_EFFECT_READ_MEMORY;
    if (hir_name_eq_text(expr->as.atomic.op, "store"))
      return AST_SEMA_EFFECT_ATOMIC | AST_SEMA_EFFECT_WRITE_MEMORY;
    return AST_SEMA_EFFECT_ATOMIC | AST_SEMA_EFFECT_READ_MEMORY |
           AST_SEMA_EFFECT_WRITE_MEMORY;
  default:
    return 0u;
  }
}

static AstHirValueId lower_call(HirBuilder *b, const AstExpr *expr,
                                const AstSemaType *type, AstSemaExprFact fact) {
  AstHirIntrinsicKind ik = intrinsic_from_call(expr->as.call.callee);
  if ((ik == AST_HIR_INTRINSIC_SIZEOF || ik == AST_HIR_INTRINSIC_OFFSETOF ||
       ik == AST_HIR_INTRINSIC_FEATURE) &&
      fact.constant.kind != AST_SEMA_CONST_NONE) {
    return emit_constant(b, type, expr->span, fact.constant, expr);
  }
  uint32_t arg_count = expr->as.call.args.count;
  if (ik == AST_HIR_INTRINSIC_SIZEOF && arg_count == 1u) {
    const AstExpr *arg = expr->as.call.args.items[0];
    const AstSemaType *operand_type = NULL;
    if (arg && arg->kind == AST_EXPR_NAME) {
      const AstSemaGenericBinding *binding =
          builder_find_specialization_binding(b, arg->as.name.name);
      if (binding && !binding->is_const)
        operand_type = binding->type_value;
      if (!operand_type) {
        HirBinding *local = find_binding(b, arg->as.name.name);
        if (local)
          operand_type = local->type;
      }
    }
    if (!operand_type)
      operand_type = builder_expr_type(b, arg);
    uint64_t size = ast_sema_type_size(operand_type);
    if (operand_type && size != AST_SEMA_UNKNOWN_SIZE) {
      AstSemaConstInfo constant = {0};
      constant.kind = AST_SEMA_CONST_INTEGER;
      constant.lo = size;
      return emit_constant(b, type, expr->span, constant, expr);
    }
    if (b->specialization_values) {
      hir_function_diag(
          b->function, AST_HIR_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, expr->span,
          "sizeof() remained layout-dependent after generic specialization");
      return emit_undef(b, type, expr->span, expr);
    }
    /* The generic template is not machine-lowered.  Preserve a zero-operand
     * intrinsic as a typed placeholder; monomorphization lowers the source
     * body again and folds it above using the concrete specialization. */
    AstHirInstruction deferred;
    memset(&deferred, 0, sizeof(deferred));
    deferred.op = AST_HIR_OP_INTRINSIC;
    deferred.intrinsic = AST_HIR_INTRINSIC_SIZEOF;
    deferred.result = AST_HIR_INVALID_ID;
    deferred.local = AST_HIR_INVALID_ID;
    deferred.span = expr->span;
    deferred.source_expr = expr;
    return hir_emit_value_instruction(b->function, b->current, deferred, type,
                                      NULL, 0u);
  }
  uint32_t extra = ik == AST_HIR_INTRINSIC_NONE ? 1u : 0u;
  uint32_t operand_count = arg_count + extra;
  AstHirValueId small[8];
  AstHirValueId *ops =
      operand_count <= ASTER_ARRAY_COUNT(small)
          ? small
          : (AstHirValueId *)malloc((size_t)operand_count * sizeof(*ops));
  if (!ops && operand_count) {
    b->function->oom = 1u;
    return AST_HIR_INVALID_ID;
  }
  uint32_t at = 0u;
  if (ik == AST_HIR_INTRINSIC_NONE) {
    const AstExpr *callee = expr->as.call.callee;
    /* A function designator obtained from *p denotes the code address.
       It must not load bytes of executable memory as a function value. */
    if (callee && callee->kind == AST_EXPR_UNARY &&
        callee->as.unary.op == AST_TOK_STAR &&
        ast_sema_type_kind(builder_expr_type(b, callee)) ==
            AST_SEMA_TYPE_FUNCTION)
      callee = callee->as.unary.operand;
    ops[at++] = lower_expr(b, callee);
  }
  for (uint32_t i = 0u; i < arg_count; ++i) {
    const AstExpr *arg = expr->as.call.args.items[i];
    if ((ik == AST_HIR_INTRINSIC_SIZEOF || ik == AST_HIR_INTRINSIC_OFFSETOF) &&
        arg && arg->kind == AST_EXPR_TYPE)
      continue;
    ops[at++] = lower_expr(b, arg);
  }
  AstHirInstruction inst;
  memset(&inst, 0, sizeof(inst));
  inst.op =
      ik == AST_HIR_INTRINSIC_NONE ? AST_HIR_OP_CALL : AST_HIR_OP_INTRINSIC;
  inst.result = AST_HIR_INVALID_ID;
  inst.local = AST_HIR_INVALID_ID;
  inst.span = expr->span;
  inst.effects = hir_expr_instruction_effects(expr, type, ik);
  inst.intrinsic = ik;
  inst.call_signature = fact.call_signature;
  inst.source_expr = expr;
  AstHirValueId result = AST_HIR_INVALID_ID;
  if (type && ast_sema_type_kind(type) != AST_SEMA_TYPE_VOID)
    result = hir_emit_value_instruction(b->function, b->current, inst, type,
                                        ops, at);
  else
    hir_emit_instruction(b->function, b->current, inst, ops, at);
  if (ops != small)
    free(ops);
  return result;
}

static AstHirValueId lower_conditional(HirBuilder *b, const AstExpr *expr,
                                       const AstSemaType *type) {
  AstHirValueId cond = lower_expr(b, expr->as.conditional.condition);
  AstHirBlockId then_block = new_block(b, expr->as.conditional.then_expr->span);
  AstHirBlockId else_block = new_block(b, expr->as.conditional.else_expr->span);
  AstHirBlockId merge_block = new_block(b, expr->span);
  if (then_block == AST_HIR_INVALID_ID || else_block == AST_HIR_INVALID_ID ||
      merge_block == AST_HIR_INVALID_ID)
    return AST_HIR_INVALID_ID;
  emit_cond_br(b, cond, then_block, else_block, expr->span);
  set_current(b, then_block);
  AstHirValueId tv = lower_expr(b, expr->as.conditional.then_expr);
  if (!hir_block_terminated(b->function, b->current))
    emit_br(b, merge_block, expr->as.conditional.then_expr->span, &tv, 1u);
  set_current(b, else_block);
  AstHirValueId fv = lower_expr(b, expr->as.conditional.else_expr);
  if (!hir_block_terminated(b->function, b->current))
    emit_br(b, merge_block, expr->as.conditional.else_expr->span, &fv, 1u);
  AstHirValueId arg =
      hir_add_block_argument(b->function, merge_block, type, expr->span);
  set_current(b, merge_block);
  return arg;
}

static uint32_t struct_field_index(const AstSemaType *base_type,
                                   AstName member) {
  if (!base_type)
    return UINT32_MAX;
  AstSemaTypeKind k = ast_sema_type_kind(base_type);
  if (k == AST_SEMA_TYPE_PTR || k == AST_SEMA_TYPE_CAP)
    base_type = ast_sema_type_element(base_type);
  uint32_t count = ast_sema_type_struct_field_count(base_type);
  for (uint32_t i = 0u; i < count; ++i) {
    AstSemaFieldInfo f;
    if (ast_sema_type_struct_field(base_type, i, &f) &&
        hir_name_eq(f.name, member))
      return i;
  }
  return UINT32_MAX;
}

/* Comparisons have a bool result, so the operand context comes from the
 * other operand, not the result type. Sema has already checked that an
 * untyped constant fits this concrete numeric type. Materialize it directly:
 * emitting a cast would leave an unrepresentable untyped vreg in MIR. */
static AstHirValueId lower_binary_operand(HirBuilder *b, const AstExpr *expr,
                                          const AstSemaType *other_type) {
  AstSemaExprFact fact;
  if (other_type && builder_expr_fact(b, expr, &fact) && fact.type &&
      fact.constant.kind != AST_SEMA_CONST_NONE) {
    AstSemaTypeKind source = ast_sema_type_kind(fact.type);
    AstSemaTypeKind target = ast_sema_type_kind(other_type);
    if ((source == AST_SEMA_TYPE_UNTYPED_INT &&
         (target == AST_SEMA_TYPE_INT || target == AST_SEMA_TYPE_BITS)) ||
        (source == AST_SEMA_TYPE_UNTYPED_FLOAT &&
         target == AST_SEMA_TYPE_FLOAT))
      return emit_constant(b, other_type, expr->span, fact.constant, expr);
  }
  return lower_expr(b, expr);
}

static AstHirValueId lower_expr_impl(HirBuilder *b, const AstExpr *expr) {
  AstSemaExprFact fact;
  memset(&fact, 0, sizeof(fact));
  if (!builder_expr_fact(b, expr, &fact) && expr->kind != AST_EXPR_MACRO_REF) {
    hir_function_diag(b->function, AST_HIR_DIAG_MISSING_SEMA_FACT,
                      AST_DIAG_ERROR, expr->span,
                      "missing semantic fact for %s expression",
                      ast_expr_kind_name(expr->kind));
  }
  const AstSemaType *type = fact.type;
  switch (expr->kind) {
  case AST_EXPR_ERROR:
    return emit_undef(b, type, expr->span, expr);
  case AST_EXPR_INTEGER:
  case AST_EXPR_FLOAT:
  case AST_EXPR_CHAR:
  case AST_EXPR_BOOL:
    return emit_constant(b, type, expr->span, fact.constant, expr);
  case AST_EXPR_STRING:
  case AST_EXPR_BYTE_STRING:
  case AST_EXPR_C_STRING:
    return emit_string_literal(b, expr, type);
  case AST_EXPR_NAME: {
    if (fact.resolution_kind == AST_SEMA_RESOLUTION_GENERIC_CONST)
      return emit_generic_const(b, expr, type, &fact);
    HirBinding *binding = find_binding(b, expr->as.name.name);
    AstHirValueId value = binding
                              ? emit_local_load(b, binding, expr->span, expr)
                              : lower_symbol_value(b, expr, type, fact);
    return coerce_value(b, value, type, expr->span, expr);
  }
  case AST_EXPR_TYPE:
    return emit_undef(b, type, expr->span, expr);
  case AST_EXPR_UNARY: {
    if (expr->as.unary.op == AST_TOK_AMP)
      return lower_address_of(b, expr->as.unary.operand, type, expr->span);
    /* The magnitude of a signed minimum literal need not fit the
       result type. Sema has checked the complete unary expression;
       preserve that typed constant instead of emitting an untyped
       positive magnitude which no machine register can represent. */
    if (fact.constant.kind != AST_SEMA_CONST_NONE && type &&
        ast_sema_type_kind(type) != AST_SEMA_TYPE_UNTYPED_INT &&
        ast_sema_type_kind(type) != AST_SEMA_TYPE_UNTYPED_FLOAT &&
        (expr->as.unary.operand->kind == AST_EXPR_INTEGER ||
         expr->as.unary.operand->kind == AST_EXPR_FLOAT) &&
        (expr->as.unary.op == AST_TOK_MINUS ||
         expr->as.unary.op == AST_TOK_PLUS ||
         expr->as.unary.op == AST_TOK_TILDE))
      return emit_constant(b, type, expr->span, fact.constant, expr);
    AstHirValueId operand = lower_expr(b, expr->as.unary.operand);
    AstHirInstruction i;
    memset(&i, 0, sizeof(i));
    i.op =
        expr->as.unary.op == AST_TOK_STAR ? AST_HIR_OP_LOAD : AST_HIR_OP_UNARY;
    i.result = AST_HIR_INVALID_ID;
    i.local = AST_HIR_INVALID_ID;
    i.span = expr->span;
    i.token_op = expr->as.unary.op;
    i.effects =
        hir_expr_instruction_effects(expr, type, AST_HIR_INTRINSIC_NONE);
    i.source_expr = expr;
    return hir_emit_value_instruction(b->function, b->current, i, type,
                                      &operand, 1u);
  }
  case AST_EXPR_BINARY: {
    AstHirValueId ops[2];
    ops[0] = lower_binary_operand(b, expr->as.binary.left,
                                  builder_expr_type(b, expr->as.binary.right));
    ops[1] = lower_binary_operand(b, expr->as.binary.right,
                                  builder_expr_type(b, expr->as.binary.left));
    AstHirInstruction i;
    memset(&i, 0, sizeof(i));
    i.op = AST_HIR_OP_BINARY;
    i.result = AST_HIR_INVALID_ID;
    i.local = AST_HIR_INVALID_ID;
    i.span = expr->span;
    i.token_op = expr->as.binary.op;
    i.effects =
        hir_expr_instruction_effects(expr, type, AST_HIR_INTRINSIC_NONE);
    i.source_expr = expr;
    return hir_emit_value_instruction(b->function, b->current, i, type, ops,
                                      2u);
  }
  case AST_EXPR_CONDITIONAL:
    return lower_conditional(b, expr, type);
  case AST_EXPR_CALL:
    return lower_call(b, expr, type, fact);
  case AST_EXPR_INDEX: {
    const AstSemaType *aggregate = builder_expr_type(b, expr->as.index.base);
    if (fact.is_lvalue && aggregate && type &&
        !sema_type_is_dependent(type, 0u) &&
        (ast_sema_type_kind(aggregate) == AST_SEMA_TYPE_ARRAY ||
         ast_sema_type_kind(aggregate) == AST_SEMA_TYPE_PTR ||
         ast_sema_type_kind(aggregate) == AST_SEMA_TYPE_CAP) &&
        ast_sema_type_kind(type) != AST_SEMA_TYPE_STRUCT &&
        ast_sema_type_kind(type) != AST_SEMA_TYPE_ARRAY &&
        ast_sema_type_kind(type) != AST_SEMA_TYPE_TUPLE) {
      const AstSemaType *pointer_type = sema_make_pointer(
          (AstSema *)b->program->sema, AST_SEMA_TYPE_PTR, type, 0u);
      AstHirValueId address =
          lower_address_of(b, expr, pointer_type, expr->span);
      AstHirInstruction load = {0};
      load.op = AST_HIR_OP_LOAD;
      load.result = AST_HIR_INVALID_ID;
      load.local = AST_HIR_INVALID_ID;
      load.span = expr->span;
      load.source_expr = expr;
      load.effects = AST_SEMA_EFFECT_READ_MEMORY;
      return hir_emit_value_instruction(b->function, b->current, load, type,
                                        &address, 1u);
    }
    AstHirValueId ops[2] = {lower_expr(b, expr->as.index.base),
                            lower_expr(b, expr->as.index.index)};
    AstHirInstruction i;
    memset(&i, 0, sizeof(i));
    i.op = AST_HIR_OP_INDEX;
    i.result = AST_HIR_INVALID_ID;
    i.local = AST_HIR_INVALID_ID;
    i.span = expr->span;
    i.effects = 0u;
    i.source_expr = expr;
    const AstSemaType *base_type = ops[0] < b->function->value_count
                                       ? b->function->values[ops[0]].type
                                       : NULL;
    if (base_type && (ast_sema_type_kind(base_type) == AST_SEMA_TYPE_PTR ||
                      ast_sema_type_kind(base_type) == AST_SEMA_TYPE_CAP))
      i.effects |= AST_SEMA_EFFECT_READ_MEMORY;
    return hir_emit_value_instruction(b->function, b->current, i, type, ops,
                                      2u);
  }
  case AST_EXPR_MEMBER: {
    if (fact.resolution_kind == AST_SEMA_RESOLUTION_TOP_LEVEL) {
      AstHirValueId value = lower_symbol_value(b, expr, type, fact);
      return coerce_value(b, value, type, expr->span, expr);
    }
    /* Read the scalar field through its address, not by loading the
     * entire enclosing aggregate into a virtual register. The latter
     * fails on large globals and loses pointer-member read effects.
     * Non-addressable aggregate values retain normal extraction. */
    const AstSemaType *record = builder_expr_type(b, expr->as.member.base);
    if (record && (ast_sema_type_kind(record) == AST_SEMA_TYPE_PTR ||
                   ast_sema_type_kind(record) == AST_SEMA_TYPE_CAP))
      record = ast_sema_type_element(record);
    if (fact.is_lvalue && record &&
        ast_sema_type_kind(record) == AST_SEMA_TYPE_STRUCT && type &&
        !sema_type_is_dependent(type, 0u) &&
        ast_sema_type_kind(type) != AST_SEMA_TYPE_STRUCT &&
        ast_sema_type_kind(type) != AST_SEMA_TYPE_ARRAY &&
        ast_sema_type_kind(type) != AST_SEMA_TYPE_TUPLE) {
      const AstSemaType *pointer_type = sema_make_pointer(
          (AstSema *)b->program->sema, AST_SEMA_TYPE_PTR, type, 0u);
      AstHirValueId address =
          lower_address_of(b, expr, pointer_type, expr->span);
      AstHirInstruction load = {0};
      load.op = AST_HIR_OP_LOAD;
      load.result = AST_HIR_INVALID_ID;
      load.local = AST_HIR_INVALID_ID;
      load.span = expr->span;
      load.source_expr = expr;
      load.effects = AST_SEMA_EFFECT_READ_MEMORY;
      return hir_emit_value_instruction(b->function, b->current, load, type,
                                        &address, 1u);
    }
    AstHirValueId base = lower_expr(b, expr->as.member.base);
    AstHirInstruction i;
    memset(&i, 0, sizeof(i));
    i.op = AST_HIR_OP_MEMBER;
    i.result = AST_HIR_INVALID_ID;
    i.local = AST_HIR_INVALID_ID;
    i.span = expr->span;
    i.symbol = expr->as.member.member;
    i.effects = 0u;
    i.source_expr = expr;
    const AstSemaType *base_type =
        base < b->function->value_count ? b->function->values[base].type : NULL;
    i.aux_index = struct_field_index(base_type, expr->as.member.member);
    return hir_emit_value_instruction(b->function, b->current, i, type, &base,
                                      1u);
  }
  case AST_EXPR_LOAD: {
    AstHirValueId address = lower_expr(b, expr->as.load.address);
    AstHirInstruction i;
    memset(&i, 0, sizeof(i));
    i.op = AST_HIR_OP_LOAD;
    i.result = AST_HIR_INVALID_ID;
    i.local = AST_HIR_INVALID_ID;
    i.span = expr->span;
    i.effects =
        hir_expr_instruction_effects(expr, type, AST_HIR_INTRINSIC_NONE);
    i.source_expr = expr;
    return hir_emit_value_instruction(b->function, b->current, i, type,
                                      &address, 1u);
  }
  case AST_EXPR_CAST: {
    /* Preserve Sema's checked, converted constant. In particular,
     * cast<u16>(256 * 16 - 1) has no runtime untyped operands. */
    if (fact.constant.kind != AST_SEMA_CONST_NONE)
      return emit_constant(b, type, expr->span, fact.constant, expr);
    AstHirValueId value = lower_expr(b, expr->as.cast.value);
    AstHirInstruction i;
    memset(&i, 0, sizeof(i));
    i.op = AST_HIR_OP_CAST;
    i.result = AST_HIR_INVALID_ID;
    i.local = AST_HIR_INVALID_ID;
    i.span = expr->span;
    i.token_op = expr->as.cast.op;
    i.effects =
        hir_expr_instruction_effects(expr, type, AST_HIR_INTRINSIC_NONE);
    i.source_expr = expr;
    return hir_emit_value_instruction(b->function, b->current, i, type, &value,
                                      1u);
  }
  case AST_EXPR_ATOMIC: {
    uint32_t count = expr->as.atomic.args.count + 1u;
    AstHirValueId small[8];
    AstHirValueId *ops =
        count <= ASTER_ARRAY_COUNT(small)
            ? small
            : (AstHirValueId *)malloc((size_t)count * sizeof(*ops));
    if (!ops) {
      b->function->oom = 1u;
      return AST_HIR_INVALID_ID;
    }
    ops[0] = lower_expr(b, expr->as.atomic.address);
    for (uint32_t i = 0u; i < expr->as.atomic.args.count; ++i)
      ops[i + 1u] = lower_expr(b, expr->as.atomic.args.items[i]);
    AstHirInstruction inst;
    memset(&inst, 0, sizeof(inst));
    inst.op = AST_HIR_OP_ATOMIC;
    inst.result = AST_HIR_INVALID_ID;
    inst.local = AST_HIR_INVALID_ID;
    inst.span = expr->span;
    inst.symbol = expr->as.atomic.op;
    hir_atomic_memory_orders(expr, &inst.memory_order,
                             &inst.failure_memory_order);
    inst.effects =
        hir_expr_instruction_effects(expr, type, AST_HIR_INTRINSIC_NONE);
    inst.source_expr = expr;
    AstHirValueId result = AST_HIR_INVALID_ID;
    if (type && ast_sema_type_kind(type) != AST_SEMA_TYPE_VOID)
      result = hir_emit_value_instruction(b->function, b->current, inst, type,
                                          ops, count);
    else
      hir_emit_instruction(b->function, b->current, inst, ops, count);
    if (ops != small)
      free(ops);
    return result;
  }
  case AST_EXPR_ATTR:
    if (is_fence_expr(expr)) {
      AstHirInstruction i;
      memset(&i, 0, sizeof(i));
      i.op = AST_HIR_OP_FENCE;
      i.result = AST_HIR_INVALID_ID;
      i.local = AST_HIR_INVALID_ID;
      i.span = expr->span;
      i.effects = AST_SEMA_EFFECT_ATOMIC | AST_SEMA_EFFECT_READ_MEMORY |
                  AST_SEMA_EFFECT_WRITE_MEMORY;
      i.memory_order = hir_fence_memory_order(expr);
      i.source_expr = expr;
      hir_emit_instruction(b->function, b->current, i, NULL, 0u);
      return AST_HIR_INVALID_ID;
    }
    return lower_expr(b, expr->as.attributed.base);
  case AST_EXPR_MACRO_REF:
    hir_function_diag(
        b->function, AST_HIR_DIAG_UNSUPPORTED_MACRO_VALUE, AST_DIAG_ERROR,
        expr->span, "macro metavariable reached HIR lowering before expansion");
    return emit_undef(b, type, expr->span, expr);
  default:
    hir_function_diag(b->function, AST_HIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                      expr->span, "unsupported expression kind %u",
                      (unsigned)expr->kind);
    return emit_undef(b, type, expr->span, expr);
  }
}

static AstHirValueId lower_expr(HirBuilder *b, const AstExpr *expr) {
  if (!expr)
    return AST_HIR_INVALID_ID;
  if (++b->recursion_depth > b->program->config.recursion_limit) {
    b->recursion_depth--;
    hir_function_diag(b->function, AST_HIR_DIAG_RECURSION_LIMIT, AST_DIAG_ERROR,
                      expr->span, "HIR expression recursion limit exceeded");
    return AST_HIR_INVALID_ID;
  }
  AstHirValueId out = lower_expr_impl(b, expr);
  b->recursion_depth--;
  return out;
}

static int precreate_labels_stmt(HirBuilder *b, const AstStmt *stmt,
                                 uint32_t depth);
static int precreate_labels_block(HirBuilder *b, const AstBlock *block,
                                  uint32_t depth) {
  if (!block)
    return 1;
  for (uint32_t i = 0u; i < block->statements.count; ++i)
    if (!precreate_labels_stmt(b, block->statements.items[i], depth))
      return 0;
  return 1;
}

static int precreate_label(HirBuilder *b, AstName name, AstSpan span,
                           uint32_t depth) {
  if (find_label(b, name))
    return 1;
  if (!builder_reserve_labels(b, b->label_count + 1u)) {
    b->function->oom = 1u;
    return 0;
  }
  HirLabel *l = &b->labels[b->label_count++];
  l->name = name;
  l->scope_depth = depth;
  l->block = new_block(b, span);
  return l->block != AST_HIR_INVALID_ID;
}

static int precreate_labels_stmt(HirBuilder *b, const AstStmt *stmt,
                                 uint32_t depth) {
  if (!stmt)
    return 1;
  switch (stmt->kind) {
  case AST_STMT_BLOCK_LABEL:
    return precreate_label(b, stmt->as.block_label.label, stmt->span, depth);
  case AST_STMT_BLOCK:
    return precreate_labels_block(b, stmt->as.block_stmt.block, depth + 1u);
  case AST_STMT_IF:
    if (!precreate_labels_block(b, stmt->as.if_stmt.then_block, depth + 1u))
      return 0;
    if (stmt->as.if_stmt.else_branch)
      return precreate_labels_stmt(b, stmt->as.if_stmt.else_branch, depth);
    return 1;
  case AST_STMT_WHILE:
    return precreate_labels_block(b, stmt->as.while_stmt.body, depth + 1u);
  case AST_STMT_LOOP:
    return precreate_labels_block(b, stmt->as.loop_stmt.body, depth + 1u);
  case AST_STMT_UNSAFE:
    return precreate_labels_block(b, stmt->as.unsafe_stmt.body, depth + 1u);
  case AST_STMT_REQUIRES: {
    int enabled = 0;
    if (hir_const_bool(b->program, stmt->as.requires_stmt.condition,
                       &enabled) &&
        !enabled)
      return 1;
    return precreate_labels_block(b, stmt->as.requires_stmt.body, depth + 1u);
  }
  case AST_STMT_COMPTIME:
    if (stmt->as.comptime_stmt.inner) {
      if (stmt->as.comptime_stmt.inner->kind == AST_STMT_IF) {
        const AstStmt *inner = stmt->as.comptime_stmt.inner;
        int enabled = 0;
        if (hir_const_bool(b->program, inner->as.if_stmt.condition, &enabled)) {
          if (enabled)
            return precreate_labels_block(b, inner->as.if_stmt.then_block,
                                          depth + 1u);
          if (inner->as.if_stmt.else_branch)
            return precreate_labels_stmt(b, inner->as.if_stmt.else_branch,
                                         depth);
          return 1;
        }
      }
      return precreate_labels_stmt(b, stmt->as.comptime_stmt.inner, depth);
    }
    return precreate_labels_block(b, stmt->as.comptime_stmt.body, depth + 1u);
  case AST_STMT_FOR: {
    uint64_t begin = 0u, end = 0u;
    if (hir_const_u64(b->program, stmt->as.for_stmt.begin, &begin) &&
        hir_const_u64(b->program, stmt->as.for_stmt.end, &end) && begin >= end)
      return 1;
    return precreate_labels_block(b, stmt->as.for_stmt.body, depth + 1u);
  }
  default:
    return 1;
  }
}

static int lower_block(HirBuilder *b, const AstBlock *block, int create_scope);

static int lower_global_assignment(HirBuilder *b, AstName name,
                                   AstHirValueId value, AstSpan span,
                                   const AstStmt *source) {
  AstSemaSymbolInfo info;
  memset(&info, 0, sizeof(info));
  if (!resolve_simple_symbol(b, b->module->name, name, &info))
    return 0;
  AstHirInstruction i;
  memset(&i, 0, sizeof(i));
  i.op = AST_HIR_OP_GLOBAL_STORE;
  i.result = AST_HIR_INVALID_ID;
  i.local = AST_HIR_INVALID_ID;
  i.span = span;
  i.module = b->module->name;
  i.symbol = name;
  i.effects = AST_SEMA_EFFECT_WRITE_MEMORY;
  i.source_stmt = source;
  return hir_emit_instruction(b->function, b->current, i, &value, 1u) !=
         AST_HIR_INVALID_ID;
}

static int lower_native(HirBuilder *b, const AstStmt *stmt) {
  const AstNativeBridge *bridge = &stmt->as.native_stmt.bridge;
  uint32_t region_index = hir_build_native_region(b, stmt);
  if (region_index == UINT32_MAX)
    return 0;
  uint32_t count = bridge->input_count;
  AstHirValueId small[8];
  AstHirValueId *inputs =
      count <= ASTER_ARRAY_COUNT(small)
          ? small
          : (AstHirValueId *)malloc((size_t)count * sizeof(*inputs));
  if (!inputs && count) {
    b->function->oom = 1u;
    return 0;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    HirBinding *binding = find_binding(b, bridge->inputs[i]);
    inputs[i] = binding
                    ? emit_local_load(b, binding, bridge->inputs[i].span, NULL)
                    : AST_HIR_INVALID_ID;
  }
  AstHirInstruction native;
  memset(&native, 0, sizeof(native));
  native.op = AST_HIR_OP_NATIVE;
  native.result = AST_HIR_INVALID_ID;
  native.local = AST_HIR_INVALID_ID;
  native.span = stmt->span;
  native.source_stmt = stmt;
  native.symbol = stmt->as.native_stmt.arch;
  native.native_region = region_index;
  native.effects = AST_SEMA_EFFECT_UNSAFE | AST_SEMA_EFFECT_READ_MEMORY |
                   AST_SEMA_EFFECT_WRITE_MEMORY;
  AstHirInstId native_id =
      hir_emit_instruction(b->function, b->current, native, inputs, count);
  if (inputs != small)
    free(inputs);
  if (native_id == AST_HIR_INVALID_ID)
    return 0;
  for (uint32_t i = 0u; i < bridge->output_count; ++i) {
    HirBinding *binding = find_binding(b, bridge->outputs[i]);
    if (!binding || binding->local == AST_HIR_INVALID_ID)
      continue;
    AstHirInstruction result;
    memset(&result, 0, sizeof(result));
    result.op = AST_HIR_OP_NATIVE_RESULT;
    result.result = AST_HIR_INVALID_ID;
    result.local = binding->local;
    result.aux_index = i;
    result.native_region = region_index;
    result.span = bridge->outputs[i].span;
    result.source_stmt = stmt;
    AstHirValueId value = hir_emit_value_instruction(
        b->function, b->current, result, binding->type, NULL, 0u);
    emit_local_store(b, binding->local, value, bridge->outputs[i].span, stmt);
  }
  return 1;
}

static int lower_comptime_if(HirBuilder *b, const AstStmt *if_stmt) {
  int enabled = 0;
  if (!hir_const_bool(b->program, if_stmt->as.if_stmt.condition, &enabled)) {
    hir_function_diag(b->function, AST_HIR_DIAG_INVALID_COMPTIME,
                      AST_DIAG_ERROR, if_stmt->span,
                      "comptime if condition has no semantic constant fact");
    return 0;
  }
  if (enabled)
    return lower_block(b, if_stmt->as.if_stmt.then_block, 1);
  if (if_stmt->as.if_stmt.else_branch) {
    const AstStmt *e = if_stmt->as.if_stmt.else_branch;
    if (e->kind == AST_STMT_IF)
      return lower_comptime_if(b, e);
    if (e->kind == AST_STMT_BLOCK)
      return lower_block(b, e->as.block_stmt.block, 1);
    return 1;
  }
  return 1;
}

static int lower_comptime_for(HirBuilder *b, const AstStmt *stmt) {
  uint64_t begin = 0u, end = 0u;
  if (!hir_const_u64(b->program, stmt->as.for_stmt.begin, &begin) ||
      !hir_const_u64(b->program, stmt->as.for_stmt.end, &end)) {
    hir_function_diag(
        b->function, AST_HIR_DIAG_INVALID_COMPTIME, AST_DIAG_ERROR, stmt->span,
        "comptime for bounds are not available as semantic constants");
    return 0;
  }
  if (begin >= end)
    return 1;
  uint64_t count = end - begin;
  if (count > 65536u) {
    hir_function_diag(b->function, AST_HIR_DIAG_INVALID_COMPTIME,
                      AST_DIAG_ERROR, stmt->span,
                      "comptime for expansion is too large for HIR lowering");
    return 0;
  }
  size_t binding_base = b->binding_count;
  b->scope_depth++;
  AstSemaConstInfo c;
  memset(&c, 0, sizeof(c));
  c.kind = AST_SEMA_CONST_INTEGER;
  if (!push_const_binding(b, stmt->as.for_stmt.variable, c))
    return 0;
  HirBinding *iterator = &b->bindings[b->binding_count - 1u];
  for (uint64_t value = begin; value < end; ++value) {
    iterator->direct_constant.lo = value;
    iterator->direct_constant.hi = 0u;
    iterator->direct_constant.is_negative = 0u;
    if (!lower_block(b, stmt->as.for_stmt.body, 1))
      break;
    if (hir_block_terminated(b->function, b->current))
      break;
  }
  b->binding_count = binding_base;
  b->scope_depth--;
  return !b->function->oom;
}

static int lower_stmt(HirBuilder *b, const AstStmt *stmt) {
  if (!stmt)
    return 1;
  if (++b->recursion_depth > b->program->config.recursion_limit) {
    b->recursion_depth--;
    hir_function_diag(b->function, AST_HIR_DIAG_RECURSION_LIMIT, AST_DIAG_ERROR,
                      stmt->span, "HIR statement recursion limit exceeded");
    return 0;
  }
  int ok = 1;
  switch (stmt->kind) {
  case AST_STMT_ERROR:
    break;
  case AST_STMT_BLOCK:
    ok = lower_block(b, stmt->as.block_stmt.block, 1);
    break;
  case AST_STMT_LET:
  case AST_STMT_CONST: {
    AstName name = stmt->kind == AST_STMT_CONST ? stmt->as.const_stmt.name
                                                : stmt->as.let_stmt.name;
    AstType *ast_type = stmt->kind == AST_STMT_CONST ? stmt->as.const_stmt.type
                                                     : stmt->as.let_stmt.type;
    AstExpr *value_expr = stmt->kind == AST_STMT_CONST
                              ? stmt->as.const_stmt.value
                              : stmt->as.let_stmt.value;
    const AstSemaType *type = NULL;
    if (ast_type) {
      AstSemaTypeFact tf;
      memset(&tf, 0, sizeof(tf));
      if (ast_sema_type_fact(b->program->sema, ast_type, &tf))
        type = builder_specialize_type(b, tf.type, stmt->span);
    }
    if (!type && value_expr)
      type = builder_expr_type(b, value_expr);
    uint32_t flags = stmt->kind == AST_STMT_CONST ? AST_HIR_LOCAL_CONST
                                                  : AST_HIR_LOCAL_MUTABLE;
    AstHirLocalId local =
        hir_add_local(b->function, name, type, stmt->span, flags);
    if (!push_local_binding(b, name, local, type)) {
      ok = 0;
      break;
    }
    if (value_expr) {
      AstHirValueId value = lower_expr(b, value_expr);
      ok = emit_local_store(b, local, value, stmt->span, stmt);
    }
    break;
  }
  case AST_STMT_ASSIGN: {
    AstHirValueId value = lower_expr(b, stmt->as.assign_stmt.value);
    HirBinding *binding = find_binding(b, stmt->as.assign_stmt.name);
    if (binding && binding->local != AST_HIR_INVALID_ID)
      ok = emit_local_store(b, binding->local, value, stmt->span, stmt);
    else
      ok = lower_global_assignment(b, stmt->as.assign_stmt.name, value,
                                   stmt->span, stmt);
    if (!ok)
      hir_function_diag(b->function, AST_HIR_DIAG_UNKNOWN_BINDING,
                        AST_DIAG_ERROR, stmt->span,
                        "unknown assignment binding during HIR lowering");
    break;
  }
  case AST_STMT_EXPR:
    if (is_fence_expr(stmt->as.expr_stmt.expr)) {
      AstHirInstruction i;
      memset(&i, 0, sizeof(i));
      i.op = AST_HIR_OP_FENCE;
      i.result = AST_HIR_INVALID_ID;
      i.local = AST_HIR_INVALID_ID;
      i.span = stmt->span;
      i.source_expr = stmt->as.expr_stmt.expr;
      i.effects = AST_SEMA_EFFECT_ATOMIC | AST_SEMA_EFFECT_READ_MEMORY |
                  AST_SEMA_EFFECT_WRITE_MEMORY;
      i.memory_order = hir_fence_memory_order(stmt->as.expr_stmt.expr);
      hir_emit_instruction(b->function, b->current, i, NULL, 0u);
    } else
      (void)lower_expr(b, stmt->as.expr_stmt.expr);
    break;
  case AST_STMT_STORE: {
    AstHirValueId ops[2] = {lower_expr(b, stmt->as.store_stmt.address),
                            lower_expr(b, stmt->as.store_stmt.value)};
    AstHirInstruction i;
    memset(&i, 0, sizeof(i));
    i.op = AST_HIR_OP_STORE;
    i.result = AST_HIR_INVALID_ID;
    i.local = AST_HIR_INVALID_ID;
    i.span = stmt->span;
    i.effects = AST_SEMA_EFFECT_WRITE_MEMORY;
    if (hir_attribute_has(stmt->as.store_stmt.attributes, "volatile"))
      i.effects |= AST_SEMA_EFFECT_VOLATILE;
    if (hir_attribute_has(stmt->as.store_stmt.attributes, "nontemporal"))
      i.effects |= AST_SEMA_EFFECT_NONTEMPORAL;
    i.source_stmt = stmt;
    hir_emit_instruction(b->function, b->current, i, ops, 2u);
    break;
  }
  case AST_STMT_IF: {
    AstHirValueId cond = lower_expr(b, stmt->as.if_stmt.condition);
    AstHirBlockId then_block = new_block(b, stmt->as.if_stmt.then_block->span);
    AstHirBlockId else_block = new_block(
        b, stmt->as.if_stmt.else_branch ? stmt->as.if_stmt.else_branch->span
                                        : stmt->span);
    AstHirBlockId merge_block = new_block(b, stmt->span);
    emit_cond_br(b, cond, then_block, else_block, stmt->span);
    set_current(b, then_block);
    lower_block(b, stmt->as.if_stmt.then_block, 1);
    if (!hir_block_terminated(b->function, b->current))
      emit_br(b, merge_block, stmt->span, NULL, 0u);
    set_current(b, else_block);
    if (stmt->as.if_stmt.else_branch)
      lower_stmt(b, stmt->as.if_stmt.else_branch);
    if (!hir_block_terminated(b->function, b->current))
      emit_br(b, merge_block, stmt->span, NULL, 0u);
    set_current(b, merge_block);
    break;
  }
  case AST_STMT_WHILE: {
    AstHirBlockId cond_block =
        new_block(b, stmt->as.while_stmt.condition->span);
    AstHirBlockId body_block = new_block(b, stmt->as.while_stmt.body->span);
    AstHirBlockId exit_block = new_block(b, stmt->span);
    if (!hir_block_terminated(b->function, b->current))
      emit_br(b, cond_block, stmt->span, NULL, 0u);
    set_current(b, cond_block);
    AstHirValueId cond = lower_expr(b, stmt->as.while_stmt.condition);
    emit_cond_br(b, cond, body_block, exit_block, stmt->span);
    if (!builder_reserve_loops(b, b->loop_count + 1u)) {
      b->function->oom = 1u;
      ok = 0;
      break;
    }
    b->loops[b->loop_count++] = (HirLoopFrame){cond_block, exit_block};
    set_current(b, body_block);
    lower_block(b, stmt->as.while_stmt.body, 1);
    if (!hir_block_terminated(b->function, b->current))
      emit_br(b, cond_block, stmt->span, NULL, 0u);
    b->loop_count--;
    set_current(b, exit_block);
    break;
  }
  case AST_STMT_LOOP: {
    AstHirBlockId body_block = new_block(b, stmt->as.loop_stmt.body->span);
    AstHirBlockId exit_block = new_block(b, stmt->span);
    if (!hir_block_terminated(b->function, b->current))
      emit_br(b, body_block, stmt->span, NULL, 0u);
    if (!builder_reserve_loops(b, b->loop_count + 1u)) {
      b->function->oom = 1u;
      ok = 0;
      break;
    }
    b->loops[b->loop_count++] = (HirLoopFrame){body_block, exit_block};
    set_current(b, body_block);
    lower_block(b, stmt->as.loop_stmt.body, 1);
    if (!hir_block_terminated(b->function, b->current))
      emit_br(b, body_block, stmt->span, NULL, 0u);
    b->loop_count--;
    set_current(b, exit_block);
    break;
  }
  case AST_STMT_RETURN: {
    AstHirValueId value = stmt->as.return_stmt.value
                              ? lower_expr(b, stmt->as.return_stmt.value)
                              : AST_HIR_INVALID_ID;
    emit_return(b, value, stmt->span);
    break;
  }
  case AST_STMT_BREAK:
  case AST_STMT_CONTINUE: {
    AstHirBlockId target = AST_HIR_INVALID_ID;
    if (stmt->as.jump_stmt.has_label) {
      HirLabel *label = find_label(b, stmt->as.jump_stmt.label);
      if (label)
        target = label->block;
    } else if (b->loop_count) {
      HirLoopFrame *loop = &b->loops[b->loop_count - 1u];
      target = stmt->kind == AST_STMT_BREAK ? loop->break_block
                                            : loop->continue_block;
    }
    if (target == AST_HIR_INVALID_ID) {
      hir_function_diag(b->function, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                        stmt->span,
                        "jump target unavailable during HIR lowering");
      emit_unreachable(b, stmt->span);
    } else
      emit_br(b, target, stmt->span, NULL, 0u);
    break;
  }
  case AST_STMT_BLOCK_LABEL: {
    HirLabel *label = find_label(b, stmt->as.block_label.label);
    if (!label) {
      ok = 0;
      break;
    }
    if (!hir_block_terminated(b->function, b->current) &&
        b->current != label->block)
      emit_br(b, label->block, stmt->span, NULL, 0u);
    set_current(b, label->block);
    break;
  }
  case AST_STMT_BRANCH: {
    HirLabel *label = find_label(b, stmt->as.branch_stmt.label);
    if (!label) {
      hir_function_diag(b->function, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                        stmt->span,
                        "branch label unavailable during HIR lowering");
      emit_unreachable(b, stmt->span);
    } else
      emit_br(b, label->block, stmt->span, NULL, 0u);
    break;
  }
  case AST_STMT_UNSAFE:
    b->unsafe_depth++;
    ok = lower_block(b, stmt->as.unsafe_stmt.body, 1);
    b->unsafe_depth--;
    break;
  case AST_STMT_COMPTIME:
    if (stmt->as.comptime_stmt.inner) {
      if (stmt->as.comptime_stmt.inner->kind == AST_STMT_IF)
        ok = lower_comptime_if(b, stmt->as.comptime_stmt.inner);
      else
        ok = lower_stmt(b, stmt->as.comptime_stmt.inner);
    } else
      ok = lower_block(b, stmt->as.comptime_stmt.body, 1);
    break;
  case AST_STMT_FOR:
    ok = lower_comptime_for(b, stmt);
    break;
  case AST_STMT_REQUIRES: {
    int enabled = 0;
    if (!hir_const_bool(b->program, stmt->as.requires_stmt.condition,
                        &enabled)) {
      hir_function_diag(b->function, AST_HIR_DIAG_INVALID_COMPTIME,
                        AST_DIAG_ERROR, stmt->span,
                        "requires condition has no semantic constant fact");
      ok = 0;
    } else if (enabled)
      ok = lower_block(b, stmt->as.requires_stmt.body, 1);
    break;
  }
  case AST_STMT_BIND: {
    HirBinding *binding = find_binding(b, stmt->as.bind_stmt.value);
    AstHirValueId value = binding
                              ? emit_local_load(b, binding, stmt->span, NULL)
                              : AST_HIR_INVALID_ID;
    AstHirInstruction i;
    memset(&i, 0, sizeof(i));
    i.op = AST_HIR_OP_BIND;
    i.result = AST_HIR_INVALID_ID;
    i.local = binding ? binding->local : AST_HIR_INVALID_ID;
    i.span = stmt->span;
    i.symbol = stmt->as.bind_stmt.target;
    i.source_stmt = stmt;
    if (value != AST_HIR_INVALID_ID)
      hir_emit_instruction(b->function, b->current, i, &value, 1u);
    break;
  }
  case AST_STMT_NATIVE:
  case AST_STMT_EXACT:
    ok = lower_native(b, stmt);
    break;
  case AST_STMT_STATIC_ASSERT:
    break;
  default:
    hir_function_diag(b->function, AST_HIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                      stmt->span, "unsupported statement kind %u",
                      (unsigned)stmt->kind);
    ok = 0;
    break;
  }
  b->recursion_depth--;
  return ok && !b->function->oom;
}

static int lower_block(HirBuilder *b, const AstBlock *block, int create_scope) {
  if (!block)
    return 1;
  size_t binding_base = b->binding_count;
  if (create_scope)
    b->scope_depth++;
  for (uint32_t i = 0u; i < block->statements.count; ++i) {
    const AstStmt *stmt = block->statements.items[i];
    if (hir_block_terminated(b->function, b->current) && stmt &&
        stmt->kind != AST_STMT_BLOCK_LABEL) {
      AstHirBlockId dead = new_block(b, stmt->span);
      set_current(b, dead);
    }
    if (!lower_stmt(b, stmt)) {
      if (create_scope)
        b->scope_depth--;
      b->binding_count = binding_base;
      return 0;
    }
  }
  b->binding_count = binding_base;
  if (create_scope)
    b->scope_depth--;
  return 1;
}

static int
hir_lower_function_impl(AstHirProgram *program, AstHirFunction *fn,
                        const AstModule *module,
                        const AstSemaGenericBinding *template_bindings,
                        const AstSemaGenericBinding *specialization_values,
                        size_t specialization_count) {
  if (!program || !fn || !module || !fn->source_decl ||
      fn->source_decl->kind != AST_DECL_FN)
    return 0;
  const AstDecl *decl = fn->source_decl;
  HirBuilder b;
  memset(&b, 0, sizeof(b));
  b.program = program;
  b.function = fn;
  b.module = module;
  b.current = AST_HIR_INVALID_ID;
  b.template_bindings = template_bindings;
  b.specialization_values = specialization_values;
  b.specialization_count = specialization_count;
  if (!hir_preallocate_function_storage(fn, decl)) {
    fn->oom = 1u;
    goto cleanup;
  }
  AstHirBlockId entry = hir_add_block(fn, decl->span);
  if (entry == AST_HIR_INVALID_ID)
    return 0;
  fn->entry_block = entry;
  b.current = entry;

  if (!precreate_labels_block(&b, decl->as.fn_decl.body, 1u))
    goto cleanup;

  uint32_t param_count = decl->as.fn_decl.params.count;
  uint32_t type_param_count =
      fn->type ? ast_sema_type_function_param_count(fn->type) : 0u;
  for (uint32_t i = 0u; i < param_count; ++i) {
    const AstParam *param = &decl->as.fn_decl.params.items[i];
    const AstSemaType *type =
        i < type_param_count ? ast_sema_type_function_param(fn->type, i) : NULL;
    AstHirValueId arg = hir_add_block_argument(fn, entry, type, param->span);
    AstHirLocalId local =
        hir_add_local(fn, param->name, type, param->span,
                      AST_HIR_LOCAL_PARAMETER | AST_HIR_LOCAL_MUTABLE);
    if (arg == AST_HIR_INVALID_ID || local == AST_HIR_INVALID_ID ||
        !push_local_binding(&b, param->name, local, type))
      goto cleanup;
    if (!emit_local_store(&b, local, arg, param->span, NULL))
      goto cleanup;
  }

  if (!lower_block(&b, decl->as.fn_decl.body, 0))
    goto cleanup;
  if (!hir_block_terminated(fn, b.current)) {
    const AstSemaType *result =
        fn->type ? ast_sema_type_function_result(fn->type) : NULL;
    if (!result || ast_sema_type_kind(result) == AST_SEMA_TYPE_VOID)
      emit_return(&b, AST_HIR_INVALID_ID, decl->span);
    else
      emit_unreachable(&b, decl->span);
  }
  if (!hir_analyze_cfg(fn))
    goto cleanup;

cleanup:
  free(b.bindings);
  free(b.labels);
  free(b.loops);
  return !fn->oom && fn->diagnostic_count == 0u;
}

int hir_lower_function(AstHirProgram *program, AstHirFunction *fn,
                       const AstModule *module) {
  return hir_lower_function_impl(program, fn, module, NULL, NULL, 0u);
}

int hir_lower_function_specialized(
    AstHirProgram *program, AstHirFunction *fn, const AstModule *module,
    const AstSemaGenericBinding *template_bindings,
    const AstSemaGenericBinding *specialization_values,
    size_t specialization_count) {
  return hir_lower_function_impl(program, fn, module, template_bindings,
                                 specialization_values, specialization_count);
}
