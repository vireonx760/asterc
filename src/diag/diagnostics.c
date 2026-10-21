#include "asterasm/diagnostics.h"

#include <string.h>

static const char *ast_severity_name(AstDiagSeverity severity) {
  switch (severity) {
  case AST_DIAG_NOTE:
    return "note";
  case AST_DIAG_WARNING:
    return "warning";
  case AST_DIAG_ERROR:
    return "error";
  default:
    return "diagnostic";
  }
}

static size_t ast_source_newline(const AstSourceFile *source, size_t pos) {
  const uint8_t *s = source->data;
  size_t remaining = source->size - pos;
  if (s[pos] == '\n')
    return 1u;
  if (s[pos] == '\r')
    return remaining > 1u && s[pos + 1u] == '\n' ? 2u : 1u;
  if (remaining >= 2u && s[pos] == 0xc2u && s[pos + 1u] == 0x85u)
    return 2u;
  if (remaining >= 3u && s[pos] == 0xe2u && s[pos + 1u] == 0x80u &&
      (s[pos + 2u] == 0xa8u || s[pos + 2u] == 0xa9u))
    return 3u;
  return 0u;
}
static size_t ast_source_character(const AstSourceFile *source, size_t pos,
                                   size_t end) {
  uint32_t codepoint, width;
  return source->data[pos] >= 0x80u &&
                 ast_utf8_decode_one(source->data + pos, end - pos, &codepoint,
                                     &width)
             ? width
             : 1u;
}
static void ast_source_line_column(const AstSourceFile *source, uint64_t offset,
                                   uint32_t *line_out, uint32_t *column_out) {
  uint32_t line = 1u;
  uint32_t column = 1u;
  if (source && source->data) {
    size_t limit = offset > source->size ? source->size : (size_t)offset;
    for (size_t i = 0u; i < limit;) {
      size_t newline = ast_source_newline(source, i);
      if (newline && newline <= limit - i) {
        i += newline;
        line++;
        column = 1u;
      } else {
        i += ast_source_character(source, i, limit);
        column++;
      }
    }
  }
  if (line_out)
    *line_out = line;
  if (column_out)
    *column_out = column;
}

static void ast_print_line(FILE *out, const AstSourceFile *source,
                           uint64_t offset, uint32_t column) {
  if (!out || !source || !source->data || offset > source->size)
    return;
  size_t off = (size_t)offset;
  size_t begin = 0u;
  for (size_t i = 0u; i < off;) {
    size_t newline = ast_source_newline(source, i);
    if (newline && newline <= off - i) {
      i += newline;
      begin = i;
    } else
      i += ast_source_character(source, i, off);
  }
  size_t end = off;
  while (end < source->size && !ast_source_newline(source, end))
    end++;
  if (end > begin)
    fprintf(out, "  %.*s\n", (int)(end - begin),
            (const char *)(source->data + begin));
  if (column) {
    fputs("  ", out);
    for (size_t i = begin; i < off;) {
      fputc(source->data[i] == '\t' ? '\t' : ' ', out);
      i += ast_source_character(source, i, off);
    }
    fputs("^\n", out);
  }
}

AstDiagnosticStyle ast_diagnostic_style_default(void) {
  AstDiagnosticStyle s = {0, 1, 1};
  return s;
}

void ast_print_lexer_diagnostics(FILE *out, const AstLexedFile *lexed,
                                 const AstDiagnosticStyle *style) {
  if (!out || !lexed)
    return;
  AstDiagnosticStyle st = style ? *style : ast_diagnostic_style_default();
  for (const AstDiagnosticChunk *chunk = lexed->diagnostic_head; chunk;
       chunk = chunk->next) {
    for (uint32_t i = 0; i < chunk->count; ++i) {
      const AstDiagnostic *d = &chunk->diagnostics[i];
      fprintf(out, "%s:%u:%u: %s",
              lexed->source.path ? lexed->source.path : "<source>", d->line,
              d->column, ast_severity_name((AstDiagSeverity)d->severity));
      if (st.show_codes)
        fprintf(out, "[lex:%s]", ast_diag_code_name((AstDiagCode)d->code));
      fprintf(out, ": %s\n", d->message ? d->message : "lexer error");
      if (st.show_source)
        ast_print_line(out, &lexed->source, d->offset, d->column);
    }
  }
}

void ast_print_parser_diagnostics(FILE *out, const AstSourceFile *source,
                                  const AstParseResult *parsed,
                                  const AstDiagnosticStyle *style) {
  if (!out || !parsed)
    return;
  const AstLexedFile *expanded = ast_parse_expanded_lexed(parsed);
  if (expanded)
    source = &expanded->source;
  AstDiagnosticStyle st = style ? *style : ast_diagnostic_style_default();
  for (size_t i = 0; i < parsed->diagnostic_count; ++i) {
    const AstParseDiagnostic *d = &parsed->diagnostics[i];
    uint32_t line = 1u, column = 1u;
    ast_source_line_column(source, d->span.offset, &line, &column);
    fprintf(out, "%s:%u:%u: %s",
            source && source->path ? source->path : "<source>", line, column,
            ast_severity_name(d->severity));
    if (st.show_codes)
      fprintf(out, "[parse:%s]", ast_parse_diag_code_name(d->code));
    fprintf(out, ": %s\n", d->message ? d->message : "parser error");
    if (st.show_source && source)
      ast_print_line(out, source, d->span.offset, column);
  }
}

void ast_print_frontend_diagnostics(FILE *out, const AstFrontend *frontend,
                                    const AstDiagnosticStyle *style) {
  if (!out || !frontend)
    return;
  AstDiagnosticStyle st = style ? *style : ast_diagnostic_style_default();
  for (size_t i = 0; i < ast_frontend_file_count(frontend); ++i) {
    AstFrontendFile file;
    if (!ast_frontend_get_file(frontend, i, &file))
      continue;
    if (file.lexed)
      ast_print_lexer_diagnostics(out, file.lexed, &st);
    if (file.parsed)
      ast_print_parser_diagnostics(out, file.source, file.parsed, &st);
  }
  size_t count = 0u;
  const AstFrontendDiagnostic *diags =
      ast_frontend_diagnostics(frontend, &count);
  for (size_t i = 0; i < count; ++i) {
    const AstFrontendDiagnostic *d = &diags[i];
    uint32_t line = 1u, column = 1u;
    AstFrontendFile diag_file;
    memset(&diag_file, 0, sizeof(diag_file));
    if (d->span.file_id &&
        ast_frontend_get_file(frontend, d->span.file_id - 1u, &diag_file) &&
        diag_file.source)
      ast_source_line_column(diag_file.source, d->span.offset, &line, &column);
    fprintf(out, "%s:%u:%u: %s", d->path ? d->path : "<frontend>", line, column,
            ast_severity_name(d->severity));
    if (st.show_codes)
      fprintf(out, "[frontend:%s]", ast_frontend_diag_code_name(d->code));
    fprintf(out, ": %s\n", d->message ? d->message : "frontend error");
    if (st.show_source && d->span.file_id) {
      AstFrontendFile file;
      if (ast_frontend_get_file(frontend, d->span.file_id - 1u, &file) &&
          file.source)
        ast_print_line(out, file.source, d->span.offset, column);
    }
  }
}
