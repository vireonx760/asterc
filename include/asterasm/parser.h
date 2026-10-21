#ifndef ASTERASM_PARSER_H
#define ASTERASM_PARSER_H

#include "asterasm/ast.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASTER_DEFAULT_PARSE_RECURSION_LIMIT 256u
#define ASTER_DEFAULT_MAX_PARSE_DIAGNOSTICS 256u

typedef enum AstParseDiagCode {
  AST_PARSE_DIAG_NONE = 0,
  AST_PARSE_DIAG_EXPECTED_TOKEN = 1000,
  AST_PARSE_DIAG_EXPECTED_IDENTIFIER,
  AST_PARSE_DIAG_EXPECTED_EXPRESSION,
  AST_PARSE_DIAG_EXPECTED_TYPE,
  AST_PARSE_DIAG_EXPECTED_DECLARATION,
  AST_PARSE_DIAG_EXPECTED_STATEMENT,
  AST_PARSE_DIAG_INVALID_TOP_LEVEL,
  AST_PARSE_DIAG_DUPLICATE_MODULE_DECL,
  AST_PARSE_DIAG_MISSING_MODULE_DECL,
  AST_PARSE_DIAG_INVALID_ASSIGNMENT_TARGET,
  AST_PARSE_DIAG_INVALID_NATIVE_OPERAND,
  AST_PARSE_DIAG_INVALID_MACRO_PARAMETER,
  AST_PARSE_DIAG_RECURSION_LIMIT,
  AST_PARSE_DIAG_TOO_MANY_DIAGNOSTICS,
  AST_PARSE_DIAG_OUT_OF_MEMORY
} AstParseDiagCode;

typedef struct AstParseDiagnostic {
  AstParseDiagCode code;
  AstDiagSeverity severity;
  AstSpan span;
  const char *message;
} AstParseDiagnostic;

typedef struct AstParseConfig {
  uint32_t recursion_limit;
  uint32_t max_diagnostics;
  size_t arena_block_size;
  uint8_t allow_missing_module;
} AstParseConfig;

typedef struct AstParseStats {
  size_t tokens_consumed;
  size_t ast_nodes;
  size_t expr_nodes;
  size_t type_nodes;
  size_t arena_bytes_used;
  size_t arena_bytes_reserved;
  size_t error_count;
} AstParseStats;

typedef struct AstParseResult {
  AstArena arena;
  AstModule *module;
  AstParseDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t error_count;
  AstParseStats stats;
  void *internal;
} AstParseResult;

typedef struct AstParseBatch {
  AstParseResult *files;
  size_t count;
} AstParseBatch;

ASTER_API AstParseConfig ast_parse_config_default(void);
ASTER_API int ast_parse_file(const AstLexedFile *lexed, uint32_t file_id,
                             const AstParseConfig *config, AstParseResult *out);
/* Expanded source/tokens used by AST spans, or NULL when no expansion took
   place. Borrowed until ast_parse_result_destroy; never destroy this view. */
ASTER_API const AstLexedFile *
ast_parse_expanded_lexed(const AstParseResult *result);
ASTER_API void ast_parse_result_destroy(AstParseResult *result);
ASTER_API int ast_parse_files_parallel(const AstLexedFile *lexed_files,
                                       size_t count, uint32_t first_file_id,
                                       const AstParseConfig *config,
                                       size_t thread_count, AstParseBatch *out);
ASTER_API void ast_parse_batch_destroy(AstParseBatch *batch);
ASTER_API const char *ast_parse_diag_code_name(AstParseDiagCode code);
ASTER_API void ast_dump_module(const AstModule *module, int indent,
                               void (*write_fn)(void *user, const char *text,
                                                size_t length),
                               void *user);

#ifdef __cplusplus
}
#endif

#endif
