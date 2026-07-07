// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/lexer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FccKeywordEntry {
  const char* text;
  size_t length;
  FccTokenKind kind;
} FccKeywordEntry;

typedef struct FccInternedStringEntry {
  char* text;
  size_t length;
  uint64_t hash;
  bool occupied;
} FccInternedStringEntry;

typedef struct FccLexerStringPool {
  FccInternedStringEntry* entries;
  size_t entry_count;
  size_t entry_capacity;
} FccLexerStringPool;

/*
 * Identifier text is interned for the lifetime of the process. Later phases may
 * compare interned pointers as a fast path, but still use strcmp where text can
 * come from copied preprocessor output or other non-identical storage.
 */
static FccLexerStringPool FCC_LEXER_STRING_POOL = {0};

static uint64_t fcc_lexer_hash_bytes(const BYTE* text, size_t text_length) {
  uint64_t hash;
  size_t text_index;

  assert(text != NULL);

  hash = 14695981039346656037ULL;
  for (text_index = 0; text_index < text_length; ++text_index) {
    hash ^= (uint64_t)text[text_index];
    hash *= 1099511628211ULL;
  }

  return hash;
}

static bool fcc_lexer_string_pool_needs_grow(size_t entry_count, size_t entry_capacity) {
  if (entry_capacity == 0) {
    return true;
  }

  return ((entry_count + 1) * 10) >= (entry_capacity * 7);
}

static bool fcc_lexer_string_pool_rehash(size_t new_capacity) {
  FccInternedStringEntry* new_entries;
  size_t entry_index;

  if ((new_capacity == 0) || (new_capacity > (SIZE_MAX / sizeof(FccInternedStringEntry)))) {
    return false;
  }

  new_entries = (FccInternedStringEntry*)calloc(new_capacity, sizeof(FccInternedStringEntry));
  if (new_entries == NULL) {
    return false;
  }

  for (entry_index = 0; entry_index < FCC_LEXER_STRING_POOL.entry_capacity; ++entry_index) {
    FccInternedStringEntry* old_entry;

    old_entry = &FCC_LEXER_STRING_POOL.entries[entry_index];
    if (!old_entry->occupied) {
      continue;
    }

    {
      size_t slot_index;

      slot_index = (size_t)(old_entry->hash % (uint64_t)new_capacity);
      while (new_entries[slot_index].occupied) {
        ++slot_index;
        if (slot_index == new_capacity) {
          slot_index = 0;
        }
      }

      new_entries[slot_index] = *old_entry;
    }
  }

  free(FCC_LEXER_STRING_POOL.entries);
  FCC_LEXER_STRING_POOL.entries = new_entries;
  FCC_LEXER_STRING_POOL.entry_capacity = new_capacity;
  return true;
}

static bool fcc_lexer_string_pool_ensure_capacity(size_t required_entry_count) {
  size_t new_capacity;

  if (required_entry_count <= FCC_LEXER_STRING_POOL.entry_count) {
    return true;
  }

  if (!fcc_lexer_string_pool_needs_grow(FCC_LEXER_STRING_POOL.entry_count,
                                        FCC_LEXER_STRING_POOL.entry_capacity)) {
    return true;
  }

  new_capacity = FCC_LEXER_STRING_POOL.entry_capacity;
  if (new_capacity == 0) {
    new_capacity = 128;
  }

  while (((required_entry_count + 1) * 10) >= (new_capacity * 7)) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  return fcc_lexer_string_pool_rehash(new_capacity);
}

static const char* fcc_lexer_intern_identifier_text(const BYTE* text, size_t text_length) {
  uint64_t hash;
  size_t slot_index;

  assert(text != NULL);

  if (!fcc_lexer_string_pool_ensure_capacity(FCC_LEXER_STRING_POOL.entry_count + 1)) {
    return NULL;
  }

  hash = fcc_lexer_hash_bytes(text, text_length);
  slot_index = (size_t)(hash % (uint64_t)FCC_LEXER_STRING_POOL.entry_capacity);
  while (FCC_LEXER_STRING_POOL.entries[slot_index].occupied) {
    const FccInternedStringEntry* entry;

    entry = &FCC_LEXER_STRING_POOL.entries[slot_index];
    if ((entry->hash == hash) && (entry->length == text_length) &&
        (memcmp(entry->text, text, text_length) == 0)) {
      return entry->text;
    }

    ++slot_index;
    if (slot_index == FCC_LEXER_STRING_POOL.entry_capacity) {
      slot_index = 0;
    }
  }

  {
    char* interned_text;
    FccInternedStringEntry* new_entry;

    if (text_length >= SIZE_MAX) {
      return NULL;
    }

    interned_text = (char*)malloc(text_length + 1);
    if (interned_text == NULL) {
      return NULL;
    }

    memcpy(interned_text, text, text_length);
    interned_text[text_length] = '\0';
    new_entry = &FCC_LEXER_STRING_POOL.entries[slot_index];
    new_entry->text = interned_text;
    new_entry->length = text_length;
    new_entry->hash = hash;
    new_entry->occupied = true;
    ++FCC_LEXER_STRING_POOL.entry_count;
    return new_entry->text;
  }
}

static bool fcc_lexer_has_byte(const FccLexer* lexer, size_t lookahead) {
  assert(lexer != NULL);
  assert(lexer->source_file != NULL);

  return (lexer->next_offset + lookahead) < lexer->source_file->byte_count;
}

static BYTE fcc_lexer_peek_byte(const FccLexer* lexer, size_t lookahead) {
  if (!fcc_lexer_has_byte(lexer, lookahead)) {
    return 0;
  }

  return lexer->source_file->bytes[lexer->next_offset + lookahead];
}

static bool fcc_lexer_is_space(BYTE byte) {
  switch (byte) {
    case (BYTE)' ':
    case (BYTE)'\t':
    case (BYTE)'\n':
    case (BYTE)'\r':
    case (BYTE)'\f':
    case (BYTE)'\v':
      return true;
  }

  return false;
}

static bool fcc_lexer_is_ascii_letter(BYTE byte) {
  return (((byte >= (BYTE)'a') && (byte <= (BYTE)'z')) ||
          ((byte >= (BYTE)'A') && (byte <= (BYTE)'Z')));
}

static bool fcc_lexer_is_decimal_digit(BYTE byte) {
  return (byte >= (BYTE)'0') && (byte <= (BYTE)'9');
}

static bool fcc_lexer_is_hex_digit(BYTE byte) {
  return (fcc_lexer_is_decimal_digit(byte) || ((byte >= (BYTE)'a') && (byte <= (BYTE)'f')) ||
          ((byte >= (BYTE)'A') && (byte <= (BYTE)'F')));
}

static bool fcc_lexer_is_unsigned_suffix(BYTE byte) {
  return (byte == (BYTE)'u') || (byte == (BYTE)'U');
}

static bool fcc_lexer_is_long_suffix(BYTE byte) {
  return (byte == (BYTE)'l') || (byte == (BYTE)'L');
}

static bool fcc_lexer_is_identifier_start(BYTE byte) {
  return (fcc_lexer_is_ascii_letter(byte) || (byte == (BYTE)'_'));
}

static bool fcc_lexer_is_identifier_continue(BYTE byte) {
  return (fcc_lexer_is_identifier_start(byte) || fcc_lexer_is_decimal_digit(byte));
}

static void fcc_lexer_consume_integer_suffix(FccLexer* lexer) {
  assert(lexer != NULL);

  if (fcc_lexer_is_unsigned_suffix(fcc_lexer_peek_byte(lexer, 0))) {
    ++lexer->next_offset;
    if (fcc_lexer_is_long_suffix(fcc_lexer_peek_byte(lexer, 0))) {
      ++lexer->next_offset;
      if (fcc_lexer_is_long_suffix(fcc_lexer_peek_byte(lexer, 0))) {
        ++lexer->next_offset;
      }
    }

    return;
  }

  if (fcc_lexer_is_long_suffix(fcc_lexer_peek_byte(lexer, 0))) {
    ++lexer->next_offset;
    if (fcc_lexer_is_long_suffix(fcc_lexer_peek_byte(lexer, 0))) {
      ++lexer->next_offset;
    }

    if (fcc_lexer_is_unsigned_suffix(fcc_lexer_peek_byte(lexer, 0))) {
      ++lexer->next_offset;
    }
  }
}

static void fcc_lexer_make_token(FccToken* token, FccTokenKind kind, size_t begin_offset,
                                 size_t end_offset) {
  assert(token != NULL);
  assert(end_offset >= begin_offset);

  token->kind = kind;
  token->span.begin_offset = begin_offset;
  token->span.end_offset = end_offset;
  token->text_length = end_offset - begin_offset;
  token->interned_text = NULL;
}

static void fcc_lexer_emit_error(FccLexer* lexer, size_t begin_offset, size_t end_offset,
                                 const char* message) {
  FccSourceSpan span;

  assert(lexer != NULL);
  assert(message != NULL);

  span.begin_offset = begin_offset;
  span.end_offset = end_offset;
  fcc_diag_emit_source(lexer->diagnostics, lexer->source_file, span, FCC_DIAG_SEVERITY_ERROR,
                       message);
}

static FccTokenKind fcc_lexer_lookup_keyword(const BYTE* text, size_t text_length) {
  static const FccKeywordEntry FCC_KEYWORDS[] = {
      {"int", 3, FCC_TOKEN_KW_INT},           {"void", 4, FCC_TOKEN_KW_VOID},
      {"char", 4, FCC_TOKEN_KW_CHAR},         {"signed", 6, FCC_TOKEN_KW_SIGNED},
      {"unsigned", 8, FCC_TOKEN_KW_UNSIGNED}, {"short", 5, FCC_TOKEN_KW_SHORT},
      {"long", 4, FCC_TOKEN_KW_LONG},         {"_Bool", 5, FCC_TOKEN_KW_BOOL},
      {"struct", 6, FCC_TOKEN_KW_STRUCT},     {"union", 5, FCC_TOKEN_KW_UNION},
      {"enum", 4, FCC_TOKEN_KW_ENUM},         {"typedef", 7, FCC_TOKEN_KW_TYPEDEF},
      {"return", 6, FCC_TOKEN_KW_RETURN},     {"if", 2, FCC_TOKEN_KW_IF},
      {"else", 4, FCC_TOKEN_KW_ELSE},         {"while", 5, FCC_TOKEN_KW_WHILE},
      {"do", 2, FCC_TOKEN_KW_DO},             {"for", 3, FCC_TOKEN_KW_FOR},
      {"switch", 6, FCC_TOKEN_KW_SWITCH},
      {"case", 4, FCC_TOKEN_KW_CASE},         {"default", 7, FCC_TOKEN_KW_DEFAULT},
      {"goto", 4, FCC_TOKEN_KW_GOTO},         {"break", 5, FCC_TOKEN_KW_BREAK},
      {"continue", 8, FCC_TOKEN_KW_CONTINUE},
      {"static", 6, FCC_TOKEN_KW_STATIC},     {"extern", 6, FCC_TOKEN_KW_EXTERN},
      {"const", 5, FCC_TOKEN_KW_CONST},       {"sizeof", 6, FCC_TOKEN_KW_SIZEOF},
      {"_Alignof", 8, FCC_TOKEN_KW_ALIGNOF},
      {"_Static_assert", 14, FCC_TOKEN_KW_STATIC_ASSERT},
  };
  size_t keyword_index;

  assert(text != NULL);

  for (keyword_index = 0; keyword_index < (sizeof(FCC_KEYWORDS) / sizeof(FCC_KEYWORDS[0]));
       ++keyword_index) {
    const FccKeywordEntry* keyword_entry;

    keyword_entry = &FCC_KEYWORDS[keyword_index];
    if ((keyword_entry->length == text_length) &&
        (memcmp(text, keyword_entry->text, text_length) == 0)) {
      return keyword_entry->kind;
    }
  }

  return FCC_TOKEN_IDENTIFIER;
}

static bool fcc_lexer_skip_ignored(FccLexer* lexer, FccToken* token) {
  assert(lexer != NULL);
  assert(token != NULL);

  for (;;) {
    size_t comment_begin;

    if (!fcc_lexer_has_byte(lexer, 0)) {
      return false;
    }

    if (fcc_lexer_is_space(fcc_lexer_peek_byte(lexer, 0))) {
      ++lexer->next_offset;
      continue;
    }

    if ((fcc_lexer_peek_byte(lexer, 0) == (BYTE)'/') &&
        (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'/')) {
      lexer->next_offset += 2;
      while (fcc_lexer_has_byte(lexer, 0) && (fcc_lexer_peek_byte(lexer, 0) != (BYTE)'\n')) {
        ++lexer->next_offset;
      }

      continue;
    }

    if ((fcc_lexer_peek_byte(lexer, 0) == (BYTE)'/') &&
        (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'*')) {
      comment_begin = lexer->next_offset;
      lexer->next_offset += 2;

      while (fcc_lexer_has_byte(lexer, 0)) {
        if ((fcc_lexer_peek_byte(lexer, 0) == (BYTE)'*') &&
            (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'/')) {
          lexer->next_offset += 2;
          break;
        }

        ++lexer->next_offset;
      }

      if (!fcc_lexer_has_byte(lexer, 0) &&
          !((lexer->next_offset >= 2) &&
            (lexer->source_file->bytes[lexer->next_offset - 2] == (BYTE)'*') &&
            (lexer->source_file->bytes[lexer->next_offset - 1] == (BYTE)'/'))) {
        fcc_lexer_emit_error(lexer, comment_begin, lexer->source_file->byte_count,
                             "unterminated block comment");
        fcc_lexer_make_token(token, FCC_TOKEN_INVALID, comment_begin,
                             lexer->source_file->byte_count);
        lexer->next_offset = lexer->source_file->byte_count;
        return true;
      }

      continue;
    }

    return false;
  }
}

static void fcc_lexer_lex_identifier_or_keyword(FccLexer* lexer, FccToken* token) {
  size_t begin_offset;
  size_t end_offset;
  size_t text_length;
  const BYTE* text;
  FccTokenKind token_kind;
  const char* interned_text;

  begin_offset = lexer->next_offset;
  ++lexer->next_offset;

  while (fcc_lexer_is_identifier_continue(fcc_lexer_peek_byte(lexer, 0))) {
    ++lexer->next_offset;
  }

  end_offset = lexer->next_offset;
  text_length = end_offset - begin_offset;
  text = lexer->source_file->bytes + begin_offset;
  token_kind = fcc_lexer_lookup_keyword(text, text_length);
  fcc_lexer_make_token(token, token_kind, begin_offset, end_offset);
  if (token_kind != FCC_TOKEN_IDENTIFIER) {
    return;
  }

  interned_text = fcc_lexer_intern_identifier_text(text, text_length);
  if (interned_text == NULL) {
    fcc_lexer_emit_error(lexer, begin_offset, end_offset,
                         "out of memory while interning identifier");
    token->kind = FCC_TOKEN_INVALID;
    return;
  }

  token->interned_text = interned_text;
}

static void fcc_lexer_consume_identifier_tail(FccLexer* lexer) {
  while (fcc_lexer_is_identifier_continue(fcc_lexer_peek_byte(lexer, 0))) {
    ++lexer->next_offset;
  }
}

static void fcc_lexer_lex_float_placeholder(FccLexer* lexer, FccToken* token, size_t begin_offset) {
  if (fcc_lexer_peek_byte(lexer, 0) == (BYTE)'.') {
    ++lexer->next_offset;
    while (fcc_lexer_is_decimal_digit(fcc_lexer_peek_byte(lexer, 0))) {
      ++lexer->next_offset;
    }
  }

  if ((fcc_lexer_peek_byte(lexer, 0) == (BYTE)'e') ||
      (fcc_lexer_peek_byte(lexer, 0) == (BYTE)'E')) {
    ++lexer->next_offset;
    if ((fcc_lexer_peek_byte(lexer, 0) == (BYTE)'+') ||
        (fcc_lexer_peek_byte(lexer, 0) == (BYTE)'-')) {
      ++lexer->next_offset;
    }

    while (fcc_lexer_is_decimal_digit(fcc_lexer_peek_byte(lexer, 0))) {
      ++lexer->next_offset;
    }
  }

  fcc_lexer_emit_error(lexer, begin_offset, lexer->next_offset,
                       "floating literals are not supported yet");
  fcc_lexer_make_token(token, FCC_TOKEN_INVALID, begin_offset, lexer->next_offset);
}

static void fcc_lexer_lex_integer_literal(FccLexer* lexer, FccToken* token) {
  size_t begin_offset;
  bool is_hex_literal;

  begin_offset = lexer->next_offset;
  is_hex_literal = false;

  if ((fcc_lexer_peek_byte(lexer, 0) == (BYTE)'0') &&
      ((fcc_lexer_peek_byte(lexer, 1) == (BYTE)'x') ||
       (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'X'))) {
    is_hex_literal = true;
    lexer->next_offset += 2;

    if (!fcc_lexer_is_hex_digit(fcc_lexer_peek_byte(lexer, 0))) {
      fcc_lexer_consume_identifier_tail(lexer);
      fcc_lexer_emit_error(lexer, begin_offset, lexer->next_offset,
                           "invalid hexadecimal integer literal");
      fcc_lexer_make_token(token, FCC_TOKEN_INVALID, begin_offset, lexer->next_offset);
      return;
    }

    while (fcc_lexer_is_hex_digit(fcc_lexer_peek_byte(lexer, 0))) {
      ++lexer->next_offset;
    }
  } else {
    while (fcc_lexer_is_decimal_digit(fcc_lexer_peek_byte(lexer, 0))) {
      ++lexer->next_offset;
    }
  }

  if (!is_hex_literal && ((fcc_lexer_peek_byte(lexer, 0) == (BYTE)'.') ||
                          (fcc_lexer_peek_byte(lexer, 0) == (BYTE)'e') ||
                          (fcc_lexer_peek_byte(lexer, 0) == (BYTE)'E'))) {
    fcc_lexer_lex_float_placeholder(lexer, token, begin_offset);
    return;
  }

  fcc_lexer_consume_integer_suffix(lexer);
  if (fcc_lexer_is_identifier_continue(fcc_lexer_peek_byte(lexer, 0))) {
    fcc_lexer_consume_identifier_tail(lexer);
    fcc_lexer_emit_error(lexer, begin_offset, lexer->next_offset,
                         "invalid integer literal suffix or malformed integer literal");
    fcc_lexer_make_token(token, FCC_TOKEN_INVALID, begin_offset, lexer->next_offset);
    return;
  }

  fcc_lexer_make_token(token, FCC_TOKEN_INTEGER_LITERAL, begin_offset, lexer->next_offset);
}

static void fcc_lexer_lex_quoted_literal(FccLexer* lexer, FccToken* token, BYTE delimiter,
                                         FccTokenKind token_kind, const char* error_message) {
  size_t begin_offset;

  begin_offset = lexer->next_offset;
  ++lexer->next_offset;

  while (fcc_lexer_has_byte(lexer, 0)) {
    BYTE current_byte;

    current_byte = fcc_lexer_peek_byte(lexer, 0);
    if (current_byte == delimiter) {
      ++lexer->next_offset;
      fcc_lexer_make_token(token, token_kind, begin_offset, lexer->next_offset);
      return;
    }

    if ((current_byte == (BYTE)'\n') || (current_byte == (BYTE)'\r')) {
      fcc_lexer_emit_error(lexer, begin_offset, lexer->next_offset, error_message);
      fcc_lexer_make_token(token, FCC_TOKEN_INVALID, begin_offset, lexer->next_offset);
      return;
    }

    if (current_byte == (BYTE)'\\') {
      ++lexer->next_offset;
      if (!fcc_lexer_has_byte(lexer, 0)) {
        break;
      }
    }

    ++lexer->next_offset;
  }

  fcc_lexer_emit_error(lexer, begin_offset, lexer->source_file->byte_count, error_message);
  fcc_lexer_make_token(token, FCC_TOKEN_INVALID, begin_offset, lexer->source_file->byte_count);
  lexer->next_offset = lexer->source_file->byte_count;
}

static void fcc_lexer_lex_punctuation_or_operator(FccLexer* lexer, FccToken* token) {
  size_t begin_offset;
  BYTE current_byte;
  char error_message[FCC_MAX_DIAG_MESSAGE_LENGTH];

  begin_offset = lexer->next_offset;
  current_byte = fcc_lexer_peek_byte(lexer, 0);

  switch (current_byte) {
    case (BYTE)'(':
      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_LPAREN, begin_offset, lexer->next_offset);
      return;
    case (BYTE)')':
      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_RPAREN, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'{':
      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_LBRACE, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'}':
      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_RBRACE, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'[':
      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_LBRACKET, begin_offset, lexer->next_offset);
      return;
    case (BYTE)']':
      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_RBRACKET, begin_offset, lexer->next_offset);
      return;
    case (BYTE)';':
      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_SEMICOLON, begin_offset, lexer->next_offset);
      return;
    case (BYTE)',':
      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_COMMA, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'.':
      if ((fcc_lexer_peek_byte(lexer, 1) == (BYTE)'.') &&
          (fcc_lexer_peek_byte(lexer, 2) == (BYTE)'.')) {
        lexer->next_offset += 3;
        fcc_lexer_make_token(token, FCC_TOKEN_ELLIPSIS, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_DOT, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'?':
      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_QUESTION, begin_offset, lexer->next_offset);
      return;
    case (BYTE)':':
      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_COLON, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'+':
      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'+') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_INCREMENT, begin_offset, lexer->next_offset);
        return;
      }

      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_PLUS_ASSIGN, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_PLUS, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'-':
      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'-') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_DECREMENT, begin_offset, lexer->next_offset);
        return;
      }

      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'>') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_ARROW, begin_offset, lexer->next_offset);
        return;
      }

      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_MINUS_ASSIGN, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_MINUS, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'*':
      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_STAR_ASSIGN, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_STAR, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'/':
      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_SLASH_ASSIGN, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_SLASH, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'%':
      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_PERCENT_ASSIGN, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_PERCENT, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'=':
      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_EQUAL, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_ASSIGN, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'!':
      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_NOT_EQUAL, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_LOGICAL_NOT, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'<':
      if ((fcc_lexer_peek_byte(lexer, 1) == (BYTE)'<') &&
          (fcc_lexer_peek_byte(lexer, 2) == (BYTE)'=')) {
        lexer->next_offset += 3;
        fcc_lexer_make_token(token, FCC_TOKEN_LEFT_SHIFT_ASSIGN, begin_offset, lexer->next_offset);
        return;
      }

      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'<') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_LEFT_SHIFT, begin_offset, lexer->next_offset);
        return;
      }

      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_LESS_EQUAL, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_LESS, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'>':
      if ((fcc_lexer_peek_byte(lexer, 1) == (BYTE)'>') &&
          (fcc_lexer_peek_byte(lexer, 2) == (BYTE)'=')) {
        lexer->next_offset += 3;
        fcc_lexer_make_token(token, FCC_TOKEN_RIGHT_SHIFT_ASSIGN, begin_offset, lexer->next_offset);
        return;
      }

      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'>') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_RIGHT_SHIFT, begin_offset, lexer->next_offset);
        return;
      }

      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_GREATER_EQUAL, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_GREATER, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'&':
      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'&') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_LOGICAL_AND, begin_offset, lexer->next_offset);
        return;
      }

      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_BITWISE_AND_ASSIGN, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_BITWISE_AND, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'|':
      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'|') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_LOGICAL_OR, begin_offset, lexer->next_offset);
        return;
      }

      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_BITWISE_OR_ASSIGN, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_BITWISE_OR, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'^':
      if (fcc_lexer_peek_byte(lexer, 1) == (BYTE)'=') {
        lexer->next_offset += 2;
        fcc_lexer_make_token(token, FCC_TOKEN_BITWISE_XOR_ASSIGN, begin_offset, lexer->next_offset);
        return;
      }

      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_BITWISE_XOR, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'~':
      ++lexer->next_offset;
      fcc_lexer_make_token(token, FCC_TOKEN_BITWISE_NOT, begin_offset, lexer->next_offset);
      return;
    case (BYTE)'"':
      fcc_lexer_lex_quoted_literal(lexer, token, (BYTE)'"', FCC_TOKEN_STRING_LITERAL,
                                   "unterminated string literal");
      return;
    case (BYTE)'\'':
      fcc_lexer_lex_quoted_literal(lexer, token, (BYTE)'\'', FCC_TOKEN_CHAR_LITERAL,
                                   "unterminated character literal");
      return;
  }

  ++lexer->next_offset;
  (void)snprintf(error_message, sizeof(error_message), "invalid character 0x%02X",
                 (unsigned int)current_byte);
  fcc_lexer_emit_error(lexer, begin_offset, lexer->next_offset, error_message);
  fcc_lexer_make_token(token, FCC_TOKEN_INVALID, begin_offset, lexer->next_offset);
}

void fcc_lexer_init(FccLexer* lexer, const FccSourceFile* source_file,
                    FccDiagnostics* diagnostics) {
  assert(lexer != NULL);
  assert(source_file != NULL);
  assert(diagnostics != NULL);

  lexer->source_file = source_file;
  lexer->diagnostics = diagnostics;
  lexer->next_offset = 0;
}

void fcc_lexer_next(FccLexer* lexer, FccToken* token) {
  assert(lexer != NULL);
  assert(token != NULL);

  if (fcc_lexer_skip_ignored(lexer, token)) {
    return;
  }

  if (!fcc_lexer_has_byte(lexer, 0)) {
    fcc_lexer_make_token(token, FCC_TOKEN_END_OF_FILE, lexer->next_offset, lexer->next_offset);
    return;
  }

  if (fcc_lexer_is_identifier_start(fcc_lexer_peek_byte(lexer, 0))) {
    fcc_lexer_lex_identifier_or_keyword(lexer, token);
    return;
  }

  if (fcc_lexer_is_decimal_digit(fcc_lexer_peek_byte(lexer, 0))) {
    fcc_lexer_lex_integer_literal(lexer, token);
    return;
  }

  fcc_lexer_lex_punctuation_or_operator(lexer, token);
}
