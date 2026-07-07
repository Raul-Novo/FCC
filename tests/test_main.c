// SPDX-License-Identifier: GPL-3.0-or-later
#define WIN32_LEAN_AND_MEAN

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "fcc/ast.h"
#include "fcc/base.h"
#include "fcc/codegen.h"
#include "fcc/diag.h"
#include "fcc/driver.h"
#include "fcc/lexer.h"
#include "fcc/parser.h"
#include "fcc/preprocessor.h"
#include "fcc/sema.h"
#include "fcc/signature.h"
#include "fcc/source.h"
#include "fcc/symbol.h"
#include "fcc/token.h"
#include "fcc/type.h"

typedef struct TestContext {
  size_t passed_count;
  size_t failed_count;
} TestContext;

static FILE* test_open_temp_stream(void) {
  FILE* stream;

  stream = NULL;
  if (tmpfile_s(&stream) != 0) {
    return NULL;
  }

  return stream;
}

static bool test_expect(TestContext* context, bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "test failure: %s\n", message);
    ++context->failed_count;
    return false;
  }

  ++context->passed_count;
  return true;
}

static bool test_read_stream(FILE* stream, char* buffer, size_t buffer_size) {
  size_t bytes_read;

  if ((stream == NULL) || (buffer == NULL) || (buffer_size == 0)) {
    return false;
  }

  rewind(stream);
  bytes_read = fread(buffer, 1, buffer_size - 1, stream);
  if (ferror(stream) != 0) {
    return false;
  }

  buffer[bytes_read] = '\0';
  return true;
}

static bool test_read_path_file(const char* path, char* buffer, size_t buffer_size) {
  FILE* stream;
  bool ok;

  if ((path == NULL) || (buffer == NULL) || (buffer_size == 0)) {
    return false;
  }

  stream = NULL;
  if (fopen_s(&stream, path, "rb") != 0) {
    return false;
  }

  ok = test_read_stream(stream, buffer, buffer_size);
  (void)fclose(stream);
  return ok;
}

static bool test_path_exists_nonempty(const char* path) {
  FILE* stream;
  long file_size;

  if (path == NULL) {
    return false;
  }

  stream = NULL;
  if (fopen_s(&stream, path, "rb") != 0) {
    return false;
  }

  if (fseek(stream, 0, SEEK_END) != 0) {
    (void)fclose(stream);
    return false;
  }

  file_size = ftell(stream);
  (void)fclose(stream);
  return file_size > 0;
}

static size_t test_count_substring(const char* text, const char* needle) {
  const char* match;
  size_t count;
  size_t needle_length;

  if ((text == NULL) || (needle == NULL) || (*needle == '\0')) {
    return 0;
  }

  count = 0;
  needle_length = strlen(needle);
  match = text;
  while ((match = strstr(match, needle)) != NULL) {
    ++count;
    match += needle_length;
  }

  return count;
}

static int test_run_program(const char* path) {
  STARTUPINFOA startup_info;
  PROCESS_INFORMATION process_info;
  DWORD process_exit_code;
  DWORD wait_result;
  char command_line[FCC_MAX_PATH_LENGTH + 3];
  int written;

  if (path == NULL) {
    return -1;
  }

  written = snprintf(command_line, sizeof(command_line), "\"%s\"", path);
  if ((written < 0) || ((size_t)written >= sizeof(command_line))) {
    return -1;
  }

  memset(&startup_info, 0, sizeof(startup_info));
  memset(&process_info, 0, sizeof(process_info));
  startup_info.cb = sizeof(startup_info);
  if (!CreateProcessA(path, command_line, NULL, NULL, FALSE, 0, NULL, NULL, &startup_info,
                      &process_info)) {
    fprintf(stderr, "test failure: failed to launch '%s' (GetLastError=%lu)\n", path,
            (unsigned long)GetLastError());
    return -1;
  }

  wait_result = WaitForSingleObject(process_info.hProcess, INFINITE);
  if (wait_result == WAIT_FAILED) {
    fprintf(stderr, "test failure: failed to wait for '%s' (GetLastError=%lu)\n", path,
            (unsigned long)GetLastError());
    (void)CloseHandle(process_info.hThread);
    (void)CloseHandle(process_info.hProcess);
    return -1;
  }

  process_exit_code = 0;
  if (!GetExitCodeProcess(process_info.hProcess, &process_exit_code)) {
    fprintf(stderr, "test failure: failed to read exit code for '%s' (GetLastError=%lu)\n", path,
            (unsigned long)GetLastError());
    (void)CloseHandle(process_info.hThread);
    (void)CloseHandle(process_info.hProcess);
    return -1;
  }

  (void)CloseHandle(process_info.hThread);
  (void)CloseHandle(process_info.hProcess);
  if (process_exit_code > 2147483647UL) {
    return -1;
  }

  return (int)process_exit_code;
}

static bool test_signature_helpers(TestContext* context) {
  const FccSignatureInfo* info;
  FILE* stream;
  char buffer[256];

  info = fcc_signature_get_info();
  if (!test_expect(context, info != NULL, "signature info should be available")) {
    return false;
  }

  if (!test_expect(context, strcmp(info->short_name, "FCC") == 0,
                   "signature short name should be stable")) {
    return false;
  }

  if (!test_expect(context, strcmp(info->product_name, "Flintine Studios C Compiler") == 0,
                   "signature product name should be stable")) {
    return false;
  }

  if (!test_expect(context, strcmp(fcc_signature_get_embedded_marker(), "FCCSIG_V1") == 0,
                   "signature marker should be stable")) {
    return false;
  }

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for signature output")) {
    return false;
  }

  fcc_signature_print_version(stream);
  if (!test_read_stream(stream, buffer, sizeof(buffer))) {
    (void)fclose(stream);
    return test_expect(context, false, "signature output should be readable");
  }

  if (!test_expect(context, strstr(buffer, "FCC 0.8.0-dev") != NULL,
                   "signature output should include the version string")) {
    (void)fclose(stream);
    return false;
  }

  (void)fclose(stream);
  return true;
}

static bool test_type_and_symbol_helpers(TestContext* context) {
  FccSymbol inner_symbol;
  const FccSymbol* lookup_symbol;
  FccSymbol outer_symbol;
  FccSymbolTable table;
  FccTypeContext type_context;
  FccTypeId int_type_id;

  fcc_type_context_init(&type_context);
  fcc_symbol_table_init(&table);
  int_type_id = fcc_type_context_get_builtin(&type_context, FCC_TYPE_INT, false);
  if (!test_expect(context, strcmp(fcc_type_name(FCC_TYPE_INT), "int") == 0,
                   "type name for int should be stable")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  if (!test_expect(context, strcmp(fcc_symbol_kind_name(FCC_SYMBOL_LOCAL), "local") == 0,
                   "symbol kind name should be stable")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  if (!test_expect(context, fcc_symbol_table_push_scope(&table),
                   "symbol table should push an outer scope")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  memset(&outer_symbol, 0, sizeof(outer_symbol));
  outer_symbol.kind = FCC_SYMBOL_LOCAL;
  outer_symbol.type_id = int_type_id;
  outer_symbol.span.begin_offset = 1;
  outer_symbol.span.end_offset = 2;
  outer_symbol.name = "value";
  if (!test_expect(context, fcc_symbol_table_define(&table, &outer_symbol),
                   "symbol table should define the outer symbol")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  lookup_symbol = fcc_symbol_table_lookup(&table, "value");
  if (!test_expect(context, lookup_symbol != NULL, "outer symbol lookup should succeed")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  if (!test_expect(context, lookup_symbol->scope_depth == 1,
                   "outer symbol should be recorded in scope depth one")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  if (!test_expect(context, fcc_symbol_table_push_scope(&table),
                   "symbol table should push an inner scope")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  memset(&inner_symbol, 0, sizeof(inner_symbol));
  inner_symbol.kind = FCC_SYMBOL_PARAMETER;
  inner_symbol.type_id = int_type_id;
  inner_symbol.span.begin_offset = 3;
  inner_symbol.span.end_offset = 4;
  inner_symbol.name = "value";
  if (!test_expect(context, fcc_symbol_table_define(&table, &inner_symbol),
                   "symbol table should define the inner symbol")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  lookup_symbol = fcc_symbol_table_lookup_current_scope(&table, "value");
  if (!test_expect(context, lookup_symbol != NULL, "current scope lookup should succeed")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  if (!test_expect(context, lookup_symbol->kind == FCC_SYMBOL_PARAMETER,
                   "current scope lookup should prefer the inner symbol")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  fcc_symbol_table_pop_scope(&table);
  lookup_symbol = fcc_symbol_table_lookup(&table, "value");
  if (!test_expect(context, lookup_symbol != NULL, "lookup after pop should still succeed")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  if (!test_expect(context, lookup_symbol->kind == FCC_SYMBOL_LOCAL,
                   "lookup after pop should reveal the outer symbol")) {
    fcc_type_context_dispose(&type_context);
    fcc_symbol_table_dispose(&table);
    return false;
  }

  fcc_symbol_table_pop_scope(&table);
  fcc_symbol_table_dispose(&table);
  fcc_type_context_dispose(&type_context);
  return true;
}

static bool test_source_load_success(TestContext* context) {
  FccSourceFile source_file;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  ok = fcc_source_file_load(&source_file, "tests/fixtures/sample.c", error_buffer,
                            sizeof(error_buffer));
  if (!test_expect(context, ok, "source file should load")) {
    return false;
  }

  if (!test_expect(context, source_file.byte_count > 0, "source file must contain bytes")) {
    fcc_source_file_dispose(&source_file);
    return false;
  }

  if (!test_expect(context, source_file.line_count >= 4, "source file should have line starts")) {
    fcc_source_file_dispose(&source_file);
    return false;
  }

  if (!test_expect(context, source_file.bytes[0] == (BYTE)'i',
                   "first source byte should match fixture")) {
    fcc_source_file_dispose(&source_file);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  return true;
}

static bool test_line_column_mapping(TestContext* context) {
  FccSourceFile source_file;
  FccSourceLocation location;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  const char* return_text;
  BYTE* return_position;
  bool ok;

  ok = fcc_source_file_load(&source_file, "tests/fixtures/sample.c", error_buffer,
                            sizeof(error_buffer));
  if (!test_expect(context, ok, "fixture must load for location mapping")) {
    return false;
  }

  return_text = "return";
  return_position = (BYTE*)strstr((const char*)source_file.bytes, return_text);
  if (!test_expect(context, return_position != NULL, "fixture must contain return")) {
    fcc_source_file_dispose(&source_file);
    return false;
  }

  location =
      fcc_source_offset_to_location(&source_file, (size_t)(return_position - source_file.bytes));
  if (!test_expect(context, location.line == 3, "return should be on line 3")) {
    fcc_source_file_dispose(&source_file);
    return false;
  }

  if (!test_expect(context, location.column == 3, "return should start at column 3")) {
    fcc_source_file_dispose(&source_file);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  return true;
}

static bool test_diag_formatting(TestContext* context) {
  FccDiagnostics diagnostics;
  FccSourceLocation location;
  FILE* stream;
  char buffer[256];
  const char* expected;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for diagnostics")) {
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  location.offset = 0;
  location.line = 1;
  location.column = 1;
  fcc_diag_emit(&diagnostics, "missing.c", location, FCC_DIAG_SEVERITY_ERROR, "file not found");
  if (!test_expect(context, diagnostics.error_count == 1,
                   "diagnostic error count should increment")) {
    (void)fclose(stream);
    return false;
  }

  if (!test_read_stream(stream, buffer, sizeof(buffer))) {
    (void)fclose(stream);
    return test_expect(context, false, "diagnostic output should be readable");
  }

  expected = "missing.c(1,1): error: file not found\n";
  if (!test_expect(context, strcmp(buffer, expected) == 0, "diagnostic format should be stable")) {
    (void)fclose(stream);
    return false;
  }

  (void)fclose(stream);
  return true;
}

static bool test_token_helpers(TestContext* context) {
  FccToken token;
  FILE* stream;
  char buffer[256];
  const char* expected;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for token dump")) {
    return false;
  }

  token.kind = FCC_TOKEN_KW_RETURN;
  token.span.begin_offset = 4;
  token.span.end_offset = 10;
  token.text_length = 6;

  if (!test_expect(context, strcmp(fcc_token_kind_name(FCC_TOKEN_KW_RETURN), "KW_RETURN") == 0,
                   "token kind name should be stable")) {
    (void)fclose(stream);
    return false;
  }

  fcc_token_dump(stream, &token);
  if (!test_read_stream(stream, buffer, sizeof(buffer))) {
    (void)fclose(stream);
    return test_expect(context, false, "token dump output should be readable");
  }

  expected = "KW_RETURN span=[4,10) text_length=6\n";
  if (!test_expect(context, strcmp(buffer, expected) == 0, "token dump should be stable")) {
    (void)fclose(stream);
    return false;
  }

  (void)fclose(stream);
  return true;
}

static bool test_lexer_sequence(TestContext* context, const char* path,
                                const FccTokenKind* expected_kinds, size_t expected_count) {
  FccDiagnostics diagnostics;
  FccLexer lexer;
  FccSourceFile source_file;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  size_t token_index;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for lexer tests")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, path, error_buffer, sizeof(error_buffer)),
                   "lexer fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_lexer_init(&lexer, &source_file, &diagnostics);

  for (token_index = 0; token_index < expected_count; ++token_index) {
    FccToken token;
    char message[128];

    fcc_lexer_next(&lexer, &token);
    (void)snprintf(message, sizeof(message), "unexpected token kind at index %zu", token_index);
    if (!test_expect(context, token.kind == expected_kinds[token_index], message)) {
      fcc_source_file_dispose(&source_file);
      (void)fclose(stream);
      return false;
    }
  }

  if (!test_expect(context, diagnostics.error_count == 0,
                   "lexer sequence fixture should not emit diagnostics")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_lexer_simple_sequence(TestContext* context) {
  static const FccTokenKind EXPECTED_KINDS[] = {
      FCC_TOKEN_KW_INT,    FCC_TOKEN_IDENTIFIER, FCC_TOKEN_LPAREN,      FCC_TOKEN_KW_VOID,
      FCC_TOKEN_RPAREN,    FCC_TOKEN_LBRACE,     FCC_TOKEN_KW_RETURN,   FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_SEMICOLON, FCC_TOKEN_RBRACE,     FCC_TOKEN_END_OF_FILE,
  };

  return test_lexer_sequence(context, "tests/fixtures/sample.c", EXPECTED_KINDS,
                             sizeof(EXPECTED_KINDS) / sizeof(EXPECTED_KINDS[0]));
}

static bool test_lexer_operator_sequence(TestContext* context) {
  static const FccTokenKind EXPECTED_KINDS[] = {
      FCC_TOKEN_KW_STATIC,
      FCC_TOKEN_KW_INT,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_ASSIGN,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_KW_INT,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_LPAREN,
      FCC_TOKEN_KW_VOID,
      FCC_TOKEN_RPAREN,
      FCC_TOKEN_LBRACE,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_PLUS_ASSIGN,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_DECREMENT,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_INCREMENT,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_ASSIGN,
      FCC_TOKEN_BITWISE_NOT,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_SLASH,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_PERCENT,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_MINUS,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_STAR,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_ASSIGN,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_LBRACKET,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_RBRACKET,
      FCC_TOKEN_PLUS,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_DOT,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_ASSIGN,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_ARROW,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_ASSIGN,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_LPAREN,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_COMMA,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_RPAREN,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_KW_IF,
      FCC_TOKEN_LPAREN,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_GREATER_EQUAL,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_LOGICAL_AND,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_NOT_EQUAL,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_RPAREN,
      FCC_TOKEN_LBRACE,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_ASSIGN,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_LEFT_SHIFT,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_RBRACE,
      FCC_TOKEN_KW_ELSE,
      FCC_TOKEN_LBRACE,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_ASSIGN,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_BITWISE_OR,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_BITWISE_XOR,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_BITWISE_AND,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_RBRACE,
      FCC_TOKEN_KW_RETURN,
      FCC_TOKEN_LOGICAL_NOT,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_QUESTION,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_COLON,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_RBRACE,
      FCC_TOKEN_END_OF_FILE,
  };

  return test_lexer_sequence(context, "tests/fixtures/lex_operators.c", EXPECTED_KINDS,
                             sizeof(EXPECTED_KINDS) / sizeof(EXPECTED_KINDS[0]));
}

static bool test_lexer_alignof_sequence(TestContext* context) {
  static const FccTokenKind EXPECTED_KINDS[] = {
      FCC_TOKEN_KW_INT,    FCC_TOKEN_IDENTIFIER, FCC_TOKEN_LPAREN,
      FCC_TOKEN_KW_VOID,   FCC_TOKEN_RPAREN,     FCC_TOKEN_LBRACE,
      FCC_TOKEN_KW_RETURN, FCC_TOKEN_KW_ALIGNOF, FCC_TOKEN_LPAREN,
      FCC_TOKEN_KW_INT,    FCC_TOKEN_RPAREN,     FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_RBRACE,    FCC_TOKEN_END_OF_FILE,
  };

  return test_lexer_sequence(context, "tests/fixtures/parser_alignof.c", EXPECTED_KINDS,
                             sizeof(EXPECTED_KINDS) / sizeof(EXPECTED_KINDS[0]));
}

static bool test_lexer_static_assert_sequence(TestContext* context) {
  static const FccTokenKind EXPECTED_KINDS[] = {
      FCC_TOKEN_KW_STATIC_ASSERT,
      FCC_TOKEN_LPAREN,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_COMMA,
      FCC_TOKEN_STRING_LITERAL,
      FCC_TOKEN_RPAREN,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_KW_INT,
      FCC_TOKEN_IDENTIFIER,
      FCC_TOKEN_LPAREN,
      FCC_TOKEN_KW_VOID,
      FCC_TOKEN_RPAREN,
      FCC_TOKEN_LBRACE,
      FCC_TOKEN_KW_STATIC_ASSERT,
      FCC_TOKEN_LPAREN,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_COMMA,
      FCC_TOKEN_STRING_LITERAL,
      FCC_TOKEN_RPAREN,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_KW_RETURN,
      FCC_TOKEN_INTEGER_LITERAL,
      FCC_TOKEN_SEMICOLON,
      FCC_TOKEN_RBRACE,
      FCC_TOKEN_END_OF_FILE,
  };

  return test_lexer_sequence(context, "tests/fixtures/parser_static_assert.c", EXPECTED_KINDS,
                             sizeof(EXPECTED_KINDS) / sizeof(EXPECTED_KINDS[0]));
}

static bool test_lexer_accepts_long_identifier(TestContext* context) {
  FccDiagnostics diagnostics;
  FccLexer lexer;
  FccSourceFile source_file;
  FccToken token;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool saw_invalid;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for lexer diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/identifier_too_long.c",
                                        error_buffer, sizeof(error_buffer)),
                   "long identifier fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_lexer_init(&lexer, &source_file, &diagnostics);
  saw_invalid = false;
  do {
    fcc_lexer_next(&lexer, &token);
    if (token.kind == FCC_TOKEN_INVALID) {
      saw_invalid = true;
    }
  } while (token.kind != FCC_TOKEN_END_OF_FILE);

  if (!test_expect(context, diagnostics.error_count == 0,
                   "long identifier should be accepted without diagnostics")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, !saw_invalid, "long identifier should not produce invalid tokens")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_lexer_rejects_invalid_character(TestContext* context) {
  FccDiagnostics diagnostics;
  FccLexer lexer;
  FccSourceFile source_file;
  FccToken token;
  FILE* stream;
  char buffer[512];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool saw_invalid;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for lexer diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/invalid_character.c",
                                        error_buffer, sizeof(error_buffer)),
                   "invalid character fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_lexer_init(&lexer, &source_file, &diagnostics);
  saw_invalid = false;
  do {
    fcc_lexer_next(&lexer, &token);
    if (token.kind == FCC_TOKEN_INVALID) {
      saw_invalid = true;
    }
  } while (token.kind != FCC_TOKEN_END_OF_FILE);

  if (!test_expect(context, diagnostics.error_count == 1,
                   "invalid character should emit one diagnostic")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, saw_invalid, "invalid character should produce an invalid token")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_read_stream(stream, buffer, sizeof(buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return test_expect(context, false, "invalid character diagnostics should be readable");
  }

  if (!test_expect(context, strstr(buffer, "invalid character 0x40") != NULL,
                   "invalid character diagnostic should include the byte value")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_lexer_rejects_unterminated_comment(TestContext* context) {
  FccDiagnostics diagnostics;
  FccLexer lexer;
  FccSourceFile source_file;
  FccToken token;
  FILE* stream;
  char buffer[512];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for lexer diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/unterminated_comment.c",
                                        error_buffer, sizeof(error_buffer)),
                   "unterminated comment fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_lexer_init(&lexer, &source_file, &diagnostics);
  do {
    fcc_lexer_next(&lexer, &token);
  } while (token.kind != FCC_TOKEN_END_OF_FILE);

  if (!test_expect(context, diagnostics.error_count == 1,
                   "unterminated comment should emit one diagnostic")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_read_stream(stream, buffer, sizeof(buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return test_expect(context, false, "unterminated comment diagnostics should be readable");
  }

  if (!test_expect(context, strstr(buffer, "unterminated block comment") != NULL,
                   "unterminated comment diagnostic should be stable")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_valid_translation_unit(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser tests")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/parser_valid.c", error_buffer,
                                        sizeof(error_buffer)),
                   "parser fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept the valid fixture")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, diagnostics.error_count == 0,
                   "valid parser fixture should not emit diagnostics")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit != NULL, "parser should produce a translation unit")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->function_count == 2,
                   "parser fixture should contain two functions")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, strcmp(translation_unit->functions[0]->name, "sum_to") == 0,
                   "first function name should be sum_to")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->functions[0]->return_type.kind == FCC_AST_TYPE_INT,
                   "first function should return int")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->functions[0]->parameter_count == 1,
                   "first function should have one parameter")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   strcmp(translation_unit->functions[0]->parameters[0].name, "limit") == 0,
                   "parameter name should be limit")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   translation_unit->functions[0]->body->kind == FCC_AST_STATEMENT_COMPOUND,
                   "first function body should be compound")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->functions[0]->body->data.compound.item_count == 4,
                   "sum_to should contain four top-level statements")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   translation_unit->functions[0]->body->data.compound.items[2]->kind ==
                       FCC_AST_STATEMENT_WHILE,
                   "sum_to should contain a while statement")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, strcmp(translation_unit->functions[1]->name, "spin") == 0,
                   "second function name should be spin")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->functions[1]->return_type.kind == FCC_AST_TYPE_VOID,
                   "second function should return void")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->functions[1]->body->data.compound.item_count == 3,
                   "spin should contain three top-level statements")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   translation_unit->functions[1]->body->data.compound.items[1]->kind ==
                       FCC_AST_STATEMENT_FOR,
                   "spin should contain a for statement")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_reports_syntax_error(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* stream;
  char buffer[512];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/parser_missing_semicolon.c",
                                        error_buffer, sizeof(error_buffer)),
                   "invalid parser fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, !ok, "parser should reject the invalid fixture")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, diagnostics.error_count == 1,
                   "syntax error fixture should emit one diagnostic")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_read_stream(stream, buffer, sizeof(buffer))) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return test_expect(context, false, "parser diagnostics should be readable");
  }

  if (!test_expect(context, strstr(buffer, "expected ';' after local declaration") != NULL,
                   "parser diagnostic should identify the missing semicolon")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_function_declaration(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file,
                                        "tests/fixtures/parser_function_declaration.c",
                                        error_buffer, sizeof(error_buffer)),
                   "function declaration fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept standalone function declarations")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, diagnostics.error_count == 0,
                   "function declaration fixture should not emit diagnostics")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit != NULL,
                   "parser should produce a translation unit for function declarations")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->function_count == 2,
                   "function declaration fixture should contain two top-level functions")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, !translation_unit->functions[0]->has_body,
                   "the first function should be declaration-only")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->functions[1]->has_body,
                   "the second function should be a definition")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_function_call(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/parser_function_call.c",
                                        error_buffer, sizeof(error_buffer)),
                   "function call fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept direct function calls")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, diagnostics.error_count == 0,
                   "function call fixture should not emit diagnostics")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit != NULL,
                   "parser should produce a translation unit for function calls")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->function_count == 2,
                   "function call fixture should contain two functions")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->functions[1]->body != NULL,
                   "the second function should have a body")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->functions[1]->body->data.compound.item_count == 1,
                   "the second function should contain one statement")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   translation_unit->functions[1]->body->data.compound.items[0]->kind ==
                       FCC_AST_STATEMENT_RETURN,
                   "the function call fixture should return the call result")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   translation_unit->functions[1]
                           ->body->data.compound.items[0]
                           ->data.return_statement.expression->kind == FCC_AST_EXPRESSION_CALL,
                   "the return expression should be a call")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_function_pointer_declarators(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/function_pointer_valid.c",
                                        error_buffer, sizeof(error_buffer)),
                   "function pointer fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept function pointer declarators")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, diagnostics.error_count == 0,
                   "function pointer fixture should not emit parser diagnostics")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit != NULL,
                   "parser should produce a translation unit for function pointers")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->global_count == 2,
                   "function pointer fixture should contain two typedef declarations")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->globals[0]->type.is_function_pointer,
                   "first typedef should be a function pointer type")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   translation_unit->globals[0]->type.function_pointer_parameter_count == 1,
                   "Unary should have one parameter type")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   translation_unit->globals[0]->type.function_pointer_parameters[0].name == NULL,
                   "function pointer typedef parameter may be anonymous")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_sizeof(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  const FccAstExpression* expression;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/parser_sizeof.c",
                                        error_buffer, sizeof(error_buffer)),
                   "sizeof parser fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept sizeof expressions")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  expression = translation_unit->functions[0]
                   ->body->data.compound.items[2]
                   ->data.return_statement.expression;
  if (!test_expect(context, expression->kind == FCC_AST_EXPRESSION_SIZEOF,
                   "return expression should be a sizeof node")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, !expression->data.sizeof_expression.has_type_operand,
                   "sizeof fixture should use an expression operand")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_alignof(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  const FccAstExpression* expression;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/parser_alignof.c",
                                        error_buffer, sizeof(error_buffer)),
                   "_Alignof parser fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept _Alignof expressions")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  expression = translation_unit->functions[0]
                   ->body->data.compound.items[0]
                   ->data.return_statement.expression;
  if (!test_expect(context, expression->kind == FCC_AST_EXPRESSION_ALIGNOF,
                   "return expression should be an _Alignof node")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, expression->data.alignof_expression.type.kind == FCC_AST_TYPE_INT,
                   "_Alignof fixture should use an int type operand")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_static_assert(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  const FccAstStatement* first_statement;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/parser_static_assert.c",
                                        error_buffer, sizeof(error_buffer)),
                   "_Static_assert parser fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept _Static_assert declarations")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->static_assertion_count == 1,
                   "translation unit should keep one top-level static assertion")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  first_statement = translation_unit->functions[0]->body->data.compound.items[0];
  if (!test_expect(context, first_statement->kind == FCC_AST_STATEMENT_STATIC_ASSERT,
                   "function body should keep the block static assertion")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_global_variables(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/globals_valid.c",
                                        error_buffer, sizeof(error_buffer)),
                   "global parser fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept top-level globals")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, diagnostics.error_count == 0,
                   "global parser fixture should not emit diagnostics")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->global_count == 2,
                   "global parser fixture should contain two globals")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->function_count == 1,
                   "global parser fixture should contain one function")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->globals[0]->initializer != NULL,
                   "the first global should have an initializer")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_recursive_declarators(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  const FccAstFunctionDefinition* function_definition;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/recursive_types_valid.c",
                                        error_buffer, sizeof(error_buffer)),
                   "recursive declarator fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept recursive declarators")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, diagnostics.error_count == 0,
                   "recursive declarator fixture should not emit diagnostics")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->global_count == 3,
                   "recursive fixture should contain three globals")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->function_count == 1,
                   "recursive fixture should contain one function")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->globals[0]->type.pointer_depth == 1,
                   "global_ptr should have one pointer level")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   (translation_unit->globals[1]->type.array_count == 1) &&
                       !translation_unit->globals[1]->type.array_bounds[0].is_vla &&
                       (translation_unit->globals[1]->type.array_bounds[0].element_count == 8),
                   "global_values should keep a fixed array bound")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  function_definition = translation_unit->functions[0];
  if (!test_expect(context, function_definition->parameter_count == 3,
                   "consume should keep three parameters")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, function_definition->parameters[0].type.pointer_depth == 1,
                   "first parameter should be pointer-to-int")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   (function_definition->parameters[1].type.array_count == 1) &&
                       !function_definition->parameters[1].type.array_bounds[0].is_vla &&
                       (function_definition->parameters[1].type.array_bounds[0].element_count == 4),
                   "second parameter should keep its array declarator")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   (function_definition->parameters[2].type.kind == FCC_AST_TYPE_VOID) &&
                       (function_definition->parameters[2].type.pointer_depth == 1),
                   "third parameter should be void pointer")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   (function_definition->body->data.compound.item_count == 5) &&
                       (function_definition->body->data.compound.items[1]->kind ==
                        FCC_AST_STATEMENT_DECLARATION),
                   "function body should keep declaration ordering")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(
          context,
          (function_definition->body->data.compound.items[1]->data.declaration.type.array_count ==
           2) &&
              (function_definition->body->data.compound.items[1]
                   ->data.declaration.type.array_bounds[0]
                   .element_count == 2) &&
              (function_definition->body->data.compound.items[1]
                   ->data.declaration.type.array_bounds[1]
                   .element_count == 3),
          "matrix declaration should preserve multi-dimensional bounds")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(
          context,
          (function_definition->body->data.compound.items[3]->data.declaration.type.array_count ==
           1) &&
              function_definition->body->data.compound.items[3]
                  ->data.declaration.type.array_bounds[0]
                  .is_vla,
          "open_values should preserve unsized array declarator")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_struct_union_declarations(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/struct_union_layout_valid.c",
                                        error_buffer, sizeof(error_buffer)),
                   "struct/union parser fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept struct/union declarations")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, diagnostics.error_count == 0,
                   "struct/union parser fixture should not emit diagnostics")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->global_count == 4,
                   "struct/union fixture should contain four globals")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->function_count == 1,
                   "struct/union fixture should contain one function")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->functions[0]->parameter_count == 0,
                   "size_values should have a void parameter list")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   (translation_unit->globals[0]->type.kind == FCC_AST_TYPE_STRUCT) &&
                       translation_unit->globals[0]->type.is_record_definition &&
                       (translation_unit->globals[0]->type.record_field_count == 3),
                   "layout_global should carry a struct definition with three fields")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   (translation_unit->globals[1]->type.kind == FCC_AST_TYPE_UNION) &&
                       translation_unit->globals[1]->type.is_record_definition &&
                       (translation_unit->globals[1]->type.record_field_count == 2),
                   "value_global should carry a union definition with two fields")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   (translation_unit->globals[2]->type.kind == FCC_AST_TYPE_STRUCT) &&
                       (translation_unit->globals[2]->type.pointer_depth == 1) &&
                       !translation_unit->globals[2]->type.is_record_definition,
                   "head should use an incomplete tagged struct pointer")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   (translation_unit->globals[3]->type.kind == FCC_AST_TYPE_STRUCT) &&
                       translation_unit->globals[3]->type.is_record_definition &&
                       (translation_unit->globals[3]->type.record_field_count == 2),
                   "node_global should complete the tagged struct with two fields")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_enum_declarations(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/enum_valid.c", error_buffer,
                                        sizeof(error_buffer)),
                   "enum parser fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept enum declarations")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, diagnostics.error_count == 0,
                   "enum parser fixture should not emit diagnostics")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, translation_unit->global_count == 4,
                   "enum fixture should contain four top-level declarations")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   (translation_unit->globals[0]->name == NULL) &&
                       (translation_unit->globals[0]->type.kind == FCC_AST_TYPE_ENUM) &&
                       translation_unit->globals[0]->type.is_enum_definition &&
                       (translation_unit->globals[0]->type.enumerator_count == 3),
                   "first enum declaration should be a type-only enum definition")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   (translation_unit->globals[1]->type.kind == FCC_AST_TYPE_ENUM) &&
                       translation_unit->globals[1]->type.is_enum_definition &&
                       (translation_unit->globals[1]->type.enumerator_count == 3),
                   "typedef enum should carry its enumerator list")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_member_and_subscript_expressions(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/member_subscript_valid.c",
                                        error_buffer, sizeof(error_buffer)),
                   "member/subscript parser fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept member and subscript expressions")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   translation_unit->functions[1]
                           ->body->data.compound.items[2]
                           ->data.expression_statement.expression->data.assign.target->kind ==
                       FCC_AST_EXPRESSION_MEMBER,
                   "pair.left assignment target should parse as member access")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   translation_unit->functions[1]
                           ->body->data.compound.items[4]
                           ->data.expression_statement.expression->data.assign.target->kind ==
                       FCC_AST_EXPRESSION_SUBSCRIPT,
                   "values[0] assignment target should parse as subscript access")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_parser_accepts_compound_and_update_expressions(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for parser diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/compound_update_valid.c",
                                        error_buffer, sizeof(error_buffer)),
                   "compound/update parser fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "parser should accept compound assignments and updates")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   translation_unit->functions[0]
                           ->body->data.compound.items[3]
                           ->data.expression_statement.expression->kind ==
                       FCC_AST_EXPRESSION_COMPOUND_ASSIGN,
                   "value += assignment should parse as compound assignment")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context,
                   translation_unit->functions[0]
                           ->body->data.compound.items[6]
                           ->data.expression_statement.expression->kind ==
                       FCC_AST_EXPRESSION_UPDATE,
                   "++value should parse as an update expression")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_sema_fixture(TestContext* context, const char* path, bool expected_ok,
                              size_t expected_error_count, const char* expected_message) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* stream;
  char buffer[1024];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for sema tests")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, path, error_buffer, sizeof(error_buffer)),
                   "semantic fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "semantic fixture should parse")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  ok = fcc_sema_check_translation_unit(&source_file, translation_unit, &diagnostics);
  if (!test_expect(context, ok == expected_ok, "semantic analysis result mismatch")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, diagnostics.error_count == expected_error_count,
                   "semantic analysis diagnostic count mismatch")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (expected_message != NULL) {
    if (!test_read_stream(stream, buffer, sizeof(buffer))) {
      fcc_ast_context_dispose(&ast_context);
      fcc_source_file_dispose(&source_file);
      (void)fclose(stream);
      return test_expect(context, false, "semantic diagnostics should be readable");
    }

    if (!test_expect(context, strstr(buffer, expected_message) != NULL,
                     "semantic diagnostic text should match")) {
      fcc_ast_context_dispose(&ast_context);
      fcc_source_file_dispose(&source_file);
      (void)fclose(stream);
      return false;
    }
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_sema_valid_translation_unit(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/parser_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_sizeof(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sizeof_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_sizeof_constants(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sizeof_constants.c", true, 0, NULL);
}

static bool test_sema_accepts_alignof(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/alignof_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_static_assert(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/static_assert_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_enums(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/enum_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_switch(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/switch_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_casts_and_char_literals(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/cast_char_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_member_and_subscript(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/member_subscript_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_pointer_arithmetic(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/pointer_arithmetic_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_compound_and_update(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/compound_update_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_extended_scalar_types(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/scalar_types_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_integer_literal_suffixes(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/integer_suffixes_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_array_constant_bounds(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/array_constant_bound_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_const_pointer_declarators(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/const_pointer_declarator_valid.c", true, 0,
                           NULL);
}

static bool test_sema_accepts_conditional_expression(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/conditional_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_do_goto_and_labels(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/control_flow_goto_do_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_string_literal_concatenation(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/string_concat_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_declspec_attribute(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/declspec_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_initializer_lists(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/initializer_list_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_null_and_typedef_compatibility(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/null_typedef_compat_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_array_pointer_declarators_and_parameter_decay(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/array_pointer_decay_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_function_pointers(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/function_pointer_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_global_function_pointer_initializers(TestContext* context) {
  return test_sema_fixture(context,
                           "tests/fixtures/global_function_pointer_initializer_valid.c", true, 0,
                           NULL);
}

static bool test_sema_accepts_void_casts(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/void_cast_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_unsized_initializer_sizeof(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/unsized_initializer_sizeof_valid.c", true, 0,
                           NULL);
}

static bool test_sema_accepts_struct_copy(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/struct_copy_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_variadic_functions(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/variadic_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_declared_external_call(TestContext* context) {
  return test_sema_fixture(context, "tests/toolchain/codegen_answer_harness.c", true, 0, NULL);
}

static bool test_sema_accepts_global_variables(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/globals_valid.c", true, 0, NULL);
}

static bool test_sema_accepts_recursive_declarators(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/recursive_types_valid.c", true, 0, NULL);
}

static bool test_sema_struct_union_layout(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSemaResult sema_result;
  FccSourceFile source_file;
  const FccSemaObjectInfo* head_info;
  const FccSemaObjectInfo* layout_info;
  const FccSemaObjectInfo* node_info;
  const FccSemaObjectInfo* value_info;
  const FccTypeRecordField* field;
  FILE* stream;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool align_ok;
  bool ok;
  bool size_ok;
  FccTypeId node_type_id;
  size_t alignment;
  size_t size;

  stream = test_open_temp_stream();
  if (!test_expect(context, stream != NULL, "tmpfile should be available for sema diagnostics")) {
    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/struct_union_layout_valid.c",
                                        error_buffer, sizeof(error_buffer)),
                   "struct/union sema fixture should load")) {
    (void)fclose(stream);
    return false;
  }

  fcc_diag_init(&diagnostics, stream);
  fcc_ast_context_init(&ast_context);
  fcc_sema_result_init(&sema_result);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "struct/union sema fixture should parse")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  ok =
      fcc_sema_analyze_translation_unit(&source_file, translation_unit, &diagnostics, &sema_result);
  if (!test_expect(context, ok, "struct/union sema fixture should analyze")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  layout_info = fcc_sema_result_find_object_info(&sema_result, translation_unit->globals[0]);
  value_info = fcc_sema_result_find_object_info(&sema_result, translation_unit->globals[1]);
  head_info = fcc_sema_result_find_object_info(&sema_result, translation_unit->globals[2]);
  node_info = fcc_sema_result_find_object_info(&sema_result, translation_unit->globals[3]);
  if (!test_expect(context,
                   (layout_info != NULL) && (value_info != NULL) && (head_info != NULL) &&
                       (node_info != NULL),
                   "sema object info should be available for all struct/union globals")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  size = fcc_type_size_of(&sema_result.type_context, layout_info->type_id, &size_ok);
  alignment = fcc_type_alignment_of(&sema_result.type_context, layout_info->type_id, &align_ok);
  if (!test_expect(context, size_ok && align_ok && (size == 12) && (alignment == 4),
                   "struct Layout should have size 12 and alignment 4")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  field = fcc_type_record_find_field(&sema_result.type_context, layout_info->type_id, "a");
  if (!test_expect(context, (field != NULL) && (field->offset == 0),
                   "Layout.a should have offset 0")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  field = fcc_type_record_find_field(&sema_result.type_context, layout_info->type_id, "b");
  if (!test_expect(context, (field != NULL) && (field->offset == 4),
                   "Layout.b should have offset 4")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  field = fcc_type_record_find_field(&sema_result.type_context, layout_info->type_id, "c");
  if (!test_expect(context, (field != NULL) && (field->offset == 8),
                   "Layout.c should have offset 8")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  size = fcc_type_size_of(&sema_result.type_context, value_info->type_id, &size_ok);
  alignment = fcc_type_alignment_of(&sema_result.type_context, value_info->type_id, &align_ok);
  if (!test_expect(context, size_ok && align_ok && (size == 4) && (alignment == 4),
                   "union Value should have size 4 and alignment 4")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  field = fcc_type_record_find_field(&sema_result.type_context, value_info->type_id, "as_int");
  if (!test_expect(context, (field != NULL) && (field->offset == 0),
                   "Value.as_int should have offset 0")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  field = fcc_type_record_find_field(&sema_result.type_context, value_info->type_id, "bytes");
  if (!test_expect(context, (field != NULL) && (field->offset == 0),
                   "Value.bytes should have offset 0")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  if (!test_expect(context, fcc_type_is_pointer(&sema_result.type_context, head_info->type_id),
                   "head should have pointer type")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  node_type_id = fcc_type_get_pointee_type(&sema_result.type_context, head_info->type_id);
  if (!test_expect(context,
                   (node_type_id == node_info->type_id) &&
                       fcc_type_is_complete(&sema_result.type_context, node_type_id),
                   "forward-declared struct Node should be completed before analysis ends")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  size = fcc_type_size_of(&sema_result.type_context, node_info->type_id, &size_ok);
  alignment = fcc_type_alignment_of(&sema_result.type_context, node_info->type_id, &align_ok);
  if (!test_expect(context, size_ok && align_ok && (size == 16) && (alignment == 8),
                   "struct Node should have size 16 and alignment 8")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  field = fcc_type_record_find_field(&sema_result.type_context, node_info->type_id, "next");
  if (!test_expect(context, (field != NULL) && (field->offset == 8),
                   "Node.next should have offset 8")) {
    fcc_sema_result_dispose(&sema_result);
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(stream);
    return false;
  }

  fcc_sema_result_dispose(&sema_result);
  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(stream);
  return true;
}

static bool test_sema_allows_nested_shadowing(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_scope_valid.c", true, 0, NULL);
}

static bool test_sema_rejects_undeclared_identifier(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_undeclared_identifier.c", false, 1,
                           "use of undeclared identifier 'missing_value'");
}

static bool test_sema_rejects_top_level_redefinition(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_parameter_redefinition.c", false, 1,
                           "redefinition of local 'value'");
}

static bool test_sema_rejects_break_outside_loop(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_break_outside_loop.c", false, 1,
                           "break is only valid inside a loop or switch");
}

static bool test_sema_rejects_return_value_in_void_function(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_return_value_in_void.c", false, 1,
                           "void function must not return a value");
}

static bool test_sema_rejects_invalid_assignment_target(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_invalid_assignment.c", false, 1,
                           "assignment target is not an lvalue");
}

static bool test_sema_rejects_duplicate_function(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_duplicate_function.c", false, 1,
                           "redefinition of function 'duplicate'");
}

static bool test_sema_rejects_conflicting_function_declaration(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_conflicting_function_declaration.c", false,
                           1, "conflicting declaration of function 'answer'");
}

static bool test_sema_rejects_wrong_argument_count(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_wrong_argument_count.c", false, 1,
                           "call to 'answer' has the wrong number of arguments");
}

static bool test_sema_rejects_function_pointer_wrong_argument_count(TestContext* context) {
  return test_sema_fixture(context,
                           "tests/fixtures/sema_function_pointer_wrong_argument_count.c", false, 1,
                           "call to 'fn' has the wrong number of arguments");
}

static bool test_sema_rejects_variadic_too_few_arguments(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_variadic_too_few_arguments.c", false, 1,
                           "call to 'fixed' has the wrong number of arguments");
}

static bool test_sema_rejects_sizeof_void(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_sizeof_void.c", false, 1,
                           "sizeof(void) is not supported in this phase");
}

static bool test_sema_rejects_alignof_void(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_alignof_void.c", false, 1,
                           "_Alignof operand has incomplete or unsupported type");
}

static bool test_sema_rejects_static_assert_failure(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_static_assert_fail.c", false, 1,
                           "static assertion failed: int should be eight bytes");
}

static bool test_sema_rejects_static_assert_non_constant(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_static_assert_not_constant.c", false, 1,
                           "_Static_assert expression must be an integer constant expression");
}

static bool test_sema_rejects_struct_incomplete_field(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_struct_incomplete_field.c", false, 2,
                           "field 'value' has incomplete type");
}

static bool test_sema_rejects_struct_duplicate_field(TestContext* context) {
  return test_sema_fixture(context, "tests/fixtures/sema_struct_duplicate_field.c", false, 2,
                           "duplicate field 'value' in struct definition");
}

static bool test_sema_rejects_global_bad_initializer(TestContext* context) {
  return test_sema_fixture(
      context, "tests/fixtures/sema_global_bad_initializer.c", false, 1,
      "global initializers must be constant expressions in this phase");
}

static bool test_sema_rejects_global_aggregate_bad_initializer(TestContext* context) {
  return test_sema_fixture(
      context, "tests/fixtures/sema_global_aggregate_bad_initializer.c", false, 1,
      "global initializers must be constant expressions in this phase");
}

static bool test_preprocessor_expands_local_includes(TestContext* context) {
  FccDiagnostics diagnostics;
  FccPreprocessorOptions options;
  FccSourceFile source_file;
  FILE* output_stream;
  FILE* error_stream;
  char output_buffer[2048];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  output_stream = test_open_temp_stream();
  error_stream = test_open_temp_stream();
  if (!test_expect(context, (output_stream != NULL) && (error_stream != NULL),
                   "tmpfiles should be available for preprocessor tests")) {
    if (output_stream != NULL) {
      (void)fclose(output_stream);
    }

    if (error_stream != NULL) {
      (void)fclose(error_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/pp_include_main.c",
                                        error_buffer, sizeof(error_buffer)),
                   "preprocessor include fixture should load")) {
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  options.include_directories = NULL;
  options.include_directory_count = 0;
  options.sysroot_directory = NULL;
  fcc_diag_init(&diagnostics, error_stream);
  ok = fcc_preprocessor_run(&source_file, &options, &diagnostics, output_stream);
  if (!test_expect(context, ok, "preprocessor include fixture should succeed")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_read_stream(output_stream, output_buffer, sizeof(output_buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return test_expect(context, false, "preprocessor output should be readable");
  }

  if (!test_expect(context, strstr(output_buffer, "int once_value(void)") != NULL,
                   "preprocessor output should include the header body")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, test_count_substring(output_buffer, "int once_value(void)") == 1,
                   "pragma once should suppress duplicate include output")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "#pragma once") == NULL,
                   "preprocessor output should not contain the pragma directive")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(output_stream);
  (void)fclose(error_stream);
  return true;
}

static bool test_preprocessor_ignores_unknown_pragmas(TestContext* context) {
  FccDiagnostics diagnostics;
  FccPreprocessorOptions options;
  FccSourceFile source_file;
  FILE* output_stream;
  FILE* error_stream;
  char output_buffer[1024];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  output_stream = test_open_temp_stream();
  error_stream = test_open_temp_stream();
  if (!test_expect(context, (output_stream != NULL) && (error_stream != NULL),
                   "tmpfiles should be available for preprocessor pragma tests")) {
    if (output_stream != NULL) {
      (void)fclose(output_stream);
    }

    if (error_stream != NULL) {
      (void)fclose(error_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/pp_ignored_pragma.c",
                                        error_buffer, sizeof(error_buffer)),
                   "preprocessor pragma fixture should load")) {
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  options.include_directories = NULL;
  options.include_directory_count = 0;
  options.sysroot_directory = NULL;
  fcc_diag_init(&diagnostics, error_stream);
  ok = fcc_preprocessor_run(&source_file, &options, &diagnostics, output_stream);
  if (!test_expect(context, ok, "preprocessor pragma fixture should succeed")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_read_stream(output_stream, output_buffer, sizeof(output_buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return test_expect(context, false, "preprocessor pragma output should be readable");
  }

  if (!test_expect(context, strstr(output_buffer, "int kept(void)") != NULL,
                   "preprocessor output should keep source after ignored pragma")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "#pragma") == NULL,
                   "preprocessor output should not contain ignored pragma directives")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(output_stream);
  (void)fclose(error_stream);
  return true;
}

static bool test_preprocessor_expands_macros_and_conditionals(TestContext* context) {
  FccDiagnostics diagnostics;
  FccPreprocessorOptions options;
  FccSourceFile source_file;
  FILE* output_stream;
  FILE* error_stream;
  char output_buffer[2048];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  output_stream = test_open_temp_stream();
  error_stream = test_open_temp_stream();
  if (!test_expect(context, (output_stream != NULL) && (error_stream != NULL),
                   "tmpfiles should be available for macro preprocessor tests")) {
    if (output_stream != NULL) {
      (void)fclose(output_stream);
    }

    if (error_stream != NULL) {
      (void)fclose(error_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/pp_macros.c", error_buffer,
                                        sizeof(error_buffer)),
                   "macro preprocessor fixture should load")) {
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  options.include_directories = NULL;
  options.include_directory_count = 0;
  options.sysroot_directory = NULL;
  fcc_diag_init(&diagnostics, error_stream);
  ok = fcc_preprocessor_run(&source_file, &options, &diagnostics, output_stream);
  if (!test_expect(context, ok, "macro preprocessor fixture should succeed")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_read_stream(output_stream, output_buffer, sizeof(output_buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return test_expect(context, false, "macro preprocessor output should be readable");
  }

  if (!test_expect(context, test_count_substring(output_buffer, "return 42;") == 3,
                   "macro expansion should replace VALUE in each active branch")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return 0;") == NULL,
                   "inactive conditional branches should not appear in output")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "#ifdef") == NULL,
                   "conditional directives should not be copied to output")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(output_stream);
  (void)fclose(error_stream);
  return true;
}

static bool test_preprocessor_expands_function_macros(TestContext* context) {
  FccDiagnostics diagnostics;
  FccPreprocessorOptions options;
  FccSourceFile source_file;
  FILE* output_stream;
  FILE* error_stream;
  char output_buffer[8192];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  output_stream = test_open_temp_stream();
  error_stream = test_open_temp_stream();
  if (!test_expect(context, (output_stream != NULL) && (error_stream != NULL),
                   "tmpfiles should be available for function-like macro tests")) {
    if (output_stream != NULL) {
      (void)fclose(output_stream);
    }

    if (error_stream != NULL) {
      (void)fclose(error_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/pp_function_macros.c",
                                        error_buffer, sizeof(error_buffer)),
                   "function-like macro preprocessor fixture should load")) {
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  options.include_directories = NULL;
  options.include_directory_count = 0;
  options.sysroot_directory = NULL;
  fcc_diag_init(&diagnostics, error_stream);
  ok = fcc_preprocessor_run(&source_file, &options, &diagnostics, output_stream);
  if (!test_expect(context, ok, "function-like macro preprocessor fixture should succeed")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_read_stream(output_stream, output_buffer, sizeof(output_buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return test_expect(context, false, "function-like macro output should be readable");
  }

  if (!test_expect(context, strstr(output_buffer, "return ((20) + (22));") != NULL,
                   "function-like macros should substitute ordinary arguments")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return ((1) + (2));") != NULL,
                   "function-like macros should allow whitespace before invocation '('")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return (((1 + 2)) + ((3 + 4)));") != NULL,
                   "function-like macro arguments should preserve nested parentheses")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return 0;") != NULL,
                   "zero-argument function-like macros should expand")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return ( + 1);") != NULL,
                   "single-parameter function-like macros should allow an empty argument")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return ((1) ? (5) : (4));") != NULL,
                   "function-like macros should substitute repeated parameter lists")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return ((5) + (42));") != NULL,
                   "function-like macro replacement should recursively rescan arguments")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return ((6) + (7));") != NULL,
                   "object-like macro replacement should be rescanned with following tokens")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return ((8) + (42));") != NULL,
                   "variadic macros should substitute and rescan __VA_ARGS__")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return 3 + 4;") != NULL,
                   "variadic-only macros should substitute the full argument tail")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return 11;") != NULL,
                   "variadic macros should allow an empty variadic tail")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return 42;") != NULL,
                   "token-pasting macros should join numeric tokens")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return joined_value;") != NULL,
                   "token-pasting macros should join identifier tokens")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return \"left + right\";") != NULL,
                   "stringizing macros should preserve argument spelling")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return \"\\\"quoted\\\"\";") != NULL,
                   "stringizing macros should escape quoted argument text")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return \"left, right\";") != NULL,
                   "variadic macros should stringize the variadic argument tail")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "\"ADD(left, right)\"") != NULL,
                   "function-like macro names inside string literals should not expand")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "/* ADD(1, 2) */") != NULL,
                   "function-like macro names inside comments should not expand")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "SELF_TYPE self_macro_value(void)") != NULL,
                   "self-referential macros should stabilize during rescanning")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(output_stream);
  (void)fclose(error_stream);
  return true;
}

static bool test_preprocessor_stabilizes_unavailable_macro_tokens(TestContext* context) {
  FccDiagnostics diagnostics;
  FccPreprocessorOptions options;
  FccSourceFile source_file;
  FILE* output_stream;
  FILE* error_stream;
  char output_buffer[2048];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  output_stream = test_open_temp_stream();
  error_stream = test_open_temp_stream();
  if (!test_expect(context, (output_stream != NULL) && (error_stream != NULL),
                   "tmpfiles should be available for macro ineligibility tests")) {
    if (output_stream != NULL) {
      (void)fclose(output_stream);
    }

    if (error_stream != NULL) {
      (void)fclose(error_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/pp_macro_ineligibility.c",
                                        error_buffer, sizeof(error_buffer)),
                   "macro ineligibility fixture should load")) {
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  options.include_directories = NULL;
  options.include_directory_count = 0;
  options.sysroot_directory = NULL;
  fcc_diag_init(&diagnostics, error_stream);
  ok = fcc_preprocessor_run(&source_file, &options, &diagnostics, output_stream);
  if (!test_expect(context, ok, "macro ineligibility preprocessor fixture should succeed")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_read_stream(output_stream, output_buffer, sizeof(output_buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return test_expect(context, false, "macro ineligibility output should be readable");
  }

  if (!test_expect(context, strstr(output_buffer, "return SELF_PLUS + 42;") != NULL,
                   "self-referential macro tokens should become unavailable after one expansion")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return LEFT + 42;") != NULL,
                   "mutually-recursive macro tokens should stabilize with unavailable marking")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(output_stream);
  (void)fclose(error_stream);
  return true;
}

static bool test_preprocessor_expands_if_elif_and_defined(TestContext* context) {
  FccDiagnostics diagnostics;
  FccPreprocessorOptions options;
  FccSourceFile source_file;
  FILE* output_stream;
  FILE* error_stream;
  char output_buffer[4096];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  output_stream = test_open_temp_stream();
  error_stream = test_open_temp_stream();
  if (!test_expect(context, (output_stream != NULL) && (error_stream != NULL),
                   "tmpfiles should be available for #if preprocessor tests")) {
    if (output_stream != NULL) {
      (void)fclose(output_stream);
    }

    if (error_stream != NULL) {
      (void)fclose(error_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/pp_if_conditionals.c",
                                        error_buffer, sizeof(error_buffer)),
                   "#if preprocessor fixture should load")) {
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  options.include_directories = NULL;
  options.include_directory_count = 0;
  options.sysroot_directory = NULL;
  fcc_diag_init(&diagnostics, error_stream);
  ok = fcc_preprocessor_run(&source_file, &options, &diagnostics, output_stream);
  if (!test_expect(context, ok, "#if preprocessor fixture should succeed")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_read_stream(output_stream, output_buffer, sizeof(output_buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return test_expect(context, false, "#if preprocessor output should be readable");
  }

  if (!test_expect(context, strstr(output_buffer, "return 1;") != NULL,
                   "predefined macro should be usable in active #if branches")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return 21 + 1;") != NULL,
                   "#elif expression should select the expected branch and expand macros")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "defined_without_parentheses") != NULL,
                   "defined without parentheses should be accepted")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "inactive_unknown_name") == NULL,
                   "undefined identifiers in #if should evaluate to zero")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "#elif") == NULL,
                   "conditional directives should not appear in #if output")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(output_stream);
  (void)fclose(error_stream);
  return true;
}

static bool test_preprocessor_splices_logical_lines(TestContext* context) {
  FccDiagnostics diagnostics;
  FccPreprocessorOptions options;
  FccSourceFile source_file;
  FILE* output_stream;
  FILE* error_stream;
  char output_buffer[2048];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  output_stream = test_open_temp_stream();
  error_stream = test_open_temp_stream();
  if (!test_expect(context, (output_stream != NULL) && (error_stream != NULL),
                   "tmpfiles should be available for line-splice preprocessor tests")) {
    if (output_stream != NULL) {
      (void)fclose(output_stream);
    }

    if (error_stream != NULL) {
      (void)fclose(error_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/pp_line_splice.c",
                                        error_buffer, sizeof(error_buffer)),
                   "line-splice preprocessor fixture should load")) {
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  options.include_directories = NULL;
  options.include_directory_count = 0;
  options.sysroot_directory = NULL;
  fcc_diag_init(&diagnostics, error_stream);
  ok = fcc_preprocessor_run(&source_file, &options, &diagnostics, output_stream);
  if (!test_expect(context, ok, "line-splice preprocessor fixture should succeed")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_read_stream(output_stream, output_buffer, sizeof(output_buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return test_expect(context, false, "line-splice preprocessor output should be readable");
  }

  if (!test_expect(context, strstr(output_buffer, "return 42;") != NULL,
                   "spliced macro replacement should be expanded")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "return 20 + 22;") != NULL,
                   "spliced source lines should be emitted as logical lines")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_expect(context, strstr(output_buffer, "spliced_if") != NULL,
                   "spliced #if expressions should be evaluated")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(output_stream);
  (void)fclose(error_stream);
  return true;
}

static bool test_preprocessor_rejects_unsupported_directive(TestContext* context) {
  FccDiagnostics diagnostics;
  FccPreprocessorOptions options;
  FccSourceFile source_file;
  FILE* output_stream;
  FILE* error_stream;
  char diag_buffer[1024];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  output_stream = test_open_temp_stream();
  error_stream = test_open_temp_stream();
  if (!test_expect(context, (output_stream != NULL) && (error_stream != NULL),
                   "tmpfiles should be available for preprocessor diagnostics")) {
    if (output_stream != NULL) {
      (void)fclose(output_stream);
    }

    if (error_stream != NULL) {
      (void)fclose(error_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/pp_unsupported_directive.c",
                                        error_buffer, sizeof(error_buffer)),
                   "unsupported preprocessor fixture should load")) {
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  options.include_directories = NULL;
  options.include_directory_count = 0;
  options.sysroot_directory = NULL;
  fcc_diag_init(&diagnostics, error_stream);
  ok = fcc_preprocessor_run(&source_file, &options, &diagnostics, output_stream);
  if (!test_expect(context, !ok, "unsupported preprocessor directive should fail")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_read_stream(error_stream, diag_buffer, sizeof(diag_buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return test_expect(context, false, "preprocessor diagnostics should be readable");
  }

  if (!test_expect(context,
                   strstr(diag_buffer, "unsupported preprocessor directive 'warning'") != NULL,
                   "preprocessor diagnostics should describe the unsupported directive")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(output_stream);
  (void)fclose(error_stream);
  return true;
}

static bool test_preprocessor_rejects_variadic_macro_too_few_arguments(TestContext* context) {
  FccDiagnostics diagnostics;
  FccPreprocessorOptions options;
  FccSourceFile source_file;
  FILE* output_stream;
  FILE* error_stream;
  char diag_buffer[1024];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  output_stream = test_open_temp_stream();
  error_stream = test_open_temp_stream();
  if (!test_expect(context, (output_stream != NULL) && (error_stream != NULL),
                   "tmpfiles should be available for variadic macro diagnostics")) {
    if (output_stream != NULL) {
      (void)fclose(output_stream);
    }

    if (error_stream != NULL) {
      (void)fclose(error_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file,
                                        "tests/fixtures/pp_variadic_too_few_arguments.c",
                                        error_buffer, sizeof(error_buffer)),
                   "variadic diagnostic fixture should load")) {
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  options.include_directories = NULL;
  options.include_directory_count = 0;
  options.sysroot_directory = NULL;
  fcc_diag_init(&diagnostics, error_stream);
  ok = fcc_preprocessor_run(&source_file, &options, &diagnostics, output_stream);
  if (!test_expect(context, !ok, "variadic macro missing a fixed argument should fail")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_read_stream(error_stream, diag_buffer, sizeof(diag_buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return test_expect(context, false, "variadic macro diagnostics should be readable");
  }

  if (!test_expect(context,
                   strstr(diag_buffer,
                          "function-like macro argument count does not match definition") != NULL,
                   "variadic macro diagnostics should describe the arity error")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(output_stream);
  (void)fclose(error_stream);
  return true;
}

static bool test_preprocessor_reports_unterminated_conditional(TestContext* context) {
  FccDiagnostics diagnostics;
  FccPreprocessorOptions options;
  FccSourceFile source_file;
  FILE* output_stream;
  FILE* error_stream;
  char diag_buffer[1024];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  output_stream = test_open_temp_stream();
  error_stream = test_open_temp_stream();
  if (!test_expect(context, (output_stream != NULL) && (error_stream != NULL),
                   "tmpfiles should be available for conditional diagnostics")) {
    if (output_stream != NULL) {
      (void)fclose(output_stream);
    }

    if (error_stream != NULL) {
      (void)fclose(error_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/pp_unterminated_ifdef.c",
                                        error_buffer, sizeof(error_buffer)),
                   "unterminated conditional fixture should load")) {
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  options.include_directories = NULL;
  options.include_directory_count = 0;
  options.sysroot_directory = NULL;
  fcc_diag_init(&diagnostics, error_stream);
  ok = fcc_preprocessor_run(&source_file, &options, &diagnostics, output_stream);
  if (!test_expect(context, !ok, "unterminated conditional should fail")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  if (!test_read_stream(error_stream, diag_buffer, sizeof(diag_buffer))) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return test_expect(context, false, "unterminated conditional diagnostics should be readable");
  }

  if (!test_expect(context,
                   strstr(diag_buffer, "unterminated conditional directive block at end of file") !=
                       NULL,
                   "unterminated conditional diagnostics should be explicit")) {
    fcc_source_file_dispose(&source_file);
    (void)fclose(output_stream);
    (void)fclose(error_stream);
    return false;
  }

  fcc_source_file_dispose(&source_file);
  (void)fclose(output_stream);
  (void)fclose(error_stream);
  return true;
}

static bool test_ast_dump_output(TestContext* context) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* dump_stream;
  FILE* diag_stream;
  char buffer[2048];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  dump_stream = test_open_temp_stream();
  diag_stream = test_open_temp_stream();
  if (!test_expect(context, (dump_stream != NULL) && (diag_stream != NULL),
                   "tmpfiles should be available for AST dump")) {
    if (dump_stream != NULL) {
      (void)fclose(dump_stream);
    }

    if (diag_stream != NULL) {
      (void)fclose(diag_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, "tests/fixtures/parser_valid.c", error_buffer,
                                        sizeof(error_buffer)),
                   "AST dump fixture should load")) {
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  fcc_diag_init(&diagnostics, diag_stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "AST dump fixture should parse")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  fcc_ast_dump_translation_unit(dump_stream, translation_unit);
  if (!test_read_stream(dump_stream, buffer, sizeof(buffer))) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return test_expect(context, false, "AST dump output should be readable");
  }

  if (!test_expect(context, strstr(buffer, "translation_unit function_count=2") != NULL,
                   "AST dump should include the translation unit header")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  if (!test_expect(context, strstr(buffer, "function name=\"sum_to\" return_type=int") != NULL,
                   "AST dump should include the first function")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  if (!test_expect(context, strstr(buffer, "while") != NULL,
                   "AST dump should include the while statement")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  if (!test_expect(context, strstr(buffer, "for") != NULL,
                   "AST dump should include the for statement")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(dump_stream);
  (void)fclose(diag_stream);
  return true;
}

static bool test_codegen_output(TestContext* context, const char* path, const char* expected_one,
                                const char* expected_two) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccSourceFile source_file;
  FILE* diag_stream;
  FILE* dump_stream;
  char buffer[4096];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  bool ok;

  dump_stream = test_open_temp_stream();
  diag_stream = test_open_temp_stream();
  if (!test_expect(context, (dump_stream != NULL) && (diag_stream != NULL),
                   "tmpfiles should be available for codegen output")) {
    if (dump_stream != NULL) {
      (void)fclose(dump_stream);
    }

    if (diag_stream != NULL) {
      (void)fclose(diag_stream);
    }

    return false;
  }

  if (!test_expect(context,
                   fcc_source_file_load(&source_file, path, error_buffer, sizeof(error_buffer)),
                   "codegen fixture should load")) {
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  fcc_diag_init(&diagnostics, diag_stream);
  fcc_ast_context_init(&ast_context);
  translation_unit = NULL;
  ok = fcc_parser_parse_translation_unit(&source_file, &diagnostics, &ast_context,
                                         &translation_unit);
  if (!test_expect(context, ok, "codegen fixture should parse")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  ok = fcc_sema_check_translation_unit(&source_file, translation_unit, &diagnostics);
  if (!test_expect(context, ok, "codegen fixture should pass semantic analysis")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  ok = fcc_codegen_emit_nasm_x64(dump_stream, &source_file, translation_unit, &diagnostics);
  if (!test_expect(context, ok, "codegen should succeed for the fixture")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  if (!test_read_stream(dump_stream, buffer, sizeof(buffer))) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return test_expect(context, false, "codegen output should be readable");
  }

  if (!test_expect(context, strstr(buffer, expected_one) != NULL,
                   "codegen output should include the first expected fragment")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  if (!test_expect(context, strstr(buffer, expected_two) != NULL,
                   "codegen output should include the second expected fragment")) {
    fcc_ast_context_dispose(&ast_context);
    fcc_source_file_dispose(&source_file);
    (void)fclose(dump_stream);
    (void)fclose(diag_stream);
    return false;
  }

  fcc_ast_context_dispose(&ast_context);
  fcc_source_file_dispose(&source_file);
  (void)fclose(dump_stream);
  (void)fclose(diag_stream);
  return true;
}

static bool test_codegen_simple_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/codegen_answer.c", "global answer",
                             "mov eax, 42");
}

static bool test_codegen_control_flow_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/parser_valid.c", "cmp eax, 0", "jmp .L");
}

static bool test_codegen_sizeof_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/sizeof_valid.c", "mov eax, 4",
                             "global answer");
}

static bool test_codegen_sizeof_constants_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/sizeof_constants.c", "  dd 16",
                             "mov eax, 24");
}

static bool test_codegen_alignof_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/alignof_valid.c", "global global_align",
                             "mov eax, 8");
}

static bool test_codegen_static_assert_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/static_assert_valid.c", "global main",
                             "mov eax, 4");
}

static bool test_codegen_enum_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/enum_valid.c", "  dd 21", "mov eax, 8");
}

static bool test_codegen_switch_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/switch_valid.c", "cmp eax, 2", "je .L");
}

static bool test_codegen_cast_char_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/cast_char_valid.c", "  dd 10",
                             "movzx eax, al");
}

static bool test_codegen_member_subscript_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/member_subscript_valid.c", "imul rax, rax, 4",
                             "add rax, 4");
}

static bool test_codegen_pointer_arithmetic_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/pointer_arithmetic_valid.c",
                             "imul rax, rax, 4", "cmp rax, rcx");
}

static bool test_codegen_function_pointer_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/function_pointer_valid.c",
                             "call qword [rsp +", "lea rax, [rel inc]");
}

static bool test_codegen_global_function_pointer_initializer_output(TestContext* context) {
  return test_codegen_output(context,
                             "tests/fixtures/global_function_pointer_initializer_valid.c",
                             "  dq inc", "  dq twice");
}

static bool test_codegen_compound_update_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/compound_update_valid.c", "imul eax, ecx",
                             "add rax, 4");
}

static bool test_codegen_extended_scalar_types_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/scalar_types_valid.c", "  dq 6", "  dw 2");
}

static bool test_codegen_conditional_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/conditional_valid.c", "mov eax, 2", "je .L");
}

static bool test_codegen_large_integer_literal_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/large_integer_codegen_valid.c",
                             "mov rax, 18446744073709551615", "ret");
}

static bool test_codegen_aggregate_copy_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/aggregate_copy_codegen_valid.c",
                             "mov byte [r8], 0", "mov byte [r8], al");
}

static bool test_codegen_global_aggregate_initializers_output(TestContext* context) {
  return test_codegen_output(context,
                             "tests/fixtures/global_aggregate_initializers_codegen_valid.c",
                             "INFO:", "dq FCC_STR");
}

static bool test_codegen_local_aggregate_initializers_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/local_aggregate_initializers_codegen_valid.c",
                             "lea rax, [rel FCC_STR0]", "mov dword [rbp -");
}

static bool test_codegen_unsigned_64_binary_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/unsigned_64_binary_codegen_valid.c",
                             "div rcx", "seta al");
}

static bool test_codegen_signed_mixed_width_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/signed_mixed_width_codegen_valid.c",
                             "movsxd rax, eax", "setl al");
}

static bool test_codegen_register_aggregate_parameter_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/register_aggregate_parameter_codegen_valid.c",
                             "push r9", "mov rax, qword [rsp + 8]");
}

static bool test_codegen_sign_extend_return_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/sign_extend_return_codegen_valid.c",
                             "movsxd rax, eax", "movsx rax, al");
}

static bool test_codegen_char_escape_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/char_escape_codegen_valid.c", "  dd 11",
                             "  dd 12");
}

static bool test_codegen_external_call_output(TestContext* context) {
  return test_codegen_output(context, "tests/toolchain/codegen_answer_harness.c", "extern answer",
                             "call answer");
}

static bool test_codegen_global_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/globals_valid.c", "section .data",
                             "mov eax, dword [rel seed]");
}

static bool test_codegen_many_args_output(TestContext* context) {
  return test_codegen_output(context, "tests/fixtures/many_args.c", "mov rax, qword [rbp + 48]",
                             "mov qword [rsp + 32], rax");
}

static bool test_driver_behavior(TestContext* context) {
  const char* help_argv[2];
  const char* version_argv[2];
  const char* check_argv[3];
  const char* check_globals_argv[3];
  const char* check_sizeof_constants_argv[3];
  const char* check_alignof_argv[3];
  const char* check_static_assert_argv[3];
  const char* check_enum_argv[3];
  const char* check_switch_argv[3];
  const char* check_cast_char_argv[3];
  const char* check_member_subscript_argv[3];
  const char* check_pointer_arithmetic_argv[3];
  const char* check_function_pointer_argv[3];
  const char* check_global_function_pointer_argv[3];
  const char* check_compound_update_argv[3];
  const char* check_scalar_types_argv[3];
  const char* check_preprocess_argv[3];
  const char* check_macros_argv[3];
  const char* check_function_macros_argv[3];
  const char* check_include_argv[5];
  const char* lex_only_argv[3];
  const char* parse_only_argv[3];
  const char* preprocess_argv[5];
  const char* preprocess_include_argv[7];
  const char* dump_argv[3];
  const char* dump_ast_argv[3];
  const char* emit_asm_argv[5];
  const char* emit_obj_argv[5];
  const char* invalid_output_argv[5];
  const char* link_argv[5];
  const char* link_globals_argv[5];
  const char* link_sizeof_constants_argv[5];
  const char* link_alignof_argv[5];
  const char* link_static_assert_argv[5];
  const char* link_enum_argv[5];
  const char* link_switch_argv[5];
  const char* link_cast_char_argv[5];
  const char* link_member_subscript_argv[5];
  const char* link_pointer_arithmetic_argv[5];
  const char* link_function_pointer_argv[5];
  const char* link_global_function_pointer_argv[5];
  const char* link_compound_update_argv[5];
  const char* link_scalar_types_argv[5];
  const char* link_many_args_argv[5];
  const char* missing_argv[2];
  const char* sema_error_argv[3];
  FILE* dump_ast_output;
  FILE* dump_ast_error;
  FILE* dump_output;
  FILE* dump_error;
  FILE* help_output;
  FILE* help_error;
  FILE* missing_output;
  FILE* missing_error;
  const char* emit_output_path;
  const char* link_output_path;
  FILE* sema_error_output;
  FILE* sema_error_stream;
  char emit_buffer[2048];
  char dump_ast_buffer[2048];
  char dump_buffer[1024];
  char help_buffer[4096];
  char invalid_output_buffer[512];
  char missing_buffer[512];
  char preprocess_buffer[2048];
  char sema_error_buffer[512];
  int exit_code;

  help_argv[0] = "fcc";
  help_argv[1] = "--help";
  help_output = test_open_temp_stream();
  help_error = test_open_temp_stream();
  if (!test_expect(context, (help_output != NULL) && (help_error != NULL),
                   "tmpfiles should open")) {
    if (help_output != NULL) {
      (void)fclose(help_output);
    }

    if (help_error != NULL) {
      (void)fclose(help_error);
    }

    return false;
  }

  exit_code = fcc_driver_run(2, help_argv, help_output, help_error);
  if (!test_expect(context, exit_code == 0, "help should exit cleanly")) {
    (void)fclose(help_output);
    (void)fclose(help_error);
    return false;
  }

  if (!test_read_stream(help_output, help_buffer, sizeof(help_buffer))) {
    (void)fclose(help_output);
    (void)fclose(help_error);
    return test_expect(context, false, "help output should be readable");
  }

  if (!test_expect(context,
                   strstr(help_buffer,
                          "Usage: fcc [--help] [--version] [--dump-tokens] [--dump-ast] "
                          "[--lex-only | "
                          "--parse-only") != NULL,
                   "help output should contain usage")) {
    (void)fclose(help_output);
    (void)fclose(help_error);
    return false;
  }

  if (!test_expect(context, strstr(help_buffer, "-I PATH") != NULL,
                   "help output should describe the include search flag")) {
    (void)fclose(help_output);
    (void)fclose(help_error);
    return false;
  }

  if (!test_expect(context, strstr(help_buffer, "--emit-obj") != NULL,
                   "help output should describe object emission")) {
    (void)fclose(help_output);
    (void)fclose(help_error);
    return false;
  }

  (void)fclose(help_output);
  (void)fclose(help_error);

  version_argv[0] = "fcc";
  version_argv[1] = "--version";
  help_output = test_open_temp_stream();
  help_error = test_open_temp_stream();
  if (!test_expect(context, (help_output != NULL) && (help_error != NULL),
                   "tmpfiles should open")) {
    if (help_output != NULL) {
      (void)fclose(help_output);
    }

    if (help_error != NULL) {
      (void)fclose(help_error);
    }

    return false;
  }

  exit_code = fcc_driver_run(2, version_argv, help_output, help_error);
  if (!test_expect(context, exit_code == 0, "version should exit cleanly")) {
    (void)fclose(help_output);
    (void)fclose(help_error);
    return false;
  }

  if (!test_read_stream(help_output, help_buffer, sizeof(help_buffer))) {
    (void)fclose(help_output);
    (void)fclose(help_error);
    return test_expect(context, false, "version output should be readable");
  }

  if (!test_expect(context, strstr(help_buffer, "FCC 0.8.0-dev") != NULL,
                   "version output should include the compiler identity")) {
    (void)fclose(help_output);
    (void)fclose(help_error);
    return false;
  }

  (void)fclose(help_output);
  (void)fclose(help_error);

  check_argv[0] = "fcc";
  check_argv[1] = "--check";
  check_argv[2] = "tests/fixtures/sample.c";
  exit_code = fcc_driver_run(3, check_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "check stage should return success")) {
    return false;
  }

  check_globals_argv[0] = "fcc";
  check_globals_argv[1] = "--check";
  check_globals_argv[2] = "tests/fixtures/globals_valid.c";
  exit_code = fcc_driver_run(3, check_globals_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "check stage should accept top-level globals")) {
    return false;
  }

  check_sizeof_constants_argv[0] = "fcc";
  check_sizeof_constants_argv[1] = "--check";
  check_sizeof_constants_argv[2] = "tests/fixtures/sizeof_constants.c";
  exit_code = fcc_driver_run(3, check_sizeof_constants_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should accept sizeof constant expressions")) {
    return false;
  }

  check_alignof_argv[0] = "fcc";
  check_alignof_argv[1] = "--check";
  check_alignof_argv[2] = "tests/fixtures/alignof_valid.c";
  exit_code = fcc_driver_run(3, check_alignof_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should accept _Alignof constant expressions")) {
    return false;
  }

  check_static_assert_argv[0] = "fcc";
  check_static_assert_argv[1] = "--check";
  check_static_assert_argv[2] = "tests/fixtures/static_assert_valid.c";
  exit_code = fcc_driver_run(3, check_static_assert_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should accept _Static_assert declarations")) {
    return false;
  }

  check_enum_argv[0] = "fcc";
  check_enum_argv[1] = "--check";
  check_enum_argv[2] = "tests/fixtures/enum_valid.c";
  exit_code = fcc_driver_run(3, check_enum_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "check stage should accept enum declarations")) {
    return false;
  }

  check_switch_argv[0] = "fcc";
  check_switch_argv[1] = "--check";
  check_switch_argv[2] = "tests/fixtures/switch_valid.c";
  exit_code = fcc_driver_run(3, check_switch_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "check stage should accept switch statements")) {
    return false;
  }

  check_cast_char_argv[0] = "fcc";
  check_cast_char_argv[1] = "--check";
  check_cast_char_argv[2] = "tests/fixtures/cast_char_valid.c";
  exit_code = fcc_driver_run(3, check_cast_char_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should accept casts and character literals")) {
    return false;
  }

  check_member_subscript_argv[0] = "fcc";
  check_member_subscript_argv[1] = "--check";
  check_member_subscript_argv[2] = "tests/fixtures/member_subscript_valid.c";
  exit_code = fcc_driver_run(3, check_member_subscript_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should accept member and subscript expressions")) {
    return false;
  }

  check_pointer_arithmetic_argv[0] = "fcc";
  check_pointer_arithmetic_argv[1] = "--check";
  check_pointer_arithmetic_argv[2] = "tests/fixtures/pointer_arithmetic_valid.c";
  exit_code = fcc_driver_run(3, check_pointer_arithmetic_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should accept pointer arithmetic expressions")) {
    return false;
  }

  check_function_pointer_argv[0] = "fcc";
  check_function_pointer_argv[1] = "--check";
  check_function_pointer_argv[2] = "tests/fixtures/function_pointer_valid.c";
  exit_code = fcc_driver_run(3, check_function_pointer_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "check stage should accept function pointers")) {
    return false;
  }

  check_global_function_pointer_argv[0] = "fcc";
  check_global_function_pointer_argv[1] = "--check";
  check_global_function_pointer_argv[2] =
      "tests/fixtures/global_function_pointer_initializer_valid.c";
  exit_code = fcc_driver_run(3, check_global_function_pointer_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should accept global function pointer initializers")) {
    return false;
  }

  check_compound_update_argv[0] = "fcc";
  check_compound_update_argv[1] = "--check";
  check_compound_update_argv[2] = "tests/fixtures/compound_update_valid.c";
  exit_code = fcc_driver_run(3, check_compound_update_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should accept compound assignments and updates")) {
    return false;
  }

  check_scalar_types_argv[0] = "fcc";
  check_scalar_types_argv[1] = "--check";
  check_scalar_types_argv[2] = "tests/fixtures/scalar_types_valid.c";
  exit_code = fcc_driver_run(3, check_scalar_types_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should accept extended scalar type specifiers")) {
    return false;
  }

  check_preprocess_argv[0] = "fcc";
  check_preprocess_argv[1] = "--check";
  check_preprocess_argv[2] = "tests/fixtures/pp_include_main.c";
  exit_code = fcc_driver_run(3, check_preprocess_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should preprocess local includes before parsing")) {
    return false;
  }

  check_macros_argv[0] = "fcc";
  check_macros_argv[1] = "--check";
  check_macros_argv[2] = "tests/fixtures/pp_macros.c";
  exit_code = fcc_driver_run(3, check_macros_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should preprocess macros and conditionals before parsing")) {
    return false;
  }

  check_function_macros_argv[0] = "fcc";
  check_function_macros_argv[1] = "--check";
  check_function_macros_argv[2] = "tests/fixtures/pp_function_macros.c";
  exit_code = fcc_driver_run(3, check_function_macros_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should expand stringizing and token-pasting macros before parsing")) {
    return false;
  }

  check_include_argv[0] = "fcc";
  check_include_argv[1] = "--check";
  check_include_argv[2] = "-I";
  check_include_argv[3] = "tests/include";
  check_include_argv[4] = "tests/fixtures/pp_include_search_main.c";
  exit_code = fcc_driver_run(5, check_include_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "check stage should honor -I search directories in the normal pipeline")) {
    return false;
  }

  lex_only_argv[0] = "fcc";
  lex_only_argv[1] = "--lex-only";
  lex_only_argv[2] = "tests/fixtures/parser_missing_semicolon.c";
  exit_code = fcc_driver_run(3, lex_only_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "lex-only should ignore later parse errors")) {
    return false;
  }

  parse_only_argv[0] = "fcc";
  parse_only_argv[1] = "--parse-only";
  parse_only_argv[2] = "tests/fixtures/sema_undeclared_identifier.c";
  exit_code = fcc_driver_run(3, parse_only_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "parse-only should ignore later semantic errors")) {
    return false;
  }

  sema_error_argv[0] = "fcc";
  sema_error_argv[1] = "--check";
  sema_error_argv[2] = "tests/fixtures/sema_undeclared_identifier.c";
  sema_error_output = test_open_temp_stream();
  sema_error_stream = test_open_temp_stream();
  if (!test_expect(context, (sema_error_output != NULL) && (sema_error_stream != NULL),
                   "tmpfiles should open")) {
    if (sema_error_output != NULL) {
      (void)fclose(sema_error_output);
    }

    if (sema_error_stream != NULL) {
      (void)fclose(sema_error_stream);
    }

    return false;
  }

  exit_code = fcc_driver_run(3, sema_error_argv, sema_error_output, sema_error_stream);
  if (!test_expect(context, exit_code == 1, "semantic error input should return one error")) {
    (void)fclose(sema_error_output);
    (void)fclose(sema_error_stream);
    return false;
  }

  if (!test_read_stream(sema_error_stream, sema_error_buffer, sizeof(sema_error_buffer))) {
    (void)fclose(sema_error_output);
    (void)fclose(sema_error_stream);
    return test_expect(context, false, "driver semantic diagnostics should be readable");
  }

  if (!test_expect(context,
                   strstr(sema_error_buffer, "use of undeclared identifier 'missing_value'") !=
                       NULL,
                   "driver should surface semantic diagnostics")) {
    (void)fclose(sema_error_output);
    (void)fclose(sema_error_stream);
    return false;
  }

  (void)fclose(sema_error_output);
  (void)fclose(sema_error_stream);

  dump_argv[0] = "fcc";
  dump_argv[1] = "--dump-tokens";
  dump_argv[2] = "tests/fixtures/sample.c";
  dump_output = test_open_temp_stream();
  dump_error = test_open_temp_stream();
  if (!test_expect(context, (dump_output != NULL) && (dump_error != NULL),
                   "tmpfiles should open")) {
    if (dump_output != NULL) {
      (void)fclose(dump_output);
    }

    if (dump_error != NULL) {
      (void)fclose(dump_error);
    }

    return false;
  }

  exit_code = fcc_driver_run(3, dump_argv, dump_output, dump_error);
  if (!test_expect(context, exit_code == 0, "token dump should exit cleanly")) {
    (void)fclose(dump_output);
    (void)fclose(dump_error);
    return false;
  }

  if (!test_read_stream(dump_output, dump_buffer, sizeof(dump_buffer))) {
    (void)fclose(dump_output);
    (void)fclose(dump_error);
    return test_expect(context, false, "driver token dump should be readable");
  }

  if (!test_expect(context, strstr(dump_buffer, "KW_INT 1:1 span=[0,3) text=\"int\"") != NULL,
                   "token dump should include the first keyword")) {
    (void)fclose(dump_output);
    (void)fclose(dump_error);
    return false;
  }

  if (!test_expect(context, strstr(dump_buffer, "END_OF_FILE") != NULL,
                   "token dump should include EOF")) {
    (void)fclose(dump_output);
    (void)fclose(dump_error);
    return false;
  }

  (void)fclose(dump_output);
  (void)fclose(dump_error);

  dump_ast_argv[0] = "fcc";
  dump_ast_argv[1] = "--dump-ast";
  dump_ast_argv[2] = "tests/fixtures/parser_valid.c";
  dump_ast_output = test_open_temp_stream();
  dump_ast_error = test_open_temp_stream();
  if (!test_expect(context, (dump_ast_output != NULL) && (dump_ast_error != NULL),
                   "tmpfiles should open")) {
    if (dump_ast_output != NULL) {
      (void)fclose(dump_ast_output);
    }

    if (dump_ast_error != NULL) {
      (void)fclose(dump_ast_error);
    }

    return false;
  }

  exit_code = fcc_driver_run(3, dump_ast_argv, dump_ast_output, dump_ast_error);
  if (!test_expect(context, exit_code == 0, "AST dump should exit cleanly")) {
    (void)fclose(dump_ast_output);
    (void)fclose(dump_ast_error);
    return false;
  }

  if (!test_read_stream(dump_ast_output, dump_ast_buffer, sizeof(dump_ast_buffer))) {
    (void)fclose(dump_ast_output);
    (void)fclose(dump_ast_error);
    return test_expect(context, false, "driver AST dump should be readable");
  }

  if (!test_expect(context, strstr(dump_ast_buffer, "translation_unit function_count=2") != NULL,
                   "AST dump should include the translation unit header")) {
    (void)fclose(dump_ast_output);
    (void)fclose(dump_ast_error);
    return false;
  }

  if (!test_expect(context,
                   strstr(dump_ast_buffer, "function name=\"spin\" return_type=void") != NULL,
                   "AST dump should include the second function")) {
    (void)fclose(dump_ast_output);
    (void)fclose(dump_ast_error);
    return false;
  }

  (void)fclose(dump_ast_output);
  (void)fclose(dump_ast_error);

  emit_output_path = "build/driver_emit_test.s";
  (void)remove(emit_output_path);
  emit_asm_argv[0] = "fcc";
  emit_asm_argv[1] = "--emit-asm";
  emit_asm_argv[2] = "--output";
  emit_asm_argv[3] = emit_output_path;
  emit_asm_argv[4] = "tests/toolchain/codegen_answer_harness.c";
  exit_code = fcc_driver_run(5, emit_asm_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "assembly emission should exit cleanly")) {
    (void)remove(emit_output_path);
    return false;
  }

  if (!test_expect(context, test_read_path_file(emit_output_path, emit_buffer, sizeof(emit_buffer)),
                   "driver assembly output should be readable")) {
    (void)remove(emit_output_path);
    return false;
  }

  if (!test_expect(context, strstr(emit_buffer, "section .text") != NULL,
                   "driver assembly output should contain the text section")) {
    (void)remove(emit_output_path);
    return false;
  }

  if (!test_expect(context, strstr(emit_buffer, "extern answer") != NULL,
                   "driver assembly output should contain the external declaration")) {
    (void)remove(emit_output_path);
    return false;
  }

  if (!test_expect(context, strstr(emit_buffer, "call answer") != NULL,
                   "driver assembly output should contain the emitted function call")) {
    (void)remove(emit_output_path);
    return false;
  }

  (void)remove(emit_output_path);

  emit_output_path = "build/driver_preprocess_test.i";
  (void)remove(emit_output_path);
  preprocess_argv[0] = "fcc";
  preprocess_argv[1] = "--preprocess";
  preprocess_argv[2] = "--output";
  preprocess_argv[3] = emit_output_path;
  preprocess_argv[4] = "tests/fixtures/pp_include_main.c";
  exit_code = fcc_driver_run(5, preprocess_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "preprocess stage should exit cleanly")) {
    (void)remove(emit_output_path);
    return false;
  }

  if (!test_expect(
          context,
          test_read_path_file(emit_output_path, preprocess_buffer, sizeof(preprocess_buffer)),
          "driver preprocess output should be readable")) {
    (void)remove(emit_output_path);
    return false;
  }

  if (!test_expect(context, strstr(preprocess_buffer, "int once_value(void)") != NULL,
                   "driver preprocess output should contain the included function")) {
    (void)remove(emit_output_path);
    return false;
  }

  (void)remove(emit_output_path);

  emit_output_path = "build/driver_preprocess_include_test.i";
  (void)remove(emit_output_path);
  preprocess_include_argv[0] = "fcc";
  preprocess_include_argv[1] = "--preprocess";
  preprocess_include_argv[2] = "-I";
  preprocess_include_argv[3] = "tests/include";
  preprocess_include_argv[4] = "--output";
  preprocess_include_argv[5] = emit_output_path;
  preprocess_include_argv[6] = "tests/fixtures/pp_include_search_main.c";
  exit_code = fcc_driver_run(7, preprocess_include_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "preprocess include search should exit cleanly")) {
    (void)remove(emit_output_path);
    return false;
  }

  if (!test_expect(
          context,
          test_read_path_file(emit_output_path, preprocess_buffer, sizeof(preprocess_buffer)),
          "driver include-search preprocess output should be readable")) {
    (void)remove(emit_output_path);
    return false;
  }

  if (!test_expect(context, strstr(preprocess_buffer, "int search_header_value(void)") != NULL,
                   "driver preprocess output should include the -I searched header")) {
    (void)remove(emit_output_path);
    return false;
  }

  (void)remove(emit_output_path);

  invalid_output_argv[0] = "fcc";
  invalid_output_argv[1] = "--check";
  invalid_output_argv[2] = "--output";
  invalid_output_argv[3] = "build/driver_invalid_output.txt";
  invalid_output_argv[4] = "tests/fixtures/sample.c";
  dump_output = test_open_temp_stream();
  dump_error = test_open_temp_stream();
  if (!test_expect(context, (dump_output != NULL) && (dump_error != NULL),
                   "tmpfiles should open")) {
    if (dump_output != NULL) {
      (void)fclose(dump_output);
    }

    if (dump_error != NULL) {
      (void)fclose(dump_error);
    }

    return false;
  }

  exit_code = fcc_driver_run(5, invalid_output_argv, dump_output, dump_error);
  if (!test_expect(context, exit_code == 1, "invalid output usage should fail")) {
    (void)fclose(dump_output);
    (void)fclose(dump_error);
    return false;
  }

  if (!test_read_stream(dump_error, invalid_output_buffer, sizeof(invalid_output_buffer))) {
    (void)fclose(dump_output);
    (void)fclose(dump_error);
    return test_expect(context, false, "driver option diagnostics should be readable");
  }

  if (!test_expect(context,
                   strstr(invalid_output_buffer,
                          "--output is only valid with --preprocess, --emit-asm, --emit-obj, "
                          "or "
                          "--compile-and-link") != NULL,
                   "driver should reject invalid output flag usage")) {
    (void)fclose(dump_output);
    (void)fclose(dump_error);
    return false;
  }

  (void)fclose(dump_output);
  (void)fclose(dump_error);

  emit_output_path = "build/driver_emit_obj_test.obj";
  (void)remove(emit_output_path);
  emit_obj_argv[0] = "fcc";
  emit_obj_argv[1] = "--emit-obj";
  emit_obj_argv[2] = "--output";
  emit_obj_argv[3] = emit_output_path;
  emit_obj_argv[4] = "tests/fixtures/sample.c";
  exit_code = fcc_driver_run(5, emit_obj_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "object emission should exit cleanly")) {
    (void)remove(emit_output_path);
    return false;
  }

  if (!test_expect(context, test_path_exists_nonempty(emit_output_path),
                   "driver object output should exist and be non-empty")) {
    (void)remove(emit_output_path);
    return false;
  }

  (void)remove(emit_output_path);

  link_output_path = "build/driver_link_test.exe";
  (void)remove(link_output_path);
  link_argv[0] = "fcc";
  link_argv[1] = "--compile-and-link";
  link_argv[2] = "--output";
  link_argv[3] = link_output_path;
  link_argv[4] = "tests/fixtures/sample.c";
  exit_code = fcc_driver_run(5, link_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "compile-and-link should exit cleanly")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output should run successfully")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_globals_test.exe";
  (void)remove(link_output_path);
  link_globals_argv[0] = "fcc";
  link_globals_argv[1] = "--compile-and-link";
  link_globals_argv[2] = "--output";
  link_globals_argv[3] = link_output_path;
  link_globals_argv[4] = "tests/fixtures/globals_valid.c";
  exit_code = fcc_driver_run(5, link_globals_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "compile-and-link should handle top-level globals")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with globals should run successfully")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_sizeof_constants_test.exe";
  (void)remove(link_output_path);
  link_sizeof_constants_argv[0] = "fcc";
  link_sizeof_constants_argv[1] = "--compile-and-link";
  link_sizeof_constants_argv[2] = "--output";
  link_sizeof_constants_argv[3] = link_output_path;
  link_sizeof_constants_argv[4] = "tests/fixtures/sizeof_constants.c";
  exit_code = fcc_driver_run(5, link_sizeof_constants_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "compile-and-link should handle sizeof constant expressions")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with sizeof constants should run successfully")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_alignof_test.exe";
  (void)remove(link_output_path);
  link_alignof_argv[0] = "fcc";
  link_alignof_argv[1] = "--compile-and-link";
  link_alignof_argv[2] = "--output";
  link_alignof_argv[3] = link_output_path;
  link_alignof_argv[4] = "tests/fixtures/alignof_valid.c";
  exit_code = fcc_driver_run(5, link_alignof_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "compile-and-link should handle _Alignof constant expressions")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with _Alignof constants should run successfully")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_static_assert_test.exe";
  (void)remove(link_output_path);
  link_static_assert_argv[0] = "fcc";
  link_static_assert_argv[1] = "--compile-and-link";
  link_static_assert_argv[2] = "--output";
  link_static_assert_argv[3] = link_output_path;
  link_static_assert_argv[4] = "tests/fixtures/static_assert_valid.c";
  exit_code = fcc_driver_run(5, link_static_assert_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "compile-and-link should handle _Static_assert declarations")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with _Static_assert declarations should run")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_enum_test.exe";
  (void)remove(link_output_path);
  link_enum_argv[0] = "fcc";
  link_enum_argv[1] = "--compile-and-link";
  link_enum_argv[2] = "--output";
  link_enum_argv[3] = link_output_path;
  link_enum_argv[4] = "tests/fixtures/enum_valid.c";
  exit_code = fcc_driver_run(5, link_enum_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "compile-and-link should handle enum constants")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with enum constants should run successfully")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_switch_test.exe";
  (void)remove(link_output_path);
  link_switch_argv[0] = "fcc";
  link_switch_argv[1] = "--compile-and-link";
  link_switch_argv[2] = "--output";
  link_switch_argv[3] = link_output_path;
  link_switch_argv[4] = "tests/fixtures/switch_valid.c";
  exit_code = fcc_driver_run(5, link_switch_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0, "compile-and-link should handle switch statements")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with switch statements should run successfully")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_cast_char_test.exe";
  (void)remove(link_output_path);
  link_cast_char_argv[0] = "fcc";
  link_cast_char_argv[1] = "--compile-and-link";
  link_cast_char_argv[2] = "--output";
  link_cast_char_argv[3] = link_output_path;
  link_cast_char_argv[4] = "tests/fixtures/cast_char_valid.c";
  exit_code = fcc_driver_run(5, link_cast_char_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "compile-and-link should handle casts and character literals")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with casts and character literals should run")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_member_subscript_test.exe";
  (void)remove(link_output_path);
  link_member_subscript_argv[0] = "fcc";
  link_member_subscript_argv[1] = "--compile-and-link";
  link_member_subscript_argv[2] = "--output";
  link_member_subscript_argv[3] = link_output_path;
  link_member_subscript_argv[4] = "tests/fixtures/member_subscript_valid.c";
  exit_code = fcc_driver_run(5, link_member_subscript_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "compile-and-link should handle member and subscript expressions")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with member and subscript expressions should run")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_pointer_arithmetic_test.exe";
  (void)remove(link_output_path);
  link_pointer_arithmetic_argv[0] = "fcc";
  link_pointer_arithmetic_argv[1] = "--compile-and-link";
  link_pointer_arithmetic_argv[2] = "--output";
  link_pointer_arithmetic_argv[3] = link_output_path;
  link_pointer_arithmetic_argv[4] = "tests/fixtures/pointer_arithmetic_valid.c";
  exit_code = fcc_driver_run(5, link_pointer_arithmetic_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "compile-and-link should handle pointer arithmetic expressions")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with pointer arithmetic expressions should run")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_function_pointer_test.exe";
  (void)remove(link_output_path);
  link_function_pointer_argv[0] = "fcc";
  link_function_pointer_argv[1] = "--compile-and-link";
  link_function_pointer_argv[2] = "--output";
  link_function_pointer_argv[3] = link_output_path;
  link_function_pointer_argv[4] = "tests/fixtures/function_pointer_valid.c";
  exit_code = fcc_driver_run(5, link_function_pointer_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "compile-and-link should handle function pointers")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with function pointers should run")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_global_function_pointer_test.exe";
  (void)remove(link_output_path);
  link_global_function_pointer_argv[0] = "fcc";
  link_global_function_pointer_argv[1] = "--compile-and-link";
  link_global_function_pointer_argv[2] = "--output";
  link_global_function_pointer_argv[3] = link_output_path;
  link_global_function_pointer_argv[4] =
      "tests/fixtures/global_function_pointer_initializer_valid.c";
  exit_code = fcc_driver_run(5, link_global_function_pointer_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "compile-and-link should handle global function pointer initializers")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with global function pointer initializers should run")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_compound_update_test.exe";
  (void)remove(link_output_path);
  link_compound_update_argv[0] = "fcc";
  link_compound_update_argv[1] = "--compile-and-link";
  link_compound_update_argv[2] = "--output";
  link_compound_update_argv[3] = link_output_path;
  link_compound_update_argv[4] = "tests/fixtures/compound_update_valid.c";
  exit_code = fcc_driver_run(5, link_compound_update_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "compile-and-link should handle compound assignments and updates")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with compound assignments and updates should run")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_scalar_types_test.exe";
  (void)remove(link_output_path);
  link_scalar_types_argv[0] = "fcc";
  link_scalar_types_argv[1] = "--compile-and-link";
  link_scalar_types_argv[2] = "--output";
  link_scalar_types_argv[3] = link_output_path;
  link_scalar_types_argv[4] = "tests/fixtures/scalar_types_valid.c";
  exit_code = fcc_driver_run(5, link_scalar_types_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "compile-and-link should handle extended scalar type specifiers")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with extended scalar types should run")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  link_output_path = "build/driver_link_many_args_test.exe";
  (void)remove(link_output_path);
  link_many_args_argv[0] = "fcc";
  link_many_args_argv[1] = "--compile-and-link";
  link_many_args_argv[2] = "--output";
  link_many_args_argv[3] = link_output_path;
  link_many_args_argv[4] = "tests/fixtures/many_args.c";
  exit_code = fcc_driver_run(5, link_many_args_argv, NULL, NULL);
  if (!test_expect(context, exit_code == 0,
                   "compile-and-link should handle more than four call arguments")) {
    (void)remove(link_output_path);
    return false;
  }

  if (!test_expect(context, test_run_program(link_output_path) == 0,
                   "compile-and-link output with many arguments should run successfully")) {
    (void)remove(link_output_path);
    return false;
  }

  (void)remove(link_output_path);

  missing_argv[0] = "fcc";
  missing_argv[1] = "tests/fixtures/missing.c";
  missing_output = test_open_temp_stream();
  missing_error = test_open_temp_stream();
  if (!test_expect(context, (missing_output != NULL) && (missing_error != NULL),
                   "tmpfiles should open")) {
    if (missing_output != NULL) {
      (void)fclose(missing_output);
    }

    if (missing_error != NULL) {
      (void)fclose(missing_error);
    }

    return false;
  }

  exit_code = fcc_driver_run(2, missing_argv, missing_output, missing_error);
  if (!test_expect(context, exit_code == 1, "missing input should return one error")) {
    (void)fclose(missing_output);
    (void)fclose(missing_error);
    return false;
  }

  if (!test_read_stream(missing_error, missing_buffer, sizeof(missing_buffer))) {
    (void)fclose(missing_output);
    (void)fclose(missing_error);
    return test_expect(context, false, "driver error output should be readable");
  }

  if (!test_expect(context,
                   strstr(missing_buffer, "tests/fixtures/missing.c(1,1): error: file not found") !=
                       NULL,
                   "driver error output should include a formatted diagnostic")) {
    (void)fclose(missing_output);
    (void)fclose(missing_error);
    return false;
  }

  (void)fclose(missing_output);
  (void)fclose(missing_error);
  return true;
}

int main(void) {
  TestContext context;
  bool ok;

#define RUN_TEST(TEST_FN)                                                                          \
  do {                                                                                             \
    ok = TEST_FN(&context) && ok;                                                                  \
  } while (0)

  context.passed_count = 0;
  context.failed_count = 0;

  ok = true;
  RUN_TEST(test_signature_helpers);
  RUN_TEST(test_source_load_success);
  RUN_TEST(test_line_column_mapping);
  RUN_TEST(test_diag_formatting);
  RUN_TEST(test_type_and_symbol_helpers);
  RUN_TEST(test_token_helpers);
  RUN_TEST(test_lexer_simple_sequence);
  RUN_TEST(test_lexer_operator_sequence);
  RUN_TEST(test_lexer_alignof_sequence);
  RUN_TEST(test_lexer_static_assert_sequence);
  RUN_TEST(test_lexer_accepts_long_identifier);
  RUN_TEST(test_lexer_rejects_invalid_character);
  RUN_TEST(test_lexer_rejects_unterminated_comment);
  RUN_TEST(test_parser_valid_translation_unit);
  RUN_TEST(test_parser_reports_syntax_error);
  RUN_TEST(test_parser_accepts_function_declaration);
  RUN_TEST(test_parser_accepts_function_call);
  RUN_TEST(test_parser_accepts_function_pointer_declarators);
  RUN_TEST(test_parser_accepts_sizeof);
  RUN_TEST(test_parser_accepts_alignof);
  RUN_TEST(test_parser_accepts_static_assert);
  RUN_TEST(test_parser_accepts_global_variables);
  RUN_TEST(test_parser_accepts_recursive_declarators);
  RUN_TEST(test_parser_accepts_struct_union_declarations);
  RUN_TEST(test_parser_accepts_enum_declarations);
  RUN_TEST(test_parser_accepts_member_and_subscript_expressions);
  RUN_TEST(test_parser_accepts_compound_and_update_expressions);
  RUN_TEST(test_sema_valid_translation_unit);
  RUN_TEST(test_sema_accepts_sizeof);
  RUN_TEST(test_sema_accepts_sizeof_constants);
  RUN_TEST(test_sema_accepts_alignof);
  RUN_TEST(test_sema_accepts_static_assert);
  RUN_TEST(test_sema_accepts_enums);
  RUN_TEST(test_sema_accepts_switch);
  RUN_TEST(test_sema_accepts_casts_and_char_literals);
  RUN_TEST(test_sema_accepts_member_and_subscript);
  RUN_TEST(test_sema_accepts_pointer_arithmetic);
  RUN_TEST(test_sema_accepts_compound_and_update);
  RUN_TEST(test_sema_accepts_extended_scalar_types);
  RUN_TEST(test_sema_accepts_integer_literal_suffixes);
  RUN_TEST(test_sema_accepts_array_constant_bounds);
  RUN_TEST(test_sema_accepts_const_pointer_declarators);
  RUN_TEST(test_sema_accepts_conditional_expression);
  RUN_TEST(test_sema_accepts_do_goto_and_labels);
  RUN_TEST(test_sema_accepts_string_literal_concatenation);
  RUN_TEST(test_sema_accepts_declspec_attribute);
  RUN_TEST(test_sema_accepts_initializer_lists);
  RUN_TEST(test_sema_accepts_null_and_typedef_compatibility);
  RUN_TEST(test_sema_accepts_array_pointer_declarators_and_parameter_decay);
  RUN_TEST(test_sema_accepts_function_pointers);
  RUN_TEST(test_sema_accepts_global_function_pointer_initializers);
  RUN_TEST(test_sema_accepts_void_casts);
  RUN_TEST(test_sema_accepts_unsized_initializer_sizeof);
  RUN_TEST(test_sema_accepts_struct_copy);
  RUN_TEST(test_sema_accepts_variadic_functions);
  RUN_TEST(test_sema_accepts_declared_external_call);
  RUN_TEST(test_sema_accepts_global_variables);
  RUN_TEST(test_sema_accepts_recursive_declarators);
  RUN_TEST(test_sema_struct_union_layout);
  RUN_TEST(test_sema_allows_nested_shadowing);
  RUN_TEST(test_sema_rejects_undeclared_identifier);
  RUN_TEST(test_sema_rejects_top_level_redefinition);
  RUN_TEST(test_sema_rejects_break_outside_loop);
  RUN_TEST(test_sema_rejects_return_value_in_void_function);
  RUN_TEST(test_sema_rejects_invalid_assignment_target);
  RUN_TEST(test_sema_rejects_duplicate_function);
  RUN_TEST(test_sema_rejects_conflicting_function_declaration);
  RUN_TEST(test_sema_rejects_wrong_argument_count);
  RUN_TEST(test_sema_rejects_function_pointer_wrong_argument_count);
  RUN_TEST(test_sema_rejects_variadic_too_few_arguments);
  RUN_TEST(test_sema_rejects_sizeof_void);
  RUN_TEST(test_sema_rejects_alignof_void);
  RUN_TEST(test_sema_rejects_static_assert_failure);
  RUN_TEST(test_sema_rejects_static_assert_non_constant);
  RUN_TEST(test_sema_rejects_struct_incomplete_field);
  RUN_TEST(test_sema_rejects_struct_duplicate_field);
  RUN_TEST(test_sema_rejects_global_bad_initializer);
  RUN_TEST(test_sema_rejects_global_aggregate_bad_initializer);
  RUN_TEST(test_preprocessor_expands_local_includes);
  RUN_TEST(test_preprocessor_ignores_unknown_pragmas);
  RUN_TEST(test_preprocessor_expands_macros_and_conditionals);
  RUN_TEST(test_preprocessor_expands_function_macros);
  RUN_TEST(test_preprocessor_stabilizes_unavailable_macro_tokens);
  RUN_TEST(test_preprocessor_expands_if_elif_and_defined);
  RUN_TEST(test_preprocessor_splices_logical_lines);
  RUN_TEST(test_preprocessor_rejects_unsupported_directive);
  RUN_TEST(test_preprocessor_rejects_variadic_macro_too_few_arguments);
  RUN_TEST(test_preprocessor_reports_unterminated_conditional);
  RUN_TEST(test_ast_dump_output);
  RUN_TEST(test_codegen_simple_output);
  RUN_TEST(test_codegen_control_flow_output);
  RUN_TEST(test_codegen_sizeof_output);
  RUN_TEST(test_codegen_sizeof_constants_output);
  RUN_TEST(test_codegen_alignof_output);
  RUN_TEST(test_codegen_static_assert_output);
  RUN_TEST(test_codegen_enum_output);
  RUN_TEST(test_codegen_switch_output);
  RUN_TEST(test_codegen_cast_char_output);
  RUN_TEST(test_codegen_member_subscript_output);
  RUN_TEST(test_codegen_pointer_arithmetic_output);
  RUN_TEST(test_codegen_function_pointer_output);
  RUN_TEST(test_codegen_global_function_pointer_initializer_output);
  RUN_TEST(test_codegen_compound_update_output);
  RUN_TEST(test_codegen_extended_scalar_types_output);
  RUN_TEST(test_codegen_conditional_output);
  RUN_TEST(test_codegen_large_integer_literal_output);
  RUN_TEST(test_codegen_aggregate_copy_output);
  RUN_TEST(test_codegen_global_aggregate_initializers_output);
  RUN_TEST(test_codegen_local_aggregate_initializers_output);
  RUN_TEST(test_codegen_unsigned_64_binary_output);
  RUN_TEST(test_codegen_signed_mixed_width_output);
  RUN_TEST(test_codegen_register_aggregate_parameter_output);
  RUN_TEST(test_codegen_sign_extend_return_output);
  RUN_TEST(test_codegen_char_escape_output);
  RUN_TEST(test_codegen_external_call_output);
  RUN_TEST(test_codegen_global_output);
  RUN_TEST(test_codegen_many_args_output);
  RUN_TEST(test_driver_behavior);

#undef RUN_TEST

  fprintf(stdout, "tests passed: %zu\n", context.passed_count);
  fprintf(stdout, "tests failed: %zu\n", context.failed_count);

  if (!ok || (context.failed_count != 0)) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
