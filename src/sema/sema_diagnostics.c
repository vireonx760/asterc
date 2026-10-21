#include "asterasm/diagnostics.h"
#include "asterasm/sema.h"

static const char *sema_severity_name(AstDiagSeverity severity) {
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

void ast_print_sema_diagnostics(FILE *out, const AstSema *sema,
                                const AstDiagnosticStyle *style) {
  if (!out || !sema)
    return;
  AstDiagnosticStyle st = style ? *style : ast_diagnostic_style_default();
  size_t count = 0u;
  const AstSemaDiagnostic *diags = ast_sema_diagnostics(sema, &count);
  for (size_t i = 0u; i < count; ++i) {
    const AstSemaDiagnostic *d = &diags[i];
    fprintf(out, "%s:%u:%u: %s", d->path ? d->path : "<sema>", d->line,
            d->column, sema_severity_name(d->severity));
    if (st.show_codes)
      fprintf(out, "[sema:%s]", ast_sema_diag_code_name(d->code));
    fprintf(out, ": %s\n", d->message ? d->message : "semantic error");
  }
}
