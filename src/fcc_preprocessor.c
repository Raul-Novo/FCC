// SPDX-License-Identifier: GPL-3.0-or-later
#define _CRT_SECURE_NO_WARNINGS

#include "fcc/preprocessor.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fcc/base.h"

typedef struct FccPreprocessorMacro {
  char name[FCC_MAX_IDENTIFIER_LENGTH + 1];
  char* replacement;
  bool is_function_like;
  bool is_variadic;
  char** parameters;
  size_t parameter_count;
} FccPreprocessorMacro;

typedef struct FccPreprocessorConditional {
  bool parent_active;
  bool current_active;
  bool branch_taken;
  bool saw_else;
  FccSourceSpan directive_span;
} FccPreprocessorConditional;

typedef struct FccPreprocessorBuffer {
  BYTE* bytes;
  size_t length;
  size_t capacity;
  struct FccPreprocessorUnavailableToken* unavailable_tokens;
  size_t unavailable_token_count;
  size_t unavailable_token_capacity;
} FccPreprocessorBuffer;

typedef struct FccPreprocessorUnavailableToken {
  size_t begin_offset;
  size_t end_offset;
  const FccPreprocessorMacro* macro;
} FccPreprocessorUnavailableToken;

typedef struct FccPreprocessor {
  const FccPreprocessorOptions* options;
  FccDiagnostics* diagnostics;
  FILE* output_stream;
  char** pragma_once_paths;
  size_t pragma_once_path_count;
  size_t pragma_once_path_capacity;
  FccPreprocessorMacro* macros;
  size_t macro_count;
  size_t macro_capacity;
  FccPreprocessorConditional conditional_stack[FCC_MAX_PREPROCESS_CONDITIONAL_DEPTH];
  size_t conditional_depth;
  size_t recursion_depth;
} FccPreprocessor;

typedef struct FccPreprocessorExpressionParser {
  FccPreprocessor* preprocessor;
  const FccSourceFile* source_file;
  FccSourceSpan directive_span;
  const BYTE* bytes;
  size_t cursor_offset;
  size_t line_end_offset;
  bool has_error;
} FccPreprocessorExpressionParser;

/*
 * The current preprocessor is a single output pass with bounded include and
 * conditional nesting. Macro replacement is textual and now supports bounded
 * rescanning plus C17 variadic macro tails; full macro disable marking remains
 * tracked as future work.
 */
static bool fcc_preprocessor_emit_file(FccPreprocessor* preprocessor,
                                       const FccSourceFile* source_file);
static bool fcc_preprocessor_emit_file_contents(FccPreprocessor* preprocessor,
                                                const FccSourceFile* source_file);
static bool fcc_preprocessor_define_macro(FccPreprocessor* preprocessor, const char* name,
                                          char* replacement, bool is_function_like,
                                          bool is_variadic, char** parameters,
                                          size_t parameter_count);
static void fcc_preprocessor_buffer_init(FccPreprocessorBuffer* buffer);
static void fcc_preprocessor_buffer_dispose(FccPreprocessorBuffer* buffer);
static bool fcc_preprocessor_buffer_append(FccPreprocessor* preprocessor,
                                           const FccSourceFile* source_file, FccSourceSpan span,
                                           FccPreprocessorBuffer* buffer, const BYTE* bytes,
                                           size_t byte_count);
static bool fcc_preprocessor_buffer_append_range(
    FccPreprocessor* preprocessor, const FccSourceFile* source_file, FccSourceSpan span,
    FccPreprocessorBuffer* output_buffer, const BYTE* bytes,
    const FccPreprocessorBuffer* input_buffer, size_t begin_offset, size_t end_offset);
static bool fcc_preprocessor_buffer_mark_unavailable(FccPreprocessor* preprocessor,
                                                     const FccSourceFile* source_file,
                                                     FccSourceSpan span,
                                                     FccPreprocessorBuffer* buffer,
                                                     size_t begin_offset, size_t end_offset,
                                                     const FccPreprocessorMacro* macro);
static bool fcc_preprocessor_buffer_token_is_unavailable(
    const FccPreprocessorBuffer* buffer, size_t begin_offset, size_t end_offset,
    const FccPreprocessorMacro* macro);
static bool fcc_preprocessor_is_identifier_start(BYTE byte_value);
static bool fcc_preprocessor_is_identifier_continue(BYTE byte_value);
static size_t fcc_preprocessor_skip_horizontal_space(const BYTE* bytes, size_t cursor_offset,
                                                     size_t end_offset);
static ptrdiff_t fcc_preprocessor_find_macro_by_range(const FccPreprocessor* preprocessor,
                                                      const BYTE* bytes, size_t begin_offset,
                                                      size_t end_offset);
static void fcc_preprocessor_free_string_array(char** strings, size_t string_count);
static size_t fcc_preprocessor_scan_literal(const BYTE* bytes, size_t cursor_offset,
                                            size_t line_end_offset);
static bool fcc_preprocessor_join_variadic_macro_arguments(
    FccPreprocessor* preprocessor, const FccSourceFile* source_file, FccSourceSpan invocation_span,
    char** arguments, size_t first_variadic_argument_index, size_t argument_count,
    char** joined_argument_out);
static bool fcc_preprocessor_parse_macro_invocation_arguments(
    FccPreprocessor* preprocessor, const FccSourceFile* source_file,
    const FccPreprocessorMacro* macro, const BYTE* bytes, FccSourceSpan invocation_span,
    size_t open_paren_offset, size_t line_end_offset, size_t* invocation_end_offset,
    char*** arguments_out, size_t* argument_count_out);
static bool fcc_preprocessor_build_function_macro_replacement(FccPreprocessor* preprocessor,
                                                              const FccSourceFile* source_file,
                                                              FccSourceSpan invocation_span,
                                                              const FccPreprocessorMacro* macro,
                                                              char** arguments,
                                                              const char* variadic_argument,
                                                              FccPreprocessorBuffer* output_buffer);

static void fcc_preprocessor_emit(FccPreprocessor* preprocessor, const FccSourceFile* source_file,
                                  FccSourceSpan span, FccDiagSeverity severity,
                                  const char* message) {
  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(message != NULL);

  fcc_diag_emit_source(preprocessor->diagnostics, source_file, span, severity, message);
}

static void fcc_preprocessor_emit_fatal_message(FccDiagnostics* diagnostics,
                                                const FccSourceFile* source_file,
                                                const char* message) {
  FccSourceSpan span;

  assert(diagnostics != NULL);
  assert(source_file != NULL);
  assert(message != NULL);

  span.begin_offset = 0;
  span.end_offset = 0;
  fcc_diag_emit_source(diagnostics, source_file, span, FCC_DIAG_SEVERITY_FATAL, message);
}

static bool fcc_preprocessor_write_bytes(FccPreprocessor* preprocessor,
                                         const FccSourceFile* source_file, FccSourceSpan span,
                                         const BYTE* bytes, size_t byte_count) {
  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(bytes != NULL || byte_count == 0);

  if ((byte_count != 0) &&
      (fwrite(bytes, 1, byte_count, preprocessor->output_stream) != byte_count)) {
    fcc_preprocessor_emit(preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                          "preprocessor output write failed");
    return false;
  }

  return true;
}

static bool fcc_preprocessor_macro_is_disabled(
    const FccPreprocessorMacro* const* disabled_macros, size_t disabled_macro_count,
    const FccPreprocessorMacro* macro) {
  size_t disabled_index;

  assert(disabled_macros != NULL || disabled_macro_count == 0);
  assert(macro != NULL);

  for (disabled_index = 0; disabled_index < disabled_macro_count; ++disabled_index) {
    if (disabled_macros[disabled_index] == macro) {
      return true;
    }
  }

  return false;
}

static bool fcc_preprocessor_push_disabled_macro(
    FccPreprocessor* preprocessor, const FccSourceFile* source_file, FccSourceSpan span,
    const FccPreprocessorMacro* macro, const FccPreprocessorMacro** disabled_macros,
    size_t disabled_macro_count) {
  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(macro != NULL);
  assert(disabled_macros != NULL);

  if (disabled_macro_count >= FCC_MAX_PREPROCESS_DEPTH) {
    fcc_preprocessor_emit(preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                          "preprocessor macro expansion nesting exceeds FCC_MAX_PREPROCESS_DEPTH");
    return false;
  }

  disabled_macros[disabled_macro_count] = macro;
  return true;
}

static FccSourceSpan fcc_preprocessor_make_token_span(FccSourceSpan fallback_span,
                                                      bool source_offsets_are_valid,
                                                      size_t begin_offset, size_t end_offset) {
  FccSourceSpan span;

  assert(begin_offset <= end_offset);

  if (!source_offsets_are_valid) {
    return fallback_span;
  }

  span.begin_offset = begin_offset;
  span.end_offset = end_offset;
  return span;
}

static bool fcc_preprocessor_expand_text_once(
    FccPreprocessor* preprocessor, const FccSourceFile* source_file, const BYTE* bytes,
    const FccPreprocessorBuffer* input_buffer, size_t begin_offset, size_t end_offset,
    FccSourceSpan fallback_span, bool source_offsets_are_valid,
    const FccPreprocessorMacro** disabled_macros, size_t disabled_macro_count,
    FccPreprocessorBuffer* output_buffer, bool* expanded_any) {
  size_t cursor_offset;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(bytes != NULL || begin_offset == end_offset);
  assert(begin_offset <= end_offset);
  assert(disabled_macros != NULL || disabled_macro_count == 0);
  assert(output_buffer != NULL);
  assert(expanded_any != NULL);

  if (begin_offset == end_offset) {
    return true;
  }

  cursor_offset = begin_offset;
  while (cursor_offset < end_offset) {
    size_t token_end_offset;
    FccSourceSpan span;

    if (((bytes[cursor_offset] == (BYTE)'/') && ((cursor_offset + 1) < end_offset) &&
         (bytes[cursor_offset + 1] == (BYTE)'/')) ||
        ((bytes[cursor_offset] == (BYTE)'/') && ((cursor_offset + 1) < end_offset) &&
         (bytes[cursor_offset + 1] == (BYTE)'*'))) {
      span = fcc_preprocessor_make_token_span(fallback_span, source_offsets_are_valid,
                                              cursor_offset, end_offset);
      if (!fcc_preprocessor_buffer_append_range(preprocessor, source_file, span, output_buffer,
                                                bytes, input_buffer, cursor_offset, end_offset)) {
        return false;
      }

      return true;
    }

    if ((bytes[cursor_offset] == (BYTE)'"') || (bytes[cursor_offset] == (BYTE)'\'')) {
      token_end_offset = fcc_preprocessor_scan_literal(bytes, cursor_offset, end_offset);
      span = fcc_preprocessor_make_token_span(fallback_span, source_offsets_are_valid,
                                              cursor_offset, token_end_offset);
      if (!fcc_preprocessor_buffer_append_range(preprocessor, source_file, span, output_buffer,
                                                bytes, input_buffer, cursor_offset,
                                                token_end_offset)) {
        return false;
      }

      cursor_offset = token_end_offset;
      continue;
    }

    if (fcc_preprocessor_is_identifier_start(bytes[cursor_offset])) {
      ptrdiff_t macro_index;

      token_end_offset = cursor_offset + 1;
      while ((token_end_offset < end_offset) &&
             fcc_preprocessor_is_identifier_continue(bytes[token_end_offset])) {
        ++token_end_offset;
      }

      macro_index =
          fcc_preprocessor_find_macro_by_range(preprocessor, bytes, cursor_offset, token_end_offset);
      if (macro_index >= 0) {
        FccPreprocessorMacro* macro;

        macro = &preprocessor->macros[macro_index];
        span = fcc_preprocessor_make_token_span(fallback_span, source_offsets_are_valid,
                                                cursor_offset, token_end_offset);
        if ((input_buffer != NULL) &&
            fcc_preprocessor_buffer_token_is_unavailable(input_buffer, cursor_offset,
                                                         token_end_offset, macro)) {
          if (!fcc_preprocessor_buffer_append_range(preprocessor, source_file, span, output_buffer,
                                                    bytes, input_buffer, cursor_offset,
                                                    token_end_offset)) {
            return false;
          }

          cursor_offset = token_end_offset;
          continue;
        }

        if (!fcc_preprocessor_macro_is_disabled(disabled_macros, disabled_macro_count, macro)) {
          if (macro->is_function_like) {
            char** arguments;
            FccPreprocessorBuffer replacement_buffer;
            char* variadic_argument;
            size_t argument_count;
            size_t invocation_end_offset;
            size_t open_paren_offset;

            open_paren_offset =
                fcc_preprocessor_skip_horizontal_space(bytes, token_end_offset, end_offset);
            if ((open_paren_offset >= end_offset) || (bytes[open_paren_offset] != (BYTE)'(')) {
              if (!fcc_preprocessor_buffer_append_range(preprocessor, source_file, span,
                                                        output_buffer, bytes, input_buffer,
                                                        cursor_offset, token_end_offset)) {
                return false;
              }

              cursor_offset = token_end_offset;
              continue;
            }

            arguments = NULL;
            argument_count = 0;
            variadic_argument = NULL;
            invocation_end_offset = open_paren_offset;
            span = fcc_preprocessor_make_token_span(fallback_span, source_offsets_are_valid,
                                                    cursor_offset, end_offset);
            if (!fcc_preprocessor_parse_macro_invocation_arguments(
                    preprocessor, source_file, macro, bytes, span, open_paren_offset, end_offset,
                    &invocation_end_offset, &arguments, &argument_count)) {
              return false;
            }

            span = fcc_preprocessor_make_token_span(fallback_span, source_offsets_are_valid,
                                                    cursor_offset, invocation_end_offset);
            if ((!macro->is_variadic && (argument_count != macro->parameter_count)) ||
                (macro->is_variadic && (argument_count < macro->parameter_count))) {
              fcc_preprocessor_free_string_array(arguments, argument_count);
              fcc_preprocessor_emit(preprocessor, source_file, span, FCC_DIAG_SEVERITY_ERROR,
                                    "function-like macro argument count does not match definition");
              return false;
            }

            if (macro->is_variadic &&
                !fcc_preprocessor_join_variadic_macro_arguments(
                    preprocessor, source_file, span, arguments, macro->parameter_count,
                    argument_count, &variadic_argument)) {
              fcc_preprocessor_free_string_array(arguments, argument_count);
              return false;
            }

            if (!fcc_preprocessor_push_disabled_macro(preprocessor, source_file, span, macro,
                                                      disabled_macros, disabled_macro_count)) {
              free(variadic_argument);
              fcc_preprocessor_free_string_array(arguments, argument_count);
              return false;
            }

            fcc_preprocessor_buffer_init(&replacement_buffer);
            if (!fcc_preprocessor_build_function_macro_replacement(
                    preprocessor, source_file, span, macro, arguments,
                    macro->is_variadic ? variadic_argument : NULL, &replacement_buffer)) {
              fcc_preprocessor_buffer_dispose(&replacement_buffer);
              free(variadic_argument);
              fcc_preprocessor_free_string_array(arguments, argument_count);
              return false;
            }

            *expanded_any = true;
            if (!fcc_preprocessor_expand_text_once(
                    preprocessor, source_file, replacement_buffer.bytes, &replacement_buffer, 0,
                    replacement_buffer.length, span, false, disabled_macros,
                    disabled_macro_count + 1, output_buffer, expanded_any)) {
              fcc_preprocessor_buffer_dispose(&replacement_buffer);
              free(variadic_argument);
              fcc_preprocessor_free_string_array(arguments, argument_count);
              return false;
            }

            fcc_preprocessor_buffer_dispose(&replacement_buffer);
            free(variadic_argument);
            fcc_preprocessor_free_string_array(arguments, argument_count);
            cursor_offset = invocation_end_offset;
            continue;
          } else {
            const BYTE* replacement_bytes;
            size_t replacement_length;

            if (!fcc_preprocessor_push_disabled_macro(preprocessor, source_file, span, macro,
                                                      disabled_macros, disabled_macro_count)) {
              return false;
            }

            replacement_bytes = (const BYTE*)macro->replacement;
            replacement_length = strlen(macro->replacement);
            *expanded_any = true;
            if (!fcc_preprocessor_expand_text_once(
                    preprocessor, source_file, replacement_bytes, NULL, 0, replacement_length, span,
                    false, disabled_macros, disabled_macro_count + 1, output_buffer,
                    expanded_any)) {
              return false;
            }

            cursor_offset = token_end_offset;
            continue;
          }
        }

        if (!fcc_preprocessor_buffer_append_range(preprocessor, source_file, span, output_buffer,
                                                  bytes, input_buffer, cursor_offset,
                                                  token_end_offset)) {
          return false;
        }

        if (!fcc_preprocessor_buffer_mark_unavailable(
                preprocessor, source_file, span, output_buffer,
                output_buffer->length - (token_end_offset - cursor_offset), output_buffer->length,
                macro)) {
          return false;
        }

        cursor_offset = token_end_offset;
        continue;
      }

      span = fcc_preprocessor_make_token_span(fallback_span, source_offsets_are_valid,
                                              cursor_offset, token_end_offset);
      if (!fcc_preprocessor_buffer_append_range(preprocessor, source_file, span, output_buffer,
                                                bytes, input_buffer, cursor_offset,
                                                token_end_offset)) {
        return false;
      }

      cursor_offset = token_end_offset;
      continue;
    }

    span = fcc_preprocessor_make_token_span(fallback_span, source_offsets_are_valid, cursor_offset,
                                            cursor_offset + 1);
    if (!fcc_preprocessor_buffer_append_range(preprocessor, source_file, span, output_buffer,
                                              bytes, input_buffer, cursor_offset,
                                              cursor_offset + 1)) {
      return false;
    }

    ++cursor_offset;
  }

  return true;
}

static bool fcc_preprocessor_write_source_range(FccPreprocessor* preprocessor,
                                                const FccSourceFile* source_file,
                                                size_t begin_offset, size_t end_offset) {
  FccSourceSpan span;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(begin_offset <= end_offset);
  assert(end_offset <= source_file->byte_count);

  span.begin_offset = begin_offset;
  span.end_offset = end_offset;
  return fcc_preprocessor_write_bytes(preprocessor, source_file, span,
                                      source_file->bytes + begin_offset, end_offset - begin_offset);
}

static void fcc_preprocessor_buffer_init(FccPreprocessorBuffer* buffer) {
  assert(buffer != NULL);

  buffer->bytes = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
  buffer->unavailable_tokens = NULL;
  buffer->unavailable_token_count = 0;
  buffer->unavailable_token_capacity = 0;
}

static void fcc_preprocessor_buffer_clear(FccPreprocessorBuffer* buffer) {
  assert(buffer != NULL);

  buffer->length = 0;
  buffer->unavailable_token_count = 0;
  if (buffer->bytes != NULL) {
    buffer->bytes[0] = 0;
  }
}

static void fcc_preprocessor_buffer_dispose(FccPreprocessorBuffer* buffer) {
  if (buffer == NULL) {
    return;
  }

  free(buffer->bytes);
  free(buffer->unavailable_tokens);
  fcc_preprocessor_buffer_init(buffer);
}

static bool fcc_preprocessor_buffer_reserve(FccPreprocessor* preprocessor,
                                            const FccSourceFile* source_file, FccSourceSpan span,
                                            FccPreprocessorBuffer* buffer,
                                            size_t additional_byte_count) {
  BYTE* new_bytes;
  size_t required_capacity;
  size_t new_capacity;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(buffer != NULL);

  if ((buffer->length > (SIZE_MAX - 1)) ||
      (additional_byte_count > ((SIZE_MAX - 1) - buffer->length))) {
    fcc_preprocessor_emit(preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                          "preprocessor macro expansion is too large");
    return false;
  }

  required_capacity = buffer->length + additional_byte_count + 1;
  if (required_capacity <= buffer->capacity) {
    return true;
  }

  new_capacity = (buffer->capacity == 0) ? 128 : buffer->capacity;
  while (new_capacity < required_capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      new_capacity = required_capacity;
      break;
    }

    new_capacity *= 2;
  }

  new_bytes = (BYTE*)realloc(buffer->bytes, new_capacity);
  if (new_bytes == NULL) {
    fcc_preprocessor_emit(preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                          "out of memory");
    return false;
  }

  buffer->bytes = new_bytes;
  buffer->capacity = new_capacity;
  return true;
}

static bool fcc_preprocessor_buffer_reserve_unavailable_tokens(
    FccPreprocessor* preprocessor, const FccSourceFile* source_file, FccSourceSpan span,
    FccPreprocessorBuffer* buffer, size_t additional_token_count) {
  FccPreprocessorUnavailableToken* new_tokens;
  size_t required_capacity;
  size_t new_capacity;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(buffer != NULL);

  if (additional_token_count > (SIZE_MAX - buffer->unavailable_token_count)) {
    fcc_preprocessor_emit(preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                          "preprocessor macro expansion metadata is too large");
    return false;
  }

  required_capacity = buffer->unavailable_token_count + additional_token_count;
  if (required_capacity <= buffer->unavailable_token_capacity) {
    return true;
  }

  new_capacity =
      (buffer->unavailable_token_capacity == 0) ? 8 : buffer->unavailable_token_capacity;
  while (new_capacity < required_capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      new_capacity = required_capacity;
      break;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(*new_tokens))) {
    fcc_preprocessor_emit(preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                          "preprocessor macro expansion metadata is too large");
    return false;
  }

  new_tokens = (FccPreprocessorUnavailableToken*)realloc(
      buffer->unavailable_tokens, new_capacity * sizeof(*new_tokens));
  if (new_tokens == NULL) {
    fcc_preprocessor_emit(preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                          "out of memory");
    return false;
  }

  buffer->unavailable_tokens = new_tokens;
  buffer->unavailable_token_capacity = new_capacity;
  return true;
}

static bool fcc_preprocessor_buffer_mark_unavailable(FccPreprocessor* preprocessor,
                                                     const FccSourceFile* source_file,
                                                     FccSourceSpan span,
                                                     FccPreprocessorBuffer* buffer,
                                                     size_t begin_offset, size_t end_offset,
                                                     const FccPreprocessorMacro* macro) {
  FccPreprocessorUnavailableToken* token;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(buffer != NULL);
  assert(begin_offset <= end_offset);
  assert(end_offset <= buffer->length);
  assert(macro != NULL);

  if (!fcc_preprocessor_buffer_reserve_unavailable_tokens(preprocessor, source_file, span, buffer,
                                                          1)) {
    return false;
  }

  token = &buffer->unavailable_tokens[buffer->unavailable_token_count];
  token->begin_offset = begin_offset;
  token->end_offset = end_offset;
  token->macro = macro;
  ++buffer->unavailable_token_count;
  return true;
}

static bool fcc_preprocessor_buffer_token_is_unavailable(
    const FccPreprocessorBuffer* buffer, size_t begin_offset, size_t end_offset,
    const FccPreprocessorMacro* macro) {
  size_t token_index;

  assert(buffer != NULL);
  assert(begin_offset <= end_offset);
  assert(macro != NULL);

  for (token_index = 0; token_index < buffer->unavailable_token_count; ++token_index) {
    const FccPreprocessorUnavailableToken* token;

    token = &buffer->unavailable_tokens[token_index];
    if ((token->begin_offset == begin_offset) && (token->end_offset == end_offset) &&
        (token->macro == macro)) {
      return true;
    }
  }

  return false;
}

static bool fcc_preprocessor_buffer_copy_unavailable_tokens(
    FccPreprocessor* preprocessor, const FccSourceFile* source_file, FccSourceSpan span,
    FccPreprocessorBuffer* output_buffer, const FccPreprocessorBuffer* input_buffer,
    size_t input_begin_offset, size_t input_end_offset, size_t output_begin_offset) {
  size_t token_index;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(output_buffer != NULL);
  assert(input_buffer != NULL);
  assert(input_begin_offset <= input_end_offset);

  for (token_index = 0; token_index < input_buffer->unavailable_token_count; ++token_index) {
    const FccPreprocessorUnavailableToken* token;
    size_t copied_begin_offset;
    size_t copied_end_offset;

    token = &input_buffer->unavailable_tokens[token_index];
    if ((token->begin_offset < input_begin_offset) || (token->end_offset > input_end_offset)) {
      continue;
    }

    copied_begin_offset = output_begin_offset + (token->begin_offset - input_begin_offset);
    copied_end_offset = output_begin_offset + (token->end_offset - input_begin_offset);
    if (!fcc_preprocessor_buffer_mark_unavailable(preprocessor, source_file, span, output_buffer,
                                                  copied_begin_offset, copied_end_offset,
                                                  token->macro)) {
      return false;
    }
  }

  return true;
}

static bool fcc_preprocessor_buffer_append(FccPreprocessor* preprocessor,
                                           const FccSourceFile* source_file, FccSourceSpan span,
                                           FccPreprocessorBuffer* buffer, const BYTE* bytes,
                                           size_t byte_count) {
  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(buffer != NULL);
  assert(bytes != NULL || byte_count == 0);

  if (byte_count == 0) {
    return true;
  }

  if (!fcc_preprocessor_buffer_reserve(preprocessor, source_file, span, buffer, byte_count)) {
    return false;
  }

  (void)memcpy(buffer->bytes + buffer->length, bytes, byte_count);
  buffer->length += byte_count;
  buffer->bytes[buffer->length] = 0;
  return true;
}

static bool fcc_preprocessor_buffer_append_range(
    FccPreprocessor* preprocessor, const FccSourceFile* source_file, FccSourceSpan span,
    FccPreprocessorBuffer* output_buffer, const BYTE* bytes,
    const FccPreprocessorBuffer* input_buffer, size_t begin_offset, size_t end_offset) {
  size_t output_begin_offset;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(output_buffer != NULL);
  assert(bytes != NULL || begin_offset == end_offset);
  assert(begin_offset <= end_offset);

  output_begin_offset = output_buffer->length;
  if (!fcc_preprocessor_buffer_append(preprocessor, source_file, span, output_buffer,
                                      (begin_offset == end_offset) ? bytes : (bytes + begin_offset),
                                      end_offset - begin_offset)) {
    return false;
  }

  if (input_buffer != NULL) {
    return fcc_preprocessor_buffer_copy_unavailable_tokens(
        preprocessor, source_file, span, output_buffer, input_buffer, begin_offset, end_offset,
        output_begin_offset);
  }

  return true;
}

static bool fcc_preprocessor_buffer_append_byte(FccPreprocessor* preprocessor,
                                                const FccSourceFile* source_file,
                                                FccSourceSpan span,
                                                FccPreprocessorBuffer* buffer, BYTE byte_value) {
  return fcc_preprocessor_buffer_append(preprocessor, source_file, span, buffer, &byte_value, 1);
}

static bool fcc_preprocessor_buffer_equals_range(const FccPreprocessorBuffer* buffer,
                                                 const BYTE* bytes, size_t begin_offset,
                                                 size_t end_offset) {
  size_t range_length;

  assert(buffer != NULL);
  assert(bytes != NULL || begin_offset == end_offset);
  assert(begin_offset <= end_offset);

  range_length = end_offset - begin_offset;
  if (buffer->length != range_length) {
    return false;
  }

  return (range_length == 0) || (memcmp(buffer->bytes, bytes + begin_offset, range_length) == 0);
}

static bool fcc_preprocessor_has_pragma_once_path(const FccPreprocessor* preprocessor,
                                                  const char* path) {
  size_t path_index;

  assert(preprocessor != NULL);
  assert(path != NULL);

  for (path_index = 0; path_index < preprocessor->pragma_once_path_count; ++path_index) {
    if (_stricmp(preprocessor->pragma_once_paths[path_index], path) == 0) {
      return true;
    }
  }

  return false;
}

static bool fcc_preprocessor_add_pragma_once_path(FccPreprocessor* preprocessor, const char* path) {
  char* duplicated_path;
  size_t new_capacity;
  char** new_paths;
  size_t path_length;

  assert(preprocessor != NULL);
  assert(path != NULL);

  if (fcc_preprocessor_has_pragma_once_path(preprocessor, path)) {
    return true;
  }

  if (preprocessor->pragma_once_path_count == preprocessor->pragma_once_path_capacity) {
    new_capacity = preprocessor->pragma_once_path_capacity;
    if (new_capacity == 0) {
      new_capacity = 8;
    } else {
      new_capacity *= 2;
    }

    new_paths = (char**)realloc(preprocessor->pragma_once_paths, new_capacity * sizeof(char*));
    if (new_paths == NULL) {
      return false;
    }

    preprocessor->pragma_once_paths = new_paths;
    preprocessor->pragma_once_path_capacity = new_capacity;
  }

  path_length = strlen(path);
  duplicated_path = (char*)malloc(path_length + 1);
  if (duplicated_path == NULL) {
    return false;
  }

  (void)memcpy(duplicated_path, path, path_length + 1);
  preprocessor->pragma_once_paths[preprocessor->pragma_once_path_count] = duplicated_path;
  ++preprocessor->pragma_once_path_count;
  return true;
}

static bool fcc_preprocessor_is_space(BYTE byte_value) {
  return (byte_value == (BYTE)' ') || (byte_value == (BYTE)'\t') || (byte_value == (BYTE)'\r');
}

static bool fcc_preprocessor_is_identifier_start(BYTE byte_value) {
  return ((byte_value >= (BYTE)'A') && (byte_value <= (BYTE)'Z')) ||
         ((byte_value >= (BYTE)'a') && (byte_value <= (BYTE)'z')) || (byte_value == (BYTE)'_');
}

static bool fcc_preprocessor_is_identifier_continue(BYTE byte_value) {
  return fcc_preprocessor_is_identifier_start(byte_value) ||
         ((byte_value >= (BYTE)'0') && (byte_value <= (BYTE)'9'));
}

static size_t fcc_preprocessor_skip_horizontal_space(const BYTE* bytes, size_t cursor_offset,
                                                     size_t line_end_offset) {
  assert(bytes != NULL);
  assert(cursor_offset <= line_end_offset);

  while ((cursor_offset < line_end_offset) && fcc_preprocessor_is_space(bytes[cursor_offset])) {
    ++cursor_offset;
  }

  return cursor_offset;
}

static size_t fcc_preprocessor_trim_trailing_space(const BYTE* bytes, size_t begin_offset,
                                                   size_t end_offset) {
  assert(bytes != NULL);
  assert(begin_offset <= end_offset);

  while ((end_offset > begin_offset) && fcc_preprocessor_is_space(bytes[end_offset - 1])) {
    --end_offset;
  }

  return end_offset;
}

static bool fcc_preprocessor_copy_substring(const BYTE* bytes, size_t begin_offset,
                                            size_t end_offset, char* buffer, size_t buffer_size) {
  size_t length;

  assert(bytes != NULL);
  assert(buffer != NULL);
  assert(begin_offset <= end_offset);

  length = end_offset - begin_offset;
  if (length + 1 > buffer_size) {
    return false;
  }

  (void)memcpy(buffer, bytes + begin_offset, length);
  buffer[length] = '\0';
  return true;
}

static char* fcc_preprocessor_duplicate_substring(const BYTE* bytes, size_t begin_offset,
                                                  size_t end_offset) {
  char* duplicate;
  size_t length;

  assert(bytes != NULL);
  assert(begin_offset <= end_offset);

  length = end_offset - begin_offset;
  duplicate = (char*)malloc(length + 1);
  if (duplicate == NULL) {
    return NULL;
  }

  (void)memcpy(duplicate, bytes + begin_offset, length);
  duplicate[length] = '\0';
  return duplicate;
}

static void fcc_preprocessor_free_string_array(char** strings, size_t string_count) {
  size_t string_index;

  if (strings == NULL) {
    return;
  }

  for (string_index = 0; string_index < string_count; ++string_index) {
    free(strings[string_index]);
  }

  free(strings);
}

static bool fcc_preprocessor_is_decimal_digit(BYTE byte_value) {
  return (byte_value >= (BYTE)'0') && (byte_value <= (BYTE)'9');
}

static bool fcc_preprocessor_digit_value(BYTE byte_value, unsigned int* digit_value) {
  assert(digit_value != NULL);

  if ((byte_value >= (BYTE)'0') && (byte_value <= (BYTE)'9')) {
    *digit_value = (unsigned int)(byte_value - (BYTE)'0');
    return true;
  }

  if ((byte_value >= (BYTE)'a') && (byte_value <= (BYTE)'f')) {
    *digit_value = (unsigned int)(byte_value - (BYTE)'a') + 10U;
    return true;
  }

  if ((byte_value >= (BYTE)'A') && (byte_value <= (BYTE)'F')) {
    *digit_value = (unsigned int)(byte_value - (BYTE)'A') + 10U;
    return true;
  }

  return false;
}

static bool fcc_preprocessor_define_builtin_macro(FccPreprocessor* preprocessor, const char* name,
                                                  const char* replacement_text) {
  char* replacement;
  size_t replacement_length;

  assert(preprocessor != NULL);
  assert(name != NULL);
  assert(replacement_text != NULL);

  replacement_length = strlen(replacement_text);
  replacement = (char*)malloc(replacement_length + 1);
  if (replacement == NULL) {
    return false;
  }

  (void)memcpy(replacement, replacement_text, replacement_length + 1);
  if (!fcc_preprocessor_define_macro(preprocessor, name, replacement, false, false, NULL, 0)) {
    free(replacement);
    return false;
  }

  return true;
}

static bool fcc_preprocessor_initialize_builtin_macros(FccPreprocessor* preprocessor) {
  assert(preprocessor != NULL);

  return fcc_preprocessor_define_builtin_macro(preprocessor, "__FCC__", "1") &&
         fcc_preprocessor_define_builtin_macro(preprocessor, "__STDC__", "1") &&
         fcc_preprocessor_define_builtin_macro(preprocessor, "__STDC_VERSION__", "201710L") &&
         fcc_preprocessor_define_builtin_macro(preprocessor, "_WIN32", "1") &&
         fcc_preprocessor_define_builtin_macro(preprocessor, "_WIN64", "1");
}

static bool fcc_preprocessor_current_branch_active(const FccPreprocessor* preprocessor) {
  assert(preprocessor != NULL);

  if (preprocessor->conditional_depth == 0) {
    return true;
  }

  return preprocessor->conditional_stack[preprocessor->conditional_depth - 1].current_active;
}

static ptrdiff_t fcc_preprocessor_find_macro_by_name(const FccPreprocessor* preprocessor,
                                                     const char* name) {
  size_t macro_index;

  assert(preprocessor != NULL);
  assert(name != NULL);

  for (macro_index = 0; macro_index < preprocessor->macro_count; ++macro_index) {
    if (strcmp(preprocessor->macros[macro_index].name, name) == 0) {
      return (ptrdiff_t)macro_index;
    }
  }

  return -1;
}

static ptrdiff_t fcc_preprocessor_find_macro_by_range(const FccPreprocessor* preprocessor,
                                                      const BYTE* bytes, size_t begin_offset,
                                                      size_t end_offset) {
  size_t macro_index;
  size_t macro_name_length;
  size_t range_length;

  assert(preprocessor != NULL);
  assert(bytes != NULL);
  assert(begin_offset <= end_offset);

  range_length = end_offset - begin_offset;
  for (macro_index = 0; macro_index < preprocessor->macro_count; ++macro_index) {
    macro_name_length = strlen(preprocessor->macros[macro_index].name);
    if ((macro_name_length == range_length) &&
        (memcmp(preprocessor->macros[macro_index].name, bytes + begin_offset, range_length) == 0)) {
      return (ptrdiff_t)macro_index;
    }
  }

  return -1;
}

static bool fcc_preprocessor_ensure_macro_capacity(FccPreprocessor* preprocessor) {
  FccPreprocessorMacro* new_macros;
  size_t new_capacity;

  assert(preprocessor != NULL);

  if (preprocessor->macro_count < preprocessor->macro_capacity) {
    return true;
  }

  new_capacity = preprocessor->macro_capacity;
  if (new_capacity == 0) {
    new_capacity = 16;
  } else {
    new_capacity *= 2;
  }

  new_macros =
      (FccPreprocessorMacro*)realloc(preprocessor->macros, new_capacity * sizeof(*new_macros));
  if (new_macros == NULL) {
    return false;
  }

  preprocessor->macros = new_macros;
  preprocessor->macro_capacity = new_capacity;
  return true;
}

static void fcc_preprocessor_dispose_macro_payload(FccPreprocessorMacro* macro) {
  assert(macro != NULL);

  free(macro->replacement);
  macro->replacement = NULL;
  fcc_preprocessor_free_string_array(macro->parameters, macro->parameter_count);
  macro->parameters = NULL;
  macro->parameter_count = 0;
  macro->is_function_like = false;
  macro->is_variadic = false;
}

static bool fcc_preprocessor_define_macro(FccPreprocessor* preprocessor, const char* name,
                                          char* replacement, bool is_function_like,
                                          bool is_variadic, char** parameters,
                                          size_t parameter_count) {
  ptrdiff_t existing_index;
  FccPreprocessorMacro* macro;

  assert(preprocessor != NULL);
  assert(name != NULL);
  assert(replacement != NULL);
  assert((parameter_count == 0) || (parameters != NULL));

  existing_index = fcc_preprocessor_find_macro_by_name(preprocessor, name);
  if (existing_index >= 0) {
    macro = &preprocessor->macros[existing_index];
    fcc_preprocessor_dispose_macro_payload(macro);
    macro->replacement = replacement;
    macro->is_function_like = is_function_like;
    macro->is_variadic = is_variadic;
    macro->parameters = parameters;
    macro->parameter_count = parameter_count;
    return true;
  }

  if (!fcc_preprocessor_ensure_macro_capacity(preprocessor)) {
    return false;
  }

  macro = &preprocessor->macros[preprocessor->macro_count];
  (void)memcpy(macro->name, name, strlen(name) + 1);
  macro->replacement = replacement;
  macro->is_function_like = is_function_like;
  macro->is_variadic = is_variadic;
  macro->parameters = parameters;
  macro->parameter_count = parameter_count;
  ++preprocessor->macro_count;
  return true;
}

static void fcc_preprocessor_undefine_macro(FccPreprocessor* preprocessor, const char* name) {
  ptrdiff_t macro_index;
  size_t trailing_count;

  assert(preprocessor != NULL);
  assert(name != NULL);

  macro_index = fcc_preprocessor_find_macro_by_name(preprocessor, name);
  if (macro_index < 0) {
    return;
  }

  fcc_preprocessor_dispose_macro_payload(&preprocessor->macros[macro_index]);
  trailing_count = preprocessor->macro_count - ((size_t)macro_index + 1);
  if (trailing_count != 0) {
    (void)memmove(&preprocessor->macros[macro_index], &preprocessor->macros[macro_index + 1],
                  trailing_count * sizeof(preprocessor->macros[0]));
  }

  --preprocessor->macro_count;
}

static void fcc_preprocessor_dispose_macros(FccPreprocessor* preprocessor) {
  size_t macro_index;

  assert(preprocessor != NULL);

  for (macro_index = 0; macro_index < preprocessor->macro_count; ++macro_index) {
    fcc_preprocessor_dispose_macro_payload(&preprocessor->macros[macro_index]);
  }

  free(preprocessor->macros);
  preprocessor->macros = NULL;
  preprocessor->macro_count = 0;
  preprocessor->macro_capacity = 0;
}

static bool fcc_preprocessor_build_path(const char* prefix, size_t prefix_length,
                                        bool needs_separator, const char* suffix,
                                        char* resolved_path, size_t resolved_path_size) {
  size_t suffix_length;
  size_t total_length;

  assert(prefix != NULL);
  assert(suffix != NULL);
  assert(resolved_path != NULL);
  assert(resolved_path_size > 0);

  suffix_length = strlen(suffix);
  total_length = prefix_length + (needs_separator ? 1U : 0U) + suffix_length + 1U;
  if (total_length > resolved_path_size) {
    return false;
  }

  (void)memcpy(resolved_path, prefix, prefix_length);
  if (needs_separator) {
    resolved_path[prefix_length] = '\\';
    ++prefix_length;
  }

  (void)memcpy(resolved_path + prefix_length, suffix, suffix_length + 1);
  return true;
}

static bool fcc_preprocessor_build_sysroot_include_path(const char* sysroot_directory,
                                                        const char* include_path,
                                                        char* resolved_path,
                                                        size_t resolved_path_size) {
  char include_directory[FCC_MAX_PATH_LENGTH];
  size_t sysroot_length;
  bool needs_separator;

  assert(sysroot_directory != NULL);
  assert(include_path != NULL);
  assert(resolved_path != NULL);
  assert(resolved_path_size > 0);

  sysroot_length = strlen(sysroot_directory);
  needs_separator = (sysroot_length != 0) && (sysroot_directory[sysroot_length - 1] != '\\') &&
                    (sysroot_directory[sysroot_length - 1] != '/');
  if (!fcc_preprocessor_build_path(sysroot_directory, sysroot_length, needs_separator, "include",
                                   include_directory, sizeof(include_directory))) {
    return false;
  }

  return fcc_preprocessor_build_path(include_directory, strlen(include_directory), true,
                                     include_path, resolved_path, resolved_path_size);
}

static size_t fcc_preprocessor_source_directory_length(const char* path) {
  const char* path_cursor;
  size_t directory_length;

  assert(path != NULL);

  directory_length = 0;
  for (path_cursor = path; *path_cursor != '\0'; ++path_cursor) {
    if ((*path_cursor == '\\') || (*path_cursor == '/')) {
      directory_length = (size_t)((path_cursor - path) + 1);
    }
  }

  return directory_length;
}

static bool fcc_preprocessor_try_include_candidate(FccPreprocessor* preprocessor,
                                                   const char* candidate_path,
                                                   FccSourceFile* included_source_file,
                                                   char* error_buffer, size_t error_buffer_size,
                                                   bool* loaded_source_file) {
  assert(preprocessor != NULL);
  assert(candidate_path != NULL);
  assert(included_source_file != NULL);
  assert(error_buffer != NULL);
  assert(error_buffer_size > 0);
  assert(loaded_source_file != NULL);

  *loaded_source_file = false;
  if (fcc_preprocessor_has_pragma_once_path(preprocessor, candidate_path)) {
    *loaded_source_file = true;
    return true;
  }

  if (!fcc_source_file_load(included_source_file, candidate_path, error_buffer,
                            error_buffer_size)) {
    return true;
  }

  if (!fcc_preprocessor_emit_file(preprocessor, included_source_file)) {
    fcc_source_file_dispose(included_source_file);
    return false;
  }

  *loaded_source_file = true;
  return true;
}

static bool fcc_preprocessor_process_pragma_once(FccPreprocessor* preprocessor,
                                                 const FccSourceFile* source_file,
                                                 FccSourceSpan directive_span, size_t cursor_offset,
                                                 size_t line_end_offset) {
  const BYTE* bytes;
  size_t trimmed_line_end_offset;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  bytes = source_file->bytes;
  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);
  trimmed_line_end_offset =
      fcc_preprocessor_trim_trailing_space(bytes, cursor_offset, line_end_offset);
  if ((trimmed_line_end_offset - cursor_offset != 4) ||
      (memcmp(bytes + cursor_offset, "once", 4) != 0)) {
    (void)directive_span;
    return true;
  }

  if (!fcc_preprocessor_add_pragma_once_path(preprocessor, source_file->path)) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_FATAL,
                          "out of memory");
    return false;
  }

  return true;
}

static bool fcc_preprocessor_process_include(FccPreprocessor* preprocessor,
                                             const FccSourceFile* source_file,
                                             FccSourceSpan directive_span, size_t cursor_offset,
                                             size_t line_end_offset) {
  char candidate_path[FCC_MAX_PATH_LENGTH];
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  char include_path[FCC_MAX_PATH_LENGTH];
  char message[FCC_MAX_DIAG_MESSAGE_LENGTH];
  FccSourceFile included_source_file;
  const BYTE* bytes;
  size_t directory_length;
  size_t include_begin_offset;
  size_t include_directory_index;
  size_t include_end_offset;
  BYTE closing_delimiter;
  bool is_system_include;
  int written;
  bool loaded_source_file;
  bool path_overflowed;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  bytes = source_file->bytes;
  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);
  if (cursor_offset >= line_end_offset) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "expected quoted or angle-bracket include path");
    return true;
  }

  is_system_include = false;
  if (bytes[cursor_offset] == (BYTE)'"') {
    closing_delimiter = (BYTE)'"';
  } else if (bytes[cursor_offset] == (BYTE)'<') {
    closing_delimiter = (BYTE)'>';
    is_system_include = true;
  } else {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "expected quoted or angle-bracket include path");
    return true;
  }

  include_begin_offset = cursor_offset + 1;
  include_end_offset = include_begin_offset;
  while ((include_end_offset < line_end_offset) &&
         (bytes[include_end_offset] != closing_delimiter)) {
    ++include_end_offset;
  }

  if ((include_end_offset >= line_end_offset) || (bytes[include_end_offset] != closing_delimiter)) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "unterminated include path");
    return true;
  }

  if (!fcc_preprocessor_copy_substring(bytes, include_begin_offset, include_end_offset,
                                       include_path, sizeof(include_path))) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "include path exceeds FCC_MAX_PATH_LENGTH");
    return true;
  }

  include_end_offset =
      fcc_preprocessor_skip_horizontal_space(bytes, include_end_offset + 1, line_end_offset);
  if (include_end_offset != line_end_offset) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "unexpected tokens after include path");
    return true;
  }

  error_buffer[0] = '\0';
  path_overflowed = false;
  if (!is_system_include) {
    directory_length = fcc_preprocessor_source_directory_length(source_file->path);
    if (fcc_preprocessor_build_path(source_file->path, directory_length, false, include_path,
                                    candidate_path, sizeof(candidate_path))) {
      if (!fcc_preprocessor_try_include_candidate(preprocessor, candidate_path,
                                                  &included_source_file, error_buffer,
                                                  sizeof(error_buffer), &loaded_source_file)) {
        return false;
      }

      if (loaded_source_file) {
        fcc_source_file_dispose(&included_source_file);
        return true;
      }
    } else {
      path_overflowed = true;
    }
  }

  for (include_directory_index = 0;
       include_directory_index < preprocessor->options->include_directory_count;
       ++include_directory_index) {
    const char* include_directory;
    bool needs_separator;
    size_t include_directory_length;

    include_directory = preprocessor->options->include_directories[include_directory_index];
    include_directory_length = strlen(include_directory);
    needs_separator = (include_directory_length != 0) &&
                      (include_directory[include_directory_length - 1] != '\\') &&
                      (include_directory[include_directory_length - 1] != '/');
    if (!fcc_preprocessor_build_path(include_directory, include_directory_length, needs_separator,
                                     include_path, candidate_path, sizeof(candidate_path))) {
      path_overflowed = true;
      continue;
    }

    if (!fcc_preprocessor_try_include_candidate(preprocessor, candidate_path, &included_source_file,
                                                error_buffer, sizeof(error_buffer),
                                                &loaded_source_file)) {
      return false;
    }

    if (loaded_source_file) {
      fcc_source_file_dispose(&included_source_file);
      return true;
    }
  }

  if (preprocessor->options->sysroot_directory != NULL) {
    if (!fcc_preprocessor_build_sysroot_include_path(preprocessor->options->sysroot_directory,
                                                     include_path, candidate_path,
                                                     sizeof(candidate_path))) {
      path_overflowed = true;
    } else {
      if (!fcc_preprocessor_try_include_candidate(preprocessor, candidate_path,
                                                  &included_source_file, error_buffer,
                                                  sizeof(error_buffer), &loaded_source_file)) {
        return false;
      }

      if (loaded_source_file) {
        fcc_source_file_dispose(&included_source_file);
        return true;
      }
    }
  }

  if (path_overflowed && (error_buffer[0] == '\0')) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "include path resolution exceeds FCC_MAX_PATH_LENGTH");
    return true;
  }

  written = snprintf(message, sizeof(message), "unable to include '%s': %s", include_path,
                     (error_buffer[0] != '\0') ? error_buffer : "file not found");
  if ((written < 0) || ((size_t)written >= sizeof(message))) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "include diagnostic formatting failure");
  } else {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          message);
  }

  return true;
}

static bool fcc_preprocessor_parse_define_parameters(
    FccPreprocessor* preprocessor, const FccSourceFile* source_file, FccSourceSpan directive_span,
    size_t open_paren_offset, size_t line_end_offset, size_t* after_parameters_offset,
    char*** parameters_out, size_t* parameter_count_out, bool* is_variadic_out,
    bool* parsed_out) {
  const BYTE* bytes;
  char** parameters;
  size_t parameter_capacity;
  size_t parameter_count;
  size_t cursor_offset;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(open_paren_offset < line_end_offset);
  assert(after_parameters_offset != NULL);
  assert(parameters_out != NULL);
  assert(parameter_count_out != NULL);
  assert(is_variadic_out != NULL);
  assert(parsed_out != NULL);

  bytes = source_file->bytes;
  parameters = NULL;
  parameter_capacity = 0;
  parameter_count = 0;
  cursor_offset = open_paren_offset + 1;
  *parameters_out = NULL;
  *parameter_count_out = 0;
  *is_variadic_out = false;
  *parsed_out = false;

  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);
  if ((cursor_offset < line_end_offset) && (bytes[cursor_offset] == (BYTE)')')) {
    *after_parameters_offset = cursor_offset + 1;
    *parsed_out = true;
    return true;
  }

  for (;;) {
    char* parameter;
    char** new_parameters;
    size_t duplicate_index;
    size_t parameter_begin_offset;
    size_t parameter_end_offset;
    size_t parameter_length;

    if ((cursor_offset + 2 < line_end_offset) && (bytes[cursor_offset] == (BYTE)'.') &&
        (bytes[cursor_offset + 1] == (BYTE)'.') && (bytes[cursor_offset + 2] == (BYTE)'.')) {
      cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset + 3,
                                                             line_end_offset);
      if ((cursor_offset >= line_end_offset) || (bytes[cursor_offset] != (BYTE)')')) {
        fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                              "expected ')' after variadic macro parameter");
        fcc_preprocessor_free_string_array(parameters, parameter_count);
        return true;
      }

      *after_parameters_offset = cursor_offset + 1;
      *parameters_out = parameters;
      *parameter_count_out = parameter_count;
      *is_variadic_out = true;
      *parsed_out = true;
      return true;
    }

    if ((cursor_offset >= line_end_offset) ||
        !fcc_preprocessor_is_identifier_start(bytes[cursor_offset])) {
      fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                            "expected macro parameter name");
      fcc_preprocessor_free_string_array(parameters, parameter_count);
      return true;
    }

    parameter_begin_offset = cursor_offset;
    parameter_end_offset = parameter_begin_offset;
    while ((parameter_end_offset < line_end_offset) &&
           fcc_preprocessor_is_identifier_continue(bytes[parameter_end_offset])) {
      ++parameter_end_offset;
    }

    parameter_length = parameter_end_offset - parameter_begin_offset;
    if (parameter_length > FCC_MAX_IDENTIFIER_LENGTH) {
      fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                            "macro parameter name exceeds FCC_MAX_IDENTIFIER_LENGTH");
      fcc_preprocessor_free_string_array(parameters, parameter_count);
      return true;
    }

    for (duplicate_index = 0; duplicate_index < parameter_count; ++duplicate_index) {
      if ((strlen(parameters[duplicate_index]) == parameter_length) &&
          (memcmp(parameters[duplicate_index], bytes + parameter_begin_offset, parameter_length) ==
           0)) {
        fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                              "duplicate macro parameter name");
        fcc_preprocessor_free_string_array(parameters, parameter_count);
        return true;
      }
    }

    if (parameter_count >= FCC_MAX_FUNCTION_PARAMETERS) {
      fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                            "macro parameter count exceeds FCC_MAX_FUNCTION_PARAMETERS");
      fcc_preprocessor_free_string_array(parameters, parameter_count);
      return true;
    }

    if (parameter_count == parameter_capacity) {
      size_t new_capacity;

      new_capacity = (parameter_capacity == 0) ? 4 : (parameter_capacity * 2);
      if (new_capacity > FCC_MAX_FUNCTION_PARAMETERS) {
        new_capacity = FCC_MAX_FUNCTION_PARAMETERS;
      }

      new_parameters = (char**)realloc(parameters, new_capacity * sizeof(char*));
      if (new_parameters == NULL) {
        fcc_preprocessor_free_string_array(parameters, parameter_count);
        fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_FATAL,
                              "out of memory");
        return false;
      }

      parameters = new_parameters;
      parameter_capacity = new_capacity;
    }

    parameter =
        fcc_preprocessor_duplicate_substring(bytes, parameter_begin_offset, parameter_end_offset);
    if (parameter == NULL) {
      fcc_preprocessor_free_string_array(parameters, parameter_count);
      fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_FATAL,
                            "out of memory");
      return false;
    }

    parameters[parameter_count] = parameter;
    ++parameter_count;

    cursor_offset =
        fcc_preprocessor_skip_horizontal_space(bytes, parameter_end_offset, line_end_offset);
    if (cursor_offset >= line_end_offset) {
      fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                            "expected ')' after macro parameter list");
      fcc_preprocessor_free_string_array(parameters, parameter_count);
      return true;
    }

    if (bytes[cursor_offset] == (BYTE)',') {
      ++cursor_offset;
      cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);
      if ((cursor_offset < line_end_offset) && (bytes[cursor_offset] == (BYTE)')')) {
        fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                              "expected macro parameter name after ','");
        fcc_preprocessor_free_string_array(parameters, parameter_count);
        return true;
      }

      continue;
    }

    if (bytes[cursor_offset] == (BYTE)')') {
      *after_parameters_offset = cursor_offset + 1;
      *parameters_out = parameters;
      *parameter_count_out = parameter_count;
      *parsed_out = true;
      return true;
    }

    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "expected ',' or ')' in macro parameter list");
    fcc_preprocessor_free_string_array(parameters, parameter_count);
    return true;
  }
}

static bool fcc_preprocessor_process_define(FccPreprocessor* preprocessor,
                                            const FccSourceFile* source_file,
                                            FccSourceSpan directive_span, size_t cursor_offset,
                                            size_t line_end_offset) {
  char macro_name[FCC_MAX_IDENTIFIER_LENGTH + 1];
  char* replacement;
  char** parameters;
  const BYTE* bytes;
  bool is_function_like;
  bool is_variadic;
  bool parsed_parameters;
  size_t macro_begin_offset;
  size_t macro_end_offset;
  size_t parameter_count;
  size_t replacement_begin_offset;
  size_t replacement_end_offset;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  bytes = source_file->bytes;
  parameters = NULL;
  parameter_count = 0;
  is_function_like = false;
  is_variadic = false;
  parsed_parameters = false;
  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);
  if ((cursor_offset >= line_end_offset) ||
      !fcc_preprocessor_is_identifier_start(bytes[cursor_offset])) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "expected macro name after '#define'");
    return true;
  }

  macro_begin_offset = cursor_offset;
  macro_end_offset = macro_begin_offset;
  while ((macro_end_offset < line_end_offset) &&
         fcc_preprocessor_is_identifier_continue(bytes[macro_end_offset])) {
    ++macro_end_offset;
  }

  if (!fcc_preprocessor_copy_substring(bytes, macro_begin_offset, macro_end_offset, macro_name,
                                       sizeof(macro_name))) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "macro name exceeds FCC_MAX_IDENTIFIER_LENGTH");
    return true;
  }

  if ((macro_end_offset < line_end_offset) && (bytes[macro_end_offset] == (BYTE)'(')) {
    is_function_like = true;
    if (!fcc_preprocessor_parse_define_parameters(
            preprocessor, source_file, directive_span, macro_end_offset, line_end_offset,
            &replacement_begin_offset, &parameters, &parameter_count, &is_variadic,
            &parsed_parameters)) {
      return false;
    }

    if (!parsed_parameters) {
      return true;
    }
  } else {
    replacement_begin_offset = macro_end_offset;
  }

  replacement_begin_offset =
      fcc_preprocessor_skip_horizontal_space(bytes, replacement_begin_offset, line_end_offset);
  replacement_end_offset =
      fcc_preprocessor_trim_trailing_space(bytes, replacement_begin_offset, line_end_offset);
  replacement =
      fcc_preprocessor_duplicate_substring(bytes, replacement_begin_offset, replacement_end_offset);
  if (replacement == NULL) {
    fcc_preprocessor_free_string_array(parameters, parameter_count);
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_FATAL,
                          "out of memory");
    return false;
  }

  if (!fcc_preprocessor_define_macro(preprocessor, macro_name, replacement, is_function_like,
                                     is_variadic, parameters, parameter_count)) {
    free(replacement);
    fcc_preprocessor_free_string_array(parameters, parameter_count);
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_FATAL,
                          "out of memory");
    return false;
  }

  return true;
}

static bool fcc_preprocessor_process_undef(FccPreprocessor* preprocessor,
                                           const FccSourceFile* source_file,
                                           FccSourceSpan directive_span, size_t cursor_offset,
                                           size_t line_end_offset) {
  char macro_name[FCC_MAX_IDENTIFIER_LENGTH + 1];
  const BYTE* bytes;
  size_t macro_begin_offset;
  size_t macro_end_offset;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  bytes = source_file->bytes;
  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);
  if ((cursor_offset >= line_end_offset) ||
      !fcc_preprocessor_is_identifier_start(bytes[cursor_offset])) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "expected macro name after '#undef'");
    return true;
  }

  macro_begin_offset = cursor_offset;
  macro_end_offset = macro_begin_offset;
  while ((macro_end_offset < line_end_offset) &&
         fcc_preprocessor_is_identifier_continue(bytes[macro_end_offset])) {
    ++macro_end_offset;
  }

  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, macro_end_offset, line_end_offset);
  if (cursor_offset != line_end_offset) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "unexpected tokens after macro name");
    return true;
  }

  if (!fcc_preprocessor_copy_substring(bytes, macro_begin_offset, macro_end_offset, macro_name,
                                       sizeof(macro_name))) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "macro name exceeds FCC_MAX_IDENTIFIER_LENGTH");
    return true;
  }

  fcc_preprocessor_undefine_macro(preprocessor, macro_name);
  return true;
}

static void fcc_preprocessor_expression_error(FccPreprocessorExpressionParser* parser,
                                              const char* message) {
  FccSourceSpan span;

  assert(parser != NULL);
  assert(message != NULL);

  if (parser->has_error) {
    return;
  }

  span.begin_offset = parser->cursor_offset;
  if (span.begin_offset < parser->line_end_offset) {
    span.end_offset = span.begin_offset + 1;
  } else {
    span.end_offset = parser->directive_span.end_offset;
  }

  fcc_preprocessor_emit(parser->preprocessor, parser->source_file, span, FCC_DIAG_SEVERITY_ERROR,
                        message);
  parser->has_error = true;
}

static void fcc_preprocessor_expression_skip_space(FccPreprocessorExpressionParser* parser) {
  assert(parser != NULL);

  parser->cursor_offset = fcc_preprocessor_skip_horizontal_space(
      parser->bytes, parser->cursor_offset, parser->line_end_offset);
}

static bool fcc_preprocessor_expression_match(FccPreprocessorExpressionParser* parser,
                                              const char* text) {
  size_t text_length;

  assert(parser != NULL);
  assert(text != NULL);

  fcc_preprocessor_expression_skip_space(parser);
  text_length = strlen(text);
  if ((parser->line_end_offset - parser->cursor_offset < text_length) ||
      (memcmp(parser->bytes + parser->cursor_offset, text, text_length) != 0)) {
    return false;
  }

  parser->cursor_offset += text_length;
  return true;
}

static bool fcc_preprocessor_parse_replacement_integer(const char* text, int64_t* value) {
  const char* cursor;
  uint64_t result;
  uint64_t signed_limit;
  unsigned int base;
  bool is_negative;
  bool has_digit;

  assert(text != NULL);
  assert(value != NULL);

  cursor = text;
  while ((*cursor == ' ') || (*cursor == '\t') || (*cursor == '\r') || (*cursor == '\n')) {
    ++cursor;
  }

  is_negative = false;
  if ((*cursor == '+') || (*cursor == '-')) {
    is_negative = *cursor == '-';
    ++cursor;
  }

  base = 10;
  has_digit = false;
  if (*cursor == '0') {
    has_digit = true;
    base = 8;
    ++cursor;
    if ((*cursor == 'x') || (*cursor == 'X')) {
      base = 16;
      has_digit = false;
      ++cursor;
    }
  }

  result = 0;
  for (;;) {
    unsigned int digit_value;

    if (!fcc_preprocessor_digit_value((BYTE)*cursor, &digit_value) || (digit_value >= base)) {
      break;
    }

    if (result > (UINT64_MAX - (uint64_t)digit_value) / (uint64_t)base) {
      return false;
    }

    result = (result * (uint64_t)base) + (uint64_t)digit_value;
    has_digit = true;
    ++cursor;
  }

  if (!has_digit) {
    return false;
  }

  while ((*cursor == 'u') || (*cursor == 'U') || (*cursor == 'l') || (*cursor == 'L')) {
    ++cursor;
  }

  while ((*cursor == ' ') || (*cursor == '\t') || (*cursor == '\r') || (*cursor == '\n')) {
    ++cursor;
  }

  if (*cursor != '\0') {
    return false;
  }

  signed_limit = is_negative ? ((uint64_t)INT64_MAX + 1ULL) : (uint64_t)INT64_MAX;
  if (result > signed_limit) {
    return false;
  }

  if (is_negative) {
    if (result == signed_limit) {
      *value = INT64_MIN;
    } else {
      *value = -(int64_t)result;
    }
  } else {
    *value = (int64_t)result;
  }

  return true;
}

static bool fcc_preprocessor_parse_pp_logical_or(FccPreprocessorExpressionParser* parser,
                                                 int64_t* value);

static bool fcc_preprocessor_parse_pp_number(FccPreprocessorExpressionParser* parser,
                                             int64_t* value) {
  uint64_t result;
  unsigned int base;
  bool has_digit;

  assert(parser != NULL);
  assert(value != NULL);

  base = 10;
  has_digit = false;
  if (parser->bytes[parser->cursor_offset] == (BYTE)'0') {
    has_digit = true;
    base = 8;
    ++parser->cursor_offset;
    if ((parser->cursor_offset < parser->line_end_offset) &&
        ((parser->bytes[parser->cursor_offset] == (BYTE)'x') ||
         (parser->bytes[parser->cursor_offset] == (BYTE)'X'))) {
      base = 16;
      has_digit = false;
      ++parser->cursor_offset;
    }
  }

  result = 0;
  for (;;) {
    unsigned int digit_value;

    if ((parser->cursor_offset >= parser->line_end_offset) ||
        !fcc_preprocessor_digit_value(parser->bytes[parser->cursor_offset], &digit_value) ||
        (digit_value >= base)) {
      break;
    }

    if (result > (UINT64_MAX - (uint64_t)digit_value) / (uint64_t)base) {
      fcc_preprocessor_expression_error(parser, "preprocessor integer literal is too large");
      return false;
    }

    result = (result * (uint64_t)base) + (uint64_t)digit_value;
    has_digit = true;
    ++parser->cursor_offset;
  }

  if (!has_digit) {
    fcc_preprocessor_expression_error(parser, "expected digit in preprocessor integer literal");
    return false;
  }

  if (result > (uint64_t)INT64_MAX) {
    fcc_preprocessor_expression_error(parser, "preprocessor integer literal is too large");
    return false;
  }

  while ((parser->cursor_offset < parser->line_end_offset) &&
         ((parser->bytes[parser->cursor_offset] == (BYTE)'u') ||
          (parser->bytes[parser->cursor_offset] == (BYTE)'U') ||
          (parser->bytes[parser->cursor_offset] == (BYTE)'l') ||
          (parser->bytes[parser->cursor_offset] == (BYTE)'L'))) {
    ++parser->cursor_offset;
  }

  *value = (int64_t)result;
  return true;
}

static bool fcc_preprocessor_parse_pp_identifier(FccPreprocessorExpressionParser* parser,
                                                 char* identifier, size_t identifier_size) {
  size_t begin_offset;
  size_t end_offset;

  assert(parser != NULL);
  assert(identifier != NULL);
  assert(identifier_size > 0);

  begin_offset = parser->cursor_offset;
  end_offset = begin_offset;
  while ((end_offset < parser->line_end_offset) &&
         fcc_preprocessor_is_identifier_continue(parser->bytes[end_offset])) {
    ++end_offset;
  }

  if (!fcc_preprocessor_copy_substring(parser->bytes, begin_offset, end_offset, identifier,
                                       identifier_size)) {
    fcc_preprocessor_expression_error(parser, "identifier exceeds FCC_MAX_IDENTIFIER_LENGTH");
    return false;
  }

  parser->cursor_offset = end_offset;
  return true;
}

static bool fcc_preprocessor_parse_pp_defined(FccPreprocessorExpressionParser* parser,
                                              int64_t* value) {
  char identifier[FCC_MAX_IDENTIFIER_LENGTH + 1];
  bool has_parentheses;

  assert(parser != NULL);
  assert(value != NULL);

  fcc_preprocessor_expression_skip_space(parser);
  has_parentheses = fcc_preprocessor_expression_match(parser, "(");
  fcc_preprocessor_expression_skip_space(parser);
  if ((parser->cursor_offset >= parser->line_end_offset) ||
      !fcc_preprocessor_is_identifier_start(parser->bytes[parser->cursor_offset])) {
    fcc_preprocessor_expression_error(parser, "expected macro name after 'defined'");
    return false;
  }

  if (!fcc_preprocessor_parse_pp_identifier(parser, identifier, sizeof(identifier))) {
    return false;
  }

  if (has_parentheses && !fcc_preprocessor_expression_match(parser, ")")) {
    fcc_preprocessor_expression_error(parser, "expected ')' after 'defined' macro name");
    return false;
  }

  *value = fcc_preprocessor_find_macro_by_name(parser->preprocessor, identifier) >= 0 ? 1 : 0;
  return true;
}

static bool fcc_preprocessor_parse_pp_primary(FccPreprocessorExpressionParser* parser,
                                              int64_t* value) {
  char identifier[FCC_MAX_IDENTIFIER_LENGTH + 1];

  assert(parser != NULL);
  assert(value != NULL);

  fcc_preprocessor_expression_skip_space(parser);
  if (parser->cursor_offset >= parser->line_end_offset) {
    fcc_preprocessor_expression_error(parser, "expected expression after preprocessor directive");
    return false;
  }

  if (fcc_preprocessor_expression_match(parser, "(")) {
    if (!fcc_preprocessor_parse_pp_logical_or(parser, value)) {
      return false;
    }

    if (!fcc_preprocessor_expression_match(parser, ")")) {
      fcc_preprocessor_expression_error(parser, "expected ')' in preprocessor expression");
      return false;
    }

    return true;
  }

  if (fcc_preprocessor_is_decimal_digit(parser->bytes[parser->cursor_offset])) {
    return fcc_preprocessor_parse_pp_number(parser, value);
  }

  if (fcc_preprocessor_is_identifier_start(parser->bytes[parser->cursor_offset])) {
    ptrdiff_t macro_index;

    if (!fcc_preprocessor_parse_pp_identifier(parser, identifier, sizeof(identifier))) {
      return false;
    }

    if (strcmp(identifier, "defined") == 0) {
      return fcc_preprocessor_parse_pp_defined(parser, value);
    }

    macro_index = fcc_preprocessor_find_macro_by_name(parser->preprocessor, identifier);
    if ((macro_index >= 0) && !parser->preprocessor->macros[macro_index].is_function_like) {
      const char* replacement;

      replacement = parser->preprocessor->macros[macro_index].replacement;
      if (fcc_preprocessor_parse_replacement_integer(replacement, value)) {
        return true;
      }
    }

    *value = 0;
    return true;
  }

  fcc_preprocessor_expression_error(parser, "expected preprocessor expression operand");
  return false;
}

static bool fcc_preprocessor_parse_pp_unary(FccPreprocessorExpressionParser* parser,
                                            int64_t* value) {
  assert(parser != NULL);
  assert(value != NULL);

  if (fcc_preprocessor_expression_match(parser, "+")) {
    return fcc_preprocessor_parse_pp_unary(parser, value);
  }

  if (fcc_preprocessor_expression_match(parser, "-")) {
    if (!fcc_preprocessor_parse_pp_unary(parser, value)) {
      return false;
    }

    if (*value == INT64_MIN) {
      fcc_preprocessor_expression_error(parser, "preprocessor integer negation overflows");
      return false;
    }

    *value = -*value;
    return true;
  }

  if (fcc_preprocessor_expression_match(parser, "!")) {
    if (!fcc_preprocessor_parse_pp_unary(parser, value)) {
      return false;
    }

    *value = *value == 0 ? 1 : 0;
    return true;
  }

  if (fcc_preprocessor_expression_match(parser, "~")) {
    if (!fcc_preprocessor_parse_pp_unary(parser, value)) {
      return false;
    }

    *value = ~*value;
    return true;
  }

  return fcc_preprocessor_parse_pp_primary(parser, value);
}

static bool fcc_preprocessor_parse_pp_multiplicative(FccPreprocessorExpressionParser* parser,
                                                     int64_t* value) {
  assert(parser != NULL);
  assert(value != NULL);

  if (!fcc_preprocessor_parse_pp_unary(parser, value)) {
    return false;
  }

  for (;;) {
    int64_t right_value;

    if (fcc_preprocessor_expression_match(parser, "*")) {
      if (!fcc_preprocessor_parse_pp_unary(parser, &right_value)) {
        return false;
      }

      *value *= right_value;
      continue;
    }

    if (fcc_preprocessor_expression_match(parser, "/")) {
      if (!fcc_preprocessor_parse_pp_unary(parser, &right_value)) {
        return false;
      }

      if (right_value == 0) {
        fcc_preprocessor_expression_error(parser, "division by zero in preprocessor expression");
        return false;
      }

      *value /= right_value;
      continue;
    }

    if (fcc_preprocessor_expression_match(parser, "%")) {
      if (!fcc_preprocessor_parse_pp_unary(parser, &right_value)) {
        return false;
      }

      if (right_value == 0) {
        fcc_preprocessor_expression_error(parser, "modulo by zero in preprocessor expression");
        return false;
      }

      *value %= right_value;
      continue;
    }

    return true;
  }
}

static bool fcc_preprocessor_parse_pp_additive(FccPreprocessorExpressionParser* parser,
                                               int64_t* value) {
  assert(parser != NULL);
  assert(value != NULL);

  if (!fcc_preprocessor_parse_pp_multiplicative(parser, value)) {
    return false;
  }

  for (;;) {
    int64_t right_value;

    if (fcc_preprocessor_expression_match(parser, "+")) {
      if (!fcc_preprocessor_parse_pp_multiplicative(parser, &right_value)) {
        return false;
      }

      *value += right_value;
      continue;
    }

    if (fcc_preprocessor_expression_match(parser, "-")) {
      if (!fcc_preprocessor_parse_pp_multiplicative(parser, &right_value)) {
        return false;
      }

      *value -= right_value;
      continue;
    }

    return true;
  }
}

static bool fcc_preprocessor_parse_pp_shift(FccPreprocessorExpressionParser* parser,
                                            int64_t* value) {
  assert(parser != NULL);
  assert(value != NULL);

  if (!fcc_preprocessor_parse_pp_additive(parser, value)) {
    return false;
  }

  for (;;) {
    int64_t right_value;
    bool is_left_shift;

    if (fcc_preprocessor_expression_match(parser, "<<")) {
      is_left_shift = true;
    } else if (fcc_preprocessor_expression_match(parser, ">>")) {
      is_left_shift = false;
    } else {
      return true;
    }

    if (!fcc_preprocessor_parse_pp_additive(parser, &right_value)) {
      return false;
    }

    if ((right_value < 0) || (right_value >= 63)) {
      fcc_preprocessor_expression_error(parser, "invalid shift count in preprocessor expression");
      return false;
    }

    if (is_left_shift) {
      *value = (int64_t)((uint64_t)*value << (unsigned int)right_value);
    } else {
      *value >>= (unsigned int)right_value;
    }
  }
}

static bool fcc_preprocessor_parse_pp_relational(FccPreprocessorExpressionParser* parser,
                                                 int64_t* value) {
  assert(parser != NULL);
  assert(value != NULL);

  if (!fcc_preprocessor_parse_pp_shift(parser, value)) {
    return false;
  }

  for (;;) {
    int64_t right_value;

    if (fcc_preprocessor_expression_match(parser, "<=")) {
      if (!fcc_preprocessor_parse_pp_shift(parser, &right_value)) {
        return false;
      }

      *value = *value <= right_value ? 1 : 0;
      continue;
    }

    if (fcc_preprocessor_expression_match(parser, ">=")) {
      if (!fcc_preprocessor_parse_pp_shift(parser, &right_value)) {
        return false;
      }

      *value = *value >= right_value ? 1 : 0;
      continue;
    }

    if (fcc_preprocessor_expression_match(parser, "<")) {
      if (!fcc_preprocessor_parse_pp_shift(parser, &right_value)) {
        return false;
      }

      *value = *value < right_value ? 1 : 0;
      continue;
    }

    if (fcc_preprocessor_expression_match(parser, ">")) {
      if (!fcc_preprocessor_parse_pp_shift(parser, &right_value)) {
        return false;
      }

      *value = *value > right_value ? 1 : 0;
      continue;
    }

    return true;
  }
}

static bool fcc_preprocessor_parse_pp_equality(FccPreprocessorExpressionParser* parser,
                                               int64_t* value) {
  assert(parser != NULL);
  assert(value != NULL);

  if (!fcc_preprocessor_parse_pp_relational(parser, value)) {
    return false;
  }

  for (;;) {
    int64_t right_value;

    if (fcc_preprocessor_expression_match(parser, "==")) {
      if (!fcc_preprocessor_parse_pp_relational(parser, &right_value)) {
        return false;
      }

      *value = *value == right_value ? 1 : 0;
      continue;
    }

    if (fcc_preprocessor_expression_match(parser, "!=")) {
      if (!fcc_preprocessor_parse_pp_relational(parser, &right_value)) {
        return false;
      }

      *value = *value != right_value ? 1 : 0;
      continue;
    }

    return true;
  }
}

static bool fcc_preprocessor_parse_pp_bitwise_and(FccPreprocessorExpressionParser* parser,
                                                  int64_t* value) {
  assert(parser != NULL);
  assert(value != NULL);

  if (!fcc_preprocessor_parse_pp_equality(parser, value)) {
    return false;
  }

  for (;;) {
    int64_t right_value;

    fcc_preprocessor_expression_skip_space(parser);
    if ((parser->cursor_offset >= parser->line_end_offset) ||
        (parser->bytes[parser->cursor_offset] != (BYTE)'&') ||
        (((parser->cursor_offset + 1) < parser->line_end_offset) &&
         (parser->bytes[parser->cursor_offset + 1] == (BYTE)'&'))) {
      return true;
    }

    ++parser->cursor_offset;
    if (!fcc_preprocessor_parse_pp_equality(parser, &right_value)) {
      return false;
    }

    *value &= right_value;
  }
}

static bool fcc_preprocessor_parse_pp_bitwise_xor(FccPreprocessorExpressionParser* parser,
                                                  int64_t* value) {
  assert(parser != NULL);
  assert(value != NULL);

  if (!fcc_preprocessor_parse_pp_bitwise_and(parser, value)) {
    return false;
  }

  for (;;) {
    int64_t right_value;

    if (!fcc_preprocessor_expression_match(parser, "^")) {
      return true;
    }

    if (!fcc_preprocessor_parse_pp_bitwise_and(parser, &right_value)) {
      return false;
    }

    *value ^= right_value;
  }
}

static bool fcc_preprocessor_parse_pp_bitwise_or(FccPreprocessorExpressionParser* parser,
                                                 int64_t* value) {
  assert(parser != NULL);
  assert(value != NULL);

  if (!fcc_preprocessor_parse_pp_bitwise_xor(parser, value)) {
    return false;
  }

  for (;;) {
    int64_t right_value;

    fcc_preprocessor_expression_skip_space(parser);
    if ((parser->cursor_offset >= parser->line_end_offset) ||
        (parser->bytes[parser->cursor_offset] != (BYTE)'|') ||
        (((parser->cursor_offset + 1) < parser->line_end_offset) &&
         (parser->bytes[parser->cursor_offset + 1] == (BYTE)'|'))) {
      return true;
    }

    ++parser->cursor_offset;
    if (!fcc_preprocessor_parse_pp_bitwise_xor(parser, &right_value)) {
      return false;
    }

    *value |= right_value;
  }
}

static bool fcc_preprocessor_parse_pp_logical_and(FccPreprocessorExpressionParser* parser,
                                                  int64_t* value) {
  assert(parser != NULL);
  assert(value != NULL);

  if (!fcc_preprocessor_parse_pp_bitwise_or(parser, value)) {
    return false;
  }

  for (;;) {
    int64_t right_value;

    if (!fcc_preprocessor_expression_match(parser, "&&")) {
      return true;
    }

    if (!fcc_preprocessor_parse_pp_bitwise_or(parser, &right_value)) {
      return false;
    }

    *value = ((*value != 0) && (right_value != 0)) ? 1 : 0;
  }
}

static bool fcc_preprocessor_parse_pp_logical_or(FccPreprocessorExpressionParser* parser,
                                                 int64_t* value) {
  assert(parser != NULL);
  assert(value != NULL);

  if (!fcc_preprocessor_parse_pp_logical_and(parser, value)) {
    return false;
  }

  for (;;) {
    int64_t right_value;

    if (!fcc_preprocessor_expression_match(parser, "||")) {
      return true;
    }

    if (!fcc_preprocessor_parse_pp_logical_and(parser, &right_value)) {
      return false;
    }

    *value = ((*value != 0) || (right_value != 0)) ? 1 : 0;
  }
}

static bool fcc_preprocessor_evaluate_if_expression(FccPreprocessor* preprocessor,
                                                    const FccSourceFile* source_file,
                                                    FccSourceSpan directive_span,
                                                    size_t cursor_offset, size_t line_end_offset,
                                                    bool* condition_is_true) {
  FccPreprocessorExpressionParser parser;
  int64_t expression_value;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(condition_is_true != NULL);

  parser.preprocessor = preprocessor;
  parser.source_file = source_file;
  parser.directive_span = directive_span;
  parser.bytes = source_file->bytes;
  parser.cursor_offset = cursor_offset;
  parser.line_end_offset = line_end_offset;
  parser.has_error = false;
  expression_value = 0;

  if (!fcc_preprocessor_parse_pp_logical_or(&parser, &expression_value)) {
    *condition_is_true = false;
    return true;
  }

  fcc_preprocessor_expression_skip_space(&parser);
  if (parser.cursor_offset != parser.line_end_offset) {
    fcc_preprocessor_expression_error(&parser, "unexpected tokens after preprocessor expression");
    *condition_is_true = false;
    return true;
  }

  *condition_is_true = expression_value != 0;
  return true;
}

static bool fcc_preprocessor_push_conditional(FccPreprocessor* preprocessor,
                                              const FccSourceFile* source_file,
                                              FccSourceSpan directive_span,
                                              bool condition_is_true) {
  FccPreprocessorConditional* conditional;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  if (preprocessor->conditional_depth >= FCC_MAX_PREPROCESS_CONDITIONAL_DEPTH) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "conditional nesting exceeds FCC_MAX_PREPROCESS_CONDITIONAL_DEPTH");
    return true;
  }

  conditional = &preprocessor->conditional_stack[preprocessor->conditional_depth];
  conditional->parent_active = fcc_preprocessor_current_branch_active(preprocessor);
  conditional->current_active = conditional->parent_active && condition_is_true;
  conditional->branch_taken = condition_is_true;
  conditional->saw_else = false;
  conditional->directive_span = directive_span;
  ++preprocessor->conditional_depth;
  return true;
}

static bool fcc_preprocessor_process_ifdef(FccPreprocessor* preprocessor,
                                           const FccSourceFile* source_file,
                                           FccSourceSpan directive_span, size_t cursor_offset,
                                           size_t line_end_offset, bool invert_condition) {
  char macro_name[FCC_MAX_IDENTIFIER_LENGTH + 1];
  const BYTE* bytes;
  size_t macro_begin_offset;
  size_t macro_end_offset;
  bool condition_is_true;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  bytes = source_file->bytes;
  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);
  if ((cursor_offset >= line_end_offset) ||
      !fcc_preprocessor_is_identifier_start(bytes[cursor_offset])) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          invert_condition ? "expected macro name after '#ifndef'"
                                           : "expected macro name after '#ifdef'");
    return true;
  }

  macro_begin_offset = cursor_offset;
  macro_end_offset = macro_begin_offset;
  while ((macro_end_offset < line_end_offset) &&
         fcc_preprocessor_is_identifier_continue(bytes[macro_end_offset])) {
    ++macro_end_offset;
  }

  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, macro_end_offset, line_end_offset);
  if (cursor_offset != line_end_offset) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "unexpected tokens after macro name");
    return true;
  }

  if (!fcc_preprocessor_copy_substring(bytes, macro_begin_offset, macro_end_offset, macro_name,
                                       sizeof(macro_name))) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "macro name exceeds FCC_MAX_IDENTIFIER_LENGTH");
    return true;
  }

  condition_is_true = fcc_preprocessor_find_macro_by_name(preprocessor, macro_name) >= 0;
  if (invert_condition) {
    condition_is_true = !condition_is_true;
  }

  return fcc_preprocessor_push_conditional(preprocessor, source_file, directive_span,
                                           condition_is_true);
}

static bool fcc_preprocessor_process_if(FccPreprocessor* preprocessor,
                                        const FccSourceFile* source_file,
                                        FccSourceSpan directive_span, size_t cursor_offset,
                                        size_t line_end_offset) {
  bool condition_is_true;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  condition_is_true = false;
  if (!fcc_preprocessor_evaluate_if_expression(preprocessor, source_file, directive_span,
                                               cursor_offset, line_end_offset,
                                               &condition_is_true)) {
    return false;
  }

  return fcc_preprocessor_push_conditional(preprocessor, source_file, directive_span,
                                           condition_is_true);
}

static bool fcc_preprocessor_process_elif(FccPreprocessor* preprocessor,
                                          const FccSourceFile* source_file,
                                          FccSourceSpan directive_span, size_t cursor_offset,
                                          size_t line_end_offset) {
  FccPreprocessorConditional* conditional;
  bool condition_is_true;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  if (preprocessor->conditional_depth == 0) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "'#elif' without a matching conditional");
    return true;
  }

  conditional = &preprocessor->conditional_stack[preprocessor->conditional_depth - 1];
  if (conditional->saw_else) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "'#elif' after '#else' in conditional block");
    return true;
  }

  condition_is_true = false;
  if (!fcc_preprocessor_evaluate_if_expression(preprocessor, source_file, directive_span,
                                               cursor_offset, line_end_offset,
                                               &condition_is_true)) {
    return false;
  }

  conditional->current_active =
      conditional->parent_active && !conditional->branch_taken && condition_is_true;
  if (condition_is_true) {
    conditional->branch_taken = true;
  }

  return true;
}

static bool fcc_preprocessor_process_else(FccPreprocessor* preprocessor,
                                          const FccSourceFile* source_file,
                                          FccSourceSpan directive_span, size_t cursor_offset,
                                          size_t line_end_offset) {
  FccPreprocessorConditional* conditional;
  const BYTE* bytes;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  bytes = source_file->bytes;
  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);
  if (cursor_offset != line_end_offset) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "unexpected tokens after '#else'");
    return true;
  }

  if (preprocessor->conditional_depth == 0) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "'#else' without a matching conditional");
    return true;
  }

  conditional = &preprocessor->conditional_stack[preprocessor->conditional_depth - 1];
  if (conditional->saw_else) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "duplicate '#else' in conditional block");
    return true;
  }

  conditional->current_active = conditional->parent_active && !conditional->branch_taken;
  conditional->branch_taken = true;
  conditional->saw_else = true;
  return true;
}

static bool fcc_preprocessor_process_endif(FccPreprocessor* preprocessor,
                                           const FccSourceFile* source_file,
                                           FccSourceSpan directive_span, size_t cursor_offset,
                                           size_t line_end_offset) {
  const BYTE* bytes;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  bytes = source_file->bytes;
  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);
  if (cursor_offset != line_end_offset) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "unexpected tokens after '#endif'");
    return true;
  }

  if (preprocessor->conditional_depth == 0) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "'#endif' without a matching conditional");
    return true;
  }

  --preprocessor->conditional_depth;
  return true;
}

static bool fcc_preprocessor_process_error(FccPreprocessor* preprocessor,
                                           const FccSourceFile* source_file,
                                           FccSourceSpan directive_span, size_t cursor_offset,
                                           size_t line_end_offset) {
  char message[FCC_MAX_DIAG_MESSAGE_LENGTH];
  const BYTE* bytes;
  size_t message_end_offset;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  bytes = source_file->bytes;
  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);
  message_end_offset = fcc_preprocessor_trim_trailing_space(bytes, cursor_offset, line_end_offset);
  if (cursor_offset == message_end_offset) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "#error");
    return true;
  }

  if (!fcc_preprocessor_copy_substring(bytes, cursor_offset, message_end_offset, message,
                                       sizeof(message))) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "#error message exceeds FCC_MAX_DIAG_MESSAGE_LENGTH");
    return true;
  }

  fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                        message);
  return true;
}

static size_t fcc_preprocessor_scan_literal(const BYTE* bytes, size_t cursor_offset,
                                            size_t line_end_offset) {
  BYTE quote;

  assert(bytes != NULL);
  assert(cursor_offset < line_end_offset);

  quote = bytes[cursor_offset];
  ++cursor_offset;
  while (cursor_offset < line_end_offset) {
    if (bytes[cursor_offset] == (BYTE)'\\') {
      ++cursor_offset;
      if (cursor_offset < line_end_offset) {
        ++cursor_offset;
      }

      continue;
    }

    if (bytes[cursor_offset] == quote) {
      ++cursor_offset;
      break;
    }

    ++cursor_offset;
  }

  return cursor_offset;
}

static bool fcc_preprocessor_append_macro_argument(FccPreprocessor* preprocessor,
                                                   const FccSourceFile* source_file,
                                                   FccSourceSpan invocation_span, const BYTE* bytes,
                                                   size_t begin_offset, size_t end_offset,
                                                   char*** arguments, size_t* argument_count,
                                                   size_t* argument_capacity) {
  char* argument;
  char** new_arguments;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(bytes != NULL);
  assert(begin_offset <= end_offset);
  assert(arguments != NULL);
  assert(argument_count != NULL);
  assert(argument_capacity != NULL);

  if (*argument_count >= FCC_MAX_FUNCTION_PARAMETERS) {
    fcc_preprocessor_emit(preprocessor, source_file, invocation_span, FCC_DIAG_SEVERITY_ERROR,
                          "macro argument count exceeds FCC_MAX_FUNCTION_PARAMETERS");
    return false;
  }

  if (*argument_count == *argument_capacity) {
    size_t new_capacity;

    new_capacity = (*argument_capacity == 0) ? 4 : (*argument_capacity * 2);
    if (new_capacity > FCC_MAX_FUNCTION_PARAMETERS) {
      new_capacity = FCC_MAX_FUNCTION_PARAMETERS;
    }

    new_arguments = (char**)realloc(*arguments, new_capacity * sizeof(char*));
    if (new_arguments == NULL) {
      fcc_preprocessor_emit(preprocessor, source_file, invocation_span, FCC_DIAG_SEVERITY_FATAL,
                            "out of memory");
      return false;
    }

    *arguments = new_arguments;
    *argument_capacity = new_capacity;
  }

  begin_offset = fcc_preprocessor_skip_horizontal_space(bytes, begin_offset, end_offset);
  end_offset = fcc_preprocessor_trim_trailing_space(bytes, begin_offset, end_offset);
  argument = fcc_preprocessor_duplicate_substring(bytes, begin_offset, end_offset);
  if (argument == NULL) {
    fcc_preprocessor_emit(preprocessor, source_file, invocation_span, FCC_DIAG_SEVERITY_FATAL,
                          "out of memory");
    return false;
  }

  (*arguments)[*argument_count] = argument;
  ++*argument_count;
  return true;
}

static bool fcc_preprocessor_join_variadic_macro_arguments(
    FccPreprocessor* preprocessor, const FccSourceFile* source_file, FccSourceSpan invocation_span,
    char** arguments, size_t first_variadic_argument_index, size_t argument_count,
    char** joined_argument_out) {
  char* joined_argument;
  size_t argument_index;
  size_t cursor_offset;
  size_t total_length;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(arguments != NULL || argument_count == 0);
  assert(first_variadic_argument_index <= argument_count);
  assert(joined_argument_out != NULL);

  total_length = 0;
  for (argument_index = first_variadic_argument_index; argument_index < argument_count;
       ++argument_index) {
    size_t argument_length;

    argument_length = strlen(arguments[argument_index]);
    if (argument_length > (SIZE_MAX - total_length)) {
      fcc_preprocessor_emit(preprocessor, source_file, invocation_span, FCC_DIAG_SEVERITY_FATAL,
                            "variadic macro argument list is too large");
      return false;
    }

    total_length += argument_length;
    if ((argument_index + 1) < argument_count) {
      if (total_length > (SIZE_MAX - 2)) {
        fcc_preprocessor_emit(preprocessor, source_file, invocation_span, FCC_DIAG_SEVERITY_FATAL,
                              "variadic macro argument list is too large");
        return false;
      }

      total_length += 2;
    }
  }

  if (total_length == SIZE_MAX) {
    fcc_preprocessor_emit(preprocessor, source_file, invocation_span, FCC_DIAG_SEVERITY_FATAL,
                          "variadic macro argument list is too large");
    return false;
  }

  joined_argument = (char*)malloc(total_length + 1);
  if (joined_argument == NULL) {
    fcc_preprocessor_emit(preprocessor, source_file, invocation_span, FCC_DIAG_SEVERITY_FATAL,
                          "out of memory");
    return false;
  }

  cursor_offset = 0;
  for (argument_index = first_variadic_argument_index; argument_index < argument_count;
       ++argument_index) {
    size_t argument_length;

    argument_length = strlen(arguments[argument_index]);
    if (argument_length != 0) {
      (void)memcpy(joined_argument + cursor_offset, arguments[argument_index], argument_length);
      cursor_offset += argument_length;
    }

    if ((argument_index + 1) < argument_count) {
      joined_argument[cursor_offset] = ',';
      joined_argument[cursor_offset + 1] = ' ';
      cursor_offset += 2;
    }
  }

  joined_argument[cursor_offset] = '\0';
  *joined_argument_out = joined_argument;
  return true;
}

static bool fcc_preprocessor_parse_macro_invocation_arguments(
    FccPreprocessor* preprocessor, const FccSourceFile* source_file,
    const FccPreprocessorMacro* macro, const BYTE* bytes, FccSourceSpan invocation_span,
    size_t open_paren_offset, size_t line_end_offset, size_t* invocation_end_offset,
    char*** arguments_out, size_t* argument_count_out) {
  char** arguments;
  size_t argument_begin_offset;
  size_t argument_capacity;
  size_t argument_count;
  size_t cursor_offset;
  size_t parenthesis_depth;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(macro != NULL);
  assert(bytes != NULL);
  assert(open_paren_offset < line_end_offset);
  assert(invocation_end_offset != NULL);
  assert(arguments_out != NULL);
  assert(argument_count_out != NULL);

  arguments = NULL;
  argument_capacity = 0;
  argument_count = 0;
  cursor_offset = open_paren_offset + 1;
  parenthesis_depth = 1;
  *arguments_out = NULL;
  *argument_count_out = 0;

  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);
  if ((cursor_offset < line_end_offset) && (bytes[cursor_offset] == (BYTE)')')) {
    if (macro->parameter_count == 1) {
      if (!fcc_preprocessor_append_macro_argument(preprocessor, source_file, invocation_span, bytes,
                                                  cursor_offset, cursor_offset, &arguments,
                                                  &argument_count, &argument_capacity)) {
        fcc_preprocessor_free_string_array(arguments, argument_count);
        return false;
      }
    }

    *invocation_end_offset = cursor_offset + 1;
    *arguments_out = arguments;
    *argument_count_out = argument_count;
    return true;
  }

  argument_begin_offset = cursor_offset;
  while (cursor_offset < line_end_offset) {
    if ((bytes[cursor_offset] == (BYTE)'"') || (bytes[cursor_offset] == (BYTE)'\'')) {
      cursor_offset = fcc_preprocessor_scan_literal(bytes, cursor_offset, line_end_offset);
      continue;
    }

    if ((bytes[cursor_offset] == (BYTE)'/') && ((cursor_offset + 1) < line_end_offset) &&
        (bytes[cursor_offset + 1] == (BYTE)'*')) {
      cursor_offset += 2;
      while (((cursor_offset + 1) < line_end_offset) &&
             !((bytes[cursor_offset] == (BYTE)'*') && (bytes[cursor_offset + 1] == (BYTE)'/'))) {
        ++cursor_offset;
      }

      if ((cursor_offset + 1) < line_end_offset) {
        cursor_offset += 2;
      }

      continue;
    }

    if (bytes[cursor_offset] == (BYTE)'(') {
      ++parenthesis_depth;
      ++cursor_offset;
      continue;
    }

    if (bytes[cursor_offset] == (BYTE)')') {
      --parenthesis_depth;
      if (parenthesis_depth == 0) {
        if (!fcc_preprocessor_append_macro_argument(
                preprocessor, source_file, invocation_span, bytes, argument_begin_offset,
                cursor_offset, &arguments, &argument_count, &argument_capacity)) {
          fcc_preprocessor_free_string_array(arguments, argument_count);
          return false;
        }

        *invocation_end_offset = cursor_offset + 1;
        *arguments_out = arguments;
        *argument_count_out = argument_count;
        return true;
      }

      ++cursor_offset;
      continue;
    }

    if ((bytes[cursor_offset] == (BYTE)',') && (parenthesis_depth == 1)) {
      if (!fcc_preprocessor_append_macro_argument(preprocessor, source_file, invocation_span, bytes,
                                                  argument_begin_offset, cursor_offset, &arguments,
                                                  &argument_count, &argument_capacity)) {
        fcc_preprocessor_free_string_array(arguments, argument_count);
        return false;
      }

      ++cursor_offset;
      argument_begin_offset = cursor_offset;
      continue;
    }

    ++cursor_offset;
  }

  fcc_preprocessor_free_string_array(arguments, argument_count);
  fcc_preprocessor_emit(preprocessor, source_file, invocation_span, FCC_DIAG_SEVERITY_ERROR,
                        "unterminated function-like macro invocation");
  return false;
}

static bool fcc_preprocessor_find_macro_parameter(const FccPreprocessorMacro* macro,
                                                  const BYTE* bytes, size_t begin_offset,
                                                  size_t end_offset, size_t* parameter_index_out) {
  size_t parameter_index;
  size_t parameter_length;
  size_t token_length;

  assert(macro != NULL);
  assert(bytes != NULL);
  assert(begin_offset <= end_offset);
  assert(parameter_index_out != NULL);

  token_length = end_offset - begin_offset;
  for (parameter_index = 0; parameter_index < macro->parameter_count; ++parameter_index) {
    parameter_length = strlen(macro->parameters[parameter_index]);
    if ((parameter_length == token_length) &&
        (memcmp(macro->parameters[parameter_index], bytes + begin_offset, token_length) == 0)) {
      *parameter_index_out = parameter_index;
      return true;
    }
  }

  return false;
}

static bool fcc_preprocessor_is_va_args_identifier(const FccPreprocessorMacro* macro,
                                                   const BYTE* bytes, size_t begin_offset,
                                                   size_t end_offset) {
  const char va_args_name[] = "__VA_ARGS__";
  size_t token_length;

  assert(macro != NULL);
  assert(bytes != NULL);
  assert(begin_offset <= end_offset);

  if (!macro->is_variadic) {
    return false;
  }

  token_length = end_offset - begin_offset;
  return (token_length == (sizeof(va_args_name) - 1)) &&
         (memcmp(bytes + begin_offset, va_args_name, token_length) == 0);
}

static bool fcc_preprocessor_write_stringized_argument(FccPreprocessor* preprocessor,
                                                       const FccSourceFile* source_file,
                                                       FccSourceSpan invocation_span,
                                                       FccPreprocessorBuffer* output_buffer,
                                                       const char* argument) {
  const BYTE* argument_bytes;
  size_t cursor_offset;
  size_t argument_length;
  bool pending_space;
  bool wrote_token_byte;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(output_buffer != NULL);
  assert(argument != NULL);

  argument_bytes = (const BYTE*)argument;
  argument_length = strlen(argument);
  pending_space = false;
  wrote_token_byte = false;

  if (!fcc_preprocessor_buffer_append_byte(preprocessor, source_file, invocation_span,
                                           output_buffer, (BYTE)'"')) {
    return false;
  }

  for (cursor_offset = 0; cursor_offset < argument_length; ++cursor_offset) {
    BYTE byte_value;

    byte_value = argument_bytes[cursor_offset];
    if (fcc_preprocessor_is_space(byte_value)) {
      pending_space = wrote_token_byte;
      continue;
    }

    if (pending_space) {
      if (!fcc_preprocessor_buffer_append_byte(preprocessor, source_file, invocation_span,
                                               output_buffer, (BYTE)' ')) {
        return false;
      }

      pending_space = false;
    }

    if ((byte_value == (BYTE)'"') || (byte_value == (BYTE)'\\')) {
      if (!fcc_preprocessor_buffer_append_byte(preprocessor, source_file, invocation_span,
                                               output_buffer, (BYTE)'\\')) {
        return false;
      }
    }

    if (!fcc_preprocessor_buffer_append_byte(preprocessor, source_file, invocation_span,
                                             output_buffer, byte_value)) {
      return false;
    }

    wrote_token_byte = true;
  }

  return fcc_preprocessor_buffer_append_byte(preprocessor, source_file, invocation_span,
                                             output_buffer, (BYTE)'"');
}

static bool fcc_preprocessor_build_function_macro_replacement(FccPreprocessor* preprocessor,
                                                              const FccSourceFile* source_file,
                                                              FccSourceSpan invocation_span,
                                                              const FccPreprocessorMacro* macro,
                                                              char** arguments,
                                                              const char* variadic_argument,
                                                              FccPreprocessorBuffer* output_buffer) {
  const BYTE* replacement_bytes;
  size_t cursor_offset;
  size_t replacement_length;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(macro != NULL);
  assert(arguments != NULL || macro->parameter_count == 0);
  assert(variadic_argument != NULL || !macro->is_variadic);
  assert(output_buffer != NULL);

  replacement_bytes = (const BYTE*)macro->replacement;
  replacement_length = strlen(macro->replacement);
  cursor_offset = 0;
  /*
   * Parameter names are replaced only when they appear as identifiers in the
   * replacement list. Literals are copied as-is so quoted text is not rewritten.
   */
  while (cursor_offset < replacement_length) {
    size_t token_end_offset;

    if (fcc_preprocessor_is_space(replacement_bytes[cursor_offset])) {
      size_t after_space_offset;

      after_space_offset =
          fcc_preprocessor_skip_horizontal_space(replacement_bytes, cursor_offset,
                                                 replacement_length);
      if (((after_space_offset + 1) < replacement_length) &&
          (replacement_bytes[after_space_offset] == (BYTE)'#') &&
          (replacement_bytes[after_space_offset + 1] == (BYTE)'#')) {
        cursor_offset = after_space_offset;
        continue;
      }
    }

    if ((replacement_bytes[cursor_offset] == (BYTE)'#') &&
        ((cursor_offset + 1) < replacement_length) &&
        (replacement_bytes[cursor_offset + 1] == (BYTE)'#')) {
      cursor_offset =
          fcc_preprocessor_skip_horizontal_space(replacement_bytes, cursor_offset + 2,
                                                 replacement_length);
      continue;
    }

    if (replacement_bytes[cursor_offset] == (BYTE)'#') {
      size_t stringized_token_offset;

      stringized_token_offset =
          fcc_preprocessor_skip_horizontal_space(replacement_bytes, cursor_offset + 1,
                                                 replacement_length);
      if ((stringized_token_offset < replacement_length) &&
          fcc_preprocessor_is_identifier_start(replacement_bytes[stringized_token_offset])) {
        size_t parameter_index;

        token_end_offset = stringized_token_offset + 1;
        while ((token_end_offset < replacement_length) &&
               fcc_preprocessor_is_identifier_continue(replacement_bytes[token_end_offset])) {
          ++token_end_offset;
        }

        if (fcc_preprocessor_find_macro_parameter(macro, replacement_bytes,
                                                  stringized_token_offset, token_end_offset,
                                                  &parameter_index)) {
          if (!fcc_preprocessor_write_stringized_argument(preprocessor, source_file,
                                                          invocation_span,
                                                          output_buffer,
                                                          arguments[parameter_index])) {
            return false;
          }

          cursor_offset = token_end_offset;
          continue;
        } else if (fcc_preprocessor_is_va_args_identifier(macro, replacement_bytes,
                                                          stringized_token_offset,
                                                          token_end_offset)) {
          if (!fcc_preprocessor_write_stringized_argument(preprocessor, source_file,
                                                          invocation_span,
                                                          output_buffer,
                                                          variadic_argument)) {
            return false;
          }

          cursor_offset = token_end_offset;
          continue;
        }
      }
    }

    if ((replacement_bytes[cursor_offset] == (BYTE)'"') ||
        (replacement_bytes[cursor_offset] == (BYTE)'\'')) {
      token_end_offset =
          fcc_preprocessor_scan_literal(replacement_bytes, cursor_offset, replacement_length);
      if (!fcc_preprocessor_buffer_append(preprocessor, source_file, invocation_span,
                                          output_buffer, replacement_bytes + cursor_offset,
                                          token_end_offset - cursor_offset)) {
        return false;
      }

      cursor_offset = token_end_offset;
      continue;
    }

    if (fcc_preprocessor_is_identifier_start(replacement_bytes[cursor_offset])) {
      size_t parameter_index;

      token_end_offset = cursor_offset + 1;
      while ((token_end_offset < replacement_length) &&
             fcc_preprocessor_is_identifier_continue(replacement_bytes[token_end_offset])) {
        ++token_end_offset;
      }

      if (fcc_preprocessor_find_macro_parameter(macro, replacement_bytes, cursor_offset,
                                                token_end_offset, &parameter_index)) {
        const char* argument;

        argument = arguments[parameter_index];
        if (!fcc_preprocessor_buffer_append(preprocessor, source_file, invocation_span,
                                            output_buffer, (const BYTE*)argument,
                                            strlen(argument))) {
          return false;
        }
      } else if (fcc_preprocessor_is_va_args_identifier(macro, replacement_bytes, cursor_offset,
                                                        token_end_offset)) {
        if (!fcc_preprocessor_buffer_append(preprocessor, source_file, invocation_span,
                                            output_buffer, (const BYTE*)variadic_argument,
                                            strlen(variadic_argument))) {
          return false;
        }
      } else if (!fcc_preprocessor_buffer_append(preprocessor, source_file, invocation_span,
                                                 output_buffer,
                                                 replacement_bytes + cursor_offset,
                                                 token_end_offset - cursor_offset)) {
        return false;
      }

      cursor_offset = token_end_offset;
      continue;
    }

    if (!fcc_preprocessor_buffer_append(preprocessor, source_file, invocation_span,
                                        output_buffer, replacement_bytes + cursor_offset, 1)) {
      return false;
    }

    ++cursor_offset;
  }

  return true;
}

static bool fcc_preprocessor_emit_expanded_line(FccPreprocessor* preprocessor,
                                                const FccSourceFile* source_file,
                                                size_t line_start_offset, size_t line_end_offset,
                                                size_t next_line_offset) {
  const BYTE* input_bytes;
  const FccPreprocessorMacro* disabled_macros[FCC_MAX_PREPROCESS_DEPTH];
  FccPreprocessorBuffer current_buffer;
  FccPreprocessorBuffer next_buffer;
  FccSourceSpan line_span;
  bool input_source_offsets_are_valid;
  bool wrote_expanded_line;
  size_t input_begin_offset;
  size_t input_end_offset;
  size_t rescan_depth;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(line_start_offset <= line_end_offset);
  assert(line_end_offset <= next_line_offset);

  line_span.begin_offset = line_start_offset;
  line_span.end_offset = line_end_offset;
  fcc_preprocessor_buffer_init(&current_buffer);
  fcc_preprocessor_buffer_init(&next_buffer);
  input_bytes = source_file->bytes;
  input_begin_offset = line_start_offset;
  input_end_offset = line_end_offset;
  input_source_offsets_are_valid = true;
  wrote_expanded_line = false;

  for (rescan_depth = 0; rescan_depth < FCC_MAX_PREPROCESS_DEPTH; ++rescan_depth) {
    FccPreprocessorBuffer swap_buffer;
    bool expanded_any;

    expanded_any = false;
    fcc_preprocessor_buffer_clear(&next_buffer);
    if (!fcc_preprocessor_expand_text_once(
            preprocessor, source_file, input_bytes,
            input_source_offsets_are_valid ? NULL : &current_buffer, input_begin_offset,
            input_end_offset, line_span, input_source_offsets_are_valid, disabled_macros, 0,
            &next_buffer, &expanded_any)) {
      fcc_preprocessor_buffer_dispose(&current_buffer);
      fcc_preprocessor_buffer_dispose(&next_buffer);
      return false;
    }

    if (!expanded_any ||
        fcc_preprocessor_buffer_equals_range(&next_buffer, input_bytes, input_begin_offset,
                                             input_end_offset)) {
      if (!fcc_preprocessor_write_bytes(preprocessor, source_file, line_span, next_buffer.bytes,
                                        next_buffer.length)) {
        fcc_preprocessor_buffer_dispose(&current_buffer);
        fcc_preprocessor_buffer_dispose(&next_buffer);
        return false;
      }

      wrote_expanded_line = true;
      break;
    }

    swap_buffer = current_buffer;
    current_buffer = next_buffer;
    next_buffer = swap_buffer;
    input_bytes = current_buffer.bytes;
    input_begin_offset = 0;
    input_end_offset = current_buffer.length;
    input_source_offsets_are_valid = false;
  }

  if (!wrote_expanded_line) {
    fcc_preprocessor_emit(preprocessor, source_file, line_span, FCC_DIAG_SEVERITY_FATAL,
                          "preprocessor macro expansion nesting exceeds FCC_MAX_PREPROCESS_DEPTH");
    fcc_preprocessor_buffer_dispose(&current_buffer);
    fcc_preprocessor_buffer_dispose(&next_buffer);
    return false;
  }

  if (next_line_offset > line_end_offset) {
    if (!fcc_preprocessor_write_source_range(preprocessor, source_file, line_end_offset,
                                             next_line_offset)) {
      fcc_preprocessor_buffer_dispose(&current_buffer);
      fcc_preprocessor_buffer_dispose(&next_buffer);
      return false;
    }
  }

  fcc_preprocessor_buffer_dispose(&current_buffer);
  fcc_preprocessor_buffer_dispose(&next_buffer);
  return true;
}

static bool fcc_preprocessor_process_directive(FccPreprocessor* preprocessor,
                                               const FccSourceFile* source_file,
                                               size_t line_start_offset, size_t line_end_offset) {
  const BYTE* bytes;
  char directive_name[32];
  size_t cursor_offset;
  size_t directive_begin_offset;
  size_t directive_end_offset;
  FccSourceSpan directive_span;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(line_start_offset <= line_end_offset);

  bytes = source_file->bytes;
  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, line_start_offset, line_end_offset);
  assert((cursor_offset < line_end_offset) && (bytes[cursor_offset] == (BYTE)'#'));
  ++cursor_offset;
  cursor_offset = fcc_preprocessor_skip_horizontal_space(bytes, cursor_offset, line_end_offset);

  directive_begin_offset = cursor_offset;
  while ((cursor_offset < line_end_offset) &&
         fcc_preprocessor_is_identifier_continue(bytes[cursor_offset])) {
    ++cursor_offset;
  }

  directive_end_offset = cursor_offset;
  directive_span.begin_offset = line_start_offset;
  directive_span.end_offset = line_end_offset;
  if ((directive_begin_offset == directive_end_offset) ||
      !fcc_preprocessor_copy_substring(bytes, directive_begin_offset, directive_end_offset,
                                       directive_name, sizeof(directive_name))) {
    fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                          "expected preprocessor directive name after '#'");
    return true;
  }

  if (strcmp(directive_name, "if") == 0) {
    return fcc_preprocessor_process_if(preprocessor, source_file, directive_span, cursor_offset,
                                       line_end_offset);
  }

  if (strcmp(directive_name, "ifdef") == 0) {
    return fcc_preprocessor_process_ifdef(preprocessor, source_file, directive_span, cursor_offset,
                                          line_end_offset, false);
  }

  if (strcmp(directive_name, "ifndef") == 0) {
    return fcc_preprocessor_process_ifdef(preprocessor, source_file, directive_span, cursor_offset,
                                          line_end_offset, true);
  }

  if (strcmp(directive_name, "else") == 0) {
    return fcc_preprocessor_process_else(preprocessor, source_file, directive_span, cursor_offset,
                                         line_end_offset);
  }

  if (strcmp(directive_name, "elif") == 0) {
    return fcc_preprocessor_process_elif(preprocessor, source_file, directive_span, cursor_offset,
                                         line_end_offset);
  }

  if (strcmp(directive_name, "endif") == 0) {
    return fcc_preprocessor_process_endif(preprocessor, source_file, directive_span, cursor_offset,
                                          line_end_offset);
  }

  if (!fcc_preprocessor_current_branch_active(preprocessor)) {
    return true;
  }

  if (strcmp(directive_name, "include") == 0) {
    return fcc_preprocessor_process_include(preprocessor, source_file, directive_span,
                                            cursor_offset, line_end_offset);
  }

  if (strcmp(directive_name, "pragma") == 0) {
    return fcc_preprocessor_process_pragma_once(preprocessor, source_file, directive_span,
                                                cursor_offset, line_end_offset);
  }

  if (strcmp(directive_name, "define") == 0) {
    return fcc_preprocessor_process_define(preprocessor, source_file, directive_span, cursor_offset,
                                           line_end_offset);
  }

  if (strcmp(directive_name, "undef") == 0) {
    return fcc_preprocessor_process_undef(preprocessor, source_file, directive_span, cursor_offset,
                                          line_end_offset);
  }

  if (strcmp(directive_name, "error") == 0) {
    return fcc_preprocessor_process_error(preprocessor, source_file, directive_span, cursor_offset,
                                          line_end_offset);
  }

  {
    char message[FCC_MAX_DIAG_MESSAGE_LENGTH];
    int written;

    written = snprintf(message, sizeof(message),
                       "unsupported preprocessor directive '%s' in this phase", directive_name);
    if ((written < 0) || ((size_t)written >= sizeof(message))) {
      fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                            "preprocessor diagnostic formatting failure");
    } else {
      fcc_preprocessor_emit(preprocessor, source_file, directive_span, FCC_DIAG_SEVERITY_ERROR,
                            message);
    }
  }

  return true;
}

static bool fcc_preprocessor_make_spliced_source(FccPreprocessor* preprocessor,
                                                 const FccSourceFile* source_file,
                                                 FccSourceFile* spliced_source_file,
                                                 bool* did_splice) {
  BYTE* spliced_bytes;
  size_t read_offset;
  size_t write_offset;

  assert(preprocessor != NULL);
  assert(source_file != NULL);
  assert(spliced_source_file != NULL);
  assert(did_splice != NULL);

  *did_splice = false;
  memset(spliced_source_file, 0, sizeof(*spliced_source_file));
  spliced_bytes = (BYTE*)malloc(source_file->byte_count + 1);
  if (spliced_bytes == NULL) {
    FccSourceSpan span;

    span.begin_offset = 0;
    span.end_offset = 0;
    fcc_preprocessor_emit(preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                          "out of memory");
    return false;
  }

  read_offset = 0;
  write_offset = 0;
  while (read_offset < source_file->byte_count) {
    if ((source_file->bytes[read_offset] == (BYTE)'\\') &&
        ((read_offset + 1) < source_file->byte_count)) {
      if (source_file->bytes[read_offset + 1] == (BYTE)'\n') {
        read_offset += 2;
        *did_splice = true;
        continue;
      }

      if (((read_offset + 2) < source_file->byte_count) &&
          (source_file->bytes[read_offset + 1] == (BYTE)'\r') &&
          (source_file->bytes[read_offset + 2] == (BYTE)'\n')) {
        read_offset += 3;
        *did_splice = true;
        continue;
      }
    }

    spliced_bytes[write_offset] = source_file->bytes[read_offset];
    ++write_offset;
    ++read_offset;
  }

  spliced_bytes[write_offset] = 0;
  if (!*did_splice) {
    free(spliced_bytes);
    return true;
  }

  {
    char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];

    error_buffer[0] = '\0';
    if (!fcc_source_file_init_take_bytes(spliced_source_file, source_file->path, spliced_bytes,
                                         write_offset, error_buffer, sizeof(error_buffer))) {
      FccSourceSpan span;

      span.begin_offset = 0;
      span.end_offset = 0;
      fcc_preprocessor_emit(preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                            error_buffer[0] != '\0' ? error_buffer
                                                    : "failed to splice source file");
      return false;
    }
  }

  return true;
}

static bool fcc_preprocessor_emit_file(FccPreprocessor* preprocessor,
                                       const FccSourceFile* source_file) {
  FccSourceFile spliced_source_file;
  bool did_splice;
  bool ok;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  did_splice = false;
  if (!fcc_preprocessor_make_spliced_source(preprocessor, source_file, &spliced_source_file,
                                            &did_splice)) {
    return false;
  }

  if (!did_splice) {
    return fcc_preprocessor_emit_file_contents(preprocessor, source_file);
  }

  ok = fcc_preprocessor_emit_file_contents(preprocessor, &spliced_source_file);
  fcc_source_file_dispose(&spliced_source_file);
  return ok;
}

static bool fcc_preprocessor_emit_file_contents(FccPreprocessor* preprocessor,
                                                const FccSourceFile* source_file) {
  size_t line_start_offset;

  assert(preprocessor != NULL);
  assert(source_file != NULL);

  ++preprocessor->recursion_depth;
  if (preprocessor->recursion_depth > FCC_MAX_PREPROCESS_DEPTH) {
    FccSourceSpan span;

    --preprocessor->recursion_depth;
    span.begin_offset = 0;
    span.end_offset = 0;
    fcc_preprocessor_emit(preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                          "preprocessor include nesting exceeds FCC_MAX_PREPROCESS_DEPTH");
    return false;
  }

  line_start_offset = 0;
  while (line_start_offset < source_file->byte_count) {
    size_t cursor_offset;
    size_t line_end_offset;
    size_t next_line_offset;

    line_end_offset = line_start_offset;
    while ((line_end_offset < source_file->byte_count) &&
           (source_file->bytes[line_end_offset] != (BYTE)'\n')) {
      ++line_end_offset;
    }

    next_line_offset = line_end_offset;
    if ((next_line_offset < source_file->byte_count) &&
        (source_file->bytes[next_line_offset] == (BYTE)'\n')) {
      ++next_line_offset;
    }

    cursor_offset = fcc_preprocessor_skip_horizontal_space(source_file->bytes, line_start_offset,
                                                           line_end_offset);
    if ((cursor_offset < line_end_offset) && (source_file->bytes[cursor_offset] == (BYTE)'#')) {
      if (!fcc_preprocessor_process_directive(preprocessor, source_file, line_start_offset,
                                              line_end_offset)) {
        --preprocessor->recursion_depth;
        return false;
      }
    } else if (fcc_preprocessor_current_branch_active(preprocessor)) {
      if (!fcc_preprocessor_emit_expanded_line(preprocessor, source_file, line_start_offset,
                                               line_end_offset, next_line_offset)) {
        --preprocessor->recursion_depth;
        return false;
      }
    }

    line_start_offset = next_line_offset;
  }

  --preprocessor->recursion_depth;
  return true;
}

bool fcc_preprocessor_run(const FccSourceFile* source_file, const FccPreprocessorOptions* options,
                          FccDiagnostics* diagnostics, FILE* output_stream) {
  FccPreprocessor preprocessor;
  FccPreprocessorOptions default_options;
  size_t path_index;
  size_t starting_error_count;
  bool ok;

  assert(source_file != NULL);
  assert(diagnostics != NULL);
  assert(output_stream != NULL);

  default_options.include_directories = NULL;
  default_options.include_directory_count = 0;
  default_options.sysroot_directory = NULL;
  if (options == NULL) {
    options = &default_options;
  }

  preprocessor.options = options;
  preprocessor.diagnostics = diagnostics;
  preprocessor.output_stream = output_stream;
  preprocessor.pragma_once_paths = NULL;
  preprocessor.pragma_once_path_count = 0;
  preprocessor.pragma_once_path_capacity = 0;
  preprocessor.macros = NULL;
  preprocessor.macro_count = 0;
  preprocessor.macro_capacity = 0;
  preprocessor.conditional_depth = 0;
  preprocessor.recursion_depth = 0;
  starting_error_count = diagnostics->error_count;

  if (!fcc_preprocessor_initialize_builtin_macros(&preprocessor)) {
    FccSourceSpan span;

    span.begin_offset = 0;
    span.end_offset = 0;
    fcc_preprocessor_emit(&preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                          "out of memory");
    fcc_preprocessor_dispose_macros(&preprocessor);
    return false;
  }

  ok = fcc_preprocessor_emit_file(&preprocessor, source_file);
  if (ok && (preprocessor.conditional_depth != 0)) {
    FccPreprocessorConditional* conditional;

    conditional = &preprocessor.conditional_stack[preprocessor.conditional_depth - 1];
    fcc_preprocessor_emit(&preprocessor, source_file, conditional->directive_span,
                          FCC_DIAG_SEVERITY_ERROR,
                          "unterminated conditional directive block at end of file");
    ok = false;
  }

  for (path_index = 0; path_index < preprocessor.pragma_once_path_count; ++path_index) {
    free(preprocessor.pragma_once_paths[path_index]);
  }

  free(preprocessor.pragma_once_paths);
  fcc_preprocessor_dispose_macros(&preprocessor);
  if (!ok) {
    return false;
  }

  if (ferror(output_stream) != 0) {
    FccSourceSpan span;

    span.begin_offset = 0;
    span.end_offset = 0;
    fcc_preprocessor_emit(&preprocessor, source_file, span, FCC_DIAG_SEVERITY_FATAL,
                          "preprocessor output write failed");
    return false;
  }

  return diagnostics->error_count == starting_error_count;
}

bool fcc_preprocessor_run_to_source(const FccSourceFile* source_file,
                                    const FccPreprocessorOptions* options,
                                    FccDiagnostics* diagnostics,
                                    FccSourceFile* preprocessed_source_file, char* error_buffer,
                                    size_t error_buffer_size) {
  FILE* output_stream;
  BYTE* bytes;
  long file_size_long;
  size_t file_size;
  bool ok;

  assert(source_file != NULL);
  assert(diagnostics != NULL);
  assert(preprocessed_source_file != NULL);

  output_stream = NULL;
  bytes = NULL;
  fcc_source_file_dispose(preprocessed_source_file);
  if ((error_buffer != NULL) && (error_buffer_size != 0)) {
    error_buffer[0] = '\0';
  }

  output_stream = tmpfile();
  if (output_stream == NULL) {
    if ((error_buffer != NULL) && (error_buffer_size != 0)) {
      (void)snprintf(error_buffer, error_buffer_size, "%s",
                     "failed to create temporary preprocessor stream");
    }

    fcc_preprocessor_emit_fatal_message(diagnostics, source_file,
                                        "failed to create temporary preprocessor stream");
    return false;
  }

  ok = fcc_preprocessor_run(source_file, options, diagnostics, output_stream);
  if (!ok) {
    (void)fclose(output_stream);
    return false;
  }

  if (fflush(output_stream) != 0) {
    if ((error_buffer != NULL) && (error_buffer_size != 0)) {
      (void)snprintf(error_buffer, error_buffer_size, "%s", "failed to flush preprocessor output");
    }

    fcc_preprocessor_emit_fatal_message(diagnostics, source_file,
                                        "failed to flush preprocessor output");
    (void)fclose(output_stream);
    return false;
  }

  if (fseek(output_stream, 0, SEEK_END) != 0) {
    if ((error_buffer != NULL) && (error_buffer_size != 0)) {
      (void)snprintf(error_buffer, error_buffer_size, "%s",
                     "failed to determine preprocessor output size");
    }

    fcc_preprocessor_emit_fatal_message(diagnostics, source_file,
                                        "failed to determine preprocessor output size");
    (void)fclose(output_stream);
    return false;
  }

  file_size_long = ftell(output_stream);
  if (file_size_long < 0) {
    if ((error_buffer != NULL) && (error_buffer_size != 0)) {
      (void)snprintf(error_buffer, error_buffer_size, "%s",
                     "failed to determine preprocessor output size");
    }

    fcc_preprocessor_emit_fatal_message(diagnostics, source_file,
                                        "failed to determine preprocessor output size");
    (void)fclose(output_stream);
    return false;
  }

  file_size = (size_t)file_size_long;
  if (fseek(output_stream, 0, SEEK_SET) != 0) {
    if ((error_buffer != NULL) && (error_buffer_size != 0)) {
      (void)snprintf(error_buffer, error_buffer_size, "%s", "failed to rewind preprocessor output");
    }

    fcc_preprocessor_emit_fatal_message(diagnostics, source_file,
                                        "failed to rewind preprocessor output");
    (void)fclose(output_stream);
    return false;
  }

  bytes = (BYTE*)malloc(file_size + 1);
  if (bytes == NULL) {
    if ((error_buffer != NULL) && (error_buffer_size != 0)) {
      (void)snprintf(error_buffer, error_buffer_size, "%s", "out of memory");
    }

    fcc_preprocessor_emit_fatal_message(diagnostics, source_file, "out of memory");
    (void)fclose(output_stream);
    return false;
  }

  if ((file_size != 0) && (fread(bytes, 1, file_size, output_stream) != file_size)) {
    if ((error_buffer != NULL) && (error_buffer_size != 0)) {
      (void)snprintf(error_buffer, error_buffer_size, "%s", "failed to read preprocessor output");
    }

    fcc_preprocessor_emit_fatal_message(diagnostics, source_file,
                                        "failed to read preprocessor output");
    free(bytes);
    (void)fclose(output_stream);
    return false;
  }

  bytes[file_size] = 0;
  if (fclose(output_stream) != 0) {
    if ((error_buffer != NULL) && (error_buffer_size != 0)) {
      (void)snprintf(error_buffer, error_buffer_size, "%s", "failed to close preprocessor output");
    }

    fcc_preprocessor_emit_fatal_message(diagnostics, source_file,
                                        "failed to close preprocessor output");
    free(bytes);
    return false;
  }

  if (!fcc_source_file_init_take_bytes(preprocessed_source_file, source_file->path, bytes,
                                       file_size, error_buffer, error_buffer_size)) {
    fcc_preprocessor_emit_fatal_message(diagnostics, source_file,
                                        ((error_buffer != NULL) && (error_buffer[0] != '\0'))
                                            ? error_buffer
                                            : "failed to create preprocessed source");
    return false;
  }

  return true;
}
