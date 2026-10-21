#include "asterasm/lexer.h"
#include "unicode_xid.h"

#include <limits.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define ASTER_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define ASTER_FORCE_INLINE __attribute__((always_inline)) inline
#else
#define ASTER_FORCE_INLINE inline
#endif

typedef struct AstArenaBlock {
  struct AstArenaBlock *next;
  size_t capacity;
  size_t used;
  AstMaxAlign alignment_guard;
  unsigned char data[];
} AstArenaBlock;

typedef struct AstArena {
  AstArenaBlock *head;
  AstArenaBlock *current;
  size_t block_size;
} AstArena;

typedef struct AstLexStorage {
  AstArena arena;
  int oom;
} AstLexStorage;

typedef struct AstLexer {
  const uint8_t *data;
  size_t size;
  size_t pos;
  uint32_t line;
  uint32_t column;
  uint32_t saw_newline;
  AstLexConfig config;
  AstLexedFile *out;
  AstLexStorage *storage;
} AstLexer;

static size_t ast_align_up(size_t value, size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

static void ast_arena_init(AstArena *arena, size_t block_size) {
  memset(arena, 0, sizeof(*arena));
  arena->block_size = block_size ? block_size : ASTER_DEFAULT_ARENA_BLOCK_SIZE;
}

static void ast_arena_destroy(AstArena *arena) {
  AstArenaBlock *block = arena->head;
  while (block) {
    AstArenaBlock *next = block->next;
    free(block);
    block = next;
  }
  memset(arena, 0, sizeof(*arena));
}

static void *ast_arena_alloc(AstLexStorage *storage, size_t size,
                             size_t alignment) {
  AstArena *arena = &storage->arena;
  if (alignment < _Alignof(AstMaxAlign))
    alignment = _Alignof(AstMaxAlign);
  if ((alignment & (alignment - 1u)) != 0u) {
    storage->oom = 1;
    return NULL;
  }

  AstArenaBlock *block = arena->current;
  if (block) {
    size_t start = ast_align_up(block->used, alignment);
    if (start <= block->capacity && size <= block->capacity - start) {
      void *result = block->data + start;
      block->used = start + size;
      return result;
    }
  }

  size_t capacity = arena->block_size;
  size_t needed = size + alignment;
  if (capacity < needed)
    capacity = ast_align_up(needed, 4096u);
  if (capacity > SIZE_MAX - sizeof(AstArenaBlock)) {
    storage->oom = 1;
    return NULL;
  }

  AstArenaBlock *new_block =
      (AstArenaBlock *)malloc(sizeof(AstArenaBlock) + capacity);
  if (!new_block) {
    storage->oom = 1;
    return NULL;
  }
  new_block->next = NULL;
  new_block->capacity = capacity;
  new_block->used = 0;

  if (!arena->head)
    arena->head = new_block;
  if (arena->current)
    arena->current->next = new_block;
  arena->current = new_block;

  size_t start = ast_align_up(new_block->used, alignment);
  void *result = new_block->data + start;
  new_block->used = start + size;
  return result;
}

static ASTER_FORCE_INLINE int ast_is_ascii_alpha(uint8_t c) {
  uint8_t x = (uint8_t)(c | 0x20u);
  return x >= (uint8_t)'a' && x <= (uint8_t)'z';
}

static ASTER_FORCE_INLINE int ast_is_ascii_digit(uint8_t c) {
  return c >= (uint8_t)'0' && c <= (uint8_t)'9';
}

static ASTER_FORCE_INLINE int ast_is_ascii_hex(uint8_t c) {
  return ast_is_ascii_digit(c) || ((uint8_t)(c | 0x20u) >= (uint8_t)'a' &&
                                   (uint8_t)(c | 0x20u) <= (uint8_t)'f');
}

static ASTER_FORCE_INLINE int ast_is_ascii_ident_start(uint8_t c) {
  return ast_is_ascii_alpha(c) || c == (uint8_t)'_';
}

static ASTER_FORCE_INLINE int ast_is_ascii_ident_continue(uint8_t c) {
  return ast_is_ascii_ident_start(c) || ast_is_ascii_digit(c);
}

int ast_utf8_decode_one(const uint8_t *data, size_t size, uint32_t *codepoint,
                        uint32_t *width) {
  if (!data || size == 0) {
    if (width)
      *width = 0;
    return 0;
  }

  uint8_t b0 = data[0];
  if (b0 < 0x80u) {
    if (codepoint)
      *codepoint = b0;
    if (width)
      *width = 1;
    return 1;
  }

  if (b0 >= 0xC2u && b0 <= 0xDFu) {
    if (size < 2 || (data[1] & 0xC0u) != 0x80u)
      goto invalid;
    if (codepoint)
      *codepoint = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(data[1] & 0x3Fu);
    if (width)
      *width = 2;
    return 1;
  }

  if (b0 >= 0xE0u && b0 <= 0xEFu) {
    if (size < 3)
      goto invalid;
    uint8_t b1 = data[1], b2 = data[2];
    if ((b2 & 0xC0u) != 0x80u)
      goto invalid;
    if (b0 == 0xE0u) {
      if (b1 < 0xA0u || b1 > 0xBFu)
        goto invalid;
    } else if (b0 == 0xEDu) {
      if (b1 < 0x80u || b1 > 0x9Fu)
        goto invalid;
    } else if ((b1 & 0xC0u) != 0x80u) {
      goto invalid;
    }
    if (codepoint) {
      *codepoint = ((uint32_t)(b0 & 0x0Fu) << 12) |
                   ((uint32_t)(b1 & 0x3Fu) << 6) | (uint32_t)(b2 & 0x3Fu);
    }
    if (width)
      *width = 3;
    return 1;
  }

  if (b0 >= 0xF0u && b0 <= 0xF4u) {
    if (size < 4)
      goto invalid;
    uint8_t b1 = data[1], b2 = data[2], b3 = data[3];
    if ((b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u)
      goto invalid;
    if (b0 == 0xF0u) {
      if (b1 < 0x90u || b1 > 0xBFu)
        goto invalid;
    } else if (b0 == 0xF4u) {
      if (b1 < 0x80u || b1 > 0x8Fu)
        goto invalid;
    } else if ((b1 & 0xC0u) != 0x80u) {
      goto invalid;
    }
    if (codepoint) {
      *codepoint = ((uint32_t)(b0 & 0x07u) << 18) |
                   ((uint32_t)(b1 & 0x3Fu) << 12) |
                   ((uint32_t)(b2 & 0x3Fu) << 6) | (uint32_t)(b3 & 0x3Fu);
    }
    if (width)
      *width = 4;
    return 1;
  }

invalid:
  if (codepoint)
    *codepoint = 0xFFFDu;
  if (width)
    *width = 1;
  return 0;
}

int ast_unicode_is_xid_start(uint32_t codepoint) {
  return aster_unicode_xid_start(codepoint);
}

int ast_unicode_is_xid_continue(uint32_t codepoint) {
  return aster_unicode_xid_continue(codepoint);
}

const char *ast_unicode_version(void) { return ASTER_UNICODE_VERSION; }

static AstTokenChunk *ast_new_token_chunk(AstLexer *lexer) {
  AstTokenChunk *chunk = (AstTokenChunk *)ast_arena_alloc(
      lexer->storage, sizeof(AstTokenChunk), _Alignof(AstTokenChunk));
  if (!chunk)
    return NULL;
  chunk->next = NULL;
  chunk->count = 0;
  if (!lexer->out->token_head)
    lexer->out->token_head = chunk;
  if (lexer->out->token_tail)
    lexer->out->token_tail->next = chunk;
  lexer->out->token_tail = chunk;
  return chunk;
}

static AstDiagnosticChunk *ast_new_diag_chunk(AstLexer *lexer) {
  AstDiagnosticChunk *chunk = (AstDiagnosticChunk *)ast_arena_alloc(
      lexer->storage, sizeof(AstDiagnosticChunk), _Alignof(AstDiagnosticChunk));
  if (!chunk)
    return NULL;
  chunk->next = NULL;
  chunk->count = 0;
  if (!lexer->out->diagnostic_head)
    lexer->out->diagnostic_head = chunk;
  if (lexer->out->diagnostic_tail)
    lexer->out->diagnostic_tail->next = chunk;
  lexer->out->diagnostic_tail = chunk;
  return chunk;
}

static void ast_push_diag(AstLexer *lexer, AstDiagCode code, uint64_t offset,
                          uint32_t length, uint32_t line, uint32_t column,
                          const char *message) {
  AstDiagnosticChunk *chunk = lexer->out->diagnostic_tail;
  if (!chunk || chunk->count == ASTER_DIAGNOSTIC_CHUNK_CAPACITY) {
    chunk = ast_new_diag_chunk(lexer);
    if (!chunk)
      return;
  }
  AstDiagnostic *diag = &chunk->diagnostics[chunk->count++];
  diag->code = (uint32_t)code;
  diag->severity = AST_DIAG_ERROR;
  diag->offset = offset;
  diag->length = length;
  diag->line = line;
  diag->column = column;
  diag->message = message;
  lexer->out->diagnostic_count++;
  lexer->out->error_count++;
}

static void ast_emit_token(AstLexer *lexer, AstTokenKind kind, uint32_t flags,
                           size_t start, uint32_t start_line,
                           uint32_t start_column) {
  AstTokenChunk *chunk = lexer->out->token_tail;
  if (!chunk || chunk->count == ASTER_TOKEN_CHUNK_CAPACITY) {
    chunk = ast_new_token_chunk(lexer);
    if (!chunk)
      return;
  }

  if (!(flags & AST_TOKENF_TRIVIA) && lexer->saw_newline)
    flags |= AST_TOKENF_PRECEDED_BY_NEWLINE;
  AstToken *token = &chunk->tokens[chunk->count++];
  token->kind = (uint16_t)kind;
  token->flags = (uint16_t)flags;
  token->offset = (uint64_t)start;
  size_t len = lexer->pos - start;
  token->length = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
  token->line = start_line;
  token->column = start_column;
  lexer->out->token_count++;
  if (!(flags & AST_TOKENF_TRIVIA))
    lexer->saw_newline = 0;
}

static ASTER_FORCE_INLINE void ast_advance_ascii(AstLexer *lexer,
                                                 size_t count) {
  lexer->pos += count;
  lexer->column += (uint32_t)count;
}

static ASTER_FORCE_INLINE int ast_newline_width(const AstLexer *lexer,
                                                size_t pos) {
  if (pos >= lexer->size)
    return 0;
  uint8_t c = lexer->data[pos];
  if (c == '\n')
    return 1;
  if (c == '\r')
    return (pos + 1 < lexer->size && lexer->data[pos + 1] == '\n') ? 2 : 1;
  if (c == 0xC2u && pos + 1 < lexer->size && lexer->data[pos + 1] == 0x85u)
    return 2;
  if (c == 0xE2u && pos + 2 < lexer->size && lexer->data[pos + 1] == 0x80u &&
      (lexer->data[pos + 2] == 0xA8u || lexer->data[pos + 2] == 0xA9u))
    return 3;
  return 0;
}

static ASTER_FORCE_INLINE void ast_consume_newline(AstLexer *lexer, int width) {
  lexer->pos += (size_t)width;
  lexer->line++;
  lexer->column = 1;
  lexer->saw_newline = 1;
}

static ASTER_FORCE_INLINE int ast_is_unicode_inline_whitespace(uint32_t cp) {
  return cp == 0x200Eu || cp == 0x200Fu;
}

static uint32_t ast_fnv1a32(const uint8_t *text, size_t length) {
  uint32_t hash = UINT32_C(2166136261);
  for (size_t i = 0; i < length; ++i)
    hash = (hash ^ text[i]) * UINT32_C(16777619);
  return hash;
}

static AstTokenKind ast_keyword_kind(const uint8_t *text, size_t length) {
  if (length < 2u || length > 13u)
    return AST_TOK_IDENTIFIER;
  uint32_t hash = ast_fnv1a32(text, length);
  switch (hash) {
#include "keywords.inc"
  default:
    break;
  }
  return AST_TOK_IDENTIFIER;
}

static int ast_digit_for_base(uint8_t c, unsigned base) {
  if (base == 2u)
    return c == '0' || c == '1';
  if (base == 8u)
    return c >= '0' && c <= '7';
  if (base == 10u)
    return c >= '0' && c <= '9';
  return ast_is_ascii_hex(c);
}

static int ast_scan_digits(AstLexer *lexer, unsigned base, int *had_digit,
                           int *bad_underscore) {
  int any = 0;
  int prev_underscore = 0;
  while (lexer->pos < lexer->size) {
    uint8_t c = lexer->data[lexer->pos];
    if (ast_digit_for_base(c, base)) {
      any = 1;
      prev_underscore = 0;
      ast_advance_ascii(lexer, 1);
      continue;
    }
    if (c == '_') {
      if (!any || prev_underscore)
        *bad_underscore = 1;
      prev_underscore = 1;
      ast_advance_ascii(lexer, 1);
      continue;
    }
    break;
  }
  if (prev_underscore)
    *bad_underscore = 1;
  *had_digit = any;
  return any;
}

static int ast_starts_identifier_continue(AstLexer *lexer) {
  if (lexer->pos >= lexer->size)
    return 0;
  uint8_t c = lexer->data[lexer->pos];
  if (c < 0x80u)
    return ast_is_ascii_ident_continue(c);
  if (lexer->config.flags & AST_LEX_ASCII_IDENTIFIERS_ONLY)
    return 0;
  uint32_t cp, width;
  if (!ast_utf8_decode_one(lexer->data + lexer->pos, lexer->size - lexer->pos,
                           &cp, &width))
    return 0;
  return ast_unicode_is_xid_continue(cp);
}

static void ast_lex_number(AstLexer *lexer) {
  size_t start = lexer->pos;
  uint32_t line = lexer->line, column = lexer->column;
  unsigned base = 10u;
  int is_float = 0;
  int bad_underscore = 0;
  int had_digit = 0;

  if (lexer->data[lexer->pos] == '0' && lexer->pos + 1 < lexer->size) {
    uint8_t p = lexer->data[lexer->pos + 1];
    if (p == 'x' || p == 'X')
      base = 16u;
    else if (p == 'b' || p == 'B')
      base = 2u;
    else if (p == 'o' || p == 'O')
      base = 8u;
    if (base != 10u) {
      ast_advance_ascii(lexer, 2);
      ast_scan_digits(lexer, base, &had_digit, &bad_underscore);
      if (!had_digit) {
        ast_push_diag(
            lexer, AST_DIAG_INVALID_NUMBER, start,
            (uint32_t)(lexer->pos - start), line, column,
            "numeric base prefix must be followed by at least one digit");
      }
      if (bad_underscore) {
        ast_push_diag(lexer, AST_DIAG_TRAILING_NUMERIC_UNDERSCORE, start,
                      (uint32_t)(lexer->pos - start), line, column,
                      "numeric separators must appear only between digits");
      }
      if (lexer->pos < lexer->size &&
          ast_is_ascii_digit(lexer->data[lexer->pos]) &&
          !ast_digit_for_base(lexer->data[lexer->pos], base)) {
        while (lexer->pos < lexer->size &&
               ast_is_ascii_ident_continue(lexer->data[lexer->pos]))
          ast_advance_ascii(lexer, 1);
        ast_push_diag(
            lexer, AST_DIAG_INVALID_DIGIT_FOR_BASE, start,
            (uint32_t)(lexer->pos - start), line, column,
            "numeric literal contains a digit that is invalid for its base");
      } else if (ast_starts_identifier_continue(lexer)) {
        while (lexer->pos < lexer->size) {
          uint8_t c = lexer->data[lexer->pos];
          if (c < 0x80u) {
            if (!ast_is_ascii_ident_continue(c))
              break;
            ast_advance_ascii(lexer, 1);
          } else {
            uint32_t cp, width;
            if (!ast_utf8_decode_one(lexer->data + lexer->pos,
                                     lexer->size - lexer->pos, &cp, &width) ||
                !ast_unicode_is_xid_continue(cp))
              break;
            lexer->pos += width;
            lexer->column++;
          }
        }
        ast_push_diag(
            lexer, AST_DIAG_INVALID_NUMBER, start,
            (uint32_t)(lexer->pos - start), line, column,
            "numeric literals cannot have identifier suffixes in Core v0.1");
      }
      ast_emit_token(lexer, AST_TOK_INTEGER, 0, start, line, column);
      return;
    }
  }

  ast_scan_digits(lexer, 10u, &had_digit, &bad_underscore);

  if (lexer->pos < lexer->size && lexer->data[lexer->pos] == '.' &&
      !(lexer->pos + 1 < lexer->size && lexer->data[lexer->pos + 1] == '.')) {
    is_float = 1;
    ast_advance_ascii(lexer, 1);
    if (lexer->pos < lexer->size &&
        ast_is_ascii_digit(lexer->data[lexer->pos])) {
      int frac_digit = 0;
      ast_scan_digits(lexer, 10u, &frac_digit, &bad_underscore);
    }
  }

  if (lexer->pos < lexer->size &&
      (lexer->data[lexer->pos] == 'e' || lexer->data[lexer->pos] == 'E')) {
    is_float = 1;
    ast_advance_ascii(lexer, 1);
    if (lexer->pos < lexer->size &&
        (lexer->data[lexer->pos] == '+' || lexer->data[lexer->pos] == '-'))
      ast_advance_ascii(lexer, 1);
    int exp_digit = 0;
    int exp_bad = 0;
    ast_scan_digits(lexer, 10u, &exp_digit, &exp_bad);
    bad_underscore |= exp_bad;
    if (!exp_digit) {
      ast_push_diag(lexer, AST_DIAG_INVALID_NUMBER, start,
                    (uint32_t)(lexer->pos - start), line, column,
                    "floating-point exponent must contain digits");
    }
  }

  if (bad_underscore) {
    ast_push_diag(lexer, AST_DIAG_TRAILING_NUMERIC_UNDERSCORE, start,
                  (uint32_t)(lexer->pos - start), line, column,
                  "numeric separators must appear only between digits");
  }

  if (ast_starts_identifier_continue(lexer)) {
    while (lexer->pos < lexer->size) {
      uint8_t c = lexer->data[lexer->pos];
      if (c < 0x80u) {
        if (!ast_is_ascii_ident_continue(c))
          break;
        ast_advance_ascii(lexer, 1);
      } else {
        uint32_t cp, width;
        if (!ast_utf8_decode_one(lexer->data + lexer->pos,
                                 lexer->size - lexer->pos, &cp, &width) ||
            !ast_unicode_is_xid_continue(cp))
          break;
        lexer->pos += width;
        lexer->column++;
      }
    }
    ast_push_diag(
        lexer, AST_DIAG_INVALID_NUMBER, start, (uint32_t)(lexer->pos - start),
        line, column,
        "numeric literals cannot have identifier suffixes in Core v0.1");
  }

  ast_emit_token(lexer, is_float ? AST_TOK_FLOAT : AST_TOK_INTEGER, 0, start,
                 line, column);
}

static int ast_consume_escape(AstLexer *lexer, uint32_t *flags) {
  size_t escape_start = lexer->pos;
  uint32_t line = lexer->line, column = lexer->column;
  if (lexer->pos >= lexer->size || lexer->data[lexer->pos] != '\\')
    return 0;
  ast_advance_ascii(lexer, 1);
  *flags |= AST_TOKENF_ESCAPED;
  if (lexer->pos >= lexer->size)
    return 0;

  uint8_t c = lexer->data[lexer->pos];
  switch (c) {
  case '\\':
  case '"':
  case '\'':
  case 'n':
  case 'r':
  case 't':
  case '0':
  case 'a':
  case 'b':
  case 'f':
  case 'v':
    ast_advance_ascii(lexer, 1);
    return 1;
  case 'x': {
    ast_advance_ascii(lexer, 1);
    if (lexer->pos + 2 > lexer->size ||
        !ast_is_ascii_hex(lexer->data[lexer->pos]) ||
        !ast_is_ascii_hex(lexer->data[lexer->pos + 1])) {
      ast_push_diag(lexer, AST_DIAG_INVALID_ESCAPE, escape_start,
                    (uint32_t)(lexer->pos - escape_start), line, column,
                    "\\x escape requires exactly two hexadecimal digits");
      return 0;
    }
    ast_advance_ascii(lexer, 2);
    return 1;
  }
  case 'u': {
    ast_advance_ascii(lexer, 1);
    if (lexer->pos >= lexer->size || lexer->data[lexer->pos] != '{') {
      ast_push_diag(lexer, AST_DIAG_INVALID_UNICODE_ESCAPE, escape_start,
                    (uint32_t)(lexer->pos - escape_start), line, column,
                    "Unicode escape must use the form \\u{HEX}");
      return 0;
    }
    ast_advance_ascii(lexer, 1);
    uint32_t value = 0;
    unsigned digits = 0;
    while (lexer->pos < lexer->size &&
           ast_is_ascii_hex(lexer->data[lexer->pos]) && digits < 6u) {
      uint8_t h = lexer->data[lexer->pos];
      uint32_t v = ast_is_ascii_digit(h) ? (uint32_t)(h - '0')
                                         : (uint32_t)((h | 0x20u) - 'a' + 10u);
      value = (value << 4) | v;
      digits++;
      ast_advance_ascii(lexer, 1);
    }
    if (digits == 0u || lexer->pos >= lexer->size ||
        lexer->data[lexer->pos] != '}' || value > 0x10FFFFu ||
        (value >= 0xD800u && value <= 0xDFFFu)) {
      ast_push_diag(lexer, AST_DIAG_INVALID_UNICODE_ESCAPE, escape_start,
                    (uint32_t)(lexer->pos - escape_start), line, column,
                    "Unicode escape must encode one Unicode scalar value");
      if (lexer->pos < lexer->size && lexer->data[lexer->pos] == '}')
        ast_advance_ascii(lexer, 1);
      return 0;
    }
    ast_advance_ascii(lexer, 1);
    return 1;
  }
  default:
    ast_advance_ascii(lexer, 1);
    ast_push_diag(lexer, AST_DIAG_INVALID_ESCAPE, escape_start,
                  (uint32_t)(lexer->pos - escape_start), line, column,
                  "unknown escape sequence");
    return 0;
  }
}

static void ast_lex_string(AstLexer *lexer, AstTokenKind kind,
                           size_t prefix_length) {
  size_t start = lexer->pos;
  uint32_t line = lexer->line, column = lexer->column;
  uint32_t flags = 0;

  if (prefix_length)
    ast_advance_ascii(lexer, prefix_length);
  if (lexer->pos >= lexer->size || lexer->data[lexer->pos] != '"') {
    ast_push_diag(lexer, AST_DIAG_UNTERMINATED_STRING, start,
                  (uint32_t)(lexer->pos - start), line, column,
                  "string prefix must be followed by a quote");
    ast_emit_token(lexer, kind, AST_TOKENF_ERROR, start, line, column);
    return;
  }
  ast_advance_ascii(lexer, 1);

  int terminated = 0;
  while (lexer->pos < lexer->size) {
    int nl = ast_newline_width(lexer, lexer->pos);
    if (nl) {
      ast_push_diag(lexer, AST_DIAG_NEWLINE_IN_STRING, start,
                    (uint32_t)(lexer->pos - start), line, column,
                    "newline is not allowed inside a quoted string");
      break;
    }

    uint8_t c = lexer->data[lexer->pos];
    if (c == '"') {
      ast_advance_ascii(lexer, 1);
      terminated = 1;
      break;
    }
    if (c == '\\') {
      ast_consume_escape(lexer, &flags);
      continue;
    }
    if (c == 0) {
      ast_push_diag(lexer, AST_DIAG_NUL_IN_SOURCE, lexer->pos, 1, lexer->line,
                    lexer->column, "NUL byte is not allowed in source text");
      ast_advance_ascii(lexer, 1);
      flags |= AST_TOKENF_ERROR;
      continue;
    }
    if (c < 0x80u) {
      ast_advance_ascii(lexer, 1);
      continue;
    }

    uint32_t cp, width;
    if (!ast_utf8_decode_one(lexer->data + lexer->pos, lexer->size - lexer->pos,
                             &cp, &width)) {
      ast_push_diag(lexer, AST_DIAG_INVALID_UTF8, lexer->pos, 1, lexer->line,
                    lexer->column, "invalid UTF-8 sequence");
      ast_advance_ascii(lexer, 1);
      flags |= AST_TOKENF_ERROR;
      continue;
    }
    lexer->pos += width;
    lexer->column++;
    flags |= AST_TOKENF_UNICODE;
  }

  if (!terminated) {
    ast_push_diag(lexer, AST_DIAG_UNTERMINATED_STRING, start,
                  (uint32_t)(lexer->pos - start), line, column,
                  "unterminated string literal");
    flags |= AST_TOKENF_ERROR;
  }
  ast_emit_token(lexer, kind, flags, start, line, column);
}

static void ast_lex_char(AstLexer *lexer) {
  size_t start = lexer->pos;
  uint32_t line = lexer->line, column = lexer->column;
  uint32_t flags = 0;
  ast_advance_ascii(lexer, 1);

  if (lexer->pos >= lexer->size) {
    ast_push_diag(lexer, AST_DIAG_UNTERMINATED_CHAR, start,
                  (uint32_t)(lexer->pos - start), line, column,
                  "unterminated character literal");
    ast_emit_token(lexer, AST_TOK_CHAR, AST_TOKENF_ERROR, start, line, column);
    return;
  }
  if (lexer->data[lexer->pos] == '\'') {
    ast_advance_ascii(lexer, 1);
    ast_push_diag(lexer, AST_DIAG_EMPTY_CHAR, start,
                  (uint32_t)(lexer->pos - start), line, column,
                  "character literal cannot be empty");
    ast_emit_token(lexer, AST_TOK_CHAR, AST_TOKENF_ERROR, start, line, column);
    return;
  }
  if (ast_newline_width(lexer, lexer->pos)) {
    ast_push_diag(lexer, AST_DIAG_UNTERMINATED_CHAR, start,
                  (uint32_t)(lexer->pos - start), line, column,
                  "newline is not allowed in a character literal");
    ast_emit_token(lexer, AST_TOK_CHAR, AST_TOKENF_ERROR, start, line, column);
    return;
  }

  if (lexer->data[lexer->pos] == '\\') {
    if (!ast_consume_escape(lexer, &flags))
      flags |= AST_TOKENF_ERROR;
  } else if (lexer->data[lexer->pos] < 0x80u) {
    if (lexer->data[lexer->pos] == 0) {
      ast_push_diag(lexer, AST_DIAG_NUL_IN_SOURCE, lexer->pos, 1, lexer->line,
                    lexer->column, "NUL byte is not allowed in source text");
      flags |= AST_TOKENF_ERROR;
    }
    ast_advance_ascii(lexer, 1);
  } else {
    uint32_t cp, width;
    if (!ast_utf8_decode_one(lexer->data + lexer->pos, lexer->size - lexer->pos,
                             &cp, &width)) {
      ast_push_diag(lexer, AST_DIAG_INVALID_UTF8, lexer->pos, 1, lexer->line,
                    lexer->column, "invalid UTF-8 sequence");
      ast_advance_ascii(lexer, 1);
      flags |= AST_TOKENF_ERROR;
    } else {
      lexer->pos += width;
      lexer->column++;
      flags |= AST_TOKENF_UNICODE;
    }
  }

  if (lexer->pos < lexer->size && lexer->data[lexer->pos] == '\'') {
    ast_advance_ascii(lexer, 1);
    ast_emit_token(lexer, AST_TOK_CHAR, flags, start, line, column);
    return;
  }

  size_t error_end = lexer->pos;
  while (lexer->pos < lexer->size && !ast_newline_width(lexer, lexer->pos) &&
         lexer->data[lexer->pos] != '\'') {
    uint8_t c = lexer->data[lexer->pos];
    if (c < 0x80u)
      ast_advance_ascii(lexer, 1);
    else {
      uint32_t cp, width;
      if (!ast_utf8_decode_one(lexer->data + lexer->pos,
                               lexer->size - lexer->pos, &cp, &width))
        ast_advance_ascii(lexer, 1);
      else {
        lexer->pos += width;
        lexer->column++;
      }
    }
  }
  if (lexer->pos < lexer->size && lexer->data[lexer->pos] == '\'')
    ast_advance_ascii(lexer, 1);
  AstDiagCode code = error_end == lexer->pos ? AST_DIAG_UNTERMINATED_CHAR
                                             : AST_DIAG_MULTI_CHAR;
  ast_push_diag(
      lexer, code, start, (uint32_t)(lexer->pos - start), line, column,
      code == AST_DIAG_MULTI_CHAR
          ? "character literal must contain exactly one Unicode scalar value"
          : "unterminated character literal");
  ast_emit_token(lexer, AST_TOK_CHAR, flags | AST_TOKENF_ERROR, start, line,
                 column);
}

static void ast_lex_identifier(AstLexer *lexer) {
  size_t start = lexer->pos;
  uint32_t line = lexer->line, column = lexer->column;
  uint32_t flags = 0;

  while (lexer->pos < lexer->size) {
    uint8_t c = lexer->data[lexer->pos];
    if (c < 0x80u) {
      if (!ast_is_ascii_ident_continue(c))
        break;
      ast_advance_ascii(lexer, 1);
      continue;
    }
    if (lexer->config.flags & AST_LEX_ASCII_IDENTIFIERS_ONLY)
      break;
    uint32_t cp, width;
    if (!ast_utf8_decode_one(lexer->data + lexer->pos, lexer->size - lexer->pos,
                             &cp, &width))
      break;
    if (!ast_unicode_is_xid_continue(cp))
      break;
    lexer->pos += width;
    lexer->column++;
    flags |= AST_TOKENF_UNICODE;
  }

  size_t length = lexer->pos - start;
  AstTokenKind kind = (flags & AST_TOKENF_UNICODE)
                          ? AST_TOK_IDENTIFIER
                          : ast_keyword_kind(lexer->data + start, length);
  ast_emit_token(lexer, kind, flags, start, line, column);
}

static void ast_lex_unicode_identifier(AstLexer *lexer, uint32_t first_cp,
                                       uint32_t first_width) {
  size_t start = lexer->pos;
  uint32_t line = lexer->line, column = lexer->column;
  lexer->pos += first_width;
  lexer->column++;
  uint32_t flags = AST_TOKENF_UNICODE;

  while (lexer->pos < lexer->size) {
    uint8_t c = lexer->data[lexer->pos];
    if (c < 0x80u) {
      if (!ast_is_ascii_ident_continue(c))
        break;
      ast_advance_ascii(lexer, 1);
      continue;
    }
    uint32_t cp, width;
    if (!ast_utf8_decode_one(lexer->data + lexer->pos, lexer->size - lexer->pos,
                             &cp, &width))
      break;
    if (!ast_unicode_is_xid_continue(cp))
      break;
    lexer->pos += width;
    lexer->column++;
  }
  (void)first_cp;
  ast_emit_token(lexer, AST_TOK_IDENTIFIER, flags, start, line, column);
}

static void ast_lex_line_comment(AstLexer *lexer) {
  size_t start = lexer->pos;
  uint32_t line = lexer->line, column = lexer->column;
  uint32_t flags = AST_TOKENF_TRIVIA;
  ast_advance_ascii(lexer, 2);
  while (lexer->pos < lexer->size && !ast_newline_width(lexer, lexer->pos)) {
    uint8_t c = lexer->data[lexer->pos];
    if (c < 0x80u) {
      if (c == 0) {
        ast_push_diag(lexer, AST_DIAG_NUL_IN_SOURCE, lexer->pos, 1, lexer->line,
                      lexer->column, "NUL byte is not allowed in source text");
        flags |= AST_TOKENF_ERROR;
      }
      ast_advance_ascii(lexer, 1);
    } else {
      uint32_t cp, width;
      if (!ast_utf8_decode_one(lexer->data + lexer->pos,
                               lexer->size - lexer->pos, &cp, &width)) {
        ast_push_diag(lexer, AST_DIAG_INVALID_UTF8, lexer->pos, 1, lexer->line,
                      lexer->column, "invalid UTF-8 sequence");
        ast_advance_ascii(lexer, 1);
        flags |= AST_TOKENF_ERROR;
      } else {
        lexer->pos += width;
        lexer->column++;
        flags |= AST_TOKENF_UNICODE;
      }
    }
  }
  if (lexer->config.flags & AST_LEX_EMIT_TRIVIA)
    ast_emit_token(lexer, AST_TOK_LINE_COMMENT, flags, start, line, column);
}

static void ast_lex_block_comment(AstLexer *lexer) {
  size_t start = lexer->pos;
  uint32_t line = lexer->line, column = lexer->column;
  uint32_t flags = AST_TOKENF_TRIVIA;
  uint32_t depth = 1;
  uint32_t max_depth = lexer->config.max_block_comment_depth
                           ? lexer->config.max_block_comment_depth
                           : ASTER_MAX_BLOCK_COMMENT_DEPTH;
  ast_advance_ascii(lexer, 2);

  while (lexer->pos < lexer->size) {
    if (lexer->pos + 1 < lexer->size && lexer->data[lexer->pos] == '/' &&
        lexer->data[lexer->pos + 1] == '*') {
      depth++;
      if (depth == max_depth + 1u) {
        ast_push_diag(lexer, AST_DIAG_BLOCK_COMMENT_NESTING, lexer->pos, 2,
                      lexer->line, lexer->column,
                      "block comment nesting limit exceeded");
        flags |= AST_TOKENF_ERROR;
      }
      ast_advance_ascii(lexer, 2);
      continue;
    }
    if (lexer->pos + 1 < lexer->size && lexer->data[lexer->pos] == '*' &&
        lexer->data[lexer->pos + 1] == '/') {
      ast_advance_ascii(lexer, 2);
      if (--depth == 0) {
        if (lexer->config.flags & AST_LEX_EMIT_TRIVIA)
          ast_emit_token(lexer, AST_TOK_BLOCK_COMMENT, flags, start, line,
                         column);
        return;
      }
      continue;
    }

    int nl = ast_newline_width(lexer, lexer->pos);
    if (nl) {
      ast_consume_newline(lexer, nl);
      continue;
    }

    uint8_t c = lexer->data[lexer->pos];
    if (c < 0x80u) {
      if (c == 0) {
        ast_push_diag(lexer, AST_DIAG_NUL_IN_SOURCE, lexer->pos, 1, lexer->line,
                      lexer->column, "NUL byte is not allowed in source text");
        flags |= AST_TOKENF_ERROR;
      }
      ast_advance_ascii(lexer, 1);
    } else {
      uint32_t cp, width;
      if (!ast_utf8_decode_one(lexer->data + lexer->pos,
                               lexer->size - lexer->pos, &cp, &width)) {
        ast_push_diag(lexer, AST_DIAG_INVALID_UTF8, lexer->pos, 1, lexer->line,
                      lexer->column, "invalid UTF-8 sequence");
        ast_advance_ascii(lexer, 1);
        flags |= AST_TOKENF_ERROR;
      } else {
        lexer->pos += width;
        lexer->column++;
        flags |= AST_TOKENF_UNICODE;
      }
    }
  }

  ast_push_diag(lexer, AST_DIAG_UNTERMINATED_BLOCK_COMMENT, start,
                (uint32_t)(lexer->pos - start), line, column,
                "unterminated block comment");
  if (lexer->config.flags & AST_LEX_EMIT_TRIVIA)
    ast_emit_token(lexer, AST_TOK_BLOCK_COMMENT, flags | AST_TOKENF_ERROR,
                   start, line, column);
}

static void ast_lex_whitespace(AstLexer *lexer) {
  size_t start = lexer->pos;
  uint32_t line = lexer->line, column = lexer->column;
  while (lexer->pos < lexer->size) {
    uint8_t c = lexer->data[lexer->pos];
    if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
      ast_advance_ascii(lexer, 1);
      continue;
    }
    if (c >= 0x80u && !(lexer->config.flags & AST_LEX_ASCII_IDENTIFIERS_ONLY)) {
      uint32_t cp, width;
      if (ast_utf8_decode_one(lexer->data + lexer->pos,
                              lexer->size - lexer->pos, &cp, &width) &&
          ast_is_unicode_inline_whitespace(cp)) {
        lexer->pos += width;
        lexer->column++;
        continue;
      }
    }
    break;
  }
  if (lexer->config.flags & AST_LEX_EMIT_TRIVIA)
    ast_emit_token(lexer, AST_TOK_WHITESPACE, AST_TOKENF_TRIVIA, start, line,
                   column);
}

static void ast_lex_newline(AstLexer *lexer, int width) {
  size_t start = lexer->pos;
  uint32_t line = lexer->line, column = lexer->column;
  ast_consume_newline(lexer, width);
  if (lexer->config.flags & AST_LEX_EMIT_TRIVIA)
    ast_emit_token(lexer, AST_TOK_NEWLINE, AST_TOKENF_TRIVIA, start, line,
                   column);
}

static void ast_emit_simple(AstLexer *lexer, AstTokenKind kind, size_t width) {
  size_t start = lexer->pos;
  uint32_t line = lexer->line, column = lexer->column;
  ast_advance_ascii(lexer, width);
  ast_emit_token(lexer, kind, 0, start, line, column);
}

static int ast_lex_all(AstLexer *lexer) {
  if (lexer->size >= 3 && lexer->data[0] == 0xEFu && lexer->data[1] == 0xBBu &&
      lexer->data[2] == 0xBFu) {
    if (!(lexer->config.flags & AST_LEX_ACCEPT_BOM)) {
      ast_push_diag(lexer, AST_DIAG_BOM_NOT_ALLOWED, 0, 3, 1, 1,
                    "UTF-8 BOM is not allowed by the AsterASM source format");
    }
    lexer->pos = 3;
    lexer->column = 1;
  }

  while (lexer->pos < lexer->size && !lexer->storage->oom) {
    int nl = ast_newline_width(lexer, lexer->pos);
    if (nl) {
      ast_lex_newline(lexer, nl);
      continue;
    }

    uint8_t c = lexer->data[lexer->pos];
    if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
      ast_lex_whitespace(lexer);
      continue;
    }
    if (c == 0) {
      size_t start = lexer->pos;
      uint32_t line = lexer->line, column = lexer->column;
      ast_push_diag(lexer, AST_DIAG_NUL_IN_SOURCE, start, 1, line, column,
                    "NUL byte is not allowed in source text");
      ast_advance_ascii(lexer, 1);
      ast_emit_token(lexer, AST_TOK_INVALID, AST_TOKENF_ERROR, start, line,
                     column);
      continue;
    }
    if (c == '/' && lexer->pos + 1 < lexer->size &&
        lexer->data[lexer->pos + 1] == '/') {
      ast_lex_line_comment(lexer);
      continue;
    }
    if (c == '/' && lexer->pos + 1 < lexer->size &&
        lexer->data[lexer->pos + 1] == '*') {
      ast_lex_block_comment(lexer);
      continue;
    }
    if (c == 'b' && lexer->pos + 1 < lexer->size &&
        lexer->data[lexer->pos + 1] == '"') {
      ast_lex_string(lexer, AST_TOK_BYTE_STRING, 1);
      continue;
    }
    if (c == 'c' && lexer->pos + 1 < lexer->size &&
        lexer->data[lexer->pos + 1] == '"') {
      ast_lex_string(lexer, AST_TOK_C_STRING, 1);
      continue;
    }
    if (c == '"') {
      ast_lex_string(lexer, AST_TOK_STRING, 0);
      continue;
    }
    if (c == '\'') {
      ast_lex_char(lexer);
      continue;
    }
    if (ast_is_ascii_digit(c)) {
      ast_lex_number(lexer);
      continue;
    }
    if (ast_is_ascii_ident_start(c)) {
      ast_lex_identifier(lexer);
      continue;
    }

    if (c >= 0x80u) {
      uint32_t cp, width;
      size_t start = lexer->pos;
      uint32_t line = lexer->line, column = lexer->column;
      if (!ast_utf8_decode_one(lexer->data + lexer->pos,
                               lexer->size - lexer->pos, &cp, &width)) {
        ast_push_diag(lexer, AST_DIAG_INVALID_UTF8, start, 1, line, column,
                      "invalid UTF-8 sequence");
        ast_advance_ascii(lexer, 1);
        ast_emit_token(lexer, AST_TOK_INVALID, AST_TOKENF_ERROR, start, line,
                       column);
        continue;
      }
      if (!(lexer->config.flags & AST_LEX_ASCII_IDENTIFIERS_ONLY) &&
          ast_unicode_is_xid_start(cp)) {
        ast_lex_unicode_identifier(lexer, cp, width);
        continue;
      }
      if (!(lexer->config.flags & AST_LEX_ASCII_IDENTIFIERS_ONLY) &&
          ast_is_unicode_inline_whitespace(cp)) {
        ast_lex_whitespace(lexer);
        continue;
      }
      lexer->pos += width;
      lexer->column++;
      ast_push_diag(lexer, AST_DIAG_UNEXPECTED_CHARACTER, start, width, line,
                    column,
                    "Unicode character is not valid at this lexical position");
      ast_emit_token(lexer, AST_TOK_INVALID,
                     AST_TOKENF_UNICODE | AST_TOKENF_ERROR, start, line,
                     column);
      continue;
    }

    switch (c) {
    case '(':
      ast_emit_simple(lexer, AST_TOK_LPAREN, 1);
      break;
    case ')':
      ast_emit_simple(lexer, AST_TOK_RPAREN, 1);
      break;
    case '{':
      ast_emit_simple(lexer, AST_TOK_LBRACE, 1);
      break;
    case '}':
      ast_emit_simple(lexer, AST_TOK_RBRACE, 1);
      break;
    case '[':
      ast_emit_simple(lexer, AST_TOK_LBRACKET, 1);
      break;
    case ']':
      ast_emit_simple(lexer, AST_TOK_RBRACKET, 1);
      break;
    case ',':
      ast_emit_simple(lexer, AST_TOK_COMMA, 1);
      break;
    case ';':
      ast_emit_simple(lexer, AST_TOK_SEMICOLON, 1);
      break;
    case ':':
      ast_emit_simple(lexer, AST_TOK_COLON, 1);
      break;
    case '?':
      ast_emit_simple(lexer, AST_TOK_QUESTION, 1);
      break;
    case '@':
      ast_emit_simple(lexer, AST_TOK_AT, 1);
      break;
    case '$':
      ast_emit_simple(lexer, AST_TOK_DOLLAR, 1);
      break;
    case '+':
      ast_emit_simple(lexer, AST_TOK_PLUS, 1);
      break;
    case '*':
      ast_emit_simple(lexer, AST_TOK_STAR, 1);
      break;
    case '%':
      ast_emit_simple(lexer, AST_TOK_PERCENT, 1);
      break;
    case '^':
      ast_emit_simple(lexer, AST_TOK_CARET, 1);
      break;
    case '~':
      ast_emit_simple(lexer, AST_TOK_TILDE, 1);
      break;
    case '.':
      if (lexer->pos + 1 < lexer->size && lexer->data[lexer->pos + 1] == '.')
        ast_emit_simple(lexer, AST_TOK_DOT_DOT, 2);
      else
        ast_emit_simple(lexer, AST_TOK_DOT, 1);
      break;
    case '#':
      if (lexer->pos + 1 < lexer->size && lexer->data[lexer->pos + 1] == '#')
        ast_emit_simple(lexer, AST_TOK_HASH_HASH, 2);
      else {
        size_t start = lexer->pos;
        uint32_t line = lexer->line, column = lexer->column;
        ast_advance_ascii(lexer, 1);
        ast_push_diag(lexer, AST_DIAG_UNEXPECTED_CHARACTER, start, 1, line,
                      column, "single '#' is not a valid AsterASM token");
        ast_emit_token(lexer, AST_TOK_INVALID, AST_TOKENF_ERROR, start, line,
                       column);
      }
      break;
    case '-':
      if (lexer->pos + 1 < lexer->size && lexer->data[lexer->pos + 1] == '>')
        ast_emit_simple(lexer, AST_TOK_ARROW, 2);
      else
        ast_emit_simple(lexer, AST_TOK_MINUS, 1);
      break;
    case '=':
      if (lexer->pos + 1 < lexer->size && lexer->data[lexer->pos + 1] == '=')
        ast_emit_simple(lexer, AST_TOK_EQ_EQ, 2);
      else if (lexer->pos + 1 < lexer->size &&
               lexer->data[lexer->pos + 1] == '>')
        ast_emit_simple(lexer, AST_TOK_FAT_ARROW, 2);
      else
        ast_emit_simple(lexer, AST_TOK_ASSIGN, 1);
      break;
    case '!':
      if (lexer->pos + 1 < lexer->size && lexer->data[lexer->pos + 1] == '=')
        ast_emit_simple(lexer, AST_TOK_BANG_EQ, 2);
      else
        ast_emit_simple(lexer, AST_TOK_BANG, 1);
      break;
    case '<':
      if (lexer->pos + 1 < lexer->size && lexer->data[lexer->pos + 1] == '=')
        ast_emit_simple(lexer, AST_TOK_LT_EQ, 2);
      else if (lexer->pos + 1 < lexer->size &&
               lexer->data[lexer->pos + 1] == '<')
        ast_emit_simple(lexer, AST_TOK_SHIFT_LEFT, 2);
      else
        ast_emit_simple(lexer, AST_TOK_LT, 1);
      break;
    case '>':
      if (lexer->pos + 1 < lexer->size && lexer->data[lexer->pos + 1] == '=')
        ast_emit_simple(lexer, AST_TOK_GT_EQ, 2);
      else if (lexer->pos + 1 < lexer->size &&
               lexer->data[lexer->pos + 1] == '>')
        ast_emit_simple(lexer, AST_TOK_SHIFT_RIGHT, 2);
      else
        ast_emit_simple(lexer, AST_TOK_GT, 1);
      break;
    case '&':
      if (lexer->pos + 1 < lexer->size && lexer->data[lexer->pos + 1] == '&')
        ast_emit_simple(lexer, AST_TOK_AMP_AMP, 2);
      else
        ast_emit_simple(lexer, AST_TOK_AMP, 1);
      break;
    case '|':
      if (lexer->pos + 1 < lexer->size && lexer->data[lexer->pos + 1] == '|')
        ast_emit_simple(lexer, AST_TOK_PIPE_PIPE, 2);
      else
        ast_emit_simple(lexer, AST_TOK_PIPE, 1);
      break;
    case '/':
      ast_emit_simple(lexer, AST_TOK_SLASH, 1);
      break;
    default: {
      size_t start = lexer->pos;
      uint32_t line = lexer->line, column = lexer->column;
      ast_advance_ascii(lexer, 1);
      ast_push_diag(lexer, AST_DIAG_UNEXPECTED_CHARACTER, start, 1, line,
                    column, "unexpected character");
      ast_emit_token(lexer, AST_TOK_INVALID, AST_TOKENF_ERROR, start, line,
                     column);
      break;
    }
    }
  }

  size_t eof = lexer->pos;
  ast_emit_token(lexer, AST_TOK_EOF, 0, eof, lexer->line, lexer->column);
  return lexer->storage->oom ? -1 : 0;
}

AstLexConfig ast_lex_config_default(void) {
  AstLexConfig config;
  config.flags = AST_LEX_DEFAULT;
  config.max_block_comment_depth = ASTER_MAX_BLOCK_COMMENT_DEPTH;
  config.arena_block_size = ASTER_DEFAULT_ARENA_BLOCK_SIZE;
  return config;
}

int ast_lex_source(const AstSourceFile *source, const AstLexConfig *config,
                   AstLexedFile *out) {
  if (!source || !out || (!source->data && source->size != 0))
    return -1;
  memset(out, 0, sizeof(*out));

  AstLexStorage *storage = (AstLexStorage *)calloc(1, sizeof(*storage));
  if (!storage)
    return -1;
  AstLexConfig effective = config ? *config : ast_lex_config_default();
  if (!effective.arena_block_size)
    effective.arena_block_size = ASTER_DEFAULT_ARENA_BLOCK_SIZE;
  if (!effective.max_block_comment_depth)
    effective.max_block_comment_depth = ASTER_MAX_BLOCK_COMMENT_DEPTH;
  ast_arena_init(&storage->arena, effective.arena_block_size);

  out->source = *source;
  out->internal = storage;

  AstLexer lexer;
  memset(&lexer, 0, sizeof(lexer));
  lexer.data = source->data;
  lexer.size = source->size;
  lexer.line = 1;
  lexer.column = 1;
  lexer.config = effective;
  lexer.out = out;
  lexer.storage = storage;

  int result = ast_lex_all(&lexer);
  if (storage->oom)
    result = -1;
  return result;
}

void ast_lexed_file_destroy(AstLexedFile *file) {
  if (!file)
    return;
  AstLexStorage *storage = (AstLexStorage *)file->internal;
  if (storage) {
    ast_arena_destroy(&storage->arena);
    free(storage);
  }
  memset(file, 0, sizeof(*file));
}

AstTokenCursor ast_token_cursor_begin(const AstLexedFile *file) {
  AstTokenCursor cursor;
  cursor.chunk = file ? file->token_head : NULL;
  cursor.index = 0;
  cursor.absolute_index = 0;
  return cursor;
}

const AstToken *ast_token_cursor_get(const AstTokenCursor *cursor) {
  if (!cursor || !cursor->chunk || cursor->index >= cursor->chunk->count)
    return NULL;
  return &cursor->chunk->tokens[cursor->index];
}

int ast_token_cursor_next(AstTokenCursor *cursor) {
  if (!cursor || !cursor->chunk)
    return 0;
  cursor->index++;
  cursor->absolute_index++;
  if (cursor->index >= cursor->chunk->count) {
    cursor->chunk = cursor->chunk->next;
    cursor->index = 0;
  }
  return cursor->chunk != NULL;
}

const AstToken *ast_token_at(const AstLexedFile *file, size_t index) {
  if (!file || index >= file->token_count)
    return NULL;
  const AstTokenChunk *chunk = file->token_head;
  while (chunk) {
    if (index < chunk->count)
      return &chunk->tokens[index];
    index -= chunk->count;
    chunk = chunk->next;
  }
  return NULL;
}

const char *ast_diag_code_name(AstDiagCode code) {
  switch (code) {
  case AST_DIAG_NONE:
    return "none";
  case AST_DIAG_OUT_OF_MEMORY:
    return "out_of_memory";
  case AST_DIAG_BOM_NOT_ALLOWED:
    return "bom_not_allowed";
  case AST_DIAG_INVALID_UTF8:
    return "invalid_utf8";
  case AST_DIAG_UNEXPECTED_CHARACTER:
    return "unexpected_character";
  case AST_DIAG_NUL_IN_SOURCE:
    return "nul_in_source";
  case AST_DIAG_UNTERMINATED_BLOCK_COMMENT:
    return "unterminated_block_comment";
  case AST_DIAG_BLOCK_COMMENT_NESTING:
    return "block_comment_nesting";
  case AST_DIAG_UNTERMINATED_STRING:
    return "unterminated_string";
  case AST_DIAG_NEWLINE_IN_STRING:
    return "newline_in_string";
  case AST_DIAG_INVALID_ESCAPE:
    return "invalid_escape";
  case AST_DIAG_INVALID_UNICODE_ESCAPE:
    return "invalid_unicode_escape";
  case AST_DIAG_UNTERMINATED_CHAR:
    return "unterminated_char";
  case AST_DIAG_EMPTY_CHAR:
    return "empty_char";
  case AST_DIAG_MULTI_CHAR:
    return "multi_char";
  case AST_DIAG_INVALID_NUMBER:
    return "invalid_number";
  case AST_DIAG_INVALID_DIGIT_FOR_BASE:
    return "invalid_digit_for_base";
  case AST_DIAG_TRAILING_NUMERIC_UNDERSCORE:
    return "bad_numeric_separator";
  default:
    return "unknown";
  }
}

const char *ast_token_kind_name(AstTokenKind kind) {
  static const char *names[AST_TOK_COUNT] = {
      [AST_TOK_INVALID] = "invalid",
      [AST_TOK_EOF] = "eof",
      [AST_TOK_IDENTIFIER] = "identifier",
      [AST_TOK_INTEGER] = "integer",
      [AST_TOK_FLOAT] = "float",
      [AST_TOK_STRING] = "string",
      [AST_TOK_BYTE_STRING] = "byte_string",
      [AST_TOK_C_STRING] = "c_string",
      [AST_TOK_CHAR] = "char",
      [AST_TOK_WHITESPACE] = "whitespace",
      [AST_TOK_NEWLINE] = "newline",
      [AST_TOK_LINE_COMMENT] = "line_comment",
      [AST_TOK_BLOCK_COMMENT] = "block_comment",
      [AST_TOK_LPAREN] = "(",
      [AST_TOK_RPAREN] = ")",
      [AST_TOK_LBRACE] = "{",
      [AST_TOK_RBRACE] = "}",
      [AST_TOK_LBRACKET] = "[",
      [AST_TOK_RBRACKET] = "]",
      [AST_TOK_COMMA] = ",",
      [AST_TOK_SEMICOLON] = ";",
      [AST_TOK_COLON] = ":",
      [AST_TOK_DOT] = ".",
      [AST_TOK_DOT_DOT] = "..",
      [AST_TOK_QUESTION] = "?",
      [AST_TOK_AT] = "@",
      [AST_TOK_DOLLAR] = "$",
      [AST_TOK_HASH_HASH] = "##",
      [AST_TOK_PLUS] = "+",
      [AST_TOK_MINUS] = "-",
      [AST_TOK_STAR] = "*",
      [AST_TOK_SLASH] = "/",
      [AST_TOK_PERCENT] = "%",
      [AST_TOK_AMP] = "&",
      [AST_TOK_PIPE] = "|",
      [AST_TOK_CARET] = "^",
      [AST_TOK_TILDE] = "~",
      [AST_TOK_BANG] = "!",
      [AST_TOK_ASSIGN] = "=",
      [AST_TOK_LT] = "<",
      [AST_TOK_GT] = ">",
      [AST_TOK_ARROW] = "->",
      [AST_TOK_FAT_ARROW] = "=>",
      [AST_TOK_EQ_EQ] = "==",
      [AST_TOK_BANG_EQ] = "!=",
      [AST_TOK_LT_EQ] = "<=",
      [AST_TOK_GT_EQ] = ">=",
      [AST_TOK_SHIFT_LEFT] = "<<",
      [AST_TOK_SHIFT_RIGHT] = ">>",
      [AST_TOK_AMP_AMP] = "&&",
      [AST_TOK_PIPE_PIPE] = "||",
      [AST_TOK_KW_MODULE] = "module",
      [AST_TOK_KW_TARGET] = "target",
      [AST_TOK_KW_FEATURE] = "feature",
      [AST_TOK_KW_IMPORT] = "import",
      [AST_TOK_KW_AS] = "as",
      [AST_TOK_KW_EXPORT] = "export",
      [AST_TOK_KW_TYPE] = "type",
      [AST_TOK_KW_STRUCT] = "struct",
      [AST_TOK_KW_CONST] = "const",
      [AST_TOK_KW_STATIC_ASSERT] = "static_assert",
      [AST_TOK_KW_SECTION] = "section",
      [AST_TOK_KW_GLOBAL] = "global",
      [AST_TOK_KW_EXTERN] = "extern",
      [AST_TOK_KW_FN] = "fn",
      [AST_TOK_KW_DISPATCH] = "dispatch",
      [AST_TOK_KW_WHEN] = "when",
      [AST_TOK_KW_ELSE] = "else",
      [AST_TOK_KW_LET] = "let",
      [AST_TOK_KW_STORE] = "store",
      [AST_TOK_KW_IF] = "if",
      [AST_TOK_KW_WHILE] = "while",
      [AST_TOK_KW_LOOP] = "loop",
      [AST_TOK_KW_RETURN] = "return",
      [AST_TOK_KW_BREAK] = "break",
      [AST_TOK_KW_CONTINUE] = "continue",
      [AST_TOK_KW_BLOCK] = "block",
      [AST_TOK_KW_BR] = "br",
      [AST_TOK_KW_UNSAFE] = "unsafe",
      [AST_TOK_KW_CONTROL] = "control",
      [AST_TOK_KW_COMPTIME] = "comptime",
      [AST_TOK_KW_FOR] = "for",
      [AST_TOK_KW_IN] = "in",
      [AST_TOK_KW_REQUIRES] = "requires",
      [AST_TOK_KW_BIND] = "bind",
      [AST_TOK_KW_TO] = "to",
      [AST_TOK_KW_REG] = "reg",
      [AST_TOK_KW_MACRO] = "macro",
      [AST_TOK_KW_NATIVE] = "native",
      [AST_TOK_KW_INPUTS] = "inputs",
      [AST_TOK_KW_OUTPUTS] = "outputs",
      [AST_TOK_KW_EXACT] = "exact",
      [AST_TOK_KW_BYTES] = "bytes",
      [AST_TOK_KW_LOAD] = "load",
      [AST_TOK_KW_CAST] = "cast",
      [AST_TOK_KW_TRUNC] = "trunc",
      [AST_TOK_KW_ZEXT] = "zext",
      [AST_TOK_KW_SEXT] = "sext",
      [AST_TOK_KW_BITCAST] = "bitcast",
      [AST_TOK_KW_PTRCAST] = "ptrcast",
      [AST_TOK_KW_ATOMIC] = "atomic",
      [AST_TOK_KW_TRUE] = "true",
      [AST_TOK_KW_FALSE] = "false",
      [AST_TOK_KW_VOID] = "void",
      [AST_TOK_KW_BOOL] = "bool",
      [AST_TOK_KW_I8] = "i8",
      [AST_TOK_KW_U8] = "u8",
      [AST_TOK_KW_I16] = "i16",
      [AST_TOK_KW_U16] = "u16",
      [AST_TOK_KW_I32] = "i32",
      [AST_TOK_KW_U32] = "u32",
      [AST_TOK_KW_I64] = "i64",
      [AST_TOK_KW_U64] = "u64",
      [AST_TOK_KW_I128] = "i128",
      [AST_TOK_KW_U128] = "u128",
      [AST_TOK_KW_ISIZE] = "isize",
      [AST_TOK_KW_USIZE] = "usize",
      [AST_TOK_KW_F16] = "f16",
      [AST_TOK_KW_BF16] = "bf16",
      [AST_TOK_KW_F32] = "f32",
      [AST_TOK_KW_F64] = "f64",
      [AST_TOK_KW_PTR] = "ptr",
      [AST_TOK_KW_CAP] = "cap",
      [AST_TOK_KW_BITS] = "bits",
      [AST_TOK_KW_VEC] = "vec",
      [AST_TOK_KW_MASK] = "mask",
      [AST_TOK_KW_SVEC] = "svec",
      [AST_TOK_KW_PRED] = "pred",
      [AST_TOK_KW_VOLATILE] = "volatile"};
  if ((unsigned)kind >= AST_TOK_COUNT || !names[kind])
    return "unknown";
  return names[kind];
}
