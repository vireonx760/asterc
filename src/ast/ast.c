#include "asterasm/ast.h"

#include <stdlib.h>
#include <string.h>

const char *ast_type_kind_name(AstTypeKind kind) {
  static const char *names[] = {"error", "primitive", "named", "ptr",
                                "cap",   "bits",      "vec",   "mask",
                                "svec",  "pred",      "array", "tuple"};
  return (unsigned)kind < ASTER_ARRAY_COUNT(names) ? names[kind] : "unknown";
}

const char *ast_expr_kind_name(AstExprKind kind) {
  static const char *names[] = {
      "error",       "name",     "type_operand", "integer", "float", "string",
      "byte_string", "c_string", "char",         "bool",    "unary", "binary",
      "conditional", "call",     "index",        "member",  "load",  "cast",
      "atomic",      "attr",     "macro_ref"};
  return (unsigned)kind < ASTER_ARRAY_COUNT(names) ? names[kind] : "unknown";
}

const char *ast_stmt_kind_name(AstStmtKind kind) {
  static const char *names[] = {
      "error",    "block",       "let",    "const",  "assign",       "expr",
      "store",    "if",          "while",  "loop",   "return",       "break",
      "continue", "block_label", "branch", "unsafe", "comptime",     "for",
      "requires", "bind",        "native", "exact",  "static_assert"};
  return (unsigned)kind < ASTER_ARRAY_COUNT(names) ? names[kind] : "unknown";
}

const char *ast_decl_kind_name(AstDeclKind kind) {
  static const char *names[] = {"error", "target",       "import",    "type",
                                "const", "section",      "extern_fn", "fn",
                                "macro", "static_assert"};
  return (unsigned)kind < ASTER_ARRAY_COUNT(names) ? names[kind] : "unknown";
}

int ast_name_equal(AstName a, AstName b) {
  if (!a.text || !b.text || a.span.length != b.span.length)
    return 0;
  if (a.hash != 0u && b.hash != 0u && a.hash != b.hash)
    return 0;
  return memcmp(a.text, b.text, a.span.length) == 0;
}

int ast_name_equal_cstr(AstName name, const char *text) {
  if (!name.text || !text)
    return 0;
  size_t length = strlen(text);
  return length == name.span.length && memcmp(name.text, text, length) == 0;
}

char *ast_name_dup_cstr(const AstName *name) {
  if (!name || !name->text)
    return NULL;
  size_t length = name->span.length;
  char *copy = (char *)malloc(length + 1u);
  if (!copy)
    return NULL;
  memcpy(copy, name->text, length);
  copy[length] = '\0';
  return copy;
}
