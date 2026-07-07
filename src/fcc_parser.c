// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/parser.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fcc/lexer.h"
#include "fcc/token.h"

/*
 * The parser owns token buffering and C's typedef-name disambiguation. It does
 * not validate declarations beyond the syntax needed to build a tree; semantic
 * correctness is checked later by fcc_sema.
 */
typedef struct FccParser {
  const FccSourceFile* source_file;
  FccDiagnostics* diagnostics;
  FccAstContext* ast_context;
  FccToken* tokens;
  size_t token_count;
  size_t token_capacity;
  size_t next_token_index;
  size_t recursion_depth;
  const char** typedef_names;
  size_t typedef_count;
  size_t typedef_capacity;
  size_t* typedef_scope_offsets;
  size_t typedef_scope_count;
  size_t typedef_scope_capacity;
} FccParser;

static bool fcc_parser_ensure_typedef_capacity(FccParser* parser, size_t capacity) {
  size_t new_capacity;
  const char** new_names;

  assert(parser != NULL);

  if (capacity <= parser->typedef_capacity) {
    return true;
  }

  new_capacity = parser->typedef_capacity;
  if (new_capacity == 0) {
    new_capacity = 16;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(const char*))) {
    return false;
  }

  new_names = (const char**)realloc(parser->typedef_names, new_capacity * sizeof(const char*));
  if (new_names == NULL) {
    return false;
  }

  parser->typedef_names = new_names;
  parser->typedef_capacity = new_capacity;
  return true;
}

static bool fcc_parser_ensure_typedef_scope_capacity(FccParser* parser, size_t capacity) {
  size_t new_capacity;
  size_t* new_offsets;

  assert(parser != NULL);

  if (capacity <= parser->typedef_scope_capacity) {
    return true;
  }

  new_capacity = parser->typedef_scope_capacity;
  if (new_capacity == 0) {
    new_capacity = 8;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(size_t))) {
    return false;
  }

  new_offsets = (size_t*)realloc(parser->typedef_scope_offsets, new_capacity * sizeof(size_t));
  if (new_offsets == NULL) {
    return false;
  }

  parser->typedef_scope_offsets = new_offsets;
  parser->typedef_scope_capacity = new_capacity;
  return true;
}

static bool fcc_parser_push_typedef_scope(FccParser* parser) {
  assert(parser != NULL);

  if (!fcc_parser_ensure_typedef_scope_capacity(parser, parser->typedef_scope_count + 1)) {
    return false;
  }

  parser->typedef_scope_offsets[parser->typedef_scope_count] = parser->typedef_count;
  ++parser->typedef_scope_count;
  return true;
}

static void fcc_parser_pop_typedef_scope(FccParser* parser) {
  assert(parser != NULL);
  assert(parser->typedef_scope_count > 0);

  parser->typedef_count = parser->typedef_scope_offsets[parser->typedef_scope_count - 1];
  --parser->typedef_scope_count;
}

static bool fcc_parser_add_typedef_name(FccParser* parser, const char* typedef_name) {
  assert(parser != NULL);
  assert(typedef_name != NULL);
  assert(parser->typedef_scope_count > 0);

  if (!fcc_parser_ensure_typedef_capacity(parser, parser->typedef_count + 1)) {
    return false;
  }

  parser->typedef_names[parser->typedef_count] = typedef_name;
  ++parser->typedef_count;
  return true;
}

static bool fcc_parser_is_typedef_name(const FccParser* parser, const char* typedef_name) {
  size_t typedef_index;

  assert(parser != NULL);
  assert(typedef_name != NULL);

  for (typedef_index = parser->typedef_count; typedef_index > 0; --typedef_index) {
    if ((parser->typedef_names[typedef_index - 1] == typedef_name) ||
        (strcmp(parser->typedef_names[typedef_index - 1], typedef_name) == 0)) {
      return true;
    }
  }

  return false;
}

static bool fcc_parser_append_token(FccParser* parser, const FccToken* token) {
  size_t new_capacity;
  FccToken* new_tokens;

  assert(parser != NULL);
  assert(token != NULL);

  if (parser->token_count == parser->token_capacity) {
    new_capacity = parser->token_capacity;
    if (new_capacity == 0) {
      new_capacity = 64;
    } else {
      new_capacity *= 2;
    }

    if (new_capacity < parser->token_count) {
      return false;
    }

    if (new_capacity > (SIZE_MAX / sizeof(FccToken))) {
      return false;
    }

    new_tokens = (FccToken*)realloc(parser->tokens, new_capacity * sizeof(FccToken));
    if (new_tokens == NULL) {
      return false;
    }

    parser->tokens = new_tokens;
    parser->token_capacity = new_capacity;
  }

  parser->tokens[parser->token_count] = *token;
  ++parser->token_count;
  return true;
}

static bool fcc_parser_append_temp_pointer(void*** items, size_t* item_count, size_t* item_capacity,
                                           void* item) {
  size_t new_capacity;
  void** new_items;

  assert(items != NULL);
  assert(item_count != NULL);
  assert(item_capacity != NULL);

  if (*item_count == *item_capacity) {
    new_capacity = *item_capacity;
    if (new_capacity == 0) {
      new_capacity = 8;
    } else {
      new_capacity *= 2;
    }

    if (new_capacity < *item_count) {
      return false;
    }

    if (new_capacity > (SIZE_MAX / sizeof(void*))) {
      return false;
    }

    new_items = (void**)realloc(*items, new_capacity * sizeof(void*));
    if (new_items == NULL) {
      return false;
    }

    *items = new_items;
    *item_capacity = new_capacity;
  }

  (*items)[*item_count] = item;
  ++(*item_count);
  return true;
}

static bool fcc_parser_append_temp_parameter(FccAstParameter** parameters, size_t* parameter_count,
                                             size_t* parameter_capacity,
                                             const FccAstParameter* parameter) {
  size_t new_capacity;
  FccAstParameter* new_parameters;

  assert(parameters != NULL);
  assert(parameter_count != NULL);
  assert(parameter_capacity != NULL);
  assert(parameter != NULL);

  if (*parameter_count == *parameter_capacity) {
    new_capacity = *parameter_capacity;
    if (new_capacity == 0) {
      new_capacity = 4;
    } else {
      new_capacity *= 2;
    }

    if (new_capacity < *parameter_count) {
      return false;
    }

    if (new_capacity > (SIZE_MAX / sizeof(FccAstParameter))) {
      return false;
    }

    new_parameters = (FccAstParameter*)realloc(*parameters, new_capacity * sizeof(FccAstParameter));
    if (new_parameters == NULL) {
      return false;
    }

    *parameters = new_parameters;
    *parameter_capacity = new_capacity;
  }

  (*parameters)[*parameter_count] = *parameter;
  ++(*parameter_count);
  return true;
}

static bool fcc_parser_append_temp_array_bound(FccAstArrayBound** array_bounds, size_t* array_count,
                                               size_t* array_capacity,
                                               const FccAstArrayBound* array_bound) {
  size_t new_capacity;
  FccAstArrayBound* new_array_bounds;

  assert(array_bounds != NULL);
  assert(array_count != NULL);
  assert(array_capacity != NULL);
  assert(array_bound != NULL);

  if (*array_count == *array_capacity) {
    new_capacity = *array_capacity;
    if (new_capacity == 0) {
      new_capacity = 4;
    } else {
      new_capacity *= 2;
    }

    if (new_capacity < *array_count) {
      return false;
    }

    if (new_capacity > (SIZE_MAX / sizeof(FccAstArrayBound))) {
      return false;
    }

    new_array_bounds =
        (FccAstArrayBound*)realloc(*array_bounds, new_capacity * sizeof(FccAstArrayBound));
    if (new_array_bounds == NULL) {
      return false;
    }

    *array_bounds = new_array_bounds;
    *array_capacity = new_capacity;
  }

  (*array_bounds)[*array_count] = *array_bound;
  ++(*array_count);
  return true;
}

static bool fcc_parser_append_temp_record_field(FccAstRecordField** record_fields,
                                                size_t* field_count, size_t* field_capacity,
                                                const FccAstRecordField* field) {
  size_t new_capacity;
  FccAstRecordField* new_record_fields;

  assert(record_fields != NULL);
  assert(field_count != NULL);
  assert(field_capacity != NULL);
  assert(field != NULL);

  if (*field_count == *field_capacity) {
    new_capacity = *field_capacity;
    if (new_capacity == 0) {
      new_capacity = 4;
    } else {
      new_capacity *= 2;
    }

    if (new_capacity < *field_count) {
      return false;
    }

    if (new_capacity > (SIZE_MAX / sizeof(FccAstRecordField))) {
      return false;
    }

    new_record_fields =
        (FccAstRecordField*)realloc(*record_fields, new_capacity * sizeof(FccAstRecordField));
    if (new_record_fields == NULL) {
      return false;
    }

    *record_fields = new_record_fields;
    *field_capacity = new_capacity;
  }

  (*record_fields)[*field_count] = *field;
  ++(*field_count);
  return true;
}

static bool fcc_parser_append_temp_enumerator(FccAstEnumerator** enumerators,
                                              size_t* enumerator_count, size_t* enumerator_capacity,
                                              const FccAstEnumerator* enumerator) {
  size_t new_capacity;
  FccAstEnumerator* new_enumerators;

  assert(enumerators != NULL);
  assert(enumerator_count != NULL);
  assert(enumerator_capacity != NULL);
  assert(enumerator != NULL);

  if (*enumerator_count == *enumerator_capacity) {
    new_capacity = *enumerator_capacity;
    if (new_capacity == 0) {
      new_capacity = 8;
    } else {
      new_capacity *= 2;
    }

    if (new_capacity < *enumerator_count) {
      return false;
    }

    if (new_capacity > (SIZE_MAX / sizeof(FccAstEnumerator))) {
      return false;
    }

    new_enumerators =
        (FccAstEnumerator*)realloc(*enumerators, new_capacity * sizeof(FccAstEnumerator));
    if (new_enumerators == NULL) {
      return false;
    }

    *enumerators = new_enumerators;
    *enumerator_capacity = new_capacity;
  }

  (*enumerators)[*enumerator_count] = *enumerator;
  ++(*enumerator_count);
  return true;
}

static bool fcc_parser_copy_pointer_array(FccParser* parser, void*** copied_items, void** items,
                                          size_t item_count) {
  void** allocated_items;

  assert(parser != NULL);
  assert(copied_items != NULL);

  *copied_items = NULL;
  if (item_count == 0) {
    return true;
  }

  allocated_items =
      (void**)fcc_ast_context_allocate(parser->ast_context, item_count * sizeof(void*));
  if (allocated_items == NULL) {
    return false;
  }

  memcpy(allocated_items, items, item_count * sizeof(void*));
  *copied_items = allocated_items;
  return true;
}

static bool fcc_parser_copy_parameters(FccParser* parser, FccAstParameter** copied_parameters,
                                       const FccAstParameter* parameters, size_t parameter_count) {
  FccAstParameter* allocated_parameters;

  assert(parser != NULL);
  assert(copied_parameters != NULL);

  *copied_parameters = NULL;
  if (parameter_count == 0) {
    return true;
  }

  allocated_parameters = (FccAstParameter*)fcc_ast_context_allocate(
      parser->ast_context, parameter_count * sizeof(FccAstParameter));
  if (allocated_parameters == NULL) {
    return false;
  }

  memcpy(allocated_parameters, parameters, parameter_count * sizeof(FccAstParameter));
  *copied_parameters = allocated_parameters;
  return true;
}

static bool fcc_parser_copy_array_bounds(FccParser* parser, FccAstArrayBound** copied_array_bounds,
                                         const FccAstArrayBound* array_bounds, size_t array_count) {
  FccAstArrayBound* allocated_array_bounds;

  assert(parser != NULL);
  assert(copied_array_bounds != NULL);

  *copied_array_bounds = NULL;
  if (array_count == 0) {
    return true;
  }

  allocated_array_bounds = (FccAstArrayBound*)fcc_ast_context_allocate(
      parser->ast_context, array_count * sizeof(FccAstArrayBound));
  if (allocated_array_bounds == NULL) {
    return false;
  }

  memcpy(allocated_array_bounds, array_bounds, array_count * sizeof(FccAstArrayBound));
  *copied_array_bounds = allocated_array_bounds;
  return true;
}

static bool fcc_parser_copy_record_fields(FccParser* parser, FccAstRecordField** copied_fields,
                                          const FccAstRecordField* fields, size_t field_count) {
  FccAstRecordField* allocated_fields;

  assert(parser != NULL);
  assert(copied_fields != NULL);

  *copied_fields = NULL;
  if (field_count == 0) {
    return true;
  }

  allocated_fields = (FccAstRecordField*)fcc_ast_context_allocate(
      parser->ast_context, field_count * sizeof(FccAstRecordField));
  if (allocated_fields == NULL) {
    return false;
  }

  memcpy(allocated_fields, fields, field_count * sizeof(FccAstRecordField));
  *copied_fields = allocated_fields;
  return true;
}

static bool fcc_parser_copy_enumerators(FccParser* parser, FccAstEnumerator** copied_enumerators,
                                        const FccAstEnumerator* enumerators,
                                        size_t enumerator_count) {
  FccAstEnumerator* allocated_enumerators;

  assert(parser != NULL);
  assert(copied_enumerators != NULL);

  *copied_enumerators = NULL;
  if (enumerator_count == 0) {
    return true;
  }

  allocated_enumerators = (FccAstEnumerator*)fcc_ast_context_allocate(
      parser->ast_context, enumerator_count * sizeof(FccAstEnumerator));
  if (allocated_enumerators == NULL) {
    return false;
  }

  memcpy(allocated_enumerators, enumerators, enumerator_count * sizeof(FccAstEnumerator));
  *copied_enumerators = allocated_enumerators;
  return true;
}

static void fcc_parser_init_decl_specifiers(FccAstDeclSpecifiers* decl_specifiers) {
  assert(decl_specifiers != NULL);

  memset(decl_specifiers, 0, sizeof(*decl_specifiers));
  decl_specifiers->storage_class = FCC_AST_STORAGE_CLASS_NONE;
  decl_specifiers->type_kind = FCC_AST_TYPE_INT;
}

static void fcc_parser_init_declarator(FccAstDeclarator* declarator) {
  assert(declarator != NULL);

  memset(declarator, 0, sizeof(*declarator));
}

static const FccToken* fcc_parser_peek(const FccParser* parser, size_t lookahead);

static size_t fcc_parser_skip_pointer_qualifier_tokens(FccParser* parser, size_t token_offset) {
  assert(parser != NULL);

  while (fcc_parser_peek(parser, token_offset)->kind == FCC_TOKEN_KW_CONST) {
    ++token_offset;
  }

  return token_offset;
}

static size_t fcc_parser_skip_pointer_prefix_tokens(FccParser* parser, size_t token_offset) {
  assert(parser != NULL);

  while (fcc_parser_peek(parser, token_offset)->kind == FCC_TOKEN_STAR) {
    ++token_offset;
    token_offset = fcc_parser_skip_pointer_qualifier_tokens(parser, token_offset);
  }

  return token_offset;
}

static bool fcc_parser_current_named_declarator_has_identifier(FccParser* parser) {
  size_t token_offset;

  assert(parser != NULL);

  token_offset = fcc_parser_skip_pointer_prefix_tokens(parser, 0);
  if (fcc_parser_peek(parser, token_offset)->kind == FCC_TOKEN_IDENTIFIER) {
    return true;
  }

  if (fcc_parser_peek(parser, token_offset)->kind != FCC_TOKEN_LPAREN) {
    return false;
  }

  ++token_offset;
  token_offset = fcc_parser_skip_pointer_prefix_tokens(parser, token_offset);
  return fcc_parser_peek(parser, token_offset)->kind == FCC_TOKEN_IDENTIFIER;
}

static bool fcc_parser_token_is_named_identifier(const FccToken* token, const char* name) {
  assert(token != NULL);
  assert(name != NULL);

  return (token->kind == FCC_TOKEN_IDENTIFIER) && (token->interned_text != NULL) &&
         (strcmp(token->interned_text, name) == 0);
}

static bool fcc_parser_is_type_specifier_kind(const FccParser* parser, FccTokenKind kind) {
  const FccToken* token;

  assert(parser != NULL);

  if ((kind == FCC_TOKEN_KW_INT) || (kind == FCC_TOKEN_KW_VOID) || (kind == FCC_TOKEN_KW_CHAR) ||
      (kind == FCC_TOKEN_KW_SIGNED) || (kind == FCC_TOKEN_KW_UNSIGNED) ||
      (kind == FCC_TOKEN_KW_SHORT) || (kind == FCC_TOKEN_KW_LONG) || (kind == FCC_TOKEN_KW_BOOL) ||
      (kind == FCC_TOKEN_KW_STRUCT) || (kind == FCC_TOKEN_KW_UNION) ||
      (kind == FCC_TOKEN_KW_ENUM)) {
    return true;
  }

  if (kind != FCC_TOKEN_IDENTIFIER) {
    return false;
  }

  token = &parser->tokens[parser->next_token_index];
  return (token->interned_text != NULL) && fcc_parser_is_typedef_name(parser, token->interned_text);
}

static bool fcc_parser_is_declaration_start(const FccParser* parser) {
  const FccToken* token;

  assert(parser != NULL);

  token = &parser->tokens[parser->next_token_index];
  if (fcc_parser_token_is_named_identifier(token, "__declspec")) {
    return true;
  }

  switch (token->kind) {
    case FCC_TOKEN_KW_STATIC:
    case FCC_TOKEN_KW_EXTERN:
    case FCC_TOKEN_KW_CONST:
    case FCC_TOKEN_KW_TYPEDEF:
    case FCC_TOKEN_KW_STATIC_ASSERT:
      return true;
    default:
      return fcc_parser_is_type_specifier_kind(parser, token->kind);
  }
}

static bool fcc_parser_token_starts_type_name(const FccParser* parser, const FccToken* token) {
  assert(parser != NULL);
  assert(token != NULL);

  if ((token->kind == FCC_TOKEN_KW_STATIC) || (token->kind == FCC_TOKEN_KW_EXTERN) ||
      (token->kind == FCC_TOKEN_KW_CONST) || (token->kind == FCC_TOKEN_KW_TYPEDEF) ||
      (token->kind == FCC_TOKEN_KW_INT) || (token->kind == FCC_TOKEN_KW_VOID) ||
      (token->kind == FCC_TOKEN_KW_CHAR) || (token->kind == FCC_TOKEN_KW_SIGNED) ||
      (token->kind == FCC_TOKEN_KW_UNSIGNED) || (token->kind == FCC_TOKEN_KW_SHORT) ||
      (token->kind == FCC_TOKEN_KW_LONG) || (token->kind == FCC_TOKEN_KW_BOOL) ||
      (token->kind == FCC_TOKEN_KW_STRUCT) || (token->kind == FCC_TOKEN_KW_UNION) ||
      (token->kind == FCC_TOKEN_KW_ENUM)) {
    return true;
  }

  return (token->kind == FCC_TOKEN_IDENTIFIER) && (token->interned_text != NULL) &&
         fcc_parser_is_typedef_name(parser, token->interned_text);
}

static bool fcc_parser_emit_current_error(FccParser* parser, const char* message) {
  const FccToken* token;

  assert(parser != NULL);
  assert(message != NULL);

  token = &parser->tokens[parser->next_token_index];
  fcc_diag_emit_source(parser->diagnostics, parser->source_file, token->span,
                       FCC_DIAG_SEVERITY_ERROR, message);
  return false;
}

static bool fcc_parser_parse_static_assert(FccParser* parser,
                                           FccAstStaticAssert* static_assertion);

static bool fcc_parser_emit_token_error(FccParser* parser, const FccToken* token,
                                        const char* message) {
  assert(parser != NULL);
  assert(token != NULL);
  assert(message != NULL);

  fcc_diag_emit_source(parser->diagnostics, parser->source_file, token->span,
                       FCC_DIAG_SEVERITY_ERROR, message);
  return false;
}

static bool fcc_parser_out_of_memory(FccParser* parser) {
  FccSourceSpan span;

  assert(parser != NULL);

  span.begin_offset = 0;
  span.end_offset = 0;
  if (parser->token_count > 0) {
    span = parser->tokens[parser->next_token_index].span;
  }

  fcc_diag_emit_source(parser->diagnostics, parser->source_file, span, FCC_DIAG_SEVERITY_FATAL,
                       "out of memory");
  return false;
}

static bool fcc_parser_push_recursion(FccParser* parser) {
  assert(parser != NULL);

  ++parser->recursion_depth;
  if (parser->recursion_depth > FCC_MAX_PARSE_DEPTH) {
    --parser->recursion_depth;
    return fcc_parser_emit_current_error(parser, "parse nesting exceeds FCC_MAX_PARSE_DEPTH");
  }

  return true;
}

static void fcc_parser_pop_recursion(FccParser* parser) {
  assert(parser != NULL);
  assert(parser->recursion_depth > 0);

  --parser->recursion_depth;
}

static const FccToken* fcc_parser_current(const FccParser* parser) {
  assert(parser != NULL);
  assert(parser->token_count > 0);
  assert(parser->next_token_index < parser->token_count);

  return &parser->tokens[parser->next_token_index];
}

static const FccToken* fcc_parser_peek(const FccParser* parser, size_t lookahead) {
  size_t token_index;

  assert(parser != NULL);
  assert(parser->token_count > 0);

  token_index = parser->next_token_index + lookahead;
  if (token_index >= parser->token_count) {
    token_index = parser->token_count - 1;
  }

  return &parser->tokens[token_index];
}

static bool fcc_parser_is_current(const FccParser* parser, FccTokenKind kind) {
  return fcc_parser_current(parser)->kind == kind;
}

static const FccToken* fcc_parser_consume(FccParser* parser) {
  const FccToken* token;

  assert(parser != NULL);

  token = fcc_parser_current(parser);
  if (parser->next_token_index + 1 < parser->token_count) {
    ++parser->next_token_index;
  }

  return token;
}

static bool fcc_parser_match(FccParser* parser, FccTokenKind kind) {
  if (!fcc_parser_is_current(parser, kind)) {
    return false;
  }

  (void)fcc_parser_consume(parser);
  return true;
}

static bool fcc_parser_expect(FccParser* parser, FccTokenKind kind, const char* message) {
  if (!fcc_parser_is_current(parser, kind)) {
    return fcc_parser_emit_current_error(parser, message);
  }

  (void)fcc_parser_consume(parser);
  return true;
}

static bool fcc_parser_skip_declspec_attributes(FccParser* parser) {
  assert(parser != NULL);

  while (fcc_parser_token_is_named_identifier(fcc_parser_current(parser), "__declspec")) {
    size_t paren_depth;

    (void)fcc_parser_consume(parser);
    if (!fcc_parser_expect(parser, FCC_TOKEN_LPAREN, "expected '(' after __declspec")) {
      return false;
    }

    paren_depth = 1;
    while ((paren_depth != 0) && !fcc_parser_is_current(parser, FCC_TOKEN_END_OF_FILE)) {
      if (fcc_parser_is_current(parser, FCC_TOKEN_LPAREN)) {
        ++paren_depth;
      } else if (fcc_parser_is_current(parser, FCC_TOKEN_RPAREN)) {
        --paren_depth;
      }

      (void)fcc_parser_consume(parser);
    }

    if (paren_depth != 0) {
      return fcc_parser_emit_current_error(parser, "expected ')' after __declspec");
    }
  }

  return true;
}

static bool fcc_parser_get_identifier_name(const FccToken* token, const char** name_out) {
  assert(token != NULL);
  assert(name_out != NULL);

  if (token->kind != FCC_TOKEN_IDENTIFIER) {
    return false;
  }

  if (token->interned_text == NULL) {
    return false;
  }

  *name_out = token->interned_text;
  return true;
}

static bool fcc_parser_is_unsigned_integer_suffix(BYTE byte) {
  return (byte == (BYTE)'u') || (byte == (BYTE)'U');
}

static bool fcc_parser_is_long_integer_suffix(BYTE byte) {
  return (byte == (BYTE)'l') || (byte == (BYTE)'L');
}

static bool fcc_parser_validate_integer_suffix(const BYTE* text, size_t text_index,
                                               size_t text_length) {
  assert(text != NULL);
  assert(text_index <= text_length);

  if (text_index == text_length) {
    return true;
  }

  if (fcc_parser_is_unsigned_integer_suffix(text[text_index])) {
    ++text_index;
    if ((text_index < text_length) && fcc_parser_is_long_integer_suffix(text[text_index])) {
      ++text_index;
      if ((text_index < text_length) && fcc_parser_is_long_integer_suffix(text[text_index])) {
        ++text_index;
      }
    }

    return text_index == text_length;
  }

  if (fcc_parser_is_long_integer_suffix(text[text_index])) {
    ++text_index;
    if ((text_index < text_length) && fcc_parser_is_long_integer_suffix(text[text_index])) {
      ++text_index;
    }

    if ((text_index < text_length) && fcc_parser_is_unsigned_integer_suffix(text[text_index])) {
      ++text_index;
    }

    return text_index == text_length;
  }

  return false;
}

static bool fcc_parser_parse_integer_value(FccParser* parser, const FccToken* token,
                                           uint64_t* value_out) {
  const BYTE* text;
  size_t text_length;
  size_t text_index;
  uint64_t base;
  uint64_t value;

  assert(parser != NULL);
  assert(token != NULL);
  assert(value_out != NULL);

  text = parser->source_file->bytes + token->span.begin_offset;
  text_length = token->text_length;
  text_index = 0;
  base = 10;
  value = 0;

  if ((text_length >= 2) && (text[0] == (BYTE)'0') &&
      ((text[1] == (BYTE)'x') || (text[1] == (BYTE)'X'))) {
    base = 16;
    text_index = 2;
  }

  for (; text_index < text_length; ++text_index) {
    BYTE current_byte;
    uint64_t digit;

    current_byte = text[text_index];
    if (fcc_parser_is_unsigned_integer_suffix(current_byte) ||
        fcc_parser_is_long_integer_suffix(current_byte)) {
      break;
    }

    if ((current_byte >= (BYTE)'0') && (current_byte <= (BYTE)'9')) {
      digit = (uint64_t)(current_byte - (BYTE)'0');
    } else if ((current_byte >= (BYTE)'a') && (current_byte <= (BYTE)'f')) {
      digit = (uint64_t)(current_byte - (BYTE)'a' + 10);
    } else if ((current_byte >= (BYTE)'A') && (current_byte <= (BYTE)'F')) {
      digit = (uint64_t)(current_byte - (BYTE)'A' + 10);
    } else {
      return fcc_parser_emit_token_error(parser, token, "malformed integer literal");
    }

    if (digit >= base) {
      return fcc_parser_emit_token_error(parser, token, "malformed integer literal");
    }

    if (value > ((UINT64_MAX - digit) / base)) {
      return fcc_parser_emit_token_error(parser, token, "integer literal overflows uint64_t");
    }

    value = (value * base) + digit;
  }

  if (!fcc_parser_validate_integer_suffix(text, text_index, text_length)) {
    return fcc_parser_emit_token_error(parser, token, "malformed integer literal suffix");
  }

  *value_out = value;
  return true;
}

static bool fcc_parser_parse_declaration_specifiers(FccParser* parser, bool allow_void,
                                                    FccAstDeclSpecifiers* decl_specifiers,
                                                    FccAstType* type_summary);
static bool fcc_parser_parse_parameter_list(FccParser* parser, FccAstParameter** parameters,
                                            size_t* parameter_count, bool* is_variadic);

static bool fcc_parser_parse_expression(FccParser* parser, FccAstExpression** expression);

static void fcc_parser_skip_pointer_qualifiers(FccParser* parser);

static bool fcc_parser_parse_named_declarator(FccParser* parser, FccAstType* type,
                                              FccAstDeclarator* declarator,
                                              bool allow_array_suffixes,
                                              const FccToken** name_token_out);

static bool fcc_parser_parse_record_specifier(FccParser* parser, FccAstTypeKind record_kind,
                                              const FccToken* first_token,
                                              FccAstDeclSpecifiers* decl_specifiers,
                                              FccAstType* type_summary) {
  const FccToken* keyword_token;
  const FccToken* right_brace_token;
  FccAstRecordField* temp_fields;
  size_t temp_field_count;
  size_t temp_field_capacity;

  assert(parser != NULL);
  assert(first_token != NULL);
  assert(decl_specifiers != NULL);
  assert(type_summary != NULL);

  keyword_token = fcc_parser_consume(parser);
  decl_specifiers->type_kind = record_kind;
  decl_specifiers->span.begin_offset = first_token->span.begin_offset;
  decl_specifiers->span.end_offset = keyword_token->span.end_offset;

  type_summary->kind = record_kind;
  type_summary->span.begin_offset = first_token->span.begin_offset;
  type_summary->span.end_offset = keyword_token->span.end_offset;
  type_summary->tag_name = NULL;
  type_summary->record_fields = NULL;
  type_summary->record_field_count = 0;
  type_summary->is_record_definition = false;

  if (fcc_parser_is_current(parser, FCC_TOKEN_IDENTIFIER)) {
    const FccToken* tag_token;

    tag_token = fcc_parser_consume(parser);
    type_summary->tag_name = tag_token->interned_text;
    decl_specifiers->span.end_offset = tag_token->span.end_offset;
    type_summary->span.end_offset = tag_token->span.end_offset;
  }

  if (!fcc_parser_match(parser, FCC_TOKEN_LBRACE)) {
    if (type_summary->tag_name == NULL) {
      return fcc_parser_emit_current_error(parser,
                                           "expected identifier or '{' after record keyword");
    }

    return true;
  }

  type_summary->is_record_definition = true;
  temp_fields = NULL;
  temp_field_count = 0;
  temp_field_capacity = 0;
  while (!fcc_parser_is_current(parser, FCC_TOKEN_RBRACE)) {
    FccAstDeclSpecifiers field_decl_specifiers;
    FccAstDeclarator field_declarator;
    FccAstType field_type;
    FccAstRecordField field;
    const FccToken* field_name_token;
    const FccToken* semicolon_token;

    if (fcc_parser_is_current(parser, FCC_TOKEN_END_OF_FILE)) {
      free(temp_fields);
      return fcc_parser_emit_current_error(parser, "expected '}' to close record definition");
    }

    if (fcc_parser_is_current(parser, FCC_TOKEN_KW_STATIC_ASSERT)) {
      memset(&field, 0, sizeof(field));
      field.is_static_assert = true;
      if (!fcc_parser_parse_static_assert(parser, &field.static_assertion)) {
        free(temp_fields);
        return false;
      }

      field.span = field.static_assertion.span;
      if (!fcc_parser_append_temp_record_field(&temp_fields, &temp_field_count,
                                               &temp_field_capacity, &field)) {
        free(temp_fields);
        return fcc_parser_out_of_memory(parser);
      }

      continue;
    }

    if (!fcc_parser_parse_declaration_specifiers(parser, true, &field_decl_specifiers,
                                                 &field_type)) {
      free(temp_fields);
      return false;
    }

    if (!fcc_parser_parse_named_declarator(parser, &field_type, &field_declarator, true,
                                           &field_name_token)) {
      free(temp_fields);
      return false;
    }

    if (fcc_parser_is_current(parser, FCC_TOKEN_COMMA)) {
      free(temp_fields);
      return fcc_parser_emit_current_error(
          parser, "multiple declarators are not supported in record field declarations");
    }

    semicolon_token = fcc_parser_current(parser);
    if (!fcc_parser_expect(parser, FCC_TOKEN_SEMICOLON,
                           "expected ';' after record field declaration")) {
      free(temp_fields);
      return false;
    }

    memset(&field, 0, sizeof(field));
    field.syntax.decl_specifiers = field_decl_specifiers;
    field.syntax.declarator = field_declarator;
    field.type = field_type;
    field.span.begin_offset = field_type.span.begin_offset;
    field.span.end_offset = semicolon_token->span.end_offset;
    if (!fcc_parser_get_identifier_name(field_name_token, &field.name)) {
      free(temp_fields);
      return fcc_parser_emit_token_error(parser, field_name_token,
                                         "record field identifier interning failed");
    }

    if (!fcc_parser_append_temp_record_field(&temp_fields, &temp_field_count, &temp_field_capacity,
                                             &field)) {
      free(temp_fields);
      return fcc_parser_out_of_memory(parser);
    }
  }

  right_brace_token = fcc_parser_current(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_RBRACE, "expected '}' to close record definition")) {
    free(temp_fields);
    return false;
  }

  if (!fcc_parser_copy_record_fields(parser, &type_summary->record_fields, temp_fields,
                                     temp_field_count)) {
    free(temp_fields);
    return fcc_parser_out_of_memory(parser);
  }

  type_summary->record_field_count = temp_field_count;
  type_summary->span.end_offset = right_brace_token->span.end_offset;
  decl_specifiers->span.end_offset = right_brace_token->span.end_offset;
  free(temp_fields);
  return true;
}

static bool fcc_parser_parse_enum_specifier(FccParser* parser, const FccToken* first_token,
                                            FccAstDeclSpecifiers* decl_specifiers,
                                            FccAstType* type_summary) {
  const FccToken* enum_token;
  const FccToken* right_brace_token;
  FccAstEnumerator* temp_enumerators;
  size_t temp_enumerator_count;
  size_t temp_enumerator_capacity;

  assert(parser != NULL);
  assert(first_token != NULL);
  assert(decl_specifiers != NULL);
  assert(type_summary != NULL);

  enum_token = fcc_parser_consume(parser);
  decl_specifiers->type_kind = FCC_AST_TYPE_ENUM;
  decl_specifiers->span.begin_offset = first_token->span.begin_offset;
  decl_specifiers->span.end_offset = enum_token->span.end_offset;

  type_summary->kind = FCC_AST_TYPE_ENUM;
  type_summary->span.begin_offset = first_token->span.begin_offset;
  type_summary->span.end_offset = enum_token->span.end_offset;
  type_summary->tag_name = NULL;
  type_summary->enumerators = NULL;
  type_summary->enumerator_count = 0;
  type_summary->is_enum_definition = false;

  if (fcc_parser_is_current(parser, FCC_TOKEN_IDENTIFIER)) {
    const FccToken* tag_token;

    tag_token = fcc_parser_consume(parser);
    type_summary->tag_name = tag_token->interned_text;
    decl_specifiers->span.end_offset = tag_token->span.end_offset;
    type_summary->span.end_offset = tag_token->span.end_offset;
  }

  if (!fcc_parser_match(parser, FCC_TOKEN_LBRACE)) {
    if (type_summary->tag_name == NULL) {
      return fcc_parser_emit_current_error(parser, "expected identifier or '{' after enum");
    }

    return true;
  }

  type_summary->is_enum_definition = true;
  temp_enumerators = NULL;
  temp_enumerator_count = 0;
  temp_enumerator_capacity = 0;
  while (!fcc_parser_is_current(parser, FCC_TOKEN_RBRACE)) {
    FccAstEnumerator enumerator;
    const FccToken* name_token;

    if (fcc_parser_is_current(parser, FCC_TOKEN_END_OF_FILE)) {
      free(temp_enumerators);
      return fcc_parser_emit_current_error(parser, "expected '}' to close enum definition");
    }

    name_token = fcc_parser_current(parser);
    if (name_token->kind != FCC_TOKEN_IDENTIFIER) {
      free(temp_enumerators);
      return fcc_parser_emit_current_error(parser, "expected enumerator name");
    }

    memset(&enumerator, 0, sizeof(enumerator));
    enumerator.span = name_token->span;
    enumerator.name = name_token->interned_text;
    (void)fcc_parser_consume(parser);

    if (fcc_parser_match(parser, FCC_TOKEN_ASSIGN)) {
      enumerator.has_value = true;
      if (!fcc_parser_parse_expression(parser, &enumerator.value)) {
        free(temp_enumerators);
        return false;
      }

      enumerator.span.end_offset = enumerator.value->span.end_offset;
    }

    if (!fcc_parser_append_temp_enumerator(&temp_enumerators, &temp_enumerator_count,
                                           &temp_enumerator_capacity, &enumerator)) {
      free(temp_enumerators);
      return fcc_parser_out_of_memory(parser);
    }

    if (!fcc_parser_match(parser, FCC_TOKEN_COMMA)) {
      break;
    }

    if (fcc_parser_is_current(parser, FCC_TOKEN_RBRACE)) {
      break;
    }
  }

  if (temp_enumerator_count == 0) {
    free(temp_enumerators);
    return fcc_parser_emit_current_error(parser, "enum definition must contain enumerators");
  }

  right_brace_token = fcc_parser_current(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_RBRACE, "expected '}' to close enum definition")) {
    free(temp_enumerators);
    return false;
  }

  if (!fcc_parser_copy_enumerators(parser, &type_summary->enumerators, temp_enumerators,
                                   temp_enumerator_count)) {
    free(temp_enumerators);
    return fcc_parser_out_of_memory(parser);
  }

  type_summary->enumerator_count = temp_enumerator_count;
  type_summary->span.end_offset = right_brace_token->span.end_offset;
  decl_specifiers->span.end_offset = right_brace_token->span.end_offset;
  free(temp_enumerators);
  return true;
}

static bool fcc_parser_parse_declaration_specifiers(FccParser* parser, bool allow_void,
                                                    FccAstDeclSpecifiers* decl_specifiers,
                                                    FccAstType* type_summary) {
  const FccToken* first_token;
  const FccToken* token;

  assert(parser != NULL);
  assert(decl_specifiers != NULL);
  assert(type_summary != NULL);

  fcc_parser_init_decl_specifiers(decl_specifiers);
  memset(type_summary, 0, sizeof(*type_summary));
  if (!fcc_parser_skip_declspec_attributes(parser)) {
    return false;
  }

  first_token = fcc_parser_current(parser);

  while (true) {
    token = fcc_parser_current(parser);
    if (fcc_parser_token_is_named_identifier(token, "__declspec")) {
      if (!fcc_parser_skip_declspec_attributes(parser)) {
        return false;
      }

      if (decl_specifiers->span.begin_offset == 0) {
        first_token = fcc_parser_current(parser);
      }

      continue;
    }

    if (token->kind == FCC_TOKEN_KW_CONST) {
      decl_specifiers->is_const_qualified = true;
      if (decl_specifiers->span.begin_offset == 0) {
        decl_specifiers->span.begin_offset = token->span.begin_offset;
      }

      decl_specifiers->span.end_offset = token->span.end_offset;
      (void)fcc_parser_consume(parser);
      continue;
    }

    if ((token->kind == FCC_TOKEN_KW_STATIC) || (token->kind == FCC_TOKEN_KW_EXTERN) ||
        (token->kind == FCC_TOKEN_KW_TYPEDEF)) {
      FccAstStorageClass storage_class;

      storage_class = FCC_AST_STORAGE_CLASS_NONE;
      if (token->kind == FCC_TOKEN_KW_STATIC) {
        storage_class = FCC_AST_STORAGE_CLASS_STATIC;
      } else if (token->kind == FCC_TOKEN_KW_EXTERN) {
        storage_class = FCC_AST_STORAGE_CLASS_EXTERN;
      } else if (token->kind == FCC_TOKEN_KW_TYPEDEF) {
        storage_class = FCC_AST_STORAGE_CLASS_TYPEDEF;
      }

      if (decl_specifiers->storage_class != FCC_AST_STORAGE_CLASS_NONE) {
        return fcc_parser_emit_token_error(parser, token,
                                           "only one storage class specifier is supported");
      }

      decl_specifiers->storage_class = storage_class;
      if (decl_specifiers->span.begin_offset == 0) {
        decl_specifiers->span.begin_offset = token->span.begin_offset;
      }

      decl_specifiers->span.end_offset = token->span.end_offset;
      (void)fcc_parser_consume(parser);
      continue;
    }

    break;
  }

  token = fcc_parser_current(parser);
  if (allow_void && (token->kind == FCC_TOKEN_KW_VOID)) {
    decl_specifiers->type_kind = FCC_AST_TYPE_VOID;
    decl_specifiers->span.begin_offset = first_token->span.begin_offset;
    decl_specifiers->span.end_offset = token->span.end_offset;
    (void)fcc_parser_consume(parser);
  } else if ((token->kind == FCC_TOKEN_KW_INT) || (token->kind == FCC_TOKEN_KW_CHAR) ||
             (token->kind == FCC_TOKEN_KW_SIGNED) || (token->kind == FCC_TOKEN_KW_UNSIGNED) ||
             (token->kind == FCC_TOKEN_KW_SHORT) || (token->kind == FCC_TOKEN_KW_LONG) ||
             (token->kind == FCC_TOKEN_KW_BOOL)) {
    bool saw_signed;
    bool saw_unsigned;
    bool saw_char;
    bool saw_short;
    bool saw_int;
    bool saw_bool;
    size_t long_count;

    saw_signed = false;
    saw_unsigned = false;
    saw_char = false;
    saw_short = false;
    saw_int = false;
    saw_bool = false;
    long_count = 0;
    decl_specifiers->span.begin_offset = first_token->span.begin_offset;

    while (true) {
      token = fcc_parser_current(parser);
      if (token->kind == FCC_TOKEN_KW_SIGNED) {
        if (saw_signed || saw_unsigned || saw_bool) {
          return fcc_parser_emit_token_error(parser, token,
                                             "invalid signed type specifier combination");
        }

        saw_signed = true;
      } else if (token->kind == FCC_TOKEN_KW_UNSIGNED) {
        if (saw_signed || saw_unsigned || saw_bool) {
          return fcc_parser_emit_token_error(parser, token,
                                             "invalid unsigned type specifier combination");
        }

        saw_unsigned = true;
      } else if (token->kind == FCC_TOKEN_KW_CHAR) {
        if (saw_char || saw_short || saw_int || saw_bool || (long_count != 0)) {
          return fcc_parser_emit_token_error(parser, token,
                                             "invalid char type specifier combination");
        }

        saw_char = true;
      } else if (token->kind == FCC_TOKEN_KW_SHORT) {
        if (saw_char || saw_short || saw_bool || (long_count != 0)) {
          return fcc_parser_emit_token_error(parser, token,
                                             "invalid short type specifier combination");
        }

        saw_short = true;
      } else if (token->kind == FCC_TOKEN_KW_LONG) {
        if (saw_char || saw_short || saw_bool || (long_count >= 2)) {
          return fcc_parser_emit_token_error(parser, token,
                                             "invalid long type specifier combination");
        }

        ++long_count;
      } else if (token->kind == FCC_TOKEN_KW_INT) {
        if (saw_char || saw_int || saw_bool) {
          return fcc_parser_emit_token_error(parser, token,
                                             "invalid int type specifier combination");
        }

        saw_int = true;
      } else if (token->kind == FCC_TOKEN_KW_BOOL) {
        if (saw_signed || saw_unsigned || saw_char || saw_short || saw_int || (long_count != 0)) {
          return fcc_parser_emit_token_error(parser, token,
                                             "invalid _Bool type specifier combination");
        }

        saw_bool = true;
      } else {
        break;
      }

      decl_specifiers->span.end_offset = token->span.end_offset;
      (void)fcc_parser_consume(parser);
    }

    if (saw_bool) {
      decl_specifiers->type_kind = FCC_AST_TYPE_BOOL;
    } else if (saw_char) {
      if (saw_unsigned) {
        decl_specifiers->type_kind = FCC_AST_TYPE_UNSIGNED_CHAR;
      } else if (saw_signed) {
        decl_specifiers->type_kind = FCC_AST_TYPE_SIGNED_CHAR;
      } else {
        decl_specifiers->type_kind = FCC_AST_TYPE_CHAR;
      }
    } else if (saw_short) {
      decl_specifiers->type_kind = saw_unsigned ? FCC_AST_TYPE_UNSIGNED_SHORT : FCC_AST_TYPE_SHORT;
    } else if (long_count == 1) {
      decl_specifiers->type_kind = saw_unsigned ? FCC_AST_TYPE_UNSIGNED_LONG : FCC_AST_TYPE_LONG;
    } else if (long_count == 2) {
      decl_specifiers->type_kind =
          saw_unsigned ? FCC_AST_TYPE_UNSIGNED_LONG_LONG : FCC_AST_TYPE_LONG_LONG;
    } else if (saw_unsigned) {
      decl_specifiers->type_kind = FCC_AST_TYPE_UNSIGNED_INT;
    } else {
      decl_specifiers->type_kind = FCC_AST_TYPE_INT;
    }
  } else if ((token->kind == FCC_TOKEN_IDENTIFIER) && (token->interned_text != NULL) &&
             fcc_parser_is_typedef_name(parser, token->interned_text)) {
    decl_specifiers->type_kind = FCC_AST_TYPE_TYPEDEF_NAME;
    decl_specifiers->typedef_name = token->interned_text;
    decl_specifiers->span.begin_offset = first_token->span.begin_offset;
    decl_specifiers->span.end_offset = token->span.end_offset;
    (void)fcc_parser_consume(parser);
  } else if (token->kind == FCC_TOKEN_KW_STRUCT) {
    if (!fcc_parser_parse_record_specifier(parser, FCC_AST_TYPE_STRUCT, first_token,
                                           decl_specifiers, type_summary)) {
      return false;
    }
  } else if (token->kind == FCC_TOKEN_KW_UNION) {
    if (!fcc_parser_parse_record_specifier(parser, FCC_AST_TYPE_UNION, first_token, decl_specifiers,
                                           type_summary)) {
      return false;
    }
  } else if (token->kind == FCC_TOKEN_KW_ENUM) {
    if (!fcc_parser_parse_enum_specifier(parser, first_token, decl_specifiers, type_summary)) {
      return false;
    }
  } else {
    return fcc_parser_emit_token_error(parser, token, "expected type specifier");
  }

  type_summary->kind = decl_specifiers->type_kind;
  type_summary->span = decl_specifiers->span;
  type_summary->is_const_qualified = decl_specifiers->is_const_qualified;
  type_summary->typedef_name = decl_specifiers->typedef_name;
  return true;
}

static bool fcc_parser_parse_array_suffixes(FccParser* parser, FccAstType* type) {
  FccAstArrayBound* temp_array_bounds;
  size_t temp_array_count;
  size_t temp_array_capacity;

  assert(parser != NULL);
  assert(type != NULL);

  temp_array_bounds = NULL;
  temp_array_count = 0;
  temp_array_capacity = 0;
  while (fcc_parser_is_current(parser, FCC_TOKEN_LBRACKET)) {
    const FccToken* left_bracket_token;
    const FccToken* right_bracket_token;
    FccAstArrayBound array_bound;

    left_bracket_token = fcc_parser_consume(parser);
    memset(&array_bound, 0, sizeof(array_bound));
    array_bound.span.begin_offset = left_bracket_token->span.begin_offset;
    if (fcc_parser_is_current(parser, FCC_TOKEN_RBRACKET)) {
      right_bracket_token = fcc_parser_consume(parser);
      array_bound.span.end_offset = right_bracket_token->span.end_offset;
      array_bound.is_vla = true;
    } else {
      FccAstExpression* bound_expression;

      if (!fcc_parser_parse_expression(parser, &bound_expression)) {
        free(temp_array_bounds);
        return false;
      }

      array_bound.expression = bound_expression;
      if (bound_expression->kind == FCC_AST_EXPRESSION_INTEGER_LITERAL) {
        if (bound_expression->data.integer_literal.value > (uint64_t)SIZE_MAX) {
          free(temp_array_bounds);
          return fcc_parser_emit_current_error(parser, "array bound exceeds size_t range");
        }

        array_bound.element_count = (size_t)bound_expression->data.integer_literal.value;
      }

      array_bound.is_vla = false;
      right_bracket_token = fcc_parser_current(parser);
      if (!fcc_parser_expect(parser, FCC_TOKEN_RBRACKET, "expected ']' after array bound")) {
        free(temp_array_bounds);
        return false;
      }

      array_bound.span.end_offset = right_bracket_token->span.end_offset;
    }

    if (!fcc_parser_append_temp_array_bound(&temp_array_bounds, &temp_array_count,
                                            &temp_array_capacity, &array_bound)) {
      free(temp_array_bounds);
      return fcc_parser_out_of_memory(parser);
    }
  }

  if (!fcc_parser_copy_array_bounds(parser, &type->array_bounds, temp_array_bounds,
                                    temp_array_count)) {
    free(temp_array_bounds);
    return fcc_parser_out_of_memory(parser);
  }

  type->array_count = temp_array_count;
  if (type->array_count != 0) {
    type->span.end_offset = type->array_bounds[type->array_count - 1].span.end_offset;
  }

  free(temp_array_bounds);
  return true;
}

static bool fcc_parser_parse_named_declarator(FccParser* parser, FccAstType* type,
                                              FccAstDeclarator* declarator,
                                              bool allow_array_suffixes,
                                              const FccToken** name_token_out) {
  size_t leading_pointer_depth;

  assert(parser != NULL);
  assert(type != NULL);
  assert(declarator != NULL);
  assert(name_token_out != NULL);

  fcc_parser_init_declarator(declarator);

  leading_pointer_depth = 0;
  while (fcc_parser_is_current(parser, FCC_TOKEN_STAR)) {
    if (leading_pointer_depth == SIZE_MAX) {
      return fcc_parser_emit_current_error(parser, "declarator pointer depth exceeds size_t");
    }

    ++leading_pointer_depth;
    (void)fcc_parser_consume(parser);
    fcc_parser_skip_pointer_qualifiers(parser);
  }

  if (fcc_parser_is_current(parser, FCC_TOKEN_LPAREN) &&
      (fcc_parser_peek(parser, 1)->kind == FCC_TOKEN_STAR)) {
    const FccToken* inner_left_paren_token;
    const FccToken* inner_right_paren_token;
    const FccToken* parameter_right_paren_token;
    FccAstParameter* parameters;
    size_t parameter_count;
    bool is_variadic;
    size_t function_pointer_depth;

    inner_left_paren_token = fcc_parser_consume(parser);
    function_pointer_depth = 0;
    while (fcc_parser_is_current(parser, FCC_TOKEN_STAR)) {
      if (function_pointer_depth == SIZE_MAX) {
        return fcc_parser_emit_current_error(parser,
                                             "function pointer depth exceeds size_t");
      }

      ++function_pointer_depth;
      (void)fcc_parser_consume(parser);
      fcc_parser_skip_pointer_qualifiers(parser);
    }

    if (!fcc_parser_is_current(parser, FCC_TOKEN_IDENTIFIER)) {
      return fcc_parser_emit_current_error(parser,
                                           "expected identifier in function pointer declarator");
    }

    *name_token_out = fcc_parser_consume(parser);
    inner_right_paren_token = fcc_parser_current(parser);
    if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN,
                           "expected ')' after function pointer declarator")) {
      return false;
    }

    if (!fcc_parser_expect(parser, FCC_TOKEN_LPAREN,
                           "expected '(' after function pointer declarator")) {
      return false;
    }

    if (!fcc_parser_parse_parameter_list(parser, &parameters, &parameter_count, &is_variadic)) {
      return false;
    }

    parameter_right_paren_token = fcc_parser_current(parser);
    if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN,
                           "expected ')' after function pointer parameter list")) {
      return false;
    }

    if (allow_array_suffixes && fcc_parser_is_current(parser, FCC_TOKEN_LBRACKET)) {
      return fcc_parser_emit_current_error(
          parser, "array suffixes on function pointer declarators are not supported yet");
    }

    declarator->name = (*name_token_out)->interned_text;
    declarator->pointer_depth = leading_pointer_depth;
    declarator->is_function_pointer = true;
    declarator->function_pointer_depth = function_pointer_depth;
    declarator->function_pointer_parameters = parameters;
    declarator->function_pointer_parameter_count = parameter_count;
    declarator->function_pointer_is_variadic = is_variadic;
    declarator->span.begin_offset = type->span.begin_offset;
    declarator->span.end_offset = parameter_right_paren_token->span.end_offset;

    type->pointer_depth = leading_pointer_depth;
    type->is_function_pointer = true;
    type->function_pointer_depth = function_pointer_depth;
    type->function_pointer_parameters = parameters;
    type->function_pointer_parameter_count = parameter_count;
    type->function_pointer_is_variadic = is_variadic;
    type->span.end_offset = parameter_right_paren_token->span.end_offset;
    (void)inner_left_paren_token;
    (void)inner_right_paren_token;
    return true;
  }

  if (!fcc_parser_is_current(parser, FCC_TOKEN_IDENTIFIER)) {
    return fcc_parser_emit_current_error(parser, "expected identifier after declarator type");
  }

  *name_token_out = fcc_parser_consume(parser);
  declarator->name = (*name_token_out)->interned_text;
  declarator->pointer_depth = leading_pointer_depth;
  declarator->span.begin_offset = type->span.begin_offset;
  declarator->span.end_offset = (*name_token_out)->span.end_offset;
  type->pointer_depth = declarator->pointer_depth;
  type->span.end_offset = declarator->span.end_offset;
  if (allow_array_suffixes && !fcc_parser_parse_array_suffixes(parser, type)) {
    return false;
  }

  declarator->array_bounds = type->array_bounds;
  declarator->array_count = type->array_count;
  declarator->span.end_offset = type->span.end_offset;

  return true;
}

static bool fcc_parser_parse_abstract_declarator(FccParser* parser, FccAstType* type) {
  FccAstDeclarator declarator;
  size_t leading_pointer_depth;

  assert(parser != NULL);
  assert(type != NULL);

  fcc_parser_init_declarator(&declarator);
  leading_pointer_depth = 0;
  while (fcc_parser_is_current(parser, FCC_TOKEN_STAR)) {
    if (leading_pointer_depth == SIZE_MAX) {
      return fcc_parser_emit_current_error(parser, "declarator pointer depth exceeds size_t");
    }

    ++leading_pointer_depth;
    (void)fcc_parser_consume(parser);
    fcc_parser_skip_pointer_qualifiers(parser);
  }

  if (fcc_parser_is_current(parser, FCC_TOKEN_LPAREN) &&
      (fcc_parser_peek(parser, 1)->kind == FCC_TOKEN_STAR)) {
    const FccToken* parameter_right_paren_token;
    FccAstParameter* parameters;
    size_t parameter_count;
    bool is_variadic;
    size_t function_pointer_depth;

    (void)fcc_parser_consume(parser);
    function_pointer_depth = 0;
    while (fcc_parser_is_current(parser, FCC_TOKEN_STAR)) {
      if (function_pointer_depth == SIZE_MAX) {
        return fcc_parser_emit_current_error(parser,
                                             "function pointer depth exceeds size_t");
      }

      ++function_pointer_depth;
      (void)fcc_parser_consume(parser);
      fcc_parser_skip_pointer_qualifiers(parser);
    }

    if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN,
                           "expected ')' after abstract function pointer declarator")) {
      return false;
    }

    if (!fcc_parser_expect(parser, FCC_TOKEN_LPAREN,
                           "expected '(' after abstract function pointer declarator")) {
      return false;
    }

    if (!fcc_parser_parse_parameter_list(parser, &parameters, &parameter_count, &is_variadic)) {
      return false;
    }

    parameter_right_paren_token = fcc_parser_current(parser);
    if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN,
                           "expected ')' after abstract function pointer parameter list")) {
      return false;
    }

    if (fcc_parser_is_current(parser, FCC_TOKEN_LBRACKET)) {
      return fcc_parser_emit_current_error(
          parser, "array suffixes on function pointer declarators are not supported yet");
    }

    type->pointer_depth = leading_pointer_depth;
    type->is_function_pointer = true;
    type->function_pointer_depth = function_pointer_depth;
    type->function_pointer_parameters = parameters;
    type->function_pointer_parameter_count = parameter_count;
    type->function_pointer_is_variadic = is_variadic;
    type->span.end_offset = parameter_right_paren_token->span.end_offset;
    return true;
  }

  type->pointer_depth = leading_pointer_depth;
  if (!fcc_parser_parse_array_suffixes(parser, type)) {
    return false;
  }

  if (type->pointer_depth != 0) {
    type->span.end_offset = fcc_parser_peek(parser, 0)->span.begin_offset;
  }

  return true;
}

static bool fcc_parser_type_is_plain_void(const FccAstType* type) {
  assert(type != NULL);

  return (type->kind == FCC_AST_TYPE_VOID) && (type->pointer_depth == 0) &&
         (type->array_count == 0) && !type->is_function_pointer;
}

static void fcc_parser_skip_pointer_qualifiers(FccParser* parser) {
  assert(parser != NULL);

  while (fcc_parser_is_current(parser, FCC_TOKEN_KW_CONST)) {
    (void)fcc_parser_consume(parser);
  }
}

static bool fcc_parser_allocate_expression(FccParser* parser, FccAstExpression** expression) {
  *expression =
      (FccAstExpression*)fcc_ast_context_allocate(parser->ast_context, sizeof(FccAstExpression));
  if (*expression == NULL) {
    return fcc_parser_out_of_memory(parser);
  }

  return true;
}

static bool fcc_parser_allocate_statement(FccParser* parser, FccAstStatement** statement) {
  *statement =
      (FccAstStatement*)fcc_ast_context_allocate(parser->ast_context, sizeof(FccAstStatement));
  if (*statement == NULL) {
    return fcc_parser_out_of_memory(parser);
  }

  return true;
}

static bool fcc_parser_allocate_function(FccParser* parser,
                                         FccAstFunctionDefinition** function_definition) {
  *function_definition = (FccAstFunctionDefinition*)fcc_ast_context_allocate(
      parser->ast_context, sizeof(FccAstFunctionDefinition));
  if (*function_definition == NULL) {
    return fcc_parser_out_of_memory(parser);
  }

  return true;
}

static bool fcc_parser_allocate_global(FccParser* parser, FccAstGlobalVariable** global_variable) {
  *global_variable = (FccAstGlobalVariable*)fcc_ast_context_allocate(parser->ast_context,
                                                                     sizeof(FccAstGlobalVariable));
  if (*global_variable == NULL) {
    return fcc_parser_out_of_memory(parser);
  }

  return true;
}

static bool fcc_parser_allocate_static_assert(FccParser* parser,
                                              FccAstStaticAssert** static_assertion) {
  *static_assertion =
      (FccAstStaticAssert*)fcc_ast_context_allocate(parser->ast_context, sizeof(FccAstStaticAssert));
  if (*static_assertion == NULL) {
    return fcc_parser_out_of_memory(parser);
  }

  return true;
}

static bool fcc_parser_allocate_translation_unit(FccParser* parser,
                                                 FccAstTranslationUnit** translation_unit) {
  *translation_unit = (FccAstTranslationUnit*)fcc_ast_context_allocate(
      parser->ast_context, sizeof(FccAstTranslationUnit));
  if (*translation_unit == NULL) {
    return fcc_parser_out_of_memory(parser);
  }

  return true;
}

static bool fcc_parser_parse_expression(FccParser* parser, FccAstExpression** expression);
static bool fcc_parser_parse_statement(FccParser* parser, FccAstStatement** statement);
static bool fcc_parser_parse_primary_expression(FccParser* parser, FccAstExpression** expression);
static bool fcc_parser_parse_unary_expression(FccParser* parser, FccAstExpression** expression);
static bool fcc_parser_parse_argument_list(FccParser* parser, FccAstExpression*** arguments,
                                           size_t* argument_count);
static bool fcc_parser_parse_sizeof_expression(FccParser* parser, const FccToken* sizeof_token,
                                               FccAstExpression** expression);
static bool fcc_parser_parse_alignof_expression(FccParser* parser, const FccToken* alignof_token,
                                                FccAstExpression** expression);

static bool fcc_parser_parse_postfix_expression(FccParser* parser, FccAstExpression** expression) {
  FccAstExpression* left;

  assert(parser != NULL);
  assert(expression != NULL);

  if (!fcc_parser_parse_primary_expression(parser, &left)) {
    return false;
  }

  for (;;) {
    if (fcc_parser_is_current(parser, FCC_TOKEN_LPAREN)) {
      const FccToken* left_paren;
      const FccToken* right_paren;
      FccAstExpression** arguments;
      size_t argument_count;
      FccAstExpression* node;

      left_paren = fcc_parser_consume(parser);
      arguments = NULL;
      argument_count = 0;
      if (!fcc_parser_parse_argument_list(parser, &arguments, &argument_count)) {
        return false;
      }

      right_paren = fcc_parser_current(parser);
      if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN, "expected ')' after argument list")) {
        return false;
      }

      if (!fcc_parser_allocate_expression(parser, &node)) {
        return false;
      }

      node->kind = FCC_AST_EXPRESSION_CALL;
      node->span.begin_offset = left->span.begin_offset;
      node->span.end_offset = right_paren->span.end_offset;
      node->data.call.callee = left;
      node->data.call.arguments = arguments;
      node->data.call.argument_count = argument_count;
      (void)left_paren;
      left = node;
      continue;
    }

    if (fcc_parser_is_current(parser, FCC_TOKEN_LBRACKET)) {
      const FccToken* right_bracket;
      FccAstExpression* index;
      FccAstExpression* node;

      (void)fcc_parser_consume(parser);
      if (!fcc_parser_parse_expression(parser, &index)) {
        return false;
      }

      right_bracket = fcc_parser_current(parser);
      if (!fcc_parser_expect(parser, FCC_TOKEN_RBRACKET, "expected ']' after subscript")) {
        return false;
      }

      if (!fcc_parser_allocate_expression(parser, &node)) {
        return false;
      }

      node->kind = FCC_AST_EXPRESSION_SUBSCRIPT;
      node->span.begin_offset = left->span.begin_offset;
      node->span.end_offset = right_bracket->span.end_offset;
      node->data.subscript.target = left;
      node->data.subscript.index = index;
      left = node;
      continue;
    }

    if (fcc_parser_is_current(parser, FCC_TOKEN_DOT) ||
        fcc_parser_is_current(parser, FCC_TOKEN_ARROW)) {
      const FccToken* operator_token;
      const FccToken* field_token;
      FccAstExpression* node;
      const char* field_name;

      operator_token = fcc_parser_consume(parser);
      field_token = fcc_parser_current(parser);
      if (field_token->kind != FCC_TOKEN_IDENTIFIER) {
        return fcc_parser_emit_token_error(parser, field_token,
                                           "expected field name after member access operator");
      }

      if (!fcc_parser_get_identifier_name(field_token, &field_name)) {
        return fcc_parser_emit_token_error(parser, field_token,
                                           "identifier interning failed in parser");
      }

      (void)fcc_parser_consume(parser);
      if (!fcc_parser_allocate_expression(parser, &node)) {
        return false;
      }

      node->kind = FCC_AST_EXPRESSION_MEMBER;
      node->span.begin_offset = left->span.begin_offset;
      node->span.end_offset = field_token->span.end_offset;
      node->data.member.target = left;
      node->data.member.field_name = field_name;
      node->data.member.is_arrow = operator_token->kind == FCC_TOKEN_ARROW;
      left = node;
      continue;
    }

    if (fcc_parser_is_current(parser, FCC_TOKEN_INCREMENT) ||
        fcc_parser_is_current(parser, FCC_TOKEN_DECREMENT)) {
      const FccToken* operator_token;
      FccAstExpression* node;

      operator_token = fcc_parser_consume(parser);
      if (!fcc_parser_allocate_expression(parser, &node)) {
        return false;
      }

      node->kind = FCC_AST_EXPRESSION_UPDATE;
      node->span.begin_offset = left->span.begin_offset;
      node->span.end_offset = operator_token->span.end_offset;
      node->data.update.op_kind = operator_token->kind == FCC_TOKEN_INCREMENT
                                      ? FCC_AST_UPDATE_INCREMENT
                                      : FCC_AST_UPDATE_DECREMENT;
      node->data.update.target = left;
      node->data.update.is_postfix = true;
      left = node;
      continue;
    }

    break;
  }

  *expression = left;
  return true;
}

static bool fcc_parser_copy_string_literal(FccParser* parser, const FccToken* token,
                                           const char** bytes_out, size_t* length_out) {
  BYTE* decoded_bytes;
  BYTE* owned_bytes;
  const BYTE* source_bytes;
  size_t source_index;
  size_t decoded_length;

  assert(parser != NULL);
  assert(token != NULL);
  assert(bytes_out != NULL);
  assert(length_out != NULL);

  *bytes_out = NULL;
  *length_out = 0;
  if (token->text_length < 2) {
    return fcc_parser_emit_token_error(parser, token, "invalid string literal token");
  }

  decoded_bytes = (BYTE*)malloc(token->text_length);
  if (decoded_bytes == NULL) {
    return fcc_parser_out_of_memory(parser);
  }

  source_bytes = parser->source_file->bytes + token->span.begin_offset;
  decoded_length = 0;
  for (source_index = 1; source_index + 1 < token->text_length; ++source_index) {
    BYTE current_byte;

    current_byte = source_bytes[source_index];
    if (current_byte != (BYTE)'\\') {
      decoded_bytes[decoded_length] = current_byte;
      ++decoded_length;
      continue;
    }

    ++source_index;
    if (source_index + 1 > token->text_length) {
      free(decoded_bytes);
      return fcc_parser_emit_token_error(parser, token, "invalid string literal escape");
    }

    switch (source_bytes[source_index]) {
      case (BYTE)'\\':
        decoded_bytes[decoded_length] = (BYTE)'\\';
        break;
      case (BYTE)'"':
        decoded_bytes[decoded_length] = (BYTE)'"';
        break;
      case (BYTE)'\'':
        decoded_bytes[decoded_length] = (BYTE)'\'';
        break;
      case (BYTE)'?':
        decoded_bytes[decoded_length] = (BYTE)'?';
        break;
      case (BYTE)'a':
        decoded_bytes[decoded_length] = (BYTE)'\a';
        break;
      case (BYTE)'b':
        decoded_bytes[decoded_length] = (BYTE)'\b';
        break;
      case (BYTE)'f':
        decoded_bytes[decoded_length] = (BYTE)'\f';
        break;
      case (BYTE)'n':
        decoded_bytes[decoded_length] = (BYTE)'\n';
        break;
      case (BYTE)'r':
        decoded_bytes[decoded_length] = (BYTE)'\r';
        break;
      case (BYTE)'t':
        decoded_bytes[decoded_length] = (BYTE)'\t';
        break;
      case (BYTE)'v':
        decoded_bytes[decoded_length] = (BYTE)'\v';
        break;
      case (BYTE)'0':
        decoded_bytes[decoded_length] = (BYTE)'\0';
        break;
      default:
        decoded_bytes[decoded_length] = source_bytes[source_index];
        break;
    }

    ++decoded_length;
  }

  owned_bytes = (BYTE*)fcc_ast_context_allocate(parser->ast_context, decoded_length + 1);
  if (owned_bytes == NULL) {
    free(decoded_bytes);
    return fcc_parser_out_of_memory(parser);
  }

  memcpy(owned_bytes, decoded_bytes, decoded_length);
  owned_bytes[decoded_length] = '\0';
  free(decoded_bytes);
  *bytes_out = (const char*)owned_bytes;
  *length_out = decoded_length;
  return true;
}

static bool fcc_parser_parse_string_literal_expression(FccParser* parser,
                                                      FccAstExpression** expression) {
  const FccToken* first_token;
  const FccToken* last_token;
  BYTE* temp_bytes;
  size_t temp_length;
  FccAstExpression* node;
  BYTE* owned_bytes;

  assert(parser != NULL);
  assert(expression != NULL);

  first_token = fcc_parser_current(parser);
  last_token = first_token;
  temp_bytes = NULL;
  temp_length = 0;
  while (fcc_parser_is_current(parser, FCC_TOKEN_STRING_LITERAL)) {
    const FccToken* token;
    const char* decoded_bytes;
    size_t decoded_length;
    BYTE* new_temp_bytes;

    token = fcc_parser_current(parser);
    if (!fcc_parser_copy_string_literal(parser, token, &decoded_bytes, &decoded_length)) {
      free(temp_bytes);
      return false;
    }

    if (decoded_length > (SIZE_MAX - temp_length)) {
      free(temp_bytes);
      return fcc_parser_emit_token_error(parser, token, "string literal is too large");
    }

    new_temp_bytes = (BYTE*)realloc(temp_bytes, temp_length + decoded_length);
    if ((new_temp_bytes == NULL) && (temp_length + decoded_length != 0)) {
      free(temp_bytes);
      return fcc_parser_out_of_memory(parser);
    }

    temp_bytes = new_temp_bytes;
    if (decoded_length != 0) {
      memcpy(temp_bytes + temp_length, decoded_bytes, decoded_length);
    }

    temp_length += decoded_length;
    last_token = token;
    (void)fcc_parser_consume(parser);
  }

  if (!fcc_parser_allocate_expression(parser, &node)) {
    free(temp_bytes);
    return false;
  }

  owned_bytes = (BYTE*)fcc_ast_context_allocate(parser->ast_context, temp_length + 1);
  if (owned_bytes == NULL) {
    free(temp_bytes);
    return fcc_parser_out_of_memory(parser);
  }

  if (temp_length != 0) {
    memcpy(owned_bytes, temp_bytes, temp_length);
  }

  owned_bytes[temp_length] = '\0';
  free(temp_bytes);
  node->kind = FCC_AST_EXPRESSION_STRING_LITERAL;
  node->span.begin_offset = first_token->span.begin_offset;
  node->span.end_offset = last_token->span.end_offset;
  node->data.string_literal.bytes = (const char*)owned_bytes;
  node->data.string_literal.length = temp_length;
  *expression = node;
  return true;
}

static bool fcc_parser_parse_static_assert(FccParser* parser,
                                           FccAstStaticAssert* static_assertion) {
  FccAstExpression* condition;
  FccAstExpression* message_expression;
  const FccToken* static_assert_token;
  const FccToken* semicolon_token;

  assert(parser != NULL);
  assert(static_assertion != NULL);

  static_assert_token = fcc_parser_current(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_KW_STATIC_ASSERT,
                         "expected _Static_assert declaration")) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_LPAREN, "expected '(' after _Static_assert")) {
    return false;
  }

  if (!fcc_parser_parse_expression(parser, &condition)) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_COMMA,
                         "expected ',' after _Static_assert expression")) {
    return false;
  }

  if (!fcc_parser_is_current(parser, FCC_TOKEN_STRING_LITERAL)) {
    return fcc_parser_emit_current_error(parser,
                                         "expected string literal in _Static_assert declaration");
  }

  if (!fcc_parser_parse_string_literal_expression(parser, &message_expression)) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN, "expected ')' after _Static_assert message")) {
    return false;
  }

  semicolon_token = fcc_parser_current(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_SEMICOLON,
                         "expected ';' after _Static_assert declaration")) {
    return false;
  }

  memset(static_assertion, 0, sizeof(*static_assertion));
  static_assertion->span.begin_offset = static_assert_token->span.begin_offset;
  static_assertion->span.end_offset = semicolon_token->span.end_offset;
  static_assertion->condition = condition;
  static_assertion->message = message_expression->data.string_literal.bytes;
  static_assertion->message_length = message_expression->data.string_literal.length;
  return true;
}

static bool fcc_parser_parse_char_literal_value(FccParser* parser, const FccToken* token,
                                                uint64_t* value_out) {
  const BYTE* source_bytes;
  size_t source_index;
  BYTE value;

  assert(parser != NULL);
  assert(token != NULL);
  assert(value_out != NULL);

  if (token->text_length < 3) {
    return fcc_parser_emit_token_error(parser, token, "invalid character literal token");
  }

  source_bytes = parser->source_file->bytes + token->span.begin_offset;
  source_index = 1;
  if (source_bytes[source_index] != (BYTE)'\\') {
    value = source_bytes[source_index];
    ++source_index;
  } else {
    ++source_index;
    if (source_index + 1 >= token->text_length) {
      return fcc_parser_emit_token_error(parser, token, "invalid character literal escape");
    }

    switch (source_bytes[source_index]) {
      case (BYTE)'\\':
        value = (BYTE)'\\';
        break;
      case (BYTE)'\'':
        value = (BYTE)'\'';
        break;
      case (BYTE)'"':
        value = (BYTE)'"';
        break;
      case (BYTE)'?':
        value = (BYTE)'?';
        break;
      case (BYTE)'a':
        value = (BYTE)'\a';
        break;
      case (BYTE)'b':
        value = (BYTE)'\b';
        break;
      case (BYTE)'f':
        value = (BYTE)'\f';
        break;
      case (BYTE)'n':
        value = (BYTE)'\n';
        break;
      case (BYTE)'r':
        value = (BYTE)'\r';
        break;
      case (BYTE)'t':
        value = (BYTE)'\t';
        break;
      case (BYTE)'v':
        value = (BYTE)'\v';
        break;
      case (BYTE)'0':
        value = (BYTE)'\0';
        break;
      default:
        value = source_bytes[source_index];
        break;
    }

    ++source_index;
  }

  if ((source_index + 1) != token->text_length) {
    return fcc_parser_emit_token_error(parser, token,
                                       "multi-character literals are not supported yet");
  }

  *value_out = (uint64_t)value;
  return true;
}

static bool fcc_parser_parse_primary_expression(FccParser* parser, FccAstExpression** expression) {
  const FccToken* token;
  FccAstExpression* node;

  token = fcc_parser_current(parser);
  switch (token->kind) {
    case FCC_TOKEN_INTEGER_LITERAL:
      if (!fcc_parser_allocate_expression(parser, &node)) {
        return false;
      }

      if (!fcc_parser_parse_integer_value(parser, token, &node->data.integer_literal.value)) {
        return false;
      }

      node->kind = FCC_AST_EXPRESSION_INTEGER_LITERAL;
      node->span = token->span;
      (void)fcc_parser_consume(parser);
      *expression = node;
      return true;
    case FCC_TOKEN_CHAR_LITERAL:
      if (!fcc_parser_allocate_expression(parser, &node)) {
        return false;
      }

      if (!fcc_parser_parse_char_literal_value(parser, token, &node->data.integer_literal.value)) {
        return false;
      }

      node->kind = FCC_AST_EXPRESSION_INTEGER_LITERAL;
      node->span = token->span;
      (void)fcc_parser_consume(parser);
      *expression = node;
      return true;
    case FCC_TOKEN_IDENTIFIER:
      if (!fcc_parser_allocate_expression(parser, &node)) {
        return false;
      }

      if (!fcc_parser_get_identifier_name(token, &node->data.identifier.name)) {
        return fcc_parser_emit_token_error(parser, token, "identifier interning failed in parser");
      }

      node->kind = FCC_AST_EXPRESSION_IDENTIFIER;
      node->span = token->span;
      (void)fcc_parser_consume(parser);
      *expression = node;
      return true;
    case FCC_TOKEN_STRING_LITERAL:
      return fcc_parser_parse_string_literal_expression(parser, expression);
    case FCC_TOKEN_LPAREN:
      (void)fcc_parser_consume(parser);
      if (!fcc_parser_parse_expression(parser, expression)) {
        return false;
      }

      if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN,
                             "expected ')' after parenthesized expression")) {
        return false;
      }

      return true;
    default:
      return fcc_parser_emit_current_error(parser, "expected primary expression");
  }
}

static bool fcc_parser_parse_sizeof_expression(FccParser* parser, const FccToken* sizeof_token,
                                               FccAstExpression** expression) {
  FccAstDeclSpecifiers decl_specifiers;
  FccAstExpression* node;
  FccAstExpression* operand;
  FccAstType type_operand;
  const FccToken* rparen_token;
  bool has_type_operand;
  bool starts_type_name;

  assert(parser != NULL);
  assert(sizeof_token != NULL);
  assert(expression != NULL);

  operand = NULL;
  has_type_operand = false;
  starts_type_name = false;
  if (fcc_parser_is_current(parser, FCC_TOKEN_LPAREN)) {
    const FccToken* next_token;

    next_token = fcc_parser_peek(parser, 1);
    if (fcc_parser_token_starts_type_name(parser, next_token)) {
      starts_type_name = true;
    }
  }

  if (starts_type_name) {
    (void)fcc_parser_consume(parser);
    if (!fcc_parser_parse_declaration_specifiers(parser, true, &decl_specifiers, &type_operand)) {
      return false;
    }

    if (!fcc_parser_parse_abstract_declarator(parser, &type_operand)) {
      return false;
    }

    rparen_token = fcc_parser_current(parser);
    if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN, "expected ')' after sizeof type")) {
      return false;
    }

    has_type_operand = true;
    if (!fcc_parser_allocate_expression(parser, &node)) {
      return false;
    }

    node->kind = FCC_AST_EXPRESSION_SIZEOF;
    node->span.begin_offset = sizeof_token->span.begin_offset;
    node->span.end_offset = rparen_token->span.end_offset;
    node->data.sizeof_expression.has_type_operand = true;
    node->data.sizeof_expression.type = type_operand;
    node->data.sizeof_expression.operand = NULL;
    *expression = node;
    return true;
  }

  if (!fcc_parser_parse_unary_expression(parser, &operand)) {
    return false;
  }

  if (!fcc_parser_allocate_expression(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_EXPRESSION_SIZEOF;
  node->span.begin_offset = sizeof_token->span.begin_offset;
  node->span.end_offset = operand->span.end_offset;
  node->data.sizeof_expression.has_type_operand = has_type_operand;
  node->data.sizeof_expression.operand = operand;
  *expression = node;
  return true;
}

static bool fcc_parser_parse_alignof_expression(FccParser* parser, const FccToken* alignof_token,
                                                FccAstExpression** expression) {
  FccAstDeclSpecifiers decl_specifiers;
  FccAstExpression* node;
  FccAstType type_operand;
  const FccToken* rparen_token;

  assert(parser != NULL);
  assert(alignof_token != NULL);
  assert(expression != NULL);

  if (!fcc_parser_expect(parser, FCC_TOKEN_LPAREN, "expected '(' after _Alignof")) {
    return false;
  }

  if (!fcc_parser_parse_declaration_specifiers(parser, true, &decl_specifiers, &type_operand)) {
    return false;
  }

  if (!fcc_parser_parse_abstract_declarator(parser, &type_operand)) {
    return false;
  }

  rparen_token = fcc_parser_current(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN, "expected ')' after _Alignof type")) {
    return false;
  }

  if (!fcc_parser_allocate_expression(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_EXPRESSION_ALIGNOF;
  node->span.begin_offset = alignof_token->span.begin_offset;
  node->span.end_offset = rparen_token->span.end_offset;
  node->data.alignof_expression.type = type_operand;
  (void)decl_specifiers;
  *expression = node;
  return true;
}

static bool fcc_parser_parse_cast_expression(FccParser* parser, const FccToken* left_paren_token,
                                             FccAstExpression** expression) {
  FccAstDeclSpecifiers decl_specifiers;
  FccAstExpression* node;
  FccAstExpression* operand;
  FccAstType cast_type;
  const FccToken* right_paren_token;

  assert(parser != NULL);
  assert(left_paren_token != NULL);
  assert(expression != NULL);

  (void)fcc_parser_consume(parser);
  if (!fcc_parser_parse_declaration_specifiers(parser, true, &decl_specifiers, &cast_type)) {
    return false;
  }

  if (!fcc_parser_parse_abstract_declarator(parser, &cast_type)) {
    return false;
  }

  right_paren_token = fcc_parser_current(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN, "expected ')' after cast type")) {
    return false;
  }

  if (!fcc_parser_parse_unary_expression(parser, &operand)) {
    return false;
  }

  if (!fcc_parser_allocate_expression(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_EXPRESSION_CAST;
  node->span.begin_offset = left_paren_token->span.begin_offset;
  node->span.end_offset = operand->span.end_offset;
  node->data.cast.type = cast_type;
  node->data.cast.operand = operand;
  (void)right_paren_token;
  (void)decl_specifiers;
  *expression = node;
  return true;
}

static bool fcc_parser_parse_unary_expression(FccParser* parser, FccAstExpression** expression) {
  const FccToken* token;
  FccAstExpression* operand;
  FccAstExpression* node;
  FccAstUnaryOpKind op_kind;

  token = fcc_parser_current(parser);
  if ((token->kind == FCC_TOKEN_INCREMENT) || (token->kind == FCC_TOKEN_DECREMENT)) {
    (void)fcc_parser_consume(parser);
    if (!fcc_parser_parse_unary_expression(parser, &operand)) {
      return false;
    }

    if (!fcc_parser_allocate_expression(parser, &node)) {
      return false;
    }

    node->kind = FCC_AST_EXPRESSION_UPDATE;
    node->span.begin_offset = token->span.begin_offset;
    node->span.end_offset = operand->span.end_offset;
    node->data.update.op_kind =
        token->kind == FCC_TOKEN_INCREMENT ? FCC_AST_UPDATE_INCREMENT : FCC_AST_UPDATE_DECREMENT;
    node->data.update.target = operand;
    node->data.update.is_postfix = false;
    *expression = node;
    return true;
  }

  switch (token->kind) {
    case FCC_TOKEN_PLUS:
      op_kind = FCC_AST_UNARY_PLUS;
      break;
    case FCC_TOKEN_MINUS:
      op_kind = FCC_AST_UNARY_NEGATE;
      break;
    case FCC_TOKEN_LOGICAL_NOT:
      op_kind = FCC_AST_UNARY_LOGICAL_NOT;
      break;
    case FCC_TOKEN_BITWISE_NOT:
      op_kind = FCC_AST_UNARY_BITWISE_NOT;
      break;
    case FCC_TOKEN_BITWISE_AND:
      op_kind = FCC_AST_UNARY_ADDRESS_OF;
      break;
    case FCC_TOKEN_STAR:
      op_kind = FCC_AST_UNARY_DEREFERENCE;
      break;
    case FCC_TOKEN_KW_SIZEOF:
      (void)fcc_parser_consume(parser);
      return fcc_parser_parse_sizeof_expression(parser, token, expression);
    case FCC_TOKEN_KW_ALIGNOF:
      (void)fcc_parser_consume(parser);
      return fcc_parser_parse_alignof_expression(parser, token, expression);
    case FCC_TOKEN_LPAREN:
      if (fcc_parser_token_starts_type_name(parser, fcc_parser_peek(parser, 1))) {
        return fcc_parser_parse_cast_expression(parser, token, expression);
      }

      return fcc_parser_parse_postfix_expression(parser, expression);
    default:
      return fcc_parser_parse_postfix_expression(parser, expression);
  }

  (void)fcc_parser_consume(parser);
  if (!fcc_parser_parse_unary_expression(parser, &operand)) {
    return false;
  }

  if (!fcc_parser_allocate_expression(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_EXPRESSION_UNARY;
  node->span.begin_offset = token->span.begin_offset;
  node->span.end_offset = operand->span.end_offset;
  node->data.unary.op_kind = op_kind;
  node->data.unary.operand = operand;
  *expression = node;
  return true;
}

static bool fcc_parser_parse_argument_list(FccParser* parser, FccAstExpression*** arguments,
                                           size_t* argument_count) {
  FccAstExpression** temp_arguments;
  size_t temp_argument_count;
  size_t temp_argument_capacity;

  assert(parser != NULL);
  assert(arguments != NULL);
  assert(argument_count != NULL);

  *arguments = NULL;
  *argument_count = 0;
  temp_arguments = NULL;
  temp_argument_count = 0;
  temp_argument_capacity = 0;

  if (fcc_parser_is_current(parser, FCC_TOKEN_RPAREN)) {
    return true;
  }

  while (true) {
    FccAstExpression* argument;

    if (!fcc_parser_parse_expression(parser, &argument)) {
      free(temp_arguments);
      return false;
    }

    if (!fcc_parser_append_temp_pointer((void***)&temp_arguments, &temp_argument_count,
                                        &temp_argument_capacity, argument)) {
      free(temp_arguments);
      return fcc_parser_out_of_memory(parser);
    }

    if (!fcc_parser_match(parser, FCC_TOKEN_COMMA)) {
      break;
    }
  }

  if (!fcc_parser_copy_pointer_array(parser, (void***)arguments, (void**)temp_arguments,
                                     temp_argument_count)) {
    free(temp_arguments);
    return fcc_parser_out_of_memory(parser);
  }

  *argument_count = temp_argument_count;
  free(temp_arguments);
  return true;
}

static bool fcc_parser_build_binary(FccParser* parser, FccAstBinaryOpKind op_kind,
                                    FccAstExpression* left, const FccToken* op_token,
                                    FccAstExpression* right, FccAstExpression** expression) {
  FccAstExpression* node;

  assert(parser != NULL);
  assert(left != NULL);
  assert(op_token != NULL);
  assert(right != NULL);
  assert(expression != NULL);

  if (!fcc_parser_allocate_expression(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_EXPRESSION_BINARY;
  node->span.begin_offset = left->span.begin_offset;
  node->span.end_offset = right->span.end_offset;
  node->data.binary.op_kind = op_kind;
  node->data.binary.left = left;
  node->data.binary.right = right;
  *expression = node;
  (void)op_token;
  return true;
}

static bool fcc_parser_parse_multiplicative_expression(FccParser* parser,
                                                       FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_parse_unary_expression(parser, &left)) {
    return false;
  }

  while (fcc_parser_is_current(parser, FCC_TOKEN_STAR) ||
         fcc_parser_is_current(parser, FCC_TOKEN_SLASH) ||
         fcc_parser_is_current(parser, FCC_TOKEN_PERCENT)) {
    const FccToken* op_token;
    FccAstBinaryOpKind op_kind;
    FccAstExpression* right;

    op_token = fcc_parser_consume(parser);
    if (op_token->kind == FCC_TOKEN_STAR) {
      op_kind = FCC_AST_BINARY_MULTIPLY;
    } else if (op_token->kind == FCC_TOKEN_SLASH) {
      op_kind = FCC_AST_BINARY_DIVIDE;
    } else {
      op_kind = FCC_AST_BINARY_MODULO;
    }

    if (!fcc_parser_parse_unary_expression(parser, &right)) {
      return false;
    }

    if (!fcc_parser_build_binary(parser, op_kind, left, op_token, right, &left)) {
      return false;
    }
  }

  *expression = left;
  return true;
}

static bool fcc_parser_parse_additive_expression(FccParser* parser, FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_parse_multiplicative_expression(parser, &left)) {
    return false;
  }

  while (fcc_parser_is_current(parser, FCC_TOKEN_PLUS) ||
         fcc_parser_is_current(parser, FCC_TOKEN_MINUS)) {
    const FccToken* op_token;
    FccAstBinaryOpKind op_kind;
    FccAstExpression* right;

    op_token = fcc_parser_consume(parser);
    if (op_token->kind == FCC_TOKEN_PLUS) {
      op_kind = FCC_AST_BINARY_ADD;
    } else {
      op_kind = FCC_AST_BINARY_SUBTRACT;
    }

    if (!fcc_parser_parse_multiplicative_expression(parser, &right)) {
      return false;
    }

    if (!fcc_parser_build_binary(parser, op_kind, left, op_token, right, &left)) {
      return false;
    }
  }

  *expression = left;
  return true;
}

static bool fcc_parser_parse_shift_expression(FccParser* parser, FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_parse_additive_expression(parser, &left)) {
    return false;
  }

  while (fcc_parser_is_current(parser, FCC_TOKEN_LEFT_SHIFT) ||
         fcc_parser_is_current(parser, FCC_TOKEN_RIGHT_SHIFT)) {
    const FccToken* op_token;
    FccAstBinaryOpKind op_kind;
    FccAstExpression* right;

    op_token = fcc_parser_consume(parser);
    if (op_token->kind == FCC_TOKEN_LEFT_SHIFT) {
      op_kind = FCC_AST_BINARY_LEFT_SHIFT;
    } else {
      op_kind = FCC_AST_BINARY_RIGHT_SHIFT;
    }

    if (!fcc_parser_parse_additive_expression(parser, &right)) {
      return false;
    }

    if (!fcc_parser_build_binary(parser, op_kind, left, op_token, right, &left)) {
      return false;
    }
  }

  *expression = left;
  return true;
}

static bool fcc_parser_parse_relational_expression(FccParser* parser,
                                                   FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_parse_shift_expression(parser, &left)) {
    return false;
  }

  while (fcc_parser_is_current(parser, FCC_TOKEN_LESS) ||
         fcc_parser_is_current(parser, FCC_TOKEN_LESS_EQUAL) ||
         fcc_parser_is_current(parser, FCC_TOKEN_GREATER) ||
         fcc_parser_is_current(parser, FCC_TOKEN_GREATER_EQUAL)) {
    const FccToken* op_token;
    FccAstBinaryOpKind op_kind;
    FccAstExpression* right;

    op_token = fcc_parser_consume(parser);
    if (op_token->kind == FCC_TOKEN_LESS) {
      op_kind = FCC_AST_BINARY_LESS;
    } else if (op_token->kind == FCC_TOKEN_LESS_EQUAL) {
      op_kind = FCC_AST_BINARY_LESS_EQUAL;
    } else if (op_token->kind == FCC_TOKEN_GREATER) {
      op_kind = FCC_AST_BINARY_GREATER;
    } else {
      op_kind = FCC_AST_BINARY_GREATER_EQUAL;
    }

    if (!fcc_parser_parse_shift_expression(parser, &right)) {
      return false;
    }

    if (!fcc_parser_build_binary(parser, op_kind, left, op_token, right, &left)) {
      return false;
    }
  }

  *expression = left;
  return true;
}

static bool fcc_parser_parse_equality_expression(FccParser* parser, FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_parse_relational_expression(parser, &left)) {
    return false;
  }

  while (fcc_parser_is_current(parser, FCC_TOKEN_EQUAL) ||
         fcc_parser_is_current(parser, FCC_TOKEN_NOT_EQUAL)) {
    const FccToken* op_token;
    FccAstBinaryOpKind op_kind;
    FccAstExpression* right;

    op_token = fcc_parser_consume(parser);
    if (op_token->kind == FCC_TOKEN_EQUAL) {
      op_kind = FCC_AST_BINARY_EQUAL;
    } else {
      op_kind = FCC_AST_BINARY_NOT_EQUAL;
    }

    if (!fcc_parser_parse_relational_expression(parser, &right)) {
      return false;
    }

    if (!fcc_parser_build_binary(parser, op_kind, left, op_token, right, &left)) {
      return false;
    }
  }

  *expression = left;
  return true;
}

static bool fcc_parser_parse_bitwise_and_expression(FccParser* parser,
                                                    FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_parse_equality_expression(parser, &left)) {
    return false;
  }

  while (fcc_parser_is_current(parser, FCC_TOKEN_BITWISE_AND)) {
    const FccToken* op_token;
    FccAstExpression* right;

    op_token = fcc_parser_consume(parser);
    if (!fcc_parser_parse_equality_expression(parser, &right)) {
      return false;
    }

    if (!fcc_parser_build_binary(parser, FCC_AST_BINARY_BITWISE_AND, left, op_token, right,
                                 &left)) {
      return false;
    }
  }

  *expression = left;
  return true;
}

static bool fcc_parser_parse_bitwise_xor_expression(FccParser* parser,
                                                    FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_parse_bitwise_and_expression(parser, &left)) {
    return false;
  }

  while (fcc_parser_is_current(parser, FCC_TOKEN_BITWISE_XOR)) {
    const FccToken* op_token;
    FccAstExpression* right;

    op_token = fcc_parser_consume(parser);
    if (!fcc_parser_parse_bitwise_and_expression(parser, &right)) {
      return false;
    }

    if (!fcc_parser_build_binary(parser, FCC_AST_BINARY_BITWISE_XOR, left, op_token, right,
                                 &left)) {
      return false;
    }
  }

  *expression = left;
  return true;
}

static bool fcc_parser_parse_bitwise_or_expression(FccParser* parser,
                                                   FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_parse_bitwise_xor_expression(parser, &left)) {
    return false;
  }

  while (fcc_parser_is_current(parser, FCC_TOKEN_BITWISE_OR)) {
    const FccToken* op_token;
    FccAstExpression* right;

    op_token = fcc_parser_consume(parser);
    if (!fcc_parser_parse_bitwise_xor_expression(parser, &right)) {
      return false;
    }

    if (!fcc_parser_build_binary(parser, FCC_AST_BINARY_BITWISE_OR, left, op_token, right, &left)) {
      return false;
    }
  }

  *expression = left;
  return true;
}

static bool fcc_parser_parse_logical_and_expression(FccParser* parser,
                                                    FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_parse_bitwise_or_expression(parser, &left)) {
    return false;
  }

  while (fcc_parser_is_current(parser, FCC_TOKEN_LOGICAL_AND)) {
    const FccToken* op_token;
    FccAstExpression* right;

    op_token = fcc_parser_consume(parser);
    if (!fcc_parser_parse_bitwise_or_expression(parser, &right)) {
      return false;
    }

    if (!fcc_parser_build_binary(parser, FCC_AST_BINARY_LOGICAL_AND, left, op_token, right,
                                 &left)) {
      return false;
    }
  }

  *expression = left;
  return true;
}

static bool fcc_parser_parse_logical_or_expression(FccParser* parser,
                                                   FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_parse_logical_and_expression(parser, &left)) {
    return false;
  }

  while (fcc_parser_is_current(parser, FCC_TOKEN_LOGICAL_OR)) {
    const FccToken* op_token;
    FccAstExpression* right;

    op_token = fcc_parser_consume(parser);
    if (!fcc_parser_parse_logical_and_expression(parser, &right)) {
      return false;
    }

    if (!fcc_parser_build_binary(parser, FCC_AST_BINARY_LOGICAL_OR, left, op_token, right, &left)) {
      return false;
    }
  }

  *expression = left;
  return true;
}

static bool fcc_parser_compound_assignment_op(FccTokenKind token_kind,
                                              FccAstBinaryOpKind* op_kind_out) {
  assert(op_kind_out != NULL);

  switch (token_kind) {
    case FCC_TOKEN_PLUS_ASSIGN:
      *op_kind_out = FCC_AST_BINARY_ADD;
      return true;
    case FCC_TOKEN_MINUS_ASSIGN:
      *op_kind_out = FCC_AST_BINARY_SUBTRACT;
      return true;
    case FCC_TOKEN_STAR_ASSIGN:
      *op_kind_out = FCC_AST_BINARY_MULTIPLY;
      return true;
    case FCC_TOKEN_SLASH_ASSIGN:
      *op_kind_out = FCC_AST_BINARY_DIVIDE;
      return true;
    case FCC_TOKEN_PERCENT_ASSIGN:
      *op_kind_out = FCC_AST_BINARY_MODULO;
      return true;
    case FCC_TOKEN_BITWISE_AND_ASSIGN:
      *op_kind_out = FCC_AST_BINARY_BITWISE_AND;
      return true;
    case FCC_TOKEN_BITWISE_OR_ASSIGN:
      *op_kind_out = FCC_AST_BINARY_BITWISE_OR;
      return true;
    case FCC_TOKEN_BITWISE_XOR_ASSIGN:
      *op_kind_out = FCC_AST_BINARY_BITWISE_XOR;
      return true;
    case FCC_TOKEN_LEFT_SHIFT_ASSIGN:
      *op_kind_out = FCC_AST_BINARY_LEFT_SHIFT;
      return true;
    case FCC_TOKEN_RIGHT_SHIFT_ASSIGN:
      *op_kind_out = FCC_AST_BINARY_RIGHT_SHIFT;
      return true;
    case FCC_TOKEN_INVALID:
    case FCC_TOKEN_END_OF_FILE:
    case FCC_TOKEN_IDENTIFIER:
    case FCC_TOKEN_INTEGER_LITERAL:
    case FCC_TOKEN_STRING_LITERAL:
    case FCC_TOKEN_CHAR_LITERAL:
    case FCC_TOKEN_KW_INT:
    case FCC_TOKEN_KW_VOID:
    case FCC_TOKEN_KW_CHAR:
    case FCC_TOKEN_KW_SIGNED:
    case FCC_TOKEN_KW_UNSIGNED:
    case FCC_TOKEN_KW_SHORT:
    case FCC_TOKEN_KW_LONG:
    case FCC_TOKEN_KW_BOOL:
    case FCC_TOKEN_KW_STRUCT:
    case FCC_TOKEN_KW_UNION:
    case FCC_TOKEN_KW_ENUM:
    case FCC_TOKEN_KW_TYPEDEF:
    case FCC_TOKEN_KW_RETURN:
    case FCC_TOKEN_KW_IF:
    case FCC_TOKEN_KW_ELSE:
    case FCC_TOKEN_KW_WHILE:
    case FCC_TOKEN_KW_DO:
    case FCC_TOKEN_KW_FOR:
    case FCC_TOKEN_KW_SWITCH:
    case FCC_TOKEN_KW_CASE:
    case FCC_TOKEN_KW_DEFAULT:
    case FCC_TOKEN_KW_GOTO:
    case FCC_TOKEN_KW_BREAK:
    case FCC_TOKEN_KW_CONTINUE:
    case FCC_TOKEN_KW_STATIC:
    case FCC_TOKEN_KW_EXTERN:
    case FCC_TOKEN_KW_CONST:
    case FCC_TOKEN_KW_SIZEOF:
    case FCC_TOKEN_KW_ALIGNOF:
    case FCC_TOKEN_KW_STATIC_ASSERT:
    case FCC_TOKEN_LPAREN:
    case FCC_TOKEN_RPAREN:
    case FCC_TOKEN_LBRACE:
    case FCC_TOKEN_RBRACE:
    case FCC_TOKEN_LBRACKET:
    case FCC_TOKEN_RBRACKET:
    case FCC_TOKEN_SEMICOLON:
    case FCC_TOKEN_COMMA:
    case FCC_TOKEN_DOT:
    case FCC_TOKEN_ELLIPSIS:
    case FCC_TOKEN_ARROW:
    case FCC_TOKEN_QUESTION:
    case FCC_TOKEN_COLON:
    case FCC_TOKEN_PLUS:
    case FCC_TOKEN_MINUS:
    case FCC_TOKEN_STAR:
    case FCC_TOKEN_SLASH:
    case FCC_TOKEN_PERCENT:
    case FCC_TOKEN_ASSIGN:
    case FCC_TOKEN_EQUAL:
    case FCC_TOKEN_NOT_EQUAL:
    case FCC_TOKEN_LESS:
    case FCC_TOKEN_LESS_EQUAL:
    case FCC_TOKEN_GREATER:
    case FCC_TOKEN_GREATER_EQUAL:
    case FCC_TOKEN_LOGICAL_AND:
    case FCC_TOKEN_LOGICAL_OR:
    case FCC_TOKEN_LOGICAL_NOT:
    case FCC_TOKEN_BITWISE_AND:
    case FCC_TOKEN_BITWISE_OR:
    case FCC_TOKEN_BITWISE_XOR:
    case FCC_TOKEN_BITWISE_NOT:
    case FCC_TOKEN_LEFT_SHIFT:
    case FCC_TOKEN_RIGHT_SHIFT:
    case FCC_TOKEN_INCREMENT:
    case FCC_TOKEN_DECREMENT:
      return false;
  }

  return false;
}

static bool fcc_parser_parse_conditional_expression(FccParser* parser,
                                                    FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_parse_logical_or_expression(parser, &left)) {
    return false;
  }

  if (fcc_parser_match(parser, FCC_TOKEN_QUESTION)) {
    FccAstExpression* then_expression;
    FccAstExpression* else_expression;
    FccAstExpression* node;

    if (!fcc_parser_parse_expression(parser, &then_expression)) {
      return false;
    }

    if (!fcc_parser_expect(parser, FCC_TOKEN_COLON,
                           "expected ':' after conditional true expression")) {
      return false;
    }

    if (!fcc_parser_parse_conditional_expression(parser, &else_expression)) {
      return false;
    }

    if (!fcc_parser_allocate_expression(parser, &node)) {
      return false;
    }

    node->kind = FCC_AST_EXPRESSION_CONDITIONAL;
    node->span.begin_offset = left->span.begin_offset;
    node->span.end_offset = else_expression->span.end_offset;
    node->data.conditional.condition = left;
    node->data.conditional.then_expression = then_expression;
    node->data.conditional.else_expression = else_expression;
    *expression = node;
    return true;
  }

  *expression = left;
  return true;
}

static bool fcc_parser_parse_assignment_expression(FccParser* parser,
                                                   FccAstExpression** expression) {
  FccAstExpression* left;

  if (!fcc_parser_push_recursion(parser)) {
    return false;
  }

  if (!fcc_parser_parse_conditional_expression(parser, &left)) {
    fcc_parser_pop_recursion(parser);
    return false;
  }

  if (fcc_parser_is_current(parser, FCC_TOKEN_ASSIGN)) {
    const FccToken* assign_token;
    FccAstExpression* value;
    FccAstExpression* node;

    assign_token = fcc_parser_consume(parser);
    if (!fcc_parser_parse_assignment_expression(parser, &value)) {
      fcc_parser_pop_recursion(parser);
      return false;
    }

    if (!fcc_parser_allocate_expression(parser, &node)) {
      fcc_parser_pop_recursion(parser);
      return false;
    }

    node->kind = FCC_AST_EXPRESSION_ASSIGN;
    node->span.begin_offset = left->span.begin_offset;
    node->span.end_offset = value->span.end_offset;
    node->data.assign.target = left;
    node->data.assign.value = value;
    *expression = node;
    (void)assign_token;
    fcc_parser_pop_recursion(parser);
    return true;
  }

  {
    FccAstBinaryOpKind op_kind;

    if (fcc_parser_compound_assignment_op(fcc_parser_current(parser)->kind, &op_kind)) {
      const FccToken* assign_token;
      FccAstExpression* value;
      FccAstExpression* node;

      assign_token = fcc_parser_consume(parser);
      if (!fcc_parser_parse_assignment_expression(parser, &value)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      if (!fcc_parser_allocate_expression(parser, &node)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      node->kind = FCC_AST_EXPRESSION_COMPOUND_ASSIGN;
      node->span.begin_offset = left->span.begin_offset;
      node->span.end_offset = value->span.end_offset;
      node->data.compound_assign.op_kind = op_kind;
      node->data.compound_assign.target = left;
      node->data.compound_assign.value = value;
      *expression = node;
      (void)assign_token;
      fcc_parser_pop_recursion(parser);
      return true;
    }
  }

  *expression = left;
  fcc_parser_pop_recursion(parser);
  return true;
}

static bool fcc_parser_parse_expression(FccParser* parser, FccAstExpression** expression) {
  return fcc_parser_parse_assignment_expression(parser, expression);
}

static bool fcc_parser_parse_initializer_expression(FccParser* parser,
                                                    FccAstExpression** expression) {
  const FccToken* left_brace;
  const FccToken* right_brace;
  FccAstExpression** items;
  size_t item_count;
  size_t item_capacity;
  FccAstExpression* node;

  assert(parser != NULL);
  assert(expression != NULL);

  if (!fcc_parser_is_current(parser, FCC_TOKEN_LBRACE)) {
    return fcc_parser_parse_assignment_expression(parser, expression);
  }

  left_brace = fcc_parser_consume(parser);
  items = NULL;
  item_count = 0;
  item_capacity = 0;
  while (!fcc_parser_is_current(parser, FCC_TOKEN_RBRACE) &&
         !fcc_parser_is_current(parser, FCC_TOKEN_END_OF_FILE)) {
    FccAstExpression* item;

    if (!fcc_parser_parse_initializer_expression(parser, &item)) {
      free(items);
      return false;
    }

    if (!fcc_parser_append_temp_pointer((void***)&items, &item_count, &item_capacity, item)) {
      free(items);
      return fcc_parser_out_of_memory(parser);
    }

    if (!fcc_parser_match(parser, FCC_TOKEN_COMMA)) {
      break;
    }
  }

  right_brace = fcc_parser_current(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_RBRACE, "expected '}' after initializer list")) {
    free(items);
    return false;
  }

  if (!fcc_parser_allocate_expression(parser, &node)) {
    free(items);
    return false;
  }

  node->kind = FCC_AST_EXPRESSION_INITIALIZER_LIST;
  node->span.begin_offset = left_brace->span.begin_offset;
  node->span.end_offset = right_brace->span.end_offset;
  node->data.initializer_list.item_count = item_count;
  if (!fcc_parser_copy_pointer_array(parser, (void***)&node->data.initializer_list.items,
                                     (void**)items, item_count)) {
    free(items);
    return fcc_parser_out_of_memory(parser);
  }

  free(items);
  *expression = node;
  return true;
}

static bool fcc_parser_parse_local_declaration_statement(FccParser* parser,
                                                         FccAstStatement** statement) {
  FccAstDeclSpecifiers decl_specifiers;
  FccAstDeclarator declarator;
  FccAstType type;
  const FccToken* name_token;
  FccAstExpression* initializer;
  FccAstStatement* node;

  if (fcc_parser_is_current(parser, FCC_TOKEN_KW_STATIC_ASSERT)) {
    if (!fcc_parser_allocate_statement(parser, &node)) {
      return false;
    }

    node->kind = FCC_AST_STATEMENT_STATIC_ASSERT;
    if (!fcc_parser_parse_static_assert(parser, &node->data.static_assertion)) {
      return false;
    }

    node->span = node->data.static_assertion.span;
    *statement = node;
    return true;
  }

  if (!fcc_parser_parse_declaration_specifiers(parser, true, &decl_specifiers, &type)) {
    return false;
  }

  if (fcc_parser_is_current(parser, FCC_TOKEN_SEMICOLON)) {
    const FccToken* semicolon_token;

    semicolon_token = fcc_parser_consume(parser);
    if (!fcc_parser_allocate_statement(parser, &node)) {
      return false;
    }

    node->kind = FCC_AST_STATEMENT_DECLARATION;
    node->span.begin_offset = type.span.begin_offset;
    node->span.end_offset = semicolon_token->span.end_offset;
    node->data.declaration.syntax.decl_specifiers = decl_specifiers;
    fcc_parser_init_declarator(&node->data.declaration.syntax.declarator);
    node->data.declaration.type = type;
    node->data.declaration.name = NULL;
    node->data.declaration.initializer = NULL;
    *statement = node;
    return true;
  }

  if (!fcc_parser_parse_named_declarator(parser, &type, &declarator, true, &name_token)) {
    return false;
  }

  initializer = NULL;
  if (fcc_parser_match(parser, FCC_TOKEN_ASSIGN)) {
    if (!fcc_parser_parse_initializer_expression(parser, &initializer)) {
      return false;
    }
  }

  if (fcc_parser_is_current(parser, FCC_TOKEN_COMMA)) {
    return fcc_parser_emit_current_error(parser, "multiple declarators are not supported yet");
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_SEMICOLON, "expected ';' after local declaration")) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_DECLARATION;
  node->span.begin_offset = type.span.begin_offset;
  node->span.end_offset = fcc_parser_peek(parser, 0)->span.begin_offset;
  node->data.declaration.syntax.decl_specifiers = decl_specifiers;
  node->data.declaration.syntax.declarator = declarator;
  node->data.declaration.type = type;
  node->data.declaration.initializer = initializer;
  if (!fcc_parser_get_identifier_name(name_token, &node->data.declaration.name)) {
    return fcc_parser_emit_token_error(parser, name_token, "variable identifier interning failed");
  }

  if (node->data.declaration.syntax.decl_specifiers.storage_class ==
      FCC_AST_STORAGE_CLASS_TYPEDEF) {
    if (!fcc_parser_add_typedef_name(parser, node->data.declaration.name)) {
      return fcc_parser_out_of_memory(parser);
    }
  }

  *statement = node;
  return true;
}

static bool fcc_parser_parse_expression_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* start_token;
  FccAstStatement* node;
  FccAstExpression* expression;

  start_token = fcc_parser_current(parser);
  expression = NULL;
  if (!fcc_parser_match(parser, FCC_TOKEN_SEMICOLON)) {
    if (!fcc_parser_parse_expression(parser, &expression)) {
      return false;
    }

    if (!fcc_parser_expect(parser, FCC_TOKEN_SEMICOLON, "expected ';' after expression")) {
      return false;
    }
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_EXPRESSION;
  if (expression != NULL) {
    node->span.begin_offset = start_token->span.begin_offset;
    node->span.end_offset = fcc_parser_peek(parser, 0)->span.begin_offset;
  } else {
    node->span = start_token->span;
  }

  node->data.expression_statement.expression = expression;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_compound_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* left_brace;
  FccAstStatement* node;
  FccAstStatement** items;
  size_t item_count;
  size_t item_capacity;
  bool pushed_typedef_scope;

  left_brace = fcc_parser_current(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_LBRACE, "expected '{' to start compound statement")) {
    return false;
  }

  items = NULL;
  item_count = 0;
  item_capacity = 0;
  pushed_typedef_scope = false;
  if (!fcc_parser_push_typedef_scope(parser)) {
    return fcc_parser_out_of_memory(parser);
  }

  pushed_typedef_scope = true;

  while (!fcc_parser_is_current(parser, FCC_TOKEN_RBRACE) &&
         !fcc_parser_is_current(parser, FCC_TOKEN_END_OF_FILE)) {
    FccAstStatement* item;

    if (!fcc_parser_parse_statement(parser, &item)) {
      if (pushed_typedef_scope) {
        fcc_parser_pop_typedef_scope(parser);
      }

      free(items);
      return false;
    }

    if (!fcc_parser_append_temp_pointer((void***)&items, &item_count, &item_capacity, item)) {
      if (pushed_typedef_scope) {
        fcc_parser_pop_typedef_scope(parser);
      }

      free(items);
      return fcc_parser_out_of_memory(parser);
    }
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_RBRACE, "expected '}' to end compound statement")) {
    if (pushed_typedef_scope) {
      fcc_parser_pop_typedef_scope(parser);
    }

    free(items);
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    free(items);
    return false;
  }

  node->kind = FCC_AST_STATEMENT_COMPOUND;
  node->span.begin_offset = left_brace->span.begin_offset;
  node->span.end_offset = fcc_parser_peek(parser, 0)->span.begin_offset;
  node->data.compound.item_count = item_count;
  if (!fcc_parser_copy_pointer_array(parser, (void***)&node->data.compound.items, (void**)items,
                                     item_count)) {
    if (pushed_typedef_scope) {
      fcc_parser_pop_typedef_scope(parser);
    }

    free(items);
    return fcc_parser_out_of_memory(parser);
  }

  fcc_parser_pop_typedef_scope(parser);
  free(items);
  *statement = node;
  return true;
}

static bool fcc_parser_parse_return_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* return_token;
  FccAstExpression* expression;
  FccAstStatement* node;

  return_token = fcc_parser_consume(parser);
  expression = NULL;
  if (!fcc_parser_is_current(parser, FCC_TOKEN_SEMICOLON)) {
    if (!fcc_parser_parse_expression(parser, &expression)) {
      return false;
    }
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_SEMICOLON, "expected ';' after return statement")) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_RETURN;
  node->span.begin_offset = return_token->span.begin_offset;
  node->span.end_offset = fcc_parser_peek(parser, 0)->span.begin_offset;
  node->data.return_statement.expression = expression;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_break_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* break_token;
  FccAstStatement* node;

  break_token = fcc_parser_consume(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_SEMICOLON, "expected ';' after break")) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_BREAK;
  node->span.begin_offset = break_token->span.begin_offset;
  node->span.end_offset = fcc_parser_peek(parser, 0)->span.begin_offset;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_continue_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* continue_token;
  FccAstStatement* node;

  continue_token = fcc_parser_consume(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_SEMICOLON, "expected ';' after continue")) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_CONTINUE;
  node->span.begin_offset = continue_token->span.begin_offset;
  node->span.end_offset = fcc_parser_peek(parser, 0)->span.begin_offset;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_if_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* if_token;
  FccAstExpression* condition;
  FccAstStatement* then_statement;
  FccAstStatement* else_statement;
  FccAstStatement* node;

  if_token = fcc_parser_consume(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_LPAREN, "expected '(' after if")) {
    return false;
  }

  if (!fcc_parser_parse_expression(parser, &condition)) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN, "expected ')' after if condition")) {
    return false;
  }

  if (!fcc_parser_parse_statement(parser, &then_statement)) {
    return false;
  }

  else_statement = NULL;
  if (fcc_parser_match(parser, FCC_TOKEN_KW_ELSE)) {
    if (!fcc_parser_parse_statement(parser, &else_statement)) {
      return false;
    }
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_IF;
  node->span.begin_offset = if_token->span.begin_offset;
  node->span.end_offset = then_statement->span.end_offset;
  if (else_statement != NULL) {
    node->span.end_offset = else_statement->span.end_offset;
  }

  node->data.if_statement.condition = condition;
  node->data.if_statement.then_statement = then_statement;
  node->data.if_statement.else_statement = else_statement;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_while_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* while_token;
  FccAstExpression* condition;
  FccAstStatement* body;
  FccAstStatement* node;

  while_token = fcc_parser_consume(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_LPAREN, "expected '(' after while")) {
    return false;
  }

  if (!fcc_parser_parse_expression(parser, &condition)) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN, "expected ')' after while condition")) {
    return false;
  }

  if (!fcc_parser_parse_statement(parser, &body)) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_WHILE;
  node->span.begin_offset = while_token->span.begin_offset;
  node->span.end_offset = body->span.end_offset;
  node->data.while_statement.condition = condition;
  node->data.while_statement.body = body;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_do_while_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* do_token;
  FccAstStatement* body;
  FccAstExpression* condition;
  FccAstStatement* node;

  do_token = fcc_parser_consume(parser);
  if (!fcc_parser_parse_statement(parser, &body)) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_KW_WHILE, "expected while after do body")) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_LPAREN, "expected '(' after while")) {
    return false;
  }

  if (!fcc_parser_parse_expression(parser, &condition)) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN, "expected ')' after do-while condition")) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_SEMICOLON, "expected ';' after do-while")) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_DO_WHILE;
  node->span.begin_offset = do_token->span.begin_offset;
  node->span.end_offset = fcc_parser_peek(parser, 0)->span.begin_offset;
  node->data.do_while_statement.body = body;
  node->data.do_while_statement.condition = condition;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_for_init_statement(FccParser* parser, FccAstStatement** statement) {
  if (fcc_parser_is_current(parser, FCC_TOKEN_SEMICOLON)) {
    (void)fcc_parser_consume(parser);
    *statement = NULL;
    return true;
  }

  if (fcc_parser_is_declaration_start(parser)) {
    return fcc_parser_parse_local_declaration_statement(parser, statement);
  }

  return fcc_parser_parse_expression_statement(parser, statement);
}

static bool fcc_parser_parse_for_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* for_token;
  FccAstStatement* init_statement;
  FccAstExpression* condition;
  FccAstExpression* update;
  FccAstStatement* body;
  FccAstStatement* node;

  for_token = fcc_parser_consume(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_LPAREN, "expected '(' after for")) {
    return false;
  }

  if (!fcc_parser_parse_for_init_statement(parser, &init_statement)) {
    return false;
  }

  condition = NULL;
  if (!fcc_parser_is_current(parser, FCC_TOKEN_SEMICOLON)) {
    if (!fcc_parser_parse_expression(parser, &condition)) {
      return false;
    }
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_SEMICOLON, "expected ';' after for condition")) {
    return false;
  }

  update = NULL;
  if (!fcc_parser_is_current(parser, FCC_TOKEN_RPAREN)) {
    if (!fcc_parser_parse_expression(parser, &update)) {
      return false;
    }
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN, "expected ')' after for clauses")) {
    return false;
  }

  if (!fcc_parser_parse_statement(parser, &body)) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_FOR;
  node->span.begin_offset = for_token->span.begin_offset;
  node->span.end_offset = body->span.end_offset;
  node->data.for_statement.init_statement = init_statement;
  node->data.for_statement.condition = condition;
  node->data.for_statement.update = update;
  node->data.for_statement.body = body;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_switch_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* switch_token;
  FccAstExpression* condition;
  FccAstStatement* body;
  FccAstStatement* node;

  switch_token = fcc_parser_consume(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_LPAREN, "expected '(' after switch")) {
    return false;
  }

  if (!fcc_parser_parse_expression(parser, &condition)) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN, "expected ')' after switch condition")) {
    return false;
  }

  if (!fcc_parser_parse_statement(parser, &body)) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_SWITCH;
  node->span.begin_offset = switch_token->span.begin_offset;
  node->span.end_offset = body->span.end_offset;
  node->data.switch_statement.condition = condition;
  node->data.switch_statement.body = body;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_case_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* case_token;
  FccAstExpression* value;
  FccAstStatement* body;
  FccAstStatement* node;

  case_token = fcc_parser_consume(parser);
  if (!fcc_parser_parse_expression(parser, &value)) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_COLON, "expected ':' after case value")) {
    return false;
  }

  if (!fcc_parser_parse_statement(parser, &body)) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_CASE;
  node->span.begin_offset = case_token->span.begin_offset;
  node->span.end_offset = body->span.end_offset;
  node->data.case_statement.value = value;
  node->data.case_statement.statement = body;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_default_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* default_token;
  FccAstStatement* body;
  FccAstStatement* node;

  default_token = fcc_parser_consume(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_COLON, "expected ':' after default")) {
    return false;
  }

  if (!fcc_parser_parse_statement(parser, &body)) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_DEFAULT;
  node->span.begin_offset = default_token->span.begin_offset;
  node->span.end_offset = body->span.end_offset;
  node->data.default_statement.statement = body;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_goto_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* goto_token;
  const FccToken* label_token;
  FccAstStatement* node;
  const char* label_name;

  goto_token = fcc_parser_consume(parser);
  label_token = fcc_parser_current(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_IDENTIFIER, "expected label name after goto")) {
    return false;
  }

  if (!fcc_parser_get_identifier_name(label_token, &label_name)) {
    return fcc_parser_emit_token_error(parser, label_token, "label identifier interning failed");
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_SEMICOLON, "expected ';' after goto")) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_GOTO;
  node->span.begin_offset = goto_token->span.begin_offset;
  node->span.end_offset = fcc_parser_peek(parser, 0)->span.begin_offset;
  node->data.goto_statement.name = label_name;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_label_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* label_token;
  FccAstStatement* body;
  FccAstStatement* node;
  const char* label_name;

  label_token = fcc_parser_consume(parser);
  if (!fcc_parser_get_identifier_name(label_token, &label_name)) {
    return fcc_parser_emit_token_error(parser, label_token, "label identifier interning failed");
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_COLON, "expected ':' after label")) {
    return false;
  }

  if (!fcc_parser_parse_statement(parser, &body)) {
    return false;
  }

  if (!fcc_parser_allocate_statement(parser, &node)) {
    return false;
  }

  node->kind = FCC_AST_STATEMENT_LABEL;
  node->span.begin_offset = label_token->span.begin_offset;
  node->span.end_offset = body->span.end_offset;
  node->data.label_statement.name = label_name;
  node->data.label_statement.statement = body;
  *statement = node;
  return true;
}

static bool fcc_parser_parse_statement(FccParser* parser, FccAstStatement** statement) {
  const FccToken* token;

  if (!fcc_parser_push_recursion(parser)) {
    return false;
  }

  token = fcc_parser_current(parser);
  switch (token->kind) {
    case FCC_TOKEN_LBRACE:
      if (!fcc_parser_parse_compound_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    case FCC_TOKEN_KW_RETURN:
      if (!fcc_parser_parse_return_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    case FCC_TOKEN_KW_IF:
      if (!fcc_parser_parse_if_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    case FCC_TOKEN_KW_WHILE:
      if (!fcc_parser_parse_while_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    case FCC_TOKEN_KW_DO:
      if (!fcc_parser_parse_do_while_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    case FCC_TOKEN_KW_FOR:
      if (!fcc_parser_parse_for_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    case FCC_TOKEN_KW_SWITCH:
      if (!fcc_parser_parse_switch_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    case FCC_TOKEN_KW_CASE:
      if (!fcc_parser_parse_case_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    case FCC_TOKEN_KW_DEFAULT:
      if (!fcc_parser_parse_default_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    case FCC_TOKEN_KW_BREAK:
      if (!fcc_parser_parse_break_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    case FCC_TOKEN_KW_CONTINUE:
      if (!fcc_parser_parse_continue_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    case FCC_TOKEN_KW_GOTO:
      if (!fcc_parser_parse_goto_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
    default:
      if ((token->kind == FCC_TOKEN_IDENTIFIER) &&
          (fcc_parser_peek(parser, 1)->kind == FCC_TOKEN_COLON)) {
        if (!fcc_parser_parse_label_statement(parser, statement)) {
          fcc_parser_pop_recursion(parser);
          return false;
        }

        fcc_parser_pop_recursion(parser);
        return true;
      }

      if (fcc_parser_is_declaration_start(parser)) {
        if (!fcc_parser_parse_local_declaration_statement(parser, statement)) {
          fcc_parser_pop_recursion(parser);
          return false;
        }

        fcc_parser_pop_recursion(parser);
        return true;
      }

      if (!fcc_parser_parse_expression_statement(parser, statement)) {
        fcc_parser_pop_recursion(parser);
        return false;
      }

      fcc_parser_pop_recursion(parser);
      return true;
  }
}

static bool fcc_parser_parse_parameter_list(FccParser* parser, FccAstParameter** parameters,
                                            size_t* parameter_count, bool* is_variadic) {
  FccAstParameter* temp_parameters;
  size_t temp_parameter_count;
  size_t temp_parameter_capacity;

  assert(parser != NULL);
  assert(parameters != NULL);
  assert(parameter_count != NULL);
  assert(is_variadic != NULL);

  *parameters = NULL;
  *parameter_count = 0;
  *is_variadic = false;
  temp_parameters = NULL;
  temp_parameter_count = 0;
  temp_parameter_capacity = 0;

  if (fcc_parser_is_current(parser, FCC_TOKEN_RPAREN)) {
    return true;
  }

  if (fcc_parser_is_current(parser, FCC_TOKEN_KW_VOID) &&
      (fcc_parser_peek(parser, 1)->kind == FCC_TOKEN_RPAREN)) {
    (void)fcc_parser_consume(parser);
    return true;
  }

  while (true) {
    FccAstParameter parameter;
    FccAstDeclarator declarator;
    FccAstDeclSpecifiers decl_specifiers;
    const FccToken* name_token;

    if (fcc_parser_match(parser, FCC_TOKEN_ELLIPSIS)) {
      *is_variadic = true;
      break;
    }

    memset(&parameter, 0, sizeof(parameter));

    if (!fcc_parser_parse_declaration_specifiers(parser, true, &decl_specifiers, &parameter.type)) {
      free(temp_parameters);
      return false;
    }

    if (fcc_parser_current_named_declarator_has_identifier(parser)) {
      if (!fcc_parser_parse_named_declarator(parser, &parameter.type, &declarator, true,
                                             &name_token)) {
        free(temp_parameters);
        return false;
      }

      if (!fcc_parser_get_identifier_name(name_token, &parameter.name)) {
        free(temp_parameters);
        return fcc_parser_emit_token_error(parser, name_token,
                                           "parameter identifier interning failed");
      }
    } else {
      fcc_parser_init_declarator(&declarator);
      name_token = NULL;
      if (!fcc_parser_parse_abstract_declarator(parser, &parameter.type)) {
        free(temp_parameters);
        return false;
      }

      declarator.pointer_depth = parameter.type.pointer_depth;
      declarator.array_bounds = parameter.type.array_bounds;
      declarator.array_count = parameter.type.array_count;
      declarator.is_function_pointer = parameter.type.is_function_pointer;
      declarator.function_pointer_depth = parameter.type.function_pointer_depth;
      declarator.function_pointer_parameters = parameter.type.function_pointer_parameters;
      declarator.function_pointer_parameter_count =
          parameter.type.function_pointer_parameter_count;
      declarator.function_pointer_is_variadic = parameter.type.function_pointer_is_variadic;
      declarator.span = parameter.type.span;
      parameter.name = NULL;
    }

    parameter.syntax.decl_specifiers = decl_specifiers;
    parameter.syntax.declarator = declarator;
    parameter.span.begin_offset = parameter.type.span.begin_offset;
    parameter.span.end_offset = parameter.type.span.end_offset;

    if (!fcc_parser_append_temp_parameter(&temp_parameters, &temp_parameter_count,
                                          &temp_parameter_capacity, &parameter)) {
      free(temp_parameters);
      return fcc_parser_out_of_memory(parser);
    }

    if (!fcc_parser_match(parser, FCC_TOKEN_COMMA)) {
      break;
    }

    if (fcc_parser_match(parser, FCC_TOKEN_ELLIPSIS)) {
      *is_variadic = true;
      break;
    }
  }

  if (!fcc_parser_copy_parameters(parser, parameters, temp_parameters, temp_parameter_count)) {
    free(temp_parameters);
    return fcc_parser_out_of_memory(parser);
  }

  *parameter_count = temp_parameter_count;
  free(temp_parameters);
  return true;
}

static bool fcc_parser_parse_function_after_name(FccParser* parser,
                                                 const FccAstDeclSpecifiers* decl_specifiers,
                                                 const FccAstDeclarator* declarator,
                                                 FccAstType return_type, const FccToken* name_token,
                                                 FccAstFunctionDefinition** function_definition) {
  FccAstParameter* parameters;
  size_t parameter_count;
  bool is_variadic;
  FccAstStatement* body;
  FccAstFunctionDefinition* node;

  if (return_type.array_count != 0) {
    return fcc_parser_emit_token_error(parser, name_token,
                                       "function declarator cannot have array suffixes");
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_LPAREN, "expected '(' after function name")) {
    return false;
  }

  if (!fcc_parser_parse_parameter_list(parser, &parameters, &parameter_count, &is_variadic)) {
    return false;
  }

  if (!fcc_parser_expect(parser, FCC_TOKEN_RPAREN, "expected ')' after parameter list")) {
    return false;
  }

  if (fcc_parser_is_current(parser, FCC_TOKEN_SEMICOLON)) {
    const FccToken* semicolon_token;

    semicolon_token = fcc_parser_consume(parser);
    if (!fcc_parser_allocate_function(parser, &node)) {
      return false;
    }

    node->span.begin_offset = return_type.span.begin_offset;
    node->span.end_offset = semicolon_token->span.end_offset;
    node->syntax.decl_specifiers = *decl_specifiers;
    node->syntax.declarator = *declarator;
    node->return_type = return_type;
    node->parameters = parameters;
    node->parameter_count = parameter_count;
    node->is_variadic = is_variadic;
    node->has_body = false;
    node->body = NULL;
    if (!fcc_parser_get_identifier_name(name_token, &node->name)) {
      return fcc_parser_emit_token_error(parser, name_token,
                                         "function identifier interning failed");
    }

    *function_definition = node;
    return true;
  }

  if (!fcc_parser_parse_compound_statement(parser, &body)) {
    return false;
  }

  if (!fcc_parser_allocate_function(parser, &node)) {
    return false;
  }

  node->span.begin_offset = return_type.span.begin_offset;
  node->span.end_offset = body->span.end_offset;
  node->syntax.decl_specifiers = *decl_specifiers;
  node->syntax.declarator = *declarator;
  node->return_type = return_type;
  node->parameters = parameters;
  node->parameter_count = parameter_count;
  node->is_variadic = is_variadic;
  node->has_body = true;
  node->body = body;
  if (!fcc_parser_get_identifier_name(name_token, &node->name)) {
    return fcc_parser_emit_token_error(parser, name_token, "function identifier interning failed");
  }

  *function_definition = node;
  return true;
}

static bool fcc_parser_parse_global_variable(FccParser* parser,
                                             const FccAstDeclSpecifiers* decl_specifiers,
                                             const FccAstDeclarator* declarator, FccAstType type,
                                             const FccToken* name_token,
                                             FccAstGlobalVariable** global_variable) {
  FccAstExpression* initializer;
  const FccToken* semicolon_token;
  FccAstGlobalVariable* node;

  assert(parser != NULL);
  assert(name_token != NULL);
  assert(global_variable != NULL);

  if (fcc_parser_type_is_plain_void(&type)) {
    return fcc_parser_emit_token_error(parser, name_token, "global variable cannot have type void");
  }

  initializer = NULL;
  if (fcc_parser_match(parser, FCC_TOKEN_ASSIGN)) {
    if (!fcc_parser_parse_initializer_expression(parser, &initializer)) {
      return false;
    }
  }

  semicolon_token = fcc_parser_current(parser);
  if (!fcc_parser_expect(parser, FCC_TOKEN_SEMICOLON, "expected ';' after global declaration")) {
    return false;
  }

  if (!fcc_parser_allocate_global(parser, &node)) {
    return false;
  }

  node->span.begin_offset = type.span.begin_offset;
  node->span.end_offset = semicolon_token->span.end_offset;
  node->syntax.decl_specifiers = *decl_specifiers;
  node->syntax.declarator = *declarator;
  node->type = type;
  node->initializer = initializer;
  if (!fcc_parser_get_identifier_name(name_token, &node->name)) {
    return fcc_parser_emit_token_error(parser, name_token, "global identifier interning failed");
  }

  if (node->syntax.decl_specifiers.storage_class == FCC_AST_STORAGE_CLASS_TYPEDEF) {
    if (!fcc_parser_add_typedef_name(parser, node->name)) {
      return fcc_parser_out_of_memory(parser);
    }
  }

  *global_variable = node;
  return true;
}

static bool fcc_parser_tokenize(FccParser* parser) {
  FccLexer lexer;
  FccToken token;

  assert(parser != NULL);

  fcc_lexer_init(&lexer, parser->source_file, parser->diagnostics);
  do {
    fcc_lexer_next(&lexer, &token);
    if (!fcc_parser_append_token(parser, &token)) {
      return fcc_parser_out_of_memory(parser);
    }

    if (token.kind == FCC_TOKEN_INVALID) {
      return false;
    }
  } while (token.kind != FCC_TOKEN_END_OF_FILE);

  return true;
}

bool fcc_parser_parse_translation_unit(const FccSourceFile* source_file,
                                       FccDiagnostics* diagnostics, FccAstContext* ast_context,
                                       FccAstTranslationUnit** translation_unit_out) {
  FccParser parser;
  FccAstStaticAssert** temp_static_assertions;
  size_t temp_static_assertion_count;
  size_t temp_static_assertion_capacity;
  FccAstGlobalVariable** temp_globals;
  size_t temp_global_count;
  size_t temp_global_capacity;
  FccAstFunctionDefinition** temp_functions;
  size_t temp_function_count;
  size_t temp_function_capacity;
  FccAstTranslationUnit* translation_unit;
  bool parse_ok;

  assert(source_file != NULL);
  assert(diagnostics != NULL);
  assert(ast_context != NULL);
  assert(translation_unit_out != NULL);

  parser.source_file = source_file;
  parser.diagnostics = diagnostics;
  parser.ast_context = ast_context;
  parser.tokens = NULL;
  parser.token_count = 0;
  parser.token_capacity = 0;
  parser.next_token_index = 0;
  parser.recursion_depth = 0;
  parser.typedef_names = NULL;
  parser.typedef_count = 0;
  parser.typedef_capacity = 0;
  parser.typedef_scope_offsets = NULL;
  parser.typedef_scope_count = 0;
  parser.typedef_scope_capacity = 0;
  temp_static_assertions = NULL;
  temp_static_assertion_count = 0;
  temp_static_assertion_capacity = 0;
  temp_globals = NULL;
  temp_global_count = 0;
  temp_global_capacity = 0;
  temp_functions = NULL;
  temp_function_count = 0;
  temp_function_capacity = 0;
  translation_unit = NULL;
  parse_ok = false;
  *translation_unit_out = NULL;

  if (!fcc_parser_tokenize(&parser)) {
    goto cleanup;
  }

  if (!fcc_parser_push_typedef_scope(&parser)) {
    (void)fcc_parser_out_of_memory(&parser);
    goto cleanup;
  }

  while (!fcc_parser_is_current(&parser, FCC_TOKEN_END_OF_FILE)) {
    FccAstDeclSpecifiers decl_specifiers;
    FccAstDeclarator declarator;
    FccAstType type;
    const FccToken* name_token;

    if (fcc_parser_is_current(&parser, FCC_TOKEN_KW_STATIC_ASSERT)) {
      FccAstStaticAssert* static_assertion;

      if (!fcc_parser_allocate_static_assert(&parser, &static_assertion)) {
        goto cleanup;
      }

      if (!fcc_parser_parse_static_assert(&parser, static_assertion)) {
        goto cleanup;
      }

      if (!fcc_parser_append_temp_pointer((void***)&temp_static_assertions,
                                          &temp_static_assertion_count,
                                          &temp_static_assertion_capacity, static_assertion)) {
        (void)fcc_parser_out_of_memory(&parser);
        goto cleanup;
      }

      continue;
    }

    if (!fcc_parser_parse_declaration_specifiers(&parser, true, &decl_specifiers, &type)) {
      goto cleanup;
    }

    if (fcc_parser_is_current(&parser, FCC_TOKEN_SEMICOLON)) {
      const FccToken* semicolon_token;
      FccAstGlobalVariable* global_variable;

      semicolon_token = fcc_parser_consume(&parser);
      if (!fcc_parser_allocate_global(&parser, &global_variable)) {
        goto cleanup;
      }

      global_variable->span.begin_offset = type.span.begin_offset;
      global_variable->span.end_offset = semicolon_token->span.end_offset;
      global_variable->syntax.decl_specifiers = decl_specifiers;
      fcc_parser_init_declarator(&global_variable->syntax.declarator);
      global_variable->type = type;
      global_variable->name = NULL;
      global_variable->initializer = NULL;
      if (!fcc_parser_append_temp_pointer((void***)&temp_globals, &temp_global_count,
                                          &temp_global_capacity, global_variable)) {
        (void)fcc_parser_out_of_memory(&parser);
        goto cleanup;
      }

      continue;
    }

    if (!fcc_parser_parse_named_declarator(&parser, &type, &declarator, true, &name_token)) {
      goto cleanup;
    }

    if (fcc_parser_is_current(&parser, FCC_TOKEN_LPAREN)) {
      FccAstFunctionDefinition* function_definition;

      if (!fcc_parser_parse_function_after_name(&parser, &decl_specifiers, &declarator, type,
                                                name_token, &function_definition)) {
        goto cleanup;
      }

      if (!fcc_parser_append_temp_pointer((void***)&temp_functions, &temp_function_count,
                                          &temp_function_capacity, function_definition)) {
        (void)fcc_parser_out_of_memory(&parser);
        goto cleanup;
      }
    } else {
      FccAstGlobalVariable* global_variable;

      if (!fcc_parser_parse_global_variable(&parser, &decl_specifiers, &declarator, type,
                                            name_token, &global_variable)) {
        goto cleanup;
      }

      if (!fcc_parser_append_temp_pointer((void***)&temp_globals, &temp_global_count,
                                          &temp_global_capacity, global_variable)) {
        (void)fcc_parser_out_of_memory(&parser);
        goto cleanup;
      }
    }
  }

  if ((temp_function_count == 0) && (temp_global_count == 0) &&
      (temp_static_assertion_count == 0)) {
    (void)fcc_parser_emit_current_error(
        &parser, "translation unit must contain at least one top-level declaration");
    goto cleanup;
  }

  if (!fcc_parser_allocate_translation_unit(&parser, &translation_unit)) {
    goto cleanup;
  }

  if (!fcc_parser_copy_pointer_array(&parser, (void***)&translation_unit->globals,
                                     (void**)temp_globals, temp_global_count)) {
    (void)fcc_parser_out_of_memory(&parser);
    goto cleanup;
  }

  if (!fcc_parser_copy_pointer_array(&parser, (void***)&translation_unit->static_assertions,
                                     (void**)temp_static_assertions,
                                     temp_static_assertion_count)) {
    (void)fcc_parser_out_of_memory(&parser);
    goto cleanup;
  }

  if (!fcc_parser_copy_pointer_array(&parser, (void***)&translation_unit->functions,
                                     (void**)temp_functions, temp_function_count)) {
    (void)fcc_parser_out_of_memory(&parser);
    goto cleanup;
  }

  translation_unit->static_assertion_count = temp_static_assertion_count;
  translation_unit->global_count = temp_global_count;
  translation_unit->function_count = temp_function_count;
  translation_unit->span.begin_offset = SIZE_MAX;
  translation_unit->span.end_offset = 0;
  if (temp_static_assertion_count != 0) {
    translation_unit->span.begin_offset = temp_static_assertions[0]->span.begin_offset;
    translation_unit->span.end_offset =
        temp_static_assertions[temp_static_assertion_count - 1]->span.end_offset;
  }

  if (temp_global_count != 0) {
    if ((translation_unit->span.begin_offset == SIZE_MAX) ||
        (temp_globals[0]->span.begin_offset < translation_unit->span.begin_offset)) {
      translation_unit->span.begin_offset = temp_globals[0]->span.begin_offset;
    }

    if (temp_globals[temp_global_count - 1]->span.end_offset > translation_unit->span.end_offset) {
      translation_unit->span.end_offset = temp_globals[temp_global_count - 1]->span.end_offset;
    }
  }

  if (temp_function_count != 0) {
    if ((translation_unit->span.begin_offset == SIZE_MAX) ||
        (temp_functions[0]->span.begin_offset < translation_unit->span.begin_offset)) {
      translation_unit->span.begin_offset = temp_functions[0]->span.begin_offset;
    }

    if (temp_functions[temp_function_count - 1]->span.end_offset >
        translation_unit->span.end_offset) {
      translation_unit->span.end_offset = temp_functions[temp_function_count - 1]->span.end_offset;
    }
  }

  if (translation_unit->span.begin_offset == SIZE_MAX) {
    translation_unit->span.begin_offset = 0;
  }

  *translation_unit_out = translation_unit;
  parse_ok = true;

cleanup:
  while (parser.typedef_scope_count > 0) {
    fcc_parser_pop_typedef_scope(&parser);
  }

  free(temp_globals);
  free(temp_static_assertions);
  free(temp_functions);
  free(parser.tokens);
  free(parser.typedef_names);
  free(parser.typedef_scope_offsets);
  return parse_ok;
}
