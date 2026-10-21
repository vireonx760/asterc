#include "asterasm/parser.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct AstDumpCtx {
  void (*write_fn)(void *user, const char *text, size_t length);
  void *user;
} AstDumpCtx;

static int ast_dump_name_len(AstName name, const char *fallback) {
  if (name.text)
    return name.span.length > (uint32_t)INT_MAX ? INT_MAX
                                                : (int)name.span.length;
  size_t n = strlen(fallback);
  return n > (size_t)INT_MAX ? INT_MAX : (int)n;
}

static const char *ast_dump_name_ptr(AstName name, const char *fallback) {
  return name.text ? name.text : fallback;
}

#define AST_DUMP_NAME_ARGS(name, fallback)                                     \
  ast_dump_name_len((name), (fallback)), ast_dump_name_ptr((name), (fallback))

static void ast_dump_write(AstDumpCtx *d, const char *text) {
  d->write_fn(d->user, text, strlen(text));
}

static void ast_dump_printf(AstDumpCtx *d, const char *fmt, ...)
    ASTER_PRINTF_LIKE(2, 3);

static void ast_dump_printf(AstDumpCtx *d, const char *fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  if (n <= 0)
    return;
  size_t len = (size_t)n < sizeof(buffer) ? (size_t)n : sizeof(buffer) - 1u;
  d->write_fn(d->user, buffer, len);
}

static void ast_dump_indent(AstDumpCtx *d, int indent) {
  for (int i = 0; i < indent; ++i)
    ast_dump_write(d, "  ");
}

static void ast_dump_type(AstDumpCtx *d, const AstType *type, int indent);
static void ast_dump_expr(AstDumpCtx *d, const AstExpr *expr, int indent);
static void ast_dump_stmt(AstDumpCtx *d, const AstStmt *stmt, int indent);

static void ast_dump_attrs(AstDumpCtx *d, AstAttributeArray attrs, int indent) {
  for (uint32_t i = 0; i < attrs.count; ++i) {
    const AstAttribute *a = &attrs.items[i];
    ast_dump_indent(d, indent);
    ast_dump_printf(d, "attribute @%.*s args=%u\n",
                    AST_DUMP_NAME_ARGS(a->name, "?"), a->args.count);
  }
}

static void ast_dump_type(AstDumpCtx *d, const AstType *type, int indent) {
  ast_dump_indent(d, indent);
  if (!type) {
    ast_dump_write(d, "type <none>\n");
    return;
  }
  ast_dump_printf(d, "type %s", ast_type_kind_name(type->kind));
  switch (type->kind) {
  case AST_TYPE_PRIMITIVE:
    ast_dump_printf(d, " %s",
                    ast_token_kind_name(type->as.primitive.token_kind));
    break;
  case AST_TYPE_NAMED:
    ast_dump_printf(d, " %.*s", AST_DUMP_NAME_ARGS(type->as.named.name, "?"));
    break;
  case AST_TYPE_PTR:
  case AST_TYPE_CAP:
    ast_dump_printf(d, " qualifiers=0x%x", type->as.pointer.qualifiers);
    break;
  default:
    break;
  }
  ast_dump_write(d, "\n");
  switch (type->kind) {
  case AST_TYPE_PTR:
  case AST_TYPE_CAP:
    ast_dump_type(d, type->as.pointer.element, indent + 1);
    break;
  case AST_TYPE_BITS:
    ast_dump_expr(d, type->as.bits.width, indent + 1);
    break;
  case AST_TYPE_VEC:
    ast_dump_expr(d, type->as.vector.lanes, indent + 1);
    ast_dump_type(d, type->as.vector.element, indent + 1);
    break;
  case AST_TYPE_MASK:
    ast_dump_expr(d, type->as.mask.lanes, indent + 1);
    break;
  case AST_TYPE_SVEC:
  case AST_TYPE_PRED:
    ast_dump_type(d, type->as.scalable.element, indent + 1);
    break;
  case AST_TYPE_ARRAY:
    ast_dump_type(d, type->as.array.element, indent + 1);
    ast_dump_expr(d, type->as.array.count, indent + 1);
    break;
  case AST_TYPE_TUPLE:
    for (uint32_t i = 0; i < type->as.tuple.elements.count; ++i)
      ast_dump_type(d, type->as.tuple.elements.items[i], indent + 1);
    break;
  case AST_TYPE_NAMED:
    for (uint32_t i = 0; i < type->as.named.args.count; ++i) {
      const AstTypeArg *arg = &type->as.named.args.items[i];
      if (arg->kind == AST_TYPE_ARG_TYPE)
        ast_dump_type(d, arg->as.type, indent + 1);
      else
        ast_dump_expr(d, arg->as.expr, indent + 1);
    }
    break;
  default:
    break;
  }
}

static void ast_dump_expr(AstDumpCtx *d, const AstExpr *expr, int indent) {
  ast_dump_indent(d, indent);
  if (!expr) {
    ast_dump_write(d, "expr <none>\n");
    return;
  }
  ast_dump_printf(d, "expr %s", ast_expr_kind_name(expr->kind));
  if (expr->kind == AST_EXPR_NAME)
    ast_dump_printf(d, " %.*s", AST_DUMP_NAME_ARGS(expr->as.name.name, "?"));
  if (expr->kind == AST_EXPR_MEMBER)
    ast_dump_printf(d, " .%.*s",
                    AST_DUMP_NAME_ARGS(expr->as.member.member, "?"));
  if (expr->kind == AST_EXPR_UNARY)
    ast_dump_printf(d, " op=%s", ast_token_kind_name(expr->as.unary.op));
  if (expr->kind == AST_EXPR_BINARY)
    ast_dump_printf(d, " op=%s", ast_token_kind_name(expr->as.binary.op));
  if (expr->kind == AST_EXPR_CAST)
    ast_dump_printf(d, " op=%s", ast_token_kind_name(expr->as.cast.op));
  if (expr->kind == AST_EXPR_ATOMIC)
    ast_dump_printf(d, " op=%.*s", AST_DUMP_NAME_ARGS(expr->as.atomic.op, "?"));
  ast_dump_write(d, "\n");
  switch (expr->kind) {
  case AST_EXPR_TYPE:
    ast_dump_type(d, expr->as.type_operand.type, indent + 1);
    break;
  case AST_EXPR_UNARY:
    ast_dump_expr(d, expr->as.unary.operand, indent + 1);
    break;
  case AST_EXPR_BINARY:
    ast_dump_expr(d, expr->as.binary.left, indent + 1);
    ast_dump_expr(d, expr->as.binary.right, indent + 1);
    break;
  case AST_EXPR_CONDITIONAL:
    ast_dump_expr(d, expr->as.conditional.condition, indent + 1);
    ast_dump_expr(d, expr->as.conditional.then_expr, indent + 1);
    ast_dump_expr(d, expr->as.conditional.else_expr, indent + 1);
    break;
  case AST_EXPR_CALL:
    ast_dump_expr(d, expr->as.call.callee, indent + 1);
    for (uint32_t i = 0; i < expr->as.call.args.count; ++i)
      ast_dump_expr(d, expr->as.call.args.items[i], indent + 1);
    break;
  case AST_EXPR_INDEX:
    ast_dump_expr(d, expr->as.index.base, indent + 1);
    ast_dump_expr(d, expr->as.index.index, indent + 1);
    break;
  case AST_EXPR_MEMBER:
    ast_dump_expr(d, expr->as.member.base, indent + 1);
    break;
  case AST_EXPR_LOAD:
    ast_dump_type(d, expr->as.load.type, indent + 1);
    ast_dump_expr(d, expr->as.load.address, indent + 1);
    ast_dump_attrs(d, expr->as.load.attributes, indent + 1);
    break;
  case AST_EXPR_CAST:
    ast_dump_type(d, expr->as.cast.target, indent + 1);
    ast_dump_expr(d, expr->as.cast.value, indent + 1);
    break;
  case AST_EXPR_ATOMIC:
    ast_dump_type(d, expr->as.atomic.type, indent + 1);
    ast_dump_expr(d, expr->as.atomic.address, indent + 1);
    for (uint32_t i = 0; i < expr->as.atomic.args.count; ++i)
      ast_dump_expr(d, expr->as.atomic.args.items[i], indent + 1);
    ast_dump_attrs(d, expr->as.atomic.attributes, indent + 1);
    break;
  case AST_EXPR_ATTR:
    ast_dump_expr(d, expr->as.attributed.base, indent + 1);
    if (expr->as.attributed.attribute) {
      AstAttributeArray one = {expr->as.attributed.attribute, 1u};
      ast_dump_attrs(d, one, indent + 1);
    }
    break;
  default:
    break;
  }
}

static void ast_dump_block(AstDumpCtx *d, const AstBlock *block, int indent) {
  ast_dump_indent(d, indent);
  if (!block) {
    ast_dump_write(d, "block <none>\n");
    return;
  }
  ast_dump_printf(d, "block statements=%u\n", block->statements.count);
  for (uint32_t i = 0; i < block->statements.count; ++i)
    ast_dump_stmt(d, block->statements.items[i], indent + 1);
}

static void ast_dump_native(AstDumpCtx *d, const AstStmt *stmt, int indent) {
  const AstNativeStmtArray *body = &stmt->as.native_stmt.statements;
  for (uint32_t i = 0; i < body->count; ++i) {
    const AstNativeStmt *n = &body->items[i];
    ast_dump_indent(d, indent);
    if (n->kind == AST_NATIVE_BYTES)
      ast_dump_printf(d, "bytes count=%u\n", n->bytes.count);
    else {
      ast_dump_printf(d, "instruction %.*s operands=%u\n",
                      AST_DUMP_NAME_ARGS(n->mnemonic, "?"), n->operands.count);
      for (uint32_t j = 0; j < n->operands.count; ++j) {
        const AstNativeOperand *op = &n->operands.items[j];
        ast_dump_indent(d, indent + 1);
        ast_dump_printf(d, "operand %.*s%s\n",
                        AST_DUMP_NAME_ARGS(op->name, "?"),
                        op->is_memory ? " mem" : "");
        if (op->is_memory) {
          ast_dump_type(d, op->memory_type, indent + 2);
          ast_dump_expr(d, op->memory_address, indent + 2);
        } else
          ast_dump_expr(d, op->value, indent + 2);
      }
    }
  }
}

static void ast_dump_stmt(AstDumpCtx *d, const AstStmt *stmt, int indent) {
  ast_dump_indent(d, indent);
  if (!stmt) {
    ast_dump_write(d, "stmt <none>\n");
    return;
  }
  ast_dump_printf(d, "stmt %s", ast_stmt_kind_name(stmt->kind));
  switch (stmt->kind) {
  case AST_STMT_LET:
    ast_dump_printf(d, " %.*s",
                    AST_DUMP_NAME_ARGS(stmt->as.let_stmt.name, "?"));
    break;
  case AST_STMT_CONST:
    ast_dump_printf(d, " %.*s",
                    AST_DUMP_NAME_ARGS(stmt->as.const_stmt.name, "?"));
    break;
  case AST_STMT_ASSIGN:
    ast_dump_printf(d, " %.*s",
                    AST_DUMP_NAME_ARGS(stmt->as.assign_stmt.name, "?"));
    break;
  case AST_STMT_NATIVE:
  case AST_STMT_EXACT:
    ast_dump_printf(d, " arch=%.*s",
                    AST_DUMP_NAME_ARGS(stmt->as.native_stmt.arch, "?"));
    break;
  default:
    break;
  }
  ast_dump_write(d, "\n");
  switch (stmt->kind) {
  case AST_STMT_BLOCK:
    ast_dump_block(d, stmt->as.block_stmt.block, indent + 1);
    break;
  case AST_STMT_LET:
    if (stmt->as.let_stmt.type)
      ast_dump_type(d, stmt->as.let_stmt.type, indent + 1);
    if (stmt->as.let_stmt.value)
      ast_dump_expr(d, stmt->as.let_stmt.value, indent + 1);
    break;
  case AST_STMT_CONST:
    if (stmt->as.const_stmt.type)
      ast_dump_type(d, stmt->as.const_stmt.type, indent + 1);
    ast_dump_expr(d, stmt->as.const_stmt.value, indent + 1);
    break;
  case AST_STMT_ASSIGN:
    ast_dump_expr(d, stmt->as.assign_stmt.value, indent + 1);
    break;
  case AST_STMT_EXPR:
    ast_dump_expr(d, stmt->as.expr_stmt.expr, indent + 1);
    break;
  case AST_STMT_STORE:
    ast_dump_type(d, stmt->as.store_stmt.type, indent + 1);
    ast_dump_expr(d, stmt->as.store_stmt.address, indent + 1);
    ast_dump_expr(d, stmt->as.store_stmt.value, indent + 1);
    break;
  case AST_STMT_IF:
    ast_dump_expr(d, stmt->as.if_stmt.condition, indent + 1);
    ast_dump_block(d, stmt->as.if_stmt.then_block, indent + 1);
    if (stmt->as.if_stmt.else_branch)
      ast_dump_stmt(d, stmt->as.if_stmt.else_branch, indent + 1);
    break;
  case AST_STMT_WHILE:
    ast_dump_expr(d, stmt->as.while_stmt.condition, indent + 1);
    ast_dump_block(d, stmt->as.while_stmt.body, indent + 1);
    break;
  case AST_STMT_LOOP:
    ast_dump_block(d, stmt->as.loop_stmt.body, indent + 1);
    break;
  case AST_STMT_RETURN:
    if (stmt->as.return_stmt.value)
      ast_dump_expr(d, stmt->as.return_stmt.value, indent + 1);
    break;
  case AST_STMT_UNSAFE:
    ast_dump_block(d, stmt->as.unsafe_stmt.body, indent + 1);
    break;
  case AST_STMT_COMPTIME:
    if (stmt->as.comptime_stmt.inner)
      ast_dump_stmt(d, stmt->as.comptime_stmt.inner, indent + 1);
    if (stmt->as.comptime_stmt.body)
      ast_dump_block(d, stmt->as.comptime_stmt.body, indent + 1);
    break;
  case AST_STMT_FOR:
    ast_dump_expr(d, stmt->as.for_stmt.begin, indent + 1);
    ast_dump_expr(d, stmt->as.for_stmt.end, indent + 1);
    ast_dump_block(d, stmt->as.for_stmt.body, indent + 1);
    break;
  case AST_STMT_REQUIRES:
    ast_dump_expr(d, stmt->as.requires_stmt.condition, indent + 1);
    ast_dump_block(d, stmt->as.requires_stmt.body, indent + 1);
    break;
  case AST_STMT_NATIVE:
  case AST_STMT_EXACT:
    ast_dump_native(d, stmt, indent + 1);
    break;
  case AST_STMT_STATIC_ASSERT:
    ast_dump_expr(d, stmt->as.static_assert_stmt.condition, indent + 1);
    break;
  default:
    break;
  }
}

static void ast_dump_decl(AstDumpCtx *d, const AstDecl *decl, int indent) {
  ast_dump_indent(d, indent);
  if (!decl) {
    ast_dump_write(d, "decl <none>\n");
    return;
  }
  ast_dump_printf(d, "decl %s%s", ast_decl_kind_name(decl->kind),
                  decl->is_export ? " export" : "");
  switch (decl->kind) {
  case AST_DECL_IMPORT:
    ast_dump_printf(d, " %.*s",
                    AST_DUMP_NAME_ARGS(decl->as.import_decl.module, "?"));
    break;
  case AST_DECL_TYPE:
    ast_dump_printf(d, " %.*s",
                    AST_DUMP_NAME_ARGS(decl->as.type_decl.name, "?"));
    break;
  case AST_DECL_CONST:
    ast_dump_printf(d, " %.*s",
                    AST_DUMP_NAME_ARGS(decl->as.const_decl.name, "?"));
    break;
  case AST_DECL_EXTERN_FN:
    ast_dump_printf(d, " %.*s",
                    AST_DUMP_NAME_ARGS(decl->as.extern_fn.name, "?"));
    break;
  case AST_DECL_FN:
    ast_dump_printf(d, " %.*s", AST_DUMP_NAME_ARGS(decl->as.fn_decl.name, "?"));
    break;
  case AST_DECL_MACRO:
    ast_dump_printf(d, " %.*s",
                    AST_DUMP_NAME_ARGS(decl->as.macro_decl.name, "?"));
    break;
  default:
    break;
  }
  ast_dump_write(d, "\n");
  ast_dump_attrs(d, decl->attributes, indent + 1);
  switch (decl->kind) {
  case AST_DECL_TARGET:
    for (uint32_t i = 0; i < decl->as.target_decl.items.count; ++i) {
      const AstTargetItem *it = &decl->as.target_decl.items.items[i];
      ast_dump_indent(d, indent + 1);
      if (it->is_feature)
        ast_dump_printf(d, "feature sign=%d\n", (int)it->feature_sign);
      else {
        ast_dump_printf(d, "target-item %.*s\n",
                        AST_DUMP_NAME_ARGS(it->name, "?"));
        ast_dump_expr(d, it->value, indent + 2);
      }
    }
    break;
  case AST_DECL_TYPE:
    if (decl->as.type_decl.is_struct) {
      ast_dump_attrs(d, decl->as.type_decl.struct_attributes, indent + 1);
      for (uint32_t i = 0; i < decl->as.type_decl.fields.count; ++i) {
        const AstField *f = &decl->as.type_decl.fields.items[i];
        ast_dump_indent(d, indent + 1);
        ast_dump_printf(d, "field %.*s\n", AST_DUMP_NAME_ARGS(f->name, "?"));
        ast_dump_type(d, f->type, indent + 2);
      }
    } else
      ast_dump_type(d, decl->as.type_decl.type, indent + 1);
    break;
  case AST_DECL_CONST:
    if (decl->as.const_decl.type)
      ast_dump_type(d, decl->as.const_decl.type, indent + 1);
    ast_dump_expr(d, decl->as.const_decl.value, indent + 1);
    break;
  case AST_DECL_EXTERN_FN:
    for (uint32_t i = 0; i < decl->as.extern_fn.params.count; ++i) {
      ast_dump_indent(d, indent + 1);
      ast_dump_printf(
          d, "param %.*s\n",
          AST_DUMP_NAME_ARGS(decl->as.extern_fn.params.items[i].name, "?"));
      ast_dump_type(d, decl->as.extern_fn.params.items[i].type, indent + 2);
    }
    if (decl->as.extern_fn.return_type)
      ast_dump_type(d, decl->as.extern_fn.return_type, indent + 1);
    break;
  case AST_DECL_FN:
    for (uint32_t i = 0; i < decl->as.fn_decl.params.count; ++i) {
      ast_dump_indent(d, indent + 1);
      ast_dump_printf(
          d, "param %.*s\n",
          AST_DUMP_NAME_ARGS(decl->as.fn_decl.params.items[i].name, "?"));
      ast_dump_type(d, decl->as.fn_decl.params.items[i].type, indent + 2);
    }
    if (decl->as.fn_decl.return_type)
      ast_dump_type(d, decl->as.fn_decl.return_type, indent + 1);
    if (decl->as.fn_decl.body)
      ast_dump_block(d, decl->as.fn_decl.body, indent + 1);
    else {
      for (uint32_t i = 0; i < decl->as.fn_decl.dispatch_cases.count; ++i) {
        const AstDispatchCase *c = &decl->as.fn_decl.dispatch_cases.items[i];
        ast_dump_indent(d, indent + 1);
        ast_dump_printf(d, "%s -> %.*s\n", c->is_else ? "else" : "when",
                        AST_DUMP_NAME_ARGS(c->target, "?"));
        if (c->condition)
          ast_dump_expr(d, c->condition, indent + 2);
      }
    }
    break;
  case AST_DECL_MACRO:
    ast_dump_indent(d, indent + 1);
    ast_dump_printf(d, "macro-params=%u body-tokens=%zu\n",
                    decl->as.macro_decl.params.count,
                    decl->as.macro_decl.body_tokens.token_count);
    break;
  case AST_DECL_STATIC_ASSERT:
    ast_dump_expr(d, decl->as.static_assert_decl.condition, indent + 1);
    break;
  default:
    break;
  }
}

void ast_dump_module(const AstModule *module, int indent,
                     void (*write_fn)(void *user, const char *text,
                                      size_t length),
                     void *user) {
  if (!write_fn)
    return;
  AstDumpCtx d = {write_fn, user};
  ast_dump_indent(&d, indent);
  if (!module) {
    ast_dump_write(&d, "module <none>\n");
    return;
  }
  ast_dump_printf(&d, "module %.*s declarations=%u\n",
                  AST_DUMP_NAME_ARGS(module->name, "<missing>"),
                  module->declarations.count);
  ast_dump_attrs(&d, module->attributes, indent + 1);
  for (uint32_t i = 0; i < module->declarations.count; ++i)
    ast_dump_decl(&d, module->declarations.items[i], indent + 1);
}
