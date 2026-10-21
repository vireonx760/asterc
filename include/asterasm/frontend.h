#ifndef ASTERASM_FRONTEND_H
#define ASTERASM_FRONTEND_H

#include "asterasm/parser.h"
#include "asterasm/source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstFrontend AstFrontend;

typedef enum AstFrontendFlags {
  AST_FRONTEND_DEFAULT = 0,
  AST_FRONTEND_RESOLVE_IMPORTS = 1u << 0,
  AST_FRONTEND_DETECT_IMPORT_CYCLES = 1u << 1
} AstFrontendFlags;

typedef enum AstFrontendDiagCode {
  AST_FRONTEND_DIAG_NONE = 2000,
  AST_FRONTEND_DIAG_IO,
  AST_FRONTEND_DIAG_IMPORT_NOT_FOUND,
  AST_FRONTEND_DIAG_DUPLICATE_MODULE,
  AST_FRONTEND_DIAG_MODULE_PATH_MISMATCH,
  AST_FRONTEND_DIAG_IMPORT_CYCLE,
  AST_FRONTEND_DIAG_INTERNAL
} AstFrontendDiagCode;

typedef struct AstFrontendConfig {
  AstLexConfig lex;
  AstParseConfig parse;
  size_t thread_count;
  uint32_t flags;
  const char *module_extension;
} AstFrontendConfig;

typedef struct AstFrontendDiagnostic {
  AstFrontendDiagCode code;
  AstDiagSeverity severity;
  AstSpan span;
  const char *path;
  const char *message;
} AstFrontendDiagnostic;

typedef struct AstFrontendFile {
  uint32_t id;
  const char *path;
  /* Effective parser source (macro-expanded when applicable); owns AST spans.
   */
  const AstSourceFile *source;
  const AstLexedFile *lexed;
  const AstParseResult *parsed;
} AstFrontendFile;

typedef struct AstImportEdge {
  uint32_t from_file_id;
  uint32_t to_file_id;
  AstSpan import_span;
} AstImportEdge;

typedef struct AstFrontendStats {
  size_t file_count;
  size_t module_count;
  size_t import_edge_count;
  size_t source_bytes;
  size_t token_count;
  size_t ast_nodes;
  size_t lex_error_count;
  size_t parse_error_count;
  size_t frontend_error_count;
} AstFrontendStats;

ASTER_API AstFrontendConfig ast_frontend_config_default(void);
ASTER_API AstFrontend *ast_frontend_create(const AstFrontendConfig *config);
ASTER_API void ast_frontend_destroy(AstFrontend *frontend);
ASTER_API int ast_frontend_add_import_root(AstFrontend *frontend,
                                           const char *path);
ASTER_API int ast_frontend_load(AstFrontend *frontend,
                                const char *const *entry_paths,
                                size_t entry_count);
ASTER_API size_t ast_frontend_file_count(const AstFrontend *frontend);
ASTER_API int ast_frontend_get_file(const AstFrontend *frontend, size_t index,
                                    AstFrontendFile *out);
ASTER_API int ast_frontend_find_module(const AstFrontend *frontend,
                                       const char *module_name,
                                       AstFrontendFile *out);
ASTER_API const AstImportEdge *
ast_frontend_import_edges(const AstFrontend *frontend, size_t *count_out);
ASTER_API const AstFrontendDiagnostic *
ast_frontend_diagnostics(const AstFrontend *frontend, size_t *count_out);
ASTER_API AstFrontendStats ast_frontend_stats(const AstFrontend *frontend);
ASTER_API const char *ast_frontend_diag_code_name(AstFrontendDiagCode code);

#ifdef __cplusplus
}
#endif

#endif
