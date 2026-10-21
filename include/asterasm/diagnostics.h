#ifndef ASTERASM_DIAGNOSTICS_H
#define ASTERASM_DIAGNOSTICS_H

#include "asterasm/frontend.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstSema AstSema;

typedef struct AstDiagnosticStyle {
  int color;
  int show_source;
  int show_codes;
} AstDiagnosticStyle;

ASTER_API AstDiagnosticStyle ast_diagnostic_style_default(void);
ASTER_API void ast_print_lexer_diagnostics(FILE *out, const AstLexedFile *lexed,
                                           const AstDiagnosticStyle *style);
ASTER_API void ast_print_parser_diagnostics(FILE *out,
                                            const AstSourceFile *source,
                                            const AstParseResult *parsed,
                                            const AstDiagnosticStyle *style);
ASTER_API void ast_print_frontend_diagnostics(FILE *out,
                                              const AstFrontend *frontend,
                                              const AstDiagnosticStyle *style);
ASTER_API void ast_print_sema_diagnostics(FILE *out, const AstSema *sema,
                                          const AstDiagnosticStyle *style);

#ifdef __cplusplus
}
#endif

#endif
