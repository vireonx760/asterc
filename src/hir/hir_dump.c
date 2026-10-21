#include "hir_internal.h"

#include <inttypes.h>

typedef struct HirDump {
  void (*write_fn)(void *user, const char *text, size_t length);
  void *user;
} HirDump;

static void out(HirDump *d, const char *text) {
  if (d && d->write_fn && text)
    d->write_fn(d->user, text, strlen(text));
}
static void outn(HirDump *d, const char *text, size_t length) {
  if (d && d->write_fn && text && length)
    d->write_fn(d->user, text, length);
}
static void outf(HirDump *d, const char *fmt, ...) ASTER_PRINTF_LIKE(2, 3);
static void outf(HirDump *d, const char *fmt, ...) {
  char small[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(small, sizeof(small), fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((size_t)n < sizeof(small)) {
    d->write_fn(d->user, small, (size_t)n);
    return;
  }
  size_t size = (size_t)n + 1u;
  char *buf = (char *)malloc(size);
  if (!buf)
    return;
  va_start(ap, fmt);
  vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  d->write_fn(d->user, buf, (size_t)n);
  free(buf);
}

static void print_name(HirDump *d, AstName n) {
  if (!n.text) {
    out(d, "<anon>");
    return;
  }
  outf(d, "%.*s", (int)n.span.length, n.text);
}

static void print_type(HirDump *d, const AstSemaType *t, unsigned depth) {
  if (!t) {
    out(d, "<null-type>");
    return;
  }
  if (depth > 32u) {
    out(d, "<recursive-type>");
    return;
  }
  AstSemaTypeKind k = ast_sema_type_kind(t);
  switch (k) {
  case AST_SEMA_TYPE_ERROR:
    out(d, "error");
    break;
  case AST_SEMA_TYPE_VOID:
    out(d, "void");
    break;
  case AST_SEMA_TYPE_BOOL:
    out(d, "bool");
    break;
  case AST_SEMA_TYPE_INT:
    outf(d, "%c%u", ast_sema_type_is_signed(t) ? 'i' : 'u',
         ast_sema_type_bit_width(t));
    break;
  case AST_SEMA_TYPE_FLOAT:
    outf(d, "float<%u>", ast_sema_type_bit_width(t));
    break;
  case AST_SEMA_TYPE_PTR:
  case AST_SEMA_TYPE_CAP: {
    out(d, k == AST_SEMA_TYPE_PTR ? "ptr<" : "cap<");
    uint32_t q = ast_sema_type_pointer_qualifiers(t);
    if (q & AST_TYPE_QUAL_CONST)
      out(d, "const ");
    if (q & AST_TYPE_QUAL_VOLATILE)
      out(d, "volatile ");
    print_type(d, ast_sema_type_element(t), depth + 1u);
    out(d, ">");
    break;
  }
  case AST_SEMA_TYPE_BITS: {
    uint64_t w = 0;
    ast_sema_type_bits_width(t, &w);
    outf(d, "bits<%" PRIu64 ">", w);
    break;
  }
  case AST_SEMA_TYPE_VECTOR: {
    uint64_t n = 0;
    ast_sema_type_vector_lanes(t, &n);
    outf(d, "vec<%" PRIu64 ",", n);
    print_type(d, ast_sema_type_element(t), depth + 1u);
    out(d, ">");
    break;
  }
  case AST_SEMA_TYPE_MASK: {
    uint64_t n = 0;
    ast_sema_type_vector_lanes(t, &n);
    outf(d, "mask<%" PRIu64 ">", n);
    break;
  }
  case AST_SEMA_TYPE_SVECTOR:
    out(d, "svec<");
    print_type(d, ast_sema_type_element(t), depth + 1u);
    out(d, ">");
    break;
  case AST_SEMA_TYPE_PRED:
    out(d, "pred");
    break;
  case AST_SEMA_TYPE_ARRAY: {
    uint64_t n = 0;
    ast_sema_type_array_count(t, &n);
    out(d, "[");
    print_type(d, ast_sema_type_element(t), depth + 1u);
    outf(d, "; %" PRIu64 "]", n);
    break;
  }
  case AST_SEMA_TYPE_TUPLE: {
    out(d, "(");
    uint32_t n = ast_sema_type_tuple_count(t);
    for (uint32_t i = 0; i < n; ++i) {
      if (i)
        out(d, ", ");
      print_type(d, ast_sema_type_tuple_item(t, i), depth + 1u);
    }
    out(d, ")");
    break;
  }
  case AST_SEMA_TYPE_STRUCT: {
    out(d, "struct ");
    print_name(d, ast_sema_type_struct_name(t));
    break;
  }
  case AST_SEMA_TYPE_FUNCTION: {
    out(d, "fn(");
    uint32_t n = ast_sema_type_function_param_count(t);
    for (uint32_t i = 0; i < n; ++i) {
      if (i)
        out(d, ", ");
      print_type(d, ast_sema_type_function_param(t, i), depth + 1u);
    }
    out(d, ") -> ");
    print_type(d, ast_sema_type_function_result(t), depth + 1u);
    break;
  }
  case AST_SEMA_TYPE_UNTYPED_INT:
    out(d, "untyped_int");
    break;
  case AST_SEMA_TYPE_UNTYPED_FLOAT:
    out(d, "untyped_float");
    break;
  case AST_SEMA_TYPE_STRING:
    out(d, "string");
    break;
  case AST_SEMA_TYPE_BYTE_STRING:
    out(d, "byte_string");
    break;
  case AST_SEMA_TYPE_C_STRING:
    out(d, "c_string");
    break;
  case AST_SEMA_TYPE_TYPE_PARAMETER:
    out(d, "type_param");
    break;
  default:
    out(d, "<type>");
    break;
  }
}

static void print_const(HirDump *d, AstSemaConstInfo c) {
  switch (c.kind) {
  case AST_SEMA_CONST_INTEGER:
    if (c.hi)
      outf(d, "0x%016" PRIx64 "%016" PRIx64, c.hi, c.lo);
    else if (c.is_negative)
      outf(d, "-%" PRIu64, c.lo);
    else
      outf(d, "%" PRIu64, c.lo);
    break;
  case AST_SEMA_CONST_FLOAT:
    outf(d, "%.17g", c.floating);
    break;
  case AST_SEMA_CONST_BOOL:
    out(d, c.lo ? "true" : "false");
    break;
  default:
    out(d, "<literal>");
    break;
  }
}

static void print_span_ref(HirDump *d, AstSpan span) {
  outf(d, "@span(%u:%" PRIu64 "+%u)", span.file_id, span.offset, span.length);
}

static void print_hir_text(HirDump *d, AstHirText text) {
  out(d, "\"");
  for (uint32_t i = 0u; i < text.length; ++i) {
    unsigned char c = (unsigned char)text.data[i];
    switch (c) {
    case '\\':
      out(d, "\\\\");
      break;
    case '"':
      out(d, "\\\"");
      break;
    case '\n':
      out(d, "\\n");
      break;
    case '\r':
      out(d, "\\r");
      break;
    case '\t':
      out(d, "\\t");
      break;
    case '\0':
      out(d, "\\0");
      break;
    default:
      if (c < 0x20u || c == 0x7fu)
        outf(d, "\\x%02x", (unsigned)c);
      else
        outn(d, (const char *)&text.data[i], 1u);
      break;
    }
  }
  out(d, "\"");
}

static void print_text_value(HirDump *d, AstHirTextValue value) {
  if (!value.is_present || !value.text.data) {
    out(d, "-");
    return;
  }
  if (value.is_literal)
    outf(d, "%s", ast_token_kind_name(value.source_kind));
  print_hir_text(d, value.text);
}

static void print_initializer(HirDump *d, const AstHirInitializer *init,
                              unsigned depth) {
  if (!init) {
    out(d, "<missing-init>");
    return;
  }
  if (depth > 32u) {
    out(d, "<deep-init>");
    return;
  }
  switch (init->kind) {
  case AST_HIR_INIT_ZERO:
    out(d, "zero");
    break;
  case AST_HIR_INIT_CONSTANT:
    print_const(d, init->constant);
    break;
  case AST_HIR_INIT_LITERAL:
    outf(d, "literal(%s,", ast_token_kind_name(init->literal_kind));
    print_hir_text(d, init->literal);
    out(d, ")");
    break;
  case AST_HIR_INIT_SYMBOL:
    out(d, "@");
    print_name(d, init->module);
    out(d, ".");
    print_name(d, init->symbol);
    if (init->addend > 0)
      outf(d, "+%" PRId64, init->addend);
    else if (init->addend < 0)
      outf(d, "%" PRId64, init->addend);
    break;
  case AST_HIR_INIT_AGGREGATE:
    out(d, "{");
    for (uint32_t i = 0u; i < init->child_count; ++i) {
      if (i)
        out(d, ", ");
      print_initializer(d, init->children ? init->children[i] : NULL,
                        depth + 1u);
    }
    out(d, "}");
    break;
  case AST_HIR_INIT_NONE:
  default:
    out(d, "<invalid-init>");
    break;
  }
}

static void print_native_expr(HirDump *d, const AstHirNativeExpr *expr,
                              unsigned depth) {
  if (!expr) {
    out(d, "<null-native-expr>");
    return;
  }
  if (depth > 32u) {
    out(d, "<deep-native-expr>");
    return;
  }
  switch (expr->kind) {
  case AST_HIR_NATIVE_EXPR_NAME:
    print_name(d, expr->name);
    if (expr->binding_kind == AST_HIR_NATIVE_BINDING_INPUT)
      outf(d, "<in%u>", expr->binding_index);
    else if (expr->binding_kind == AST_HIR_NATIVE_BINDING_OUTPUT)
      outf(d, "<out%u>", expr->binding_index);
    break;
  case AST_HIR_NATIVE_EXPR_CONSTANT:
    print_const(d, expr->constant);
    break;
  case AST_HIR_NATIVE_EXPR_LITERAL:
    outf(d, "literal(%s,", ast_token_kind_name(expr->token_kind));
    print_hir_text(d, expr->literal);
    out(d, ")");
    break;
  case AST_HIR_NATIVE_EXPR_TYPE:
    out(d, "type<");
    print_type(d, expr->type, 0u);
    out(d, ">");
    break;
  default:
    outf(d, "%s",
         expr->kind == AST_HIR_NATIVE_EXPR_UNARY         ? "unary"
         : expr->kind == AST_HIR_NATIVE_EXPR_BINARY      ? "binary"
         : expr->kind == AST_HIR_NATIVE_EXPR_CONDITIONAL ? "cond"
         : expr->kind == AST_HIR_NATIVE_EXPR_CALL        ? "call"
         : expr->kind == AST_HIR_NATIVE_EXPR_INDEX       ? "index"
         : expr->kind == AST_HIR_NATIVE_EXPR_MEMBER      ? "member"
         : expr->kind == AST_HIR_NATIVE_EXPR_LOAD        ? "load"
         : expr->kind == AST_HIR_NATIVE_EXPR_CAST        ? "cast"
         : expr->kind == AST_HIR_NATIVE_EXPR_ATOMIC      ? "atomic"
         : expr->kind == AST_HIR_NATIVE_EXPR_ATTRIBUTE   ? "attr"
                                                         : "invalid");
    if (expr->name.text) {
      out(d, "[");
      print_name(d, expr->name);
      out(d, "]");
    }
    out(d, "(");
    for (uint32_t i = 0u; i < expr->child_count; ++i) {
      if (i)
        out(d, ", ");
      print_native_expr(d, expr->children ? expr->children[i] : NULL,
                        depth + 1u);
    }
    out(d, ")");
    break;
  }
}

static void print_native_region(HirDump *d, const AstHirNativeRegion *region,
                                uint32_t index) {
  outf(d, "  native.region @nr%u ", index);
  print_name(d, region->arch);
  if (region->flags & AST_HIR_NATIVE_EXACT)
    out(d, " [exact]");
  if (region->flags & AST_HIR_NATIVE_UNSAFE_EXACT)
    out(d, " [unsafe]");
  out(d, " inputs(");
  for (uint32_t i = 0u; i < region->input_count; ++i) {
    if (i)
      out(d, ",");
    print_name(d, region->inputs[i]);
  }
  out(d, ") outputs(");
  for (uint32_t i = 0u; i < region->output_count; ++i) {
    if (i)
      out(d, ",");
    print_name(d, region->outputs[i]);
  }
  out(d, ") {\n");
  for (uint32_t si = 0u; si < region->statement_count; ++si) {
    const AstHirNativeStmtCanonical *stmt = &region->statements[si];
    if (stmt->kind == AST_HIR_NATIVE_BYTES) {
      out(d, "    bytes {");
      for (uint32_t bi = 0u; bi < stmt->byte_count; ++bi) {
        if (bi)
          out(d, ",");
        outf(d, "0x%02x", stmt->bytes[bi]);
      }
      out(d, "}\n");
      continue;
    }
    out(d, "    ");
    print_name(d, stmt->mnemonic);
    out(d, " {");
    for (uint32_t oi = 0u; oi < stmt->operand_count; ++oi) {
      const AstHirNativeOperand *op = &stmt->operands[oi];
      if (oi)
        out(d, ", ");
      print_name(d, op->name);
      out(d, ": ");
      if (op->is_memory) {
        out(d, "mem<");
        print_type(d, op->memory_type, 0u);
        out(d, ">[");
        print_native_expr(d, op->memory_address, 0u);
        out(d, "]");
      } else
        print_native_expr(d, op->value, 0u);
    }
    out(d, "}");
    for (uint32_t ai = 0u; ai < stmt->attribute_count; ++ai) {
      const AstHirNativeAttribute *attr = &stmt->attributes[ai];
      out(d, " @");
      print_name(d, attr->name);
      if (attr->arg_count) {
        out(d, "(");
        for (uint32_t j = 0u; j < attr->arg_count; ++j) {
          if (j)
            out(d, ",");
          print_native_expr(d, attr->args ? attr->args[j] : NULL, 0u);
        }
        out(d, ")");
      }
    }
    out(d, "\n");
  }
  out(d, "  }\n");
}

static void print_effects(HirDump *d, uint32_t e) {
  if (!e)
    return;
  out(d, " effects[");
  int first = 1;
#define E(bit, name)                                                           \
  do {                                                                         \
    if (e & (bit)) {                                                           \
      if (!first)                                                              \
        out(d, ",");                                                           \
      out(d, (name));                                                          \
      first = 0;                                                               \
    }                                                                          \
  } while (0)
  E(AST_SEMA_EFFECT_READ_MEMORY, "read");
  E(AST_SEMA_EFFECT_WRITE_MEMORY, "write");
  E(AST_SEMA_EFFECT_ATOMIC, "atomic");
  E(AST_SEMA_EFFECT_CALL, "call");
  E(AST_SEMA_EFFECT_MAY_TRAP, "trap");
  E(AST_SEMA_EFFECT_VOLATILE, "volatile");
  E(AST_SEMA_EFFECT_UNSAFE, "unsafe");
#undef E
  out(d, "]");
}

static void print_instruction(HirDump *d, const AstHirFunction *fn,
                              AstHirInstId id) {
  if (!fn || id >= fn->instruction_count)
    return;
  const AstHirInstruction *i = &fn->instructions[id];
  out(d, "    ");
  if (i->result != AST_HIR_INVALID_ID)
    outf(d, "%%v%u = ", i->result);
  out(d, ast_hir_op_name(i->op));
  if (i->op == AST_HIR_OP_CONSTANT) {
    out(d, " ");
    print_const(d, i->constant);
  }
  if (i->op == AST_HIR_OP_LOCAL_LOAD || i->op == AST_HIR_OP_LOCAL_STORE ||
      i->op == AST_HIR_OP_LOCAL_ADDR || i->op == AST_HIR_OP_NATIVE_RESULT ||
      i->op == AST_HIR_OP_BIND)
    outf(d, " @l%u", i->local);
  if (i->op == AST_HIR_OP_GLOBAL_ADDR || i->op == AST_HIR_OP_GLOBAL_LOAD ||
      i->op == AST_HIR_OP_GLOBAL_STORE || i->op == AST_HIR_OP_FUNCTION_REF) {
    out(d, " @");
    print_name(d, i->module);
    out(d, ".");
    print_name(d, i->symbol);
  }
  if (i->op == AST_HIR_OP_UNARY || i->op == AST_HIR_OP_BINARY ||
      i->op == AST_HIR_OP_CAST)
    outf(d, " [%s]", ast_token_kind_name(i->token_op));
  if (i->op == AST_HIR_OP_INTRINSIC)
    outf(d, " [%s]", ast_hir_intrinsic_name(i->intrinsic));
  if (i->op == AST_HIR_OP_ATOMIC || i->op == AST_HIR_OP_NATIVE) {
    out(d, " [");
    print_name(d, i->symbol);
    out(d, "]");
  }
  if (i->op == AST_HIR_OP_ATOMIC) {
    outf(d, " order[%s", ast_memory_order_name(i->memory_order));
    if (i->failure_memory_order != AST_MEMORY_ORDER_NONE)
      outf(d, ",fail=%s", ast_memory_order_name(i->failure_memory_order));
    out(d, "]");
  }
  if (i->op == AST_HIR_OP_FENCE)
    outf(d, " order[%s]", ast_memory_order_name(i->memory_order));
  if (i->op == AST_HIR_OP_MEMBER) {
    out(d, " .");
    print_name(d, i->symbol);
    if (i->aux_index != UINT32_MAX)
      outf(d, "#%u", i->aux_index);
  }
  if (i->operand_count) {
    out(d, " (");
    for (uint32_t n = 0; n < i->operand_count; ++n) {
      if (n)
        out(d, ", ");
      outf(d, "%%v%u", i->operands[n]);
    }
    out(d, ")");
  }
  if (i->result != AST_HIR_INVALID_ID) {
    out(d, " : ");
    print_type(d, i->type, 0u);
  }
  print_effects(d, i->effects);
  out(d, "\n");
}

static void print_branch_args(HirDump *d, const AstHirValueId *args,
                              uint32_t count) {
  if (!count)
    return;
  out(d, "(");
  for (uint32_t i = 0u; i < count; ++i) {
    if (i)
      out(d, ", ");
    outf(d, "%%v%u", args[i]);
  }
  out(d, ")");
}

static void print_term(HirDump *d, const AstHirTerminator *t) {
  out(d, "    ");
  switch (t->kind) {
  case AST_HIR_TERM_BR:
    out(d, "br ^bb");
    outf(d, "%u", t->target_true);
    print_branch_args(d, t->true_args, t->true_arg_count);
    break;
  case AST_HIR_TERM_COND_BR:
    outf(d, "cond_br %%v%u, ^bb%u", t->condition_or_value, t->target_true);
    print_branch_args(d, t->true_args, t->true_arg_count);
    outf(d, ", ^bb%u", t->target_false);
    print_branch_args(d, t->false_args, t->false_arg_count);
    break;
  case AST_HIR_TERM_RETURN:
    if (t->condition_or_value == AST_HIR_INVALID_ID)
      out(d, "return");
    else
      outf(d, "return %%v%u", t->condition_or_value);
    break;
  case AST_HIR_TERM_UNREACHABLE:
    out(d, "unreachable");
    break;
  default:
    out(d, "<missing terminator>");
    break;
  }
  out(d, "\n");
}

void ast_hir_dump_function(const AstHirFunction *fn,
                           void (*write_fn)(void *, const char *, size_t),
                           void *user) {
  if (!fn || !write_fn)
    return;
  HirDump d = {write_fn, user};
  out(&d, "hir.func @");
  print_name(&d, fn->module_name);
  out(&d, ".");
  print_name(&d, fn->name);
  out(&d, " : ");
  print_type(&d, fn->type, 0u);
  if (fn->flags & AST_HIR_FUNCTION_EXTERN)
    out(&d, " [extern]");
  if (fn->flags & AST_HIR_FUNCTION_DISPATCH)
    out(&d, " [dispatch]");
  if (fn->flags & AST_HIR_FUNCTION_GENERIC)
    out(&d, " [generic]");
  if (fn->flags & AST_HIR_FUNCTION_NAKED)
    out(&d, " [naked]");
  if (fn->flags & AST_HIR_FUNCTION_LEAF)
    out(&d, " [leaf]");
  if (fn->flags & AST_HIR_FUNCTION_NO_UNWIND)
    out(&d, " [no_unwind]");
  if (fn->flags & AST_HIR_FUNCTION_INLINE)
    out(&d, " [inline]");
  if (fn->flags & AST_HIR_FUNCTION_NO_INLINE)
    out(&d, " [noinline]");
  if (fn->frame_policy != AST_HIR_FRAME_DEFAULT)
    outf(&d, " [frame=%u]", (unsigned)fn->frame_policy);
  if (fn->stack_alignment)
    outf(&d, " [stack_align=%u]", fn->stack_alignment);
  if (fn->abi.is_present) {
    out(&d, " [abi=");
    print_text_value(&d, fn->abi);
    out(&d, "]");
  }
  if (fn->link_name.is_present) {
    out(&d, " [link=");
    print_text_value(&d, fn->link_name);
    out(&d, "]");
  }
  if (fn->target_constraint.is_present) {
    out(&d, " [target=");
    print_text_value(&d, fn->target_constraint);
    out(&d, "]");
  }
  if (fn->requires_constraint.is_present) {
    out(&d, " [requires=");
    print_text_value(&d, fn->requires_constraint);
    out(&d, "]");
  }
  out(&d, " {\n");
  for (uint32_t gi = 0u; gi < fn->generic_param_count; ++gi) {
    const AstHirGenericParam *gp = &fn->generic_params[gi];
    outf(&d, "  generic @g%u ", gi);
    print_name(&d, gp->name);
    if (gp->kind == AST_HIR_GENERIC_TYPE)
      out(&d, " : type\n");
    else {
      out(&d, " : const ");
      print_type(&d, gp->const_type, 0u);
      out(&d, "\n");
    }
  }
  for (uint32_t ci = 0u; ci < fn->dispatch_case_count; ++ci) {
    const AstHirDispatchCase *c = &fn->dispatch_cases[ci];
    out(&d, "  dispatch ");
    if (c->is_else)
      out(&d, "else");
    else
      print_const(&d, c->condition);
    out(&d, " -> @");
    print_name(&d, c->target);
    out(&d, "\n");
  }
  for (uint32_t ni = 0u; ni < fn->native_region_count; ++ni)
    print_native_region(&d, &fn->native_regions[ni], ni);
  for (uint32_t li = 0u; li < fn->local_count; ++li) {
    const AstHirLocal *l = &fn->locals[li];
    outf(&d, "  local @l%u ", li);
    print_name(&d, l->name);
    out(&d, " : ");
    print_type(&d, l->type, 0u);
    out(&d, "\n");
  }
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    const AstHirBlock *b = &fn->blocks[bi];
    outf(&d, "  ^bb%u", bi);
    if (b->argument_count) {
      out(&d, "(");
      for (uint32_t ai = 0u; ai < b->argument_count; ++ai) {
        if (ai)
          out(&d, ", ");
        AstHirValueId v = b->arguments[ai];
        outf(&d, "%%v%u: ", v);
        print_type(&d, fn->values[v].type, 0u);
      }
      out(&d, ")");
    }
    outf(&d, "  ; preds=%u rpo=", b->predecessor_count);
    if (b->reverse_postorder == UINT32_MAX)
      out(&d, "- ");
    else
      outf(&d, "%u ", b->reverse_postorder);
    out(&d, "idom=");
    if (b->immediate_dominator == AST_HIR_INVALID_ID)
      out(&d, "-\n");
    else
      outf(&d, "^bb%u\n", b->immediate_dominator);
    for (uint32_t ii = 0u; ii < b->instruction_count; ++ii)
      print_instruction(&d, fn, b->instructions[ii]);
    print_term(&d, &b->term);
  }
  out(&d, "}\n");
}

void ast_hir_dump_program(const AstHirProgram *program,
                          void (*write_fn)(void *, const char *, size_t),
                          void *user) {
  if (!program || !write_fn)
    return;
  HirDump d = {write_fn, user};
  out(&d, "hir.program {\n");
  for (size_t ti = 0u; ti < program->target_count; ++ti) {
    const AstHirTarget *t = &program->targets[ti];
    out(&d, "  target @");
    print_name(&d, t->module_name);
    out(&d, " ");
    print_hir_text(&d, t->triple);
    out(&d, " {\n");
    for (uint32_t ii = 0u; ii < t->item_count; ++ii) {
      const AstHirTargetItemInfo *item = &t->items[ii];
      out(&d, "    ");
      if (item->is_feature) {
        outf(&d, "feature %c ", item->feature_sign >= 0 ? '+' : '-');
        print_hir_text(&d, item->feature);
      } else {
        print_name(&d, item->name);
        out(&d, " = ");
        if (item->constant.kind != AST_SEMA_CONST_NONE)
          print_const(&d, item->constant);
        else if (item->value_text.data)
          print_hir_text(&d, item->value_text);
        else
          print_span_ref(&d, item->value_span);
      }
      out(&d, "\n");
    }
    out(&d, "  }\n");
  }
  for (size_t mi = 0u; mi < program->module_count; ++mi) {
    out(&d, "  module @");
    print_name(&d, program->modules[mi].name);
    out(&d, "\n");
  }
  for (size_t gi = 0u; gi < program->global_count; ++gi) {
    const AstHirGlobal *g = &program->globals[gi];
    out(&d, "  global @");
    print_name(&d, g->module_name);
    out(&d, ".");
    print_name(&d, g->name);
    out(&d, " : ");
    print_type(&d, g->type, 0u);
    if (g->flags & AST_HIR_GLOBAL_CONST)
      out(&d, " [const]");
    if (g->flags & AST_HIR_GLOBAL_EXPORT)
      out(&d, " [export]");
    if (g->flags & AST_HIR_GLOBAL_READONLY)
      out(&d, " [readonly]");
    if (g->flags & AST_HIR_GLOBAL_WRITABLE)
      out(&d, " [writable]");
    if (g->flags & AST_HIR_GLOBAL_BSS)
      out(&d, " [bss]");
    if (g->alignment)
      outf(&d, " [align=%u]", g->alignment);
    if (g->section_alignment)
      outf(&d, " [section_align=%u]", g->section_alignment);
    if (g->section_name.data) {
      out(&d, " [section=");
      print_hir_text(&d, g->section_name);
      out(&d, "]");
    }
    out(&d, " = ");
    print_initializer(&d, g->initializer, 0u);
    out(&d, "\n");
  }
  for (size_t fi = 0u; fi < program->function_count; ++fi)
    ast_hir_dump_function(program->functions[fi], write_fn, user);
  out(&d, "}\n");
}
