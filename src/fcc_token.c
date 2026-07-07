// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/token.h"

#include <assert.h>
#include <stdio.h>

static FILE* fcc_token_resolve_stream(FILE* stream) {
  if (stream != NULL) {
    return stream;
  }

  return stdout;
}

static void fcc_token_dump_escaped_text(FILE* stream, const BYTE* bytes, size_t byte_count) {
  size_t byte_index;

  assert(stream != NULL);
  assert(bytes != NULL);

  for (byte_index = 0; byte_index < byte_count; ++byte_index) {
    BYTE current_byte;

    current_byte = bytes[byte_index];
    switch (current_byte) {
      case (BYTE)'\\':
        fputs("\\\\", stream);
        break;
      case (BYTE)'"':
        fputs("\\\"", stream);
        break;
      case (BYTE)'\n':
        fputs("\\n", stream);
        break;
      case (BYTE)'\r':
        fputs("\\r", stream);
        break;
      case (BYTE)'\t':
        fputs("\\t", stream);
        break;
      default:
        if ((current_byte >= (BYTE)32) && (current_byte <= (BYTE)126)) {
          fputc((int)current_byte, stream);
        } else {
          fprintf(stream, "\\x%02X", (unsigned int)current_byte);
        }

        break;
    }
  }
}

const char* fcc_token_kind_name(FccTokenKind kind) {
  switch (kind) {
    case FCC_TOKEN_INVALID:
      return "INVALID";
    case FCC_TOKEN_END_OF_FILE:
      return "END_OF_FILE";
    case FCC_TOKEN_IDENTIFIER:
      return "IDENTIFIER";
    case FCC_TOKEN_INTEGER_LITERAL:
      return "INTEGER_LITERAL";
    case FCC_TOKEN_STRING_LITERAL:
      return "STRING_LITERAL";
    case FCC_TOKEN_CHAR_LITERAL:
      return "CHAR_LITERAL";
    case FCC_TOKEN_KW_INT:
      return "KW_INT";
    case FCC_TOKEN_KW_VOID:
      return "KW_VOID";
    case FCC_TOKEN_KW_CHAR:
      return "KW_CHAR";
    case FCC_TOKEN_KW_SIGNED:
      return "KW_SIGNED";
    case FCC_TOKEN_KW_UNSIGNED:
      return "KW_UNSIGNED";
    case FCC_TOKEN_KW_SHORT:
      return "KW_SHORT";
    case FCC_TOKEN_KW_LONG:
      return "KW_LONG";
    case FCC_TOKEN_KW_BOOL:
      return "KW_BOOL";
    case FCC_TOKEN_KW_STRUCT:
      return "KW_STRUCT";
    case FCC_TOKEN_KW_UNION:
      return "KW_UNION";
    case FCC_TOKEN_KW_ENUM:
      return "KW_ENUM";
    case FCC_TOKEN_KW_TYPEDEF:
      return "KW_TYPEDEF";
    case FCC_TOKEN_KW_RETURN:
      return "KW_RETURN";
    case FCC_TOKEN_KW_IF:
      return "KW_IF";
    case FCC_TOKEN_KW_ELSE:
      return "KW_ELSE";
    case FCC_TOKEN_KW_WHILE:
      return "KW_WHILE";
    case FCC_TOKEN_KW_DO:
      return "KW_DO";
    case FCC_TOKEN_KW_FOR:
      return "KW_FOR";
    case FCC_TOKEN_KW_SWITCH:
      return "KW_SWITCH";
    case FCC_TOKEN_KW_CASE:
      return "KW_CASE";
    case FCC_TOKEN_KW_DEFAULT:
      return "KW_DEFAULT";
    case FCC_TOKEN_KW_GOTO:
      return "KW_GOTO";
    case FCC_TOKEN_KW_BREAK:
      return "KW_BREAK";
    case FCC_TOKEN_KW_CONTINUE:
      return "KW_CONTINUE";
    case FCC_TOKEN_KW_STATIC:
      return "KW_STATIC";
    case FCC_TOKEN_KW_EXTERN:
      return "KW_EXTERN";
    case FCC_TOKEN_KW_CONST:
      return "KW_CONST";
    case FCC_TOKEN_KW_SIZEOF:
      return "KW_SIZEOF";
    case FCC_TOKEN_KW_ALIGNOF:
      return "KW_ALIGNOF";
    case FCC_TOKEN_KW_STATIC_ASSERT:
      return "KW_STATIC_ASSERT";
    case FCC_TOKEN_LPAREN:
      return "LPAREN";
    case FCC_TOKEN_RPAREN:
      return "RPAREN";
    case FCC_TOKEN_LBRACE:
      return "LBRACE";
    case FCC_TOKEN_RBRACE:
      return "RBRACE";
    case FCC_TOKEN_LBRACKET:
      return "LBRACKET";
    case FCC_TOKEN_RBRACKET:
      return "RBRACKET";
    case FCC_TOKEN_SEMICOLON:
      return "SEMICOLON";
    case FCC_TOKEN_COMMA:
      return "COMMA";
    case FCC_TOKEN_DOT:
      return "DOT";
    case FCC_TOKEN_ELLIPSIS:
      return "ELLIPSIS";
    case FCC_TOKEN_ARROW:
      return "ARROW";
    case FCC_TOKEN_QUESTION:
      return "QUESTION";
    case FCC_TOKEN_COLON:
      return "COLON";
    case FCC_TOKEN_PLUS:
      return "PLUS";
    case FCC_TOKEN_MINUS:
      return "MINUS";
    case FCC_TOKEN_STAR:
      return "STAR";
    case FCC_TOKEN_SLASH:
      return "SLASH";
    case FCC_TOKEN_PERCENT:
      return "PERCENT";
    case FCC_TOKEN_ASSIGN:
      return "ASSIGN";
    case FCC_TOKEN_PLUS_ASSIGN:
      return "PLUS_ASSIGN";
    case FCC_TOKEN_MINUS_ASSIGN:
      return "MINUS_ASSIGN";
    case FCC_TOKEN_STAR_ASSIGN:
      return "STAR_ASSIGN";
    case FCC_TOKEN_SLASH_ASSIGN:
      return "SLASH_ASSIGN";
    case FCC_TOKEN_PERCENT_ASSIGN:
      return "PERCENT_ASSIGN";
    case FCC_TOKEN_BITWISE_AND_ASSIGN:
      return "BITWISE_AND_ASSIGN";
    case FCC_TOKEN_BITWISE_OR_ASSIGN:
      return "BITWISE_OR_ASSIGN";
    case FCC_TOKEN_BITWISE_XOR_ASSIGN:
      return "BITWISE_XOR_ASSIGN";
    case FCC_TOKEN_EQUAL:
      return "EQUAL";
    case FCC_TOKEN_NOT_EQUAL:
      return "NOT_EQUAL";
    case FCC_TOKEN_LESS:
      return "LESS";
    case FCC_TOKEN_LESS_EQUAL:
      return "LESS_EQUAL";
    case FCC_TOKEN_GREATER:
      return "GREATER";
    case FCC_TOKEN_GREATER_EQUAL:
      return "GREATER_EQUAL";
    case FCC_TOKEN_LEFT_SHIFT_ASSIGN:
      return "LEFT_SHIFT_ASSIGN";
    case FCC_TOKEN_RIGHT_SHIFT_ASSIGN:
      return "RIGHT_SHIFT_ASSIGN";
    case FCC_TOKEN_LOGICAL_AND:
      return "LOGICAL_AND";
    case FCC_TOKEN_LOGICAL_OR:
      return "LOGICAL_OR";
    case FCC_TOKEN_LOGICAL_NOT:
      return "LOGICAL_NOT";
    case FCC_TOKEN_BITWISE_AND:
      return "BITWISE_AND";
    case FCC_TOKEN_BITWISE_OR:
      return "BITWISE_OR";
    case FCC_TOKEN_BITWISE_XOR:
      return "BITWISE_XOR";
    case FCC_TOKEN_BITWISE_NOT:
      return "BITWISE_NOT";
    case FCC_TOKEN_LEFT_SHIFT:
      return "LEFT_SHIFT";
    case FCC_TOKEN_RIGHT_SHIFT:
      return "RIGHT_SHIFT";
    case FCC_TOKEN_INCREMENT:
      return "INCREMENT";
    case FCC_TOKEN_DECREMENT:
      return "DECREMENT";
  }

  return "UNKNOWN_TOKEN";
}

void fcc_token_dump(FILE* stream, const FccToken* token) {
  assert(token != NULL);

  fprintf(fcc_token_resolve_stream(stream), "%s span=[%zu,%zu) text_length=%zu\n",
          fcc_token_kind_name(token->kind), token->span.begin_offset, token->span.end_offset,
          token->text_length);
}

void fcc_token_dump_source(FILE* stream, const FccToken* token, const FccSourceFile* source_file) {
  FILE* output_stream;
  FccSourceLocation location;

  assert(token != NULL);

  if ((source_file == NULL) || (source_file->bytes == NULL) ||
      (token->span.end_offset < token->span.begin_offset) ||
      (token->span.end_offset > source_file->byte_count)) {
    fcc_token_dump(stream, token);
    return;
  }

  output_stream = fcc_token_resolve_stream(stream);
  location = fcc_source_offset_to_location(source_file, token->span.begin_offset);
  fprintf(output_stream, "%s %zu:%zu span=[%zu,%zu) text=\"", fcc_token_kind_name(token->kind),
          location.line, location.column, token->span.begin_offset, token->span.end_offset);
  fcc_token_dump_escaped_text(output_stream, source_file->bytes + token->span.begin_offset,
                              token->text_length);
  fprintf(output_stream, "\"\n");
}
