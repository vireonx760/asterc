#ifndef ASTER_LEXER_H
#define ASTER_LEXER_H

#include "asterasm/common.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASTER_LEXER_VERSION_MAJOR 0
#define ASTER_LEXER_VERSION_MINOR 1
#define ASTER_LEXER_VERSION_PATCH 0

#define ASTER_TOKEN_CHUNK_CAPACITY 4096u
#define ASTER_DIAGNOSTIC_CHUNK_CAPACITY 128u
#define ASTER_DEFAULT_ARENA_BLOCK_SIZE (64u * 1024u)
#define ASTER_MAX_BLOCK_COMMENT_DEPTH 32u

typedef enum AstTokenKind {
  AST_TOK_INVALID = 0,
  AST_TOK_EOF,
  AST_TOK_IDENTIFIER,
  AST_TOK_INTEGER,
  AST_TOK_FLOAT,
  AST_TOK_STRING,
  AST_TOK_BYTE_STRING,
  AST_TOK_C_STRING,
  AST_TOK_CHAR,

  AST_TOK_WHITESPACE,
  AST_TOK_NEWLINE,
  AST_TOK_LINE_COMMENT,
  AST_TOK_BLOCK_COMMENT,

  AST_TOK_LPAREN,
  AST_TOK_RPAREN,
  AST_TOK_LBRACE,
  AST_TOK_RBRACE,
  AST_TOK_LBRACKET,
  AST_TOK_RBRACKET,
  AST_TOK_COMMA,
  AST_TOK_SEMICOLON,
  AST_TOK_COLON,
  AST_TOK_DOT,
  AST_TOK_DOT_DOT,
  AST_TOK_QUESTION,
  AST_TOK_AT,
  AST_TOK_DOLLAR,
  AST_TOK_HASH_HASH,

  AST_TOK_PLUS,
  AST_TOK_MINUS,
  AST_TOK_STAR,
  AST_TOK_SLASH,
  AST_TOK_PERCENT,
  AST_TOK_AMP,
  AST_TOK_PIPE,
  AST_TOK_CARET,
  AST_TOK_TILDE,
  AST_TOK_BANG,
  AST_TOK_ASSIGN,
  AST_TOK_LT,
  AST_TOK_GT,

  AST_TOK_ARROW,
  AST_TOK_FAT_ARROW,
  AST_TOK_EQ_EQ,
  AST_TOK_BANG_EQ,
  AST_TOK_LT_EQ,
  AST_TOK_GT_EQ,
  AST_TOK_SHIFT_LEFT,
  AST_TOK_SHIFT_RIGHT,
  AST_TOK_AMP_AMP,
  AST_TOK_PIPE_PIPE,

  AST_TOK_KW_MODULE,
  AST_TOK_KW_TARGET,
  AST_TOK_KW_FEATURE,
  AST_TOK_KW_IMPORT,
  AST_TOK_KW_AS,
  AST_TOK_KW_EXPORT,
  AST_TOK_KW_TYPE,
  AST_TOK_KW_STRUCT,
  AST_TOK_KW_CONST,
  AST_TOK_KW_STATIC_ASSERT,
  AST_TOK_KW_SECTION,
  AST_TOK_KW_GLOBAL,
  AST_TOK_KW_EXTERN,
  AST_TOK_KW_FN,
  AST_TOK_KW_DISPATCH,
  AST_TOK_KW_WHEN,
  AST_TOK_KW_ELSE,
  AST_TOK_KW_LET,
  AST_TOK_KW_STORE,
  AST_TOK_KW_IF,
  AST_TOK_KW_WHILE,
  AST_TOK_KW_LOOP,
  AST_TOK_KW_RETURN,
  AST_TOK_KW_BREAK,
  AST_TOK_KW_CONTINUE,
  AST_TOK_KW_BLOCK,
  AST_TOK_KW_BR,
  AST_TOK_KW_UNSAFE,
  AST_TOK_KW_CONTROL,
  AST_TOK_KW_COMPTIME,
  AST_TOK_KW_FOR,
  AST_TOK_KW_IN,
  AST_TOK_KW_REQUIRES,
  AST_TOK_KW_BIND,
  AST_TOK_KW_TO,
  AST_TOK_KW_REG,
  AST_TOK_KW_MACRO,
  AST_TOK_KW_NATIVE,
  AST_TOK_KW_INPUTS,
  AST_TOK_KW_OUTPUTS,
  AST_TOK_KW_EXACT,
  AST_TOK_KW_BYTES,
  AST_TOK_KW_LOAD,
  AST_TOK_KW_CAST,
  AST_TOK_KW_TRUNC,
  AST_TOK_KW_ZEXT,
  AST_TOK_KW_SEXT,
  AST_TOK_KW_BITCAST,
  AST_TOK_KW_PTRCAST,
  AST_TOK_KW_ATOMIC,
  AST_TOK_KW_TRUE,
  AST_TOK_KW_FALSE,

  AST_TOK_KW_VOID,
  AST_TOK_KW_BOOL,
  AST_TOK_KW_I8,
  AST_TOK_KW_U8,
  AST_TOK_KW_I16,
  AST_TOK_KW_U16,
  AST_TOK_KW_I32,
  AST_TOK_KW_U32,
  AST_TOK_KW_I64,
  AST_TOK_KW_U64,
  AST_TOK_KW_I128,
  AST_TOK_KW_U128,
  AST_TOK_KW_ISIZE,
  AST_TOK_KW_USIZE,
  AST_TOK_KW_F16,
  AST_TOK_KW_BF16,
  AST_TOK_KW_F32,
  AST_TOK_KW_F64,
  AST_TOK_KW_PTR,
  AST_TOK_KW_CAP,
  AST_TOK_KW_BITS,
  AST_TOK_KW_VEC,
  AST_TOK_KW_MASK,
  AST_TOK_KW_SVEC,
  AST_TOK_KW_PRED,
  AST_TOK_KW_VOLATILE,

  AST_TOK_COUNT
} AstTokenKind;

typedef enum AstTokenFlags {
  AST_TOKENF_NONE = 0,
  AST_TOKENF_UNICODE = 1u << 0,
  AST_TOKENF_ESCAPED = 1u << 1,
  AST_TOKENF_TRIVIA = 1u << 2,
  AST_TOKENF_ERROR = 1u << 3,
  AST_TOKENF_PRECEDED_BY_NEWLINE = 1u << 4
} AstTokenFlags;

typedef enum AstDiagSeverity {
  AST_DIAG_NOTE = 0,
  AST_DIAG_WARNING,
  AST_DIAG_ERROR
} AstDiagSeverity;

typedef enum AstDiagCode {
  AST_DIAG_NONE = 0,
  AST_DIAG_OUT_OF_MEMORY,
  AST_DIAG_BOM_NOT_ALLOWED,
  AST_DIAG_INVALID_UTF8,
  AST_DIAG_UNEXPECTED_CHARACTER,
  AST_DIAG_NUL_IN_SOURCE,
  AST_DIAG_UNTERMINATED_BLOCK_COMMENT,
  AST_DIAG_BLOCK_COMMENT_NESTING,
  AST_DIAG_UNTERMINATED_STRING,
  AST_DIAG_NEWLINE_IN_STRING,
  AST_DIAG_INVALID_ESCAPE,
  AST_DIAG_INVALID_UNICODE_ESCAPE,
  AST_DIAG_UNTERMINATED_CHAR,
  AST_DIAG_EMPTY_CHAR,
  AST_DIAG_MULTI_CHAR,
  AST_DIAG_INVALID_NUMBER,
  AST_DIAG_INVALID_DIGIT_FOR_BASE,
  AST_DIAG_TRAILING_NUMERIC_UNDERSCORE
} AstDiagCode;

typedef enum AstLexConfigFlags {
  AST_LEX_DEFAULT = 0,
  AST_LEX_EMIT_TRIVIA = 1u << 0,
  AST_LEX_ACCEPT_BOM = 1u << 1,
  AST_LEX_ASCII_IDENTIFIERS_ONLY = 1u << 2
} AstLexConfigFlags;

typedef struct AstLexConfig {
  uint32_t flags;
  uint32_t max_block_comment_depth;
  size_t arena_block_size;
} AstLexConfig;

typedef struct AstSourceFile {
  const char *path;
  const uint8_t *data;
  size_t size;
} AstSourceFile;

typedef struct AstToken {
  uint64_t offset;
  uint32_t length;
  uint32_t line;
  uint32_t column;
  uint16_t kind;
  uint16_t flags;
} AstToken;

typedef struct AstDiagnostic {
  uint32_t code;
  uint32_t severity;
  uint64_t offset;
  uint32_t length;
  uint32_t line;
  uint32_t column;
  const char *message;
} AstDiagnostic;

typedef struct AstTokenChunk {
  struct AstTokenChunk *next;
  uint32_t count;
  AstToken tokens[ASTER_TOKEN_CHUNK_CAPACITY];
} AstTokenChunk;

typedef struct AstDiagnosticChunk {
  struct AstDiagnosticChunk *next;
  uint32_t count;
  AstDiagnostic diagnostics[ASTER_DIAGNOSTIC_CHUNK_CAPACITY];
} AstDiagnosticChunk;

typedef struct AstLexedFile {
  AstSourceFile source;
  AstTokenChunk *token_head;
  AstTokenChunk *token_tail;
  AstDiagnosticChunk *diagnostic_head;
  AstDiagnosticChunk *diagnostic_tail;
  size_t token_count;
  size_t diagnostic_count;
  size_t error_count;
  void *internal;
} AstLexedFile;

typedef struct AstLexBatch {
  AstLexedFile *files;
  size_t count;
  void *internal;
} AstLexBatch;

typedef struct AstTokenCursor {
  const AstTokenChunk *chunk;
  uint32_t index;
  size_t absolute_index;
} AstTokenCursor;

ASTER_API AstLexConfig ast_lex_config_default(void);
ASTER_API int ast_lex_source(const AstSourceFile *source,
                             const AstLexConfig *config, AstLexedFile *out);
ASTER_API void ast_lexed_file_destroy(AstLexedFile *file);

ASTER_API int ast_lex_sources_parallel(const AstSourceFile *sources,
                                       size_t count, const AstLexConfig *config,
                                       size_t thread_count, AstLexBatch *out);
ASTER_API int ast_lex_paths_parallel(const char *const *paths, size_t count,
                                     const AstLexConfig *config,
                                     size_t thread_count, AstLexBatch *out);
ASTER_API void ast_lex_batch_destroy(AstLexBatch *batch);

ASTER_API AstTokenCursor ast_token_cursor_begin(const AstLexedFile *file);
ASTER_API const AstToken *ast_token_cursor_get(const AstTokenCursor *cursor);
ASTER_API int ast_token_cursor_next(AstTokenCursor *cursor);
ASTER_API const AstToken *ast_token_at(const AstLexedFile *file, size_t index);

ASTER_API const char *ast_token_kind_name(AstTokenKind kind);
ASTER_API const char *ast_diag_code_name(AstDiagCode code);
ASTER_API const char *ast_unicode_version(void);

ASTER_API int ast_utf8_decode_one(const uint8_t *data, size_t size,
                                  uint32_t *codepoint, uint32_t *width);
ASTER_API int ast_unicode_is_xid_start(uint32_t codepoint);
ASTER_API int ast_unicode_is_xid_continue(uint32_t codepoint);

#ifdef __cplusplus
}
#endif

#endif
