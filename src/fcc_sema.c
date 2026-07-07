// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/sema.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fcc/base.h"

/*
 * Sema is the phase boundary between syntax and backend-ready facts. It resolves
 * symbols, tag scopes, type IDs, lvalue-ness, integer constants, and control-flow
 * context while leaving AST ownership with FccAstContext.
 */
typedef struct FccSema {
  const FccSourceFile* source_file;
  FccDiagnostics* diagnostics;
  FccSemaResult* result;
  FccSymbolTable symbols;
  struct FccSemaTag* tags;
  size_t tag_count;
  size_t tag_capacity;
  size_t* tag_scope_offsets;
  size_t tag_scope_count;
  size_t tag_scope_capacity;
  FccTypeId current_function_return_type_id;
  size_t loop_depth;
  size_t switch_depth;
  size_t recursion_depth;
} FccSema;

static bool fcc_sema_check_static_assert(FccSema* sema,
                                         const FccAstStaticAssert* static_assertion);

/*
 * Tags have a namespace separate from ordinary identifiers in C. Keeping tag
 * scopes here, rather than in the parser, lets incomplete and completed record
 * types be validated with semantic type information.
 */
typedef struct FccSemaTag {
  const char* name;
  FccTypeKind kind;
  FccTypeId type_id;
  size_t scope_depth;
} FccSemaTag;

typedef struct FccSemaExpressionResult {
  FccTypeId type_id;
  bool is_lvalue;
  bool is_valid;
  bool has_integer_constant;
  int64_t integer_constant_value;
  bool has_address_constant;
  const char* address_constant_symbol_name;
} FccSemaExpressionResult;

static bool fcc_sema_ensure_tag_capacity(FccSema* sema, size_t capacity) {
  size_t new_capacity;
  FccSemaTag* new_tags;

  assert(sema != NULL);

  if (capacity <= sema->tag_capacity) {
    return true;
  }

  new_capacity = sema->tag_capacity;
  if (new_capacity == 0) {
    new_capacity = 16;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(FccSemaTag))) {
    return false;
  }

  new_tags = (FccSemaTag*)realloc(sema->tags, new_capacity * sizeof(FccSemaTag));
  if (new_tags == NULL) {
    return false;
  }

  sema->tags = new_tags;
  sema->tag_capacity = new_capacity;
  return true;
}

static bool fcc_sema_ensure_tag_scope_capacity(FccSema* sema, size_t capacity) {
  size_t new_capacity;
  size_t* new_offsets;

  assert(sema != NULL);

  if (capacity <= sema->tag_scope_capacity) {
    return true;
  }

  new_capacity = sema->tag_scope_capacity;
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

  new_offsets = (size_t*)realloc(sema->tag_scope_offsets, new_capacity * sizeof(size_t));
  if (new_offsets == NULL) {
    return false;
  }

  sema->tag_scope_offsets = new_offsets;
  sema->tag_scope_capacity = new_capacity;
  return true;
}

static bool fcc_sema_push_tag_scope(FccSema* sema) {
  assert(sema != NULL);

  if (!fcc_sema_ensure_tag_scope_capacity(sema, sema->tag_scope_count + 1)) {
    return false;
  }

  sema->tag_scope_offsets[sema->tag_scope_count] = sema->tag_count;
  ++sema->tag_scope_count;
  return true;
}

static void fcc_sema_pop_tag_scope(FccSema* sema) {
  assert(sema != NULL);
  assert(sema->tag_scope_count > 0);

  sema->tag_count = sema->tag_scope_offsets[sema->tag_scope_count - 1];
  --sema->tag_scope_count;
}

static const FccSemaTag* fcc_sema_lookup_tag(const FccSema* sema, const char* name) {
  size_t tag_index;

  assert(sema != NULL);
  assert(name != NULL);

  for (tag_index = sema->tag_count; tag_index > 0; --tag_index) {
    const FccSemaTag* tag;

    tag = &sema->tags[tag_index - 1];
    if ((tag->name == name) || ((tag->name != NULL) && (strcmp(tag->name, name) == 0))) {
      return tag;
    }
  }

  return NULL;
}

static const FccSemaTag* fcc_sema_lookup_tag_current_scope(const FccSema* sema, const char* name) {
  size_t scope_begin;
  size_t tag_index;

  assert(sema != NULL);
  assert(name != NULL);

  if (sema->tag_scope_count == 0) {
    return NULL;
  }

  scope_begin = sema->tag_scope_offsets[sema->tag_scope_count - 1];
  for (tag_index = sema->tag_count; tag_index > scope_begin; --tag_index) {
    const FccSemaTag* tag;

    tag = &sema->tags[tag_index - 1];
    if ((tag->name == name) || ((tag->name != NULL) && (strcmp(tag->name, name) == 0))) {
      return tag;
    }
  }

  return NULL;
}

static bool fcc_sema_define_tag(FccSema* sema, const char* name, FccTypeKind kind,
                                FccTypeId type_id) {
  FccSemaTag* tag;

  assert(sema != NULL);
  assert(name != NULL);

  if (!fcc_sema_ensure_tag_capacity(sema, sema->tag_count + 1)) {
    return false;
  }

  tag = &sema->tags[sema->tag_count];
  tag->name = name;
  tag->kind = kind;
  tag->type_id = type_id;
  tag->scope_depth = fcc_symbol_table_scope_depth(&sema->symbols);
  ++sema->tag_count;
  return true;
}

static bool fcc_sema_result_ensure_expression_capacity(FccSemaResult* result, size_t capacity) {
  size_t new_capacity;
  FccSemaExpressionInfo* new_infos;

  assert(result != NULL);

  if (capacity <= result->expression_capacity) {
    return true;
  }

  new_capacity = result->expression_capacity;
  if (new_capacity == 0) {
    new_capacity = 32;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(FccSemaExpressionInfo))) {
    return false;
  }

  new_infos = (FccSemaExpressionInfo*)realloc(result->expression_infos,
                                              new_capacity * sizeof(FccSemaExpressionInfo));
  if (new_infos == NULL) {
    return false;
  }

  result->expression_infos = new_infos;
  result->expression_capacity = new_capacity;
  return true;
}

static bool fcc_sema_result_ensure_object_capacity(FccSemaResult* result, size_t capacity) {
  size_t new_capacity;
  FccSemaObjectInfo* new_infos;

  assert(result != NULL);

  if (capacity <= result->object_capacity) {
    return true;
  }

  new_capacity = result->object_capacity;
  if (new_capacity == 0) {
    new_capacity = 32;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(FccSemaObjectInfo))) {
    return false;
  }

  new_infos =
      (FccSemaObjectInfo*)realloc(result->object_infos, new_capacity * sizeof(FccSemaObjectInfo));
  if (new_infos == NULL) {
    return false;
  }

  result->object_infos = new_infos;
  result->object_capacity = new_capacity;
  return true;
}

static bool fcc_sema_result_append_expression_info(FccSemaResult* result,
                                                   const FccAstExpression* expression,
                                                   FccTypeId type_id, bool is_lvalue, bool is_valid,
                                                   bool has_integer_constant,
                                                   int64_t integer_constant_value,
                                                   bool has_address_constant,
                                                   const char* address_constant_symbol_name) {
  FccSemaExpressionInfo* info;

  assert(result != NULL);
  assert(expression != NULL);

  if (!fcc_sema_result_ensure_expression_capacity(result, result->expression_count + 1)) {
    return false;
  }

  info = &result->expression_infos[result->expression_count];
  info->expression = expression;
  info->type_id = type_id;
  info->is_lvalue = is_lvalue;
  info->is_valid = is_valid;
  info->has_integer_constant = has_integer_constant;
  info->integer_constant_value = integer_constant_value;
  info->has_address_constant = has_address_constant;
  info->address_constant_symbol_name = address_constant_symbol_name;
  ++result->expression_count;
  return true;
}

static bool fcc_sema_result_append_object_info(FccSemaResult* result, const void* node,
                                               const char* name, FccSymbolKind symbol_kind,
                                               FccStorageClass storage_class, FccTypeId type_id,
                                               bool has_body) {
  FccSemaObjectInfo* info;

  assert(result != NULL);
  assert(node != NULL);

  if (!fcc_sema_result_ensure_object_capacity(result, result->object_count + 1)) {
    return false;
  }

  info = &result->object_infos[result->object_count];
  info->node = node;
  info->name = name;
  info->symbol_kind = symbol_kind;
  info->storage_class = storage_class;
  info->type_id = type_id;
  info->has_body = has_body;
  ++result->object_count;
  return true;
}

static FccTypeId fcc_sema_int_type(FccSema* sema) {
  assert(sema != NULL);

  return fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_INT, false);
}

static FccTypeId fcc_sema_void_type(FccSema* sema) {
  assert(sema != NULL);

  return fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_VOID, false);
}

static FccStorageClass fcc_sema_storage_class_from_ast(FccAstStorageClass storage_class) {
  switch (storage_class) {
    case FCC_AST_STORAGE_CLASS_NONE:
      return FCC_STORAGE_CLASS_NONE;
    case FCC_AST_STORAGE_CLASS_EXTERN:
      return FCC_STORAGE_CLASS_EXTERN;
    case FCC_AST_STORAGE_CLASS_STATIC:
      return FCC_STORAGE_CLASS_STATIC;
    case FCC_AST_STORAGE_CLASS_TYPEDEF:
      return FCC_STORAGE_CLASS_TYPEDEF;
  }

  return FCC_STORAGE_CLASS_NONE;
}

static const FccSemaObjectInfo* fcc_sema_find_typedef_object(const FccSema* sema,
                                                             const char* typedef_name) {
  size_t object_index;

  assert(sema != NULL);
  assert(typedef_name != NULL);

  for (object_index = sema->result->object_count; object_index > 0; --object_index) {
    const FccSemaObjectInfo* object_info;

    object_info = &sema->result->object_infos[object_index - 1];
    if ((object_info->symbol_kind == FCC_SYMBOL_TYPEDEF) && (object_info->name != NULL) &&
        (strcmp(object_info->name, typedef_name) == 0)) {
      return object_info;
    }
  }

  return NULL;
}

static void fcc_sema_emit(FccSema* sema, FccSourceSpan span, FccDiagSeverity severity,
                          const char* message) {
  assert(sema != NULL);
  assert(message != NULL);

  fcc_diag_emit_source(sema->diagnostics, sema->source_file, span, severity, message);
}

static void fcc_sema_emit_named_message(FccSema* sema, FccSourceSpan span, FccDiagSeverity severity,
                                        const char* prefix, const char* name, const char* suffix) {
  char message[FCC_MAX_DIAG_MESSAGE_LENGTH];
  int written;

  assert(sema != NULL);
  assert(prefix != NULL);
  assert(name != NULL);
  assert(suffix != NULL);

  written = snprintf(message, sizeof(message), "%s%s%s", prefix, name, suffix);
  if ((written < 0) || ((size_t)written >= sizeof(message))) {
    fcc_sema_emit(sema, span, severity, "diagnostic formatting failure");
    return;
  }

  fcc_sema_emit(sema, span, severity, message);
}

static bool fcc_sema_out_of_memory(FccSema* sema, FccSourceSpan span) {
  assert(sema != NULL);

  fcc_sema_emit(sema, span, FCC_DIAG_SEVERITY_FATAL, "out of memory");
  return false;
}

static bool fcc_sema_handle_invalid_type_id(FccSema* sema, FccSourceSpan span) {
  assert(sema != NULL);

  if (sema->diagnostics->error_count != 0) {
    return true;
  }

  return fcc_sema_out_of_memory(sema, span);
}

static bool fcc_sema_push_recursion(FccSema* sema, FccSourceSpan span) {
  assert(sema != NULL);

  ++sema->recursion_depth;
  if (sema->recursion_depth > FCC_MAX_SEMA_DEPTH) {
    --sema->recursion_depth;
    fcc_sema_emit(sema, span, FCC_DIAG_SEVERITY_FATAL,
                  "semantic analysis nesting exceeds FCC_MAX_SEMA_DEPTH");
    return false;
  }

  return true;
}

static void fcc_sema_pop_recursion(FccSema* sema) {
  assert(sema != NULL);
  assert(sema->recursion_depth > 0);

  --sema->recursion_depth;
}

static bool fcc_sema_push_scope(FccSema* sema, FccSourceSpan span) {
  assert(sema != NULL);

  if (!fcc_symbol_table_push_scope(&sema->symbols)) {
    return fcc_sema_out_of_memory(sema, span);
  }

  if (!fcc_sema_push_tag_scope(sema)) {
    fcc_symbol_table_pop_scope(&sema->symbols);
    return fcc_sema_out_of_memory(sema, span);
  }

  return true;
}

static void fcc_sema_pop_scope(FccSema* sema) {
  assert(sema != NULL);

  fcc_symbol_table_pop_scope(&sema->symbols);
  fcc_sema_pop_tag_scope(sema);
}

static bool fcc_sema_record_expression_info(FccSema* sema, const FccAstExpression* expression,
                                            FccTypeId type_id, bool is_lvalue, bool is_valid) {
  assert(sema != NULL);
  assert(expression != NULL);

  if (!fcc_sema_result_append_expression_info(sema->result, expression, type_id, is_lvalue,
                                              is_valid, false, 0, false, NULL)) {
    return fcc_sema_out_of_memory(sema, expression->span);
  }

  return true;
}

static bool fcc_sema_record_constant_expression_info(FccSema* sema,
                                                     const FccAstExpression* expression,
                                                     FccTypeId type_id, bool is_lvalue,
                                                     bool is_valid,
                                                     int64_t integer_constant_value) {
  assert(sema != NULL);
  assert(expression != NULL);

  if (!fcc_sema_result_append_expression_info(sema->result, expression, type_id, is_lvalue,
                                              is_valid, true, integer_constant_value, false,
                                              NULL)) {
    return fcc_sema_out_of_memory(sema, expression->span);
  }

  return true;
}

static bool fcc_sema_record_address_constant_expression_info(
    FccSema* sema, const FccAstExpression* expression, FccTypeId type_id, bool is_valid,
    const char* address_constant_symbol_name) {
  assert(sema != NULL);
  assert(expression != NULL);
  assert(address_constant_symbol_name != NULL);

  if (!fcc_sema_result_append_expression_info(sema->result, expression, type_id, false, is_valid,
                                              false, 0, true, address_constant_symbol_name)) {
    return fcc_sema_out_of_memory(sema, expression->span);
  }

  return true;
}

static bool fcc_sema_record_object_info(FccSema* sema, const void* node, const char* name,
                                        FccSymbolKind symbol_kind, FccStorageClass storage_class,
                                        FccTypeId type_id, bool has_body, FccSourceSpan span) {
  assert(sema != NULL);
  assert(node != NULL);

  if (!fcc_sema_result_append_object_info(sema->result, node, name, symbol_kind, storage_class,
                                          type_id, has_body)) {
    return fcc_sema_out_of_memory(sema, span);
  }

  return true;
}

static FccTypeId fcc_sema_type_from_ast(FccSema* sema, const FccAstType* ast_type);

static FccTypeId fcc_sema_enum_type_from_ast(FccSema* sema, const FccAstType* ast_type);

static bool fcc_sema_evaluate_enum_constant_expression(FccSema* sema,
                                                       const FccAstExpression* expression,
                                                       int64_t* value_out);

static bool fcc_sema_record_field_is_duplicate(const FccTypeRecordFieldInput* fields,
                                               size_t field_count, const char* field_name) {
  size_t field_index;

  assert((fields != NULL) || (field_count == 0));
  assert(field_name != NULL);

  for (field_index = 0; field_index < field_count; ++field_index) {
    if ((fields[field_index].name != NULL) && (strcmp(fields[field_index].name, field_name) == 0)) {
      return true;
    }
  }

  return false;
}

static bool fcc_sema_define_record_from_ast(FccSema* sema, FccTypeId record_type_id,
                                            const FccAstType* ast_type, const char* record_label) {
  FccTypeRecordFieldInput* fields;
  size_t field_index;
  size_t added_field_count;
  bool has_semantic_error;

  assert(sema != NULL);
  assert(ast_type != NULL);
  assert(record_label != NULL);

  if (ast_type->record_field_count == 0) {
    fcc_sema_emit(sema, ast_type->span, FCC_DIAG_SEVERITY_ERROR,
                  "record definition must contain at least one field");
    return true;
  }

  fields = NULL;
  if (ast_type->record_field_count != 0) {
    if (ast_type->record_field_count > (SIZE_MAX / sizeof(FccTypeRecordFieldInput))) {
      return fcc_sema_out_of_memory(sema, ast_type->span);
    }

    fields = (FccTypeRecordFieldInput*)calloc(ast_type->record_field_count,
                                              sizeof(FccTypeRecordFieldInput));
    if (fields == NULL) {
      return fcc_sema_out_of_memory(sema, ast_type->span);
    }
  }

  added_field_count = 0;
  has_semantic_error = false;
  for (field_index = 0; field_index < ast_type->record_field_count; ++field_index) {
    const FccAstRecordField* field;
    FccTypeId field_type_id;

    field = &ast_type->record_fields[field_index];
    if (field->is_static_assert) {
      size_t starting_error_count;

      starting_error_count = sema->diagnostics->error_count;
      if (!fcc_sema_check_static_assert(sema, &field->static_assertion)) {
        free(fields);
        return false;
      }

      if (sema->diagnostics->error_count != starting_error_count) {
        has_semantic_error = true;
      }

      continue;
    }

    field_type_id = fcc_sema_type_from_ast(sema, &field->type);
    if (field_type_id == FCC_TYPE_ID_INVALID) {
      has_semantic_error = true;
      continue;
    }

    if (!fcc_type_is_object(&sema->result->type_context, field_type_id)) {
      fcc_sema_emit_named_message(sema, field->span, FCC_DIAG_SEVERITY_ERROR, "field '",
                                  field->name, "' does not have an object type");
      has_semantic_error = true;
      continue;
    }

    if (!fcc_type_is_complete(&sema->result->type_context, field_type_id)) {
      fcc_sema_emit_named_message(sema, field->span, FCC_DIAG_SEVERITY_ERROR, "field '",
                                  field->name, "' has incomplete type");
      has_semantic_error = true;
      continue;
    }

    if (fcc_sema_record_field_is_duplicate(fields, added_field_count, field->name)) {
      char message[FCC_MAX_DIAG_MESSAGE_LENGTH];
      int written;

      written = snprintf(message, sizeof(message), "duplicate field '%s' in %s definition",
                         field->name, record_label);
      if ((written < 0) || ((size_t)written >= sizeof(message))) {
        fcc_sema_emit(sema, field->span, FCC_DIAG_SEVERITY_ERROR, "diagnostic formatting failure");
      } else {
        fcc_sema_emit(sema, field->span, FCC_DIAG_SEVERITY_ERROR, message);
      }

      has_semantic_error = true;
      continue;
    }

    fields[added_field_count].name = field->name;
    fields[added_field_count].type_id = field_type_id;
    ++added_field_count;
  }

  if (!has_semantic_error && (added_field_count == 0)) {
    fcc_sema_emit(sema, ast_type->span, FCC_DIAG_SEVERITY_ERROR,
                  "record definition must contain at least one field");
    has_semantic_error = true;
  }

  if (!has_semantic_error &&
      !fcc_type_context_define_record(&sema->result->type_context, record_type_id, fields,
                                      added_field_count)) {
    free(fields);
    return fcc_sema_out_of_memory(sema, ast_type->span);
  }

  free(fields);
  return true;
}

static FccTypeId fcc_sema_record_type_from_ast(FccSema* sema, const FccAstType* ast_type) {
  const FccSemaTag* current_tag;
  const FccSemaTag* visible_tag;
  FccTypeKind record_kind;
  FccTypeId record_type_id;

  assert(sema != NULL);
  assert(ast_type != NULL);

  record_kind = (ast_type->kind == FCC_AST_TYPE_STRUCT) ? FCC_TYPE_STRUCT : FCC_TYPE_UNION;
  current_tag = NULL;
  visible_tag = NULL;
  record_type_id = FCC_TYPE_ID_INVALID;

  if (ast_type->tag_name != NULL) {
    current_tag = fcc_sema_lookup_tag_current_scope(sema, ast_type->tag_name);
    visible_tag = fcc_sema_lookup_tag(sema, ast_type->tag_name);
  }

  if (ast_type->is_record_definition) {
    if (ast_type->tag_name == NULL) {
      record_type_id =
          fcc_type_context_create_record(&sema->result->type_context, record_kind, NULL);
      if (record_type_id == FCC_TYPE_ID_INVALID) {
        return FCC_TYPE_ID_INVALID;
      }
    } else if (current_tag != NULL) {
      if (current_tag->kind != record_kind) {
        fcc_sema_emit_named_message(sema, ast_type->span, FCC_DIAG_SEVERITY_ERROR,
                                    "tag kind mismatch for '", ast_type->tag_name, "'");
        return FCC_TYPE_ID_INVALID;
      }

      record_type_id = current_tag->type_id;
      if (fcc_type_is_complete(&sema->result->type_context, record_type_id)) {
        fcc_sema_emit_named_message(sema, ast_type->span, FCC_DIAG_SEVERITY_ERROR,
                                    "redefinition of tagged type '", ast_type->tag_name, "'");
        return FCC_TYPE_ID_INVALID;
      }
    } else {
      record_type_id = fcc_type_context_create_record(&sema->result->type_context, record_kind,
                                                      ast_type->tag_name);
      if (record_type_id == FCC_TYPE_ID_INVALID) {
        return FCC_TYPE_ID_INVALID;
      }

      if (!fcc_sema_define_tag(sema, ast_type->tag_name, record_kind, record_type_id)) {
        (void)fcc_sema_out_of_memory(sema, ast_type->span);
        return FCC_TYPE_ID_INVALID;
      }
    }

    if (!fcc_sema_define_record_from_ast(sema, record_type_id, ast_type,
                                         record_kind == FCC_TYPE_STRUCT ? "struct" : "union")) {
      return FCC_TYPE_ID_INVALID;
    }

    return record_type_id;
  }

  if (ast_type->tag_name == NULL) {
    fcc_sema_emit(sema, ast_type->span, FCC_DIAG_SEVERITY_ERROR,
                  "anonymous record reference requires a definition");
    return FCC_TYPE_ID_INVALID;
  }

  if (visible_tag != NULL) {
    if (visible_tag->kind != record_kind) {
      fcc_sema_emit_named_message(sema, ast_type->span, FCC_DIAG_SEVERITY_ERROR,
                                  "tag kind mismatch for '", ast_type->tag_name, "'");
      return FCC_TYPE_ID_INVALID;
    }

    return visible_tag->type_id;
  }

  record_type_id =
      fcc_type_context_create_record(&sema->result->type_context, record_kind, ast_type->tag_name);
  if (record_type_id == FCC_TYPE_ID_INVALID) {
    return FCC_TYPE_ID_INVALID;
  }

  if (!fcc_sema_define_tag(sema, ast_type->tag_name, record_kind, record_type_id)) {
    (void)fcc_sema_out_of_memory(sema, ast_type->span);
    return FCC_TYPE_ID_INVALID;
  }

  return record_type_id;
}

static FccTypeId fcc_sema_type_from_ast(FccSema* sema, const FccAstType* ast_type) {
  const FccSymbol* typedef_symbol;
  FccTypeId type_id;
  size_t array_index;
  size_t pointer_index;

  assert(sema != NULL);
  assert(ast_type != NULL);

  switch (ast_type->kind) {
    case FCC_AST_TYPE_INT:
      type_id = fcc_sema_int_type(sema);
      break;
    case FCC_AST_TYPE_VOID:
      type_id = fcc_sema_void_type(sema);
      break;
    case FCC_AST_TYPE_CHAR:
      type_id = fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_CHAR, false);
      break;
    case FCC_AST_TYPE_UNSIGNED_INT:
      type_id =
          fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_UNSIGNED_INT, false);
      break;
    case FCC_AST_TYPE_UNSIGNED_CHAR:
      type_id =
          fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_UNSIGNED_CHAR, false);
      break;
    case FCC_AST_TYPE_SIGNED_CHAR:
      type_id =
          fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_SIGNED_CHAR, false);
      break;
    case FCC_AST_TYPE_SHORT:
      type_id = fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_SHORT, false);
      break;
    case FCC_AST_TYPE_UNSIGNED_SHORT:
      type_id =
          fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_UNSIGNED_SHORT, false);
      break;
    case FCC_AST_TYPE_LONG:
      type_id = fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_LONG, false);
      break;
    case FCC_AST_TYPE_UNSIGNED_LONG:
      type_id =
          fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_UNSIGNED_LONG, false);
      break;
    case FCC_AST_TYPE_LONG_LONG:
      type_id =
          fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_LONG_LONG, false);
      break;
    case FCC_AST_TYPE_UNSIGNED_LONG_LONG:
      type_id = fcc_type_context_get_builtin(&sema->result->type_context,
                                             FCC_TYPE_UNSIGNED_LONG_LONG, false);
      break;
    case FCC_AST_TYPE_BOOL:
      type_id = fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_BOOL, false);
      break;
    case FCC_AST_TYPE_TYPEDEF_NAME:
      typedef_symbol = fcc_symbol_table_lookup(&sema->symbols, ast_type->typedef_name);
      if ((typedef_symbol != NULL) && (typedef_symbol->kind == FCC_SYMBOL_TYPEDEF)) {
        type_id = typedef_symbol->type_id;
        break;
      }

      {
        const FccSemaObjectInfo* typedef_object_info;

        typedef_object_info = fcc_sema_find_typedef_object(sema, ast_type->typedef_name);
        if (typedef_object_info != NULL) {
          type_id = typedef_object_info->type_id;
          break;
        }
      }

      fcc_sema_emit_named_message(sema, ast_type->span, FCC_DIAG_SEVERITY_ERROR,
                                  "unknown typedef name '", ast_type->typedef_name, "'");
      return FCC_TYPE_ID_INVALID;
    case FCC_AST_TYPE_STRUCT:
    case FCC_AST_TYPE_UNION:
      type_id = fcc_sema_record_type_from_ast(sema, ast_type);
      break;
    case FCC_AST_TYPE_ENUM:
      type_id = fcc_sema_enum_type_from_ast(sema, ast_type);
      break;
  }

  if (type_id == FCC_TYPE_ID_INVALID) {
    return FCC_TYPE_ID_INVALID;
  }

  if (ast_type->is_const_qualified) {
    type_id = fcc_type_context_qualify_const(&sema->result->type_context, type_id);
    if (type_id == FCC_TYPE_ID_INVALID) {
      return FCC_TYPE_ID_INVALID;
    }
  }

  for (pointer_index = 0; pointer_index < ast_type->pointer_depth; ++pointer_index) {
    type_id = fcc_type_context_get_pointer(&sema->result->type_context, type_id);
    if (type_id == FCC_TYPE_ID_INVALID) {
      return FCC_TYPE_ID_INVALID;
    }
  }

  if (ast_type->is_function_pointer) {
    FccTypeId function_type_id;
    FccTypeId parameter_type_ids[FCC_MAX_FUNCTION_PARAMETERS];
    size_t parameter_index;

    if (ast_type->array_count != 0) {
      fcc_sema_emit(sema, ast_type->span, FCC_DIAG_SEVERITY_ERROR,
                    "array suffixes on function pointer declarators are not supported yet");
      return FCC_TYPE_ID_INVALID;
    }

    if (ast_type->function_pointer_parameter_count > FCC_MAX_FUNCTION_PARAMETERS) {
      fcc_sema_emit(sema, ast_type->span, FCC_DIAG_SEVERITY_FATAL,
                    "function pointer parameter count exceeds FCC_MAX_FUNCTION_PARAMETERS");
      return FCC_TYPE_ID_INVALID;
    }

    for (parameter_index = 0; parameter_index < ast_type->function_pointer_parameter_count;
         ++parameter_index) {
      parameter_type_ids[parameter_index] =
          fcc_sema_type_from_ast(sema,
                                 &ast_type->function_pointer_parameters[parameter_index].type);
      if (parameter_type_ids[parameter_index] == FCC_TYPE_ID_INVALID) {
        return FCC_TYPE_ID_INVALID;
      }

      parameter_type_ids[parameter_index] =
          fcc_type_decay_array(&sema->result->type_context, parameter_type_ids[parameter_index]);
      if (parameter_type_ids[parameter_index] == FCC_TYPE_ID_INVALID) {
        return FCC_TYPE_ID_INVALID;
      }
    }

    function_type_id = fcc_type_context_get_function(
        &sema->result->type_context, type_id, parameter_type_ids,
        ast_type->function_pointer_parameter_count, ast_type->function_pointer_is_variadic);
    if (function_type_id == FCC_TYPE_ID_INVALID) {
      return FCC_TYPE_ID_INVALID;
    }

    type_id = function_type_id;
    for (pointer_index = 0; pointer_index < ast_type->function_pointer_depth; ++pointer_index) {
      type_id = fcc_type_context_get_pointer(&sema->result->type_context, type_id);
      if (type_id == FCC_TYPE_ID_INVALID) {
        return FCC_TYPE_ID_INVALID;
      }
    }

    return type_id;
  }

  for (array_index = ast_type->array_count; array_index > 0; --array_index) {
    const FccAstArrayBound* array_bound;
    size_t element_count;

    array_bound = &ast_type->array_bounds[array_index - 1];
    element_count = array_bound->element_count;
    if (!array_bound->is_vla && (array_bound->expression != NULL)) {
      int64_t bound_value;

      if (!fcc_sema_evaluate_enum_constant_expression(sema, array_bound->expression,
                                                      &bound_value)) {
        fcc_sema_emit(sema, array_bound->span, FCC_DIAG_SEVERITY_ERROR,
                      "array bound must be an integer constant expression");
        return FCC_TYPE_ID_INVALID;
      }

      if (bound_value < 0) {
        fcc_sema_emit(sema, array_bound->span, FCC_DIAG_SEVERITY_ERROR,
                      "array bound cannot be negative");
        return FCC_TYPE_ID_INVALID;
      }

      element_count = (size_t)bound_value;
    }

    type_id = fcc_type_context_get_array(&sema->result->type_context, type_id, element_count,
                                         array_bound->is_vla);
    if (type_id == FCC_TYPE_ID_INVALID) {
      return FCC_TYPE_ID_INVALID;
    }
  }

  return type_id;
}

static bool fcc_sema_is_valid_object_type(FccSema* sema, FccTypeId type_id) {
  const FccType* type;

  assert(sema != NULL);

  type = fcc_type_context_get(&sema->result->type_context, type_id);
  if (type == NULL) {
    return false;
  }

  switch (type->kind) {
    case FCC_TYPE_INVALID:
    case FCC_TYPE_VOID:
    case FCC_TYPE_FUNCTION:
      return false;
    case FCC_TYPE_ARRAY:
      return fcc_sema_is_valid_object_type(sema, type->data.array.element_type_id);
    case FCC_TYPE_BOOL:
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_UNSIGNED_CHAR:
    case FCC_TYPE_SHORT:
    case FCC_TYPE_UNSIGNED_SHORT:
    case FCC_TYPE_INT:
    case FCC_TYPE_UNSIGNED_INT:
    case FCC_TYPE_LONG:
    case FCC_TYPE_UNSIGNED_LONG:
    case FCC_TYPE_LONG_LONG:
    case FCC_TYPE_UNSIGNED_LONG_LONG:
    case FCC_TYPE_FLOAT:
    case FCC_TYPE_DOUBLE:
    case FCC_TYPE_LONG_DOUBLE:
    case FCC_TYPE_POINTER:
    case FCC_TYPE_ENUM:
    case FCC_TYPE_TYPEDEF:
      return true;
    case FCC_TYPE_STRUCT:
    case FCC_TYPE_UNION:
      return fcc_type_is_complete(&sema->result->type_context, type_id);
  }

  return false;
}

static bool fcc_sema_type_is_void(FccSema* sema, FccTypeId type_id) {
  const FccType* type;

  assert(sema != NULL);

  type_id = fcc_type_decay_array(&sema->result->type_context, type_id);
  type = fcc_type_context_get(&sema->result->type_context, type_id);
  return (type != NULL) && (type->kind == FCC_TYPE_VOID);
}

static bool fcc_sema_unqualified_types_are_compatible(FccSema* sema, FccTypeId left_type_id,
                                                      FccTypeId right_type_id) {
  const FccType* left_type;
  const FccType* right_type;

  assert(sema != NULL);

  left_type_id = fcc_type_decay_array(&sema->result->type_context, left_type_id);
  right_type_id = fcc_type_decay_array(&sema->result->type_context, right_type_id);
  left_type_id = fcc_type_resolve_typedef(&sema->result->type_context, left_type_id);
  right_type_id = fcc_type_resolve_typedef(&sema->result->type_context, right_type_id);
  if ((left_type_id == FCC_TYPE_ID_INVALID) || (right_type_id == FCC_TYPE_ID_INVALID)) {
    return false;
  }

  if (fcc_type_equals(left_type_id, right_type_id)) {
    return true;
  }

  left_type = fcc_type_context_get(&sema->result->type_context, left_type_id);
  right_type = fcc_type_context_get(&sema->result->type_context, right_type_id);
  if ((left_type == NULL) || (right_type == NULL)) {
    return false;
  }

  if (left_type->kind != right_type->kind) {
    return false;
  }

  if ((left_type->kind == FCC_TYPE_STRUCT) || (left_type->kind == FCC_TYPE_UNION)) {
    return (left_type->data.record.tag_name == right_type->data.record.tag_name) &&
           (left_type->data.record.first_field_index == right_type->data.record.first_field_index) &&
           (left_type->data.record.field_count == right_type->data.record.field_count);
  }

  if (left_type->kind == FCC_TYPE_ENUM) {
    return left_type->data.tagged.tag_name == right_type->data.tagged.tag_name;
  }

  return fcc_type_is_integer(&sema->result->type_context, left_type_id) ||
         (left_type->kind == FCC_TYPE_VOID);
}

static bool fcc_sema_pointer_types_are_compatible(FccSema* sema, FccTypeId target_type_id,
                                                  FccTypeId value_type_id) {
  FccTypeId target_pointee_type_id;
  FccTypeId value_pointee_type_id;

  assert(sema != NULL);

  target_pointee_type_id =
      fcc_type_get_pointee_type(&sema->result->type_context, target_type_id);
  value_pointee_type_id = fcc_type_get_pointee_type(&sema->result->type_context, value_type_id);
  if ((target_pointee_type_id == FCC_TYPE_ID_INVALID) ||
      (value_pointee_type_id == FCC_TYPE_ID_INVALID)) {
    return false;
  }

  if (fcc_sema_type_is_void(sema, target_pointee_type_id) ||
      fcc_sema_type_is_void(sema, value_pointee_type_id)) {
    return true;
  }

  if (fcc_type_is_const_qualified(&sema->result->type_context, value_pointee_type_id) &&
      !fcc_type_is_const_qualified(&sema->result->type_context, target_pointee_type_id)) {
    return false;
  }

  return fcc_sema_unqualified_types_are_compatible(sema, target_pointee_type_id,
                                                   value_pointee_type_id);
}

static bool fcc_sema_result_is_null_pointer_constant(FccSema* sema,
                                                     const FccSemaExpressionResult* result) {
  assert(sema != NULL);
  assert(result != NULL);

  return result->is_valid && result->has_integer_constant && (result->integer_constant_value == 0) &&
         fcc_type_is_integer(&sema->result->type_context, result->type_id);
}

static bool fcc_sema_types_are_assignment_compatible(FccSema* sema, FccTypeId target_type_id,
                                                     FccTypeId value_type_id) {
  assert(sema != NULL);

  target_type_id = fcc_type_decay_array(&sema->result->type_context, target_type_id);
  value_type_id = fcc_type_decay_array(&sema->result->type_context, value_type_id);
  if ((target_type_id == FCC_TYPE_ID_INVALID) || (value_type_id == FCC_TYPE_ID_INVALID)) {
    return false;
  }

  if (fcc_type_equals(target_type_id, value_type_id)) {
    return true;
  }

  if (fcc_type_is_integer(&sema->result->type_context, target_type_id) &&
      fcc_type_is_integer(&sema->result->type_context, value_type_id)) {
    return true;
  }

  if (fcc_type_is_pointer(&sema->result->type_context, target_type_id) &&
      fcc_type_is_pointer(&sema->result->type_context, value_type_id)) {
    return fcc_sema_pointer_types_are_compatible(sema, target_type_id, value_type_id);
  }

  if (fcc_type_is_pointer(&sema->result->type_context, target_type_id) &&
      fcc_type_is_function(&sema->result->type_context, value_type_id)) {
    FccTypeId target_pointee_type_id;

    target_pointee_type_id =
        fcc_type_get_pointee_type(&sema->result->type_context, target_type_id);
    return (target_pointee_type_id != FCC_TYPE_ID_INVALID) &&
           fcc_sema_unqualified_types_are_compatible(sema, target_pointee_type_id,
                                                     value_type_id);
  }

  if (fcc_sema_unqualified_types_are_compatible(sema, target_type_id, value_type_id)) {
    FccTypeId resolved_target_type_id;
    const FccType* resolved_target_type;

    resolved_target_type_id = fcc_type_resolve_typedef(&sema->result->type_context,
                                                       fcc_type_decay_array(
                                                           &sema->result->type_context,
                                                           target_type_id));
    resolved_target_type =
        fcc_type_context_get(&sema->result->type_context, resolved_target_type_id);
    return (resolved_target_type != NULL) &&
           ((resolved_target_type->kind == FCC_TYPE_STRUCT) ||
            (resolved_target_type->kind == FCC_TYPE_UNION));
  }

  return false;
}

static bool fcc_sema_expression_result_can_initialize_type(FccSema* sema, FccTypeId target_type_id,
                                                           const FccSemaExpressionResult* value) {
  assert(sema != NULL);
  assert(value != NULL);

  target_type_id = fcc_type_decay_array(&sema->result->type_context, target_type_id);
  if ((target_type_id != FCC_TYPE_ID_INVALID) &&
      fcc_type_is_pointer(&sema->result->type_context, target_type_id) &&
      fcc_sema_result_is_null_pointer_constant(sema, value)) {
    return true;
  }

  return fcc_sema_types_are_assignment_compatible(sema, target_type_id, value->type_id);
}

static bool fcc_sema_function_signature_matches_symbol(FccSema* sema, FccTypeId function_type_id,
                                                       const FccSymbol* existing_symbol) {
  assert(sema != NULL);
  assert(existing_symbol != NULL);

  (void)sema;
  return fcc_type_equals(function_type_id, existing_symbol->type_id);
}

static bool fcc_sema_define_named_symbol(FccSema* sema, const FccSymbol* symbol, const char* noun) {
  char error_message[FCC_MAX_DIAG_MESSAGE_LENGTH];
  char note_message[FCC_MAX_DIAG_MESSAGE_LENGTH];
  const FccSymbol* existing_symbol;
  int written;

  assert(sema != NULL);
  assert(symbol != NULL);
  assert(noun != NULL);

  existing_symbol = fcc_symbol_table_lookup_current_scope(&sema->symbols, symbol->name);
  if (existing_symbol != NULL) {
    written = snprintf(error_message, sizeof(error_message), "redefinition of %s '%s'", noun,
                       symbol->name);
    if ((written < 0) || ((size_t)written >= sizeof(error_message))) {
      fcc_sema_emit(sema, symbol->span, FCC_DIAG_SEVERITY_ERROR, "diagnostic formatting failure");
      return true;
    }

    written = snprintf(note_message, sizeof(note_message), "previous definition of '%s' was here",
                       symbol->name);
    if ((written < 0) || ((size_t)written >= sizeof(note_message))) {
      fcc_sema_emit(sema, symbol->span, FCC_DIAG_SEVERITY_ERROR, "diagnostic formatting failure");
      return true;
    }

    fcc_sema_emit(sema, symbol->span, FCC_DIAG_SEVERITY_ERROR, error_message);
    fcc_sema_emit(sema, existing_symbol->span, FCC_DIAG_SEVERITY_NOTE, note_message);
    return true;
  }

  if (!fcc_symbol_table_define(&sema->symbols, symbol)) {
    return fcc_sema_out_of_memory(sema, symbol->span);
  }

  return true;
}

static FccSemaExpressionResult fcc_sema_invalid_expression(void) {
  FccSemaExpressionResult result;

  result.type_id = FCC_TYPE_ID_INVALID;
  result.is_lvalue = false;
  result.is_valid = false;
  result.has_integer_constant = false;
  result.integer_constant_value = 0;
  result.has_address_constant = false;
  result.address_constant_symbol_name = NULL;
  return result;
}

static bool fcc_sema_integer_literal_to_constant(uint64_t value, int64_t* value_out) {
  assert(value_out != NULL);

  if (value > (uint64_t)INT64_MAX) {
    return false;
  }

  *value_out = (int64_t)value;
  return true;
}

static bool fcc_sema_add_integer_constants(int64_t left, int64_t right, int64_t* value_out) {
  assert(value_out != NULL);

  if (((right > 0) && (left > (INT64_MAX - right))) ||
      ((right < 0) && (left < (INT64_MIN - right)))) {
    return false;
  }

  *value_out = left + right;
  return true;
}

static bool fcc_sema_subtract_integer_constants(int64_t left, int64_t right, int64_t* value_out) {
  assert(value_out != NULL);

  if (((right < 0) && (left > (INT64_MAX + right))) ||
      ((right > 0) && (left < (INT64_MIN + right)))) {
    return false;
  }

  *value_out = left - right;
  return true;
}

static bool fcc_sema_multiply_integer_constants(int64_t left, int64_t right, int64_t* value_out) {
  assert(value_out != NULL);

  if (left > 0) {
    if (((right > 0) && (left > (INT64_MAX / right))) ||
        ((right < 0) && (right < (INT64_MIN / left)))) {
      return false;
    }
  } else if (left < 0) {
    if (((right > 0) && (left < (INT64_MIN / right))) ||
        ((right < 0) && (left < (INT64_MAX / right)))) {
      return false;
    }
  }

  *value_out = left * right;
  return true;
}

static bool fcc_sema_evaluate_unary_integer_constant(FccAstUnaryOpKind op_kind,
                                                     int64_t operand_value, int64_t* value_out) {
  assert(value_out != NULL);

  switch (op_kind) {
    case FCC_AST_UNARY_PLUS:
      *value_out = operand_value;
      return true;
    case FCC_AST_UNARY_NEGATE:
      if (operand_value == INT64_MIN) {
        return false;
      }

      *value_out = -operand_value;
      return true;
    case FCC_AST_UNARY_LOGICAL_NOT:
      *value_out = (operand_value == 0) ? 1 : 0;
      return true;
    case FCC_AST_UNARY_BITWISE_NOT:
      *value_out = ~operand_value;
      return true;
    case FCC_AST_UNARY_ADDRESS_OF:
    case FCC_AST_UNARY_DEREFERENCE:
      return false;
  }

  return false;
}

static bool fcc_sema_evaluate_binary_integer_constant(FccAstBinaryOpKind op_kind,
                                                      int64_t left_value, int64_t right_value,
                                                      int64_t* value_out) {
  assert(value_out != NULL);

  switch (op_kind) {
    case FCC_AST_BINARY_ADD:
      return fcc_sema_add_integer_constants(left_value, right_value, value_out);
    case FCC_AST_BINARY_SUBTRACT:
      return fcc_sema_subtract_integer_constants(left_value, right_value, value_out);
    case FCC_AST_BINARY_MULTIPLY:
      return fcc_sema_multiply_integer_constants(left_value, right_value, value_out);
    case FCC_AST_BINARY_DIVIDE:
      if ((right_value == 0) || ((left_value == INT64_MIN) && (right_value == -1))) {
        return false;
      }

      *value_out = left_value / right_value;
      return true;
    case FCC_AST_BINARY_MODULO:
      if ((right_value == 0) || ((left_value == INT64_MIN) && (right_value == -1))) {
        return false;
      }

      *value_out = left_value % right_value;
      return true;
    case FCC_AST_BINARY_LESS:
      *value_out = (left_value < right_value) ? 1 : 0;
      return true;
    case FCC_AST_BINARY_LESS_EQUAL:
      *value_out = (left_value <= right_value) ? 1 : 0;
      return true;
    case FCC_AST_BINARY_GREATER:
      *value_out = (left_value > right_value) ? 1 : 0;
      return true;
    case FCC_AST_BINARY_GREATER_EQUAL:
      *value_out = (left_value >= right_value) ? 1 : 0;
      return true;
    case FCC_AST_BINARY_EQUAL:
      *value_out = (left_value == right_value) ? 1 : 0;
      return true;
    case FCC_AST_BINARY_NOT_EQUAL:
      *value_out = (left_value != right_value) ? 1 : 0;
      return true;
    case FCC_AST_BINARY_BITWISE_AND:
      *value_out = left_value & right_value;
      return true;
    case FCC_AST_BINARY_BITWISE_XOR:
      *value_out = left_value ^ right_value;
      return true;
    case FCC_AST_BINARY_BITWISE_OR:
      *value_out = left_value | right_value;
      return true;
    case FCC_AST_BINARY_LOGICAL_AND:
      *value_out = ((left_value != 0) && (right_value != 0)) ? 1 : 0;
      return true;
    case FCC_AST_BINARY_LOGICAL_OR:
      *value_out = ((left_value != 0) || (right_value != 0)) ? 1 : 0;
      return true;
    case FCC_AST_BINARY_LEFT_SHIFT:
      if ((left_value < 0) || (right_value < 0) || (right_value >= 63) ||
          (left_value > (INT64_MAX >> right_value))) {
        return false;
      }

      *value_out = left_value << right_value;
      return true;
    case FCC_AST_BINARY_RIGHT_SHIFT:
      if ((left_value < 0) || (right_value < 0) || (right_value >= 63)) {
        return false;
      }

      *value_out = left_value >> right_value;
      return true;
  }

  return false;
}

static bool fcc_sema_evaluate_enum_constant_expression(FccSema* sema,
                                                       const FccAstExpression* expression,
                                                       int64_t* value_out) {
  int64_t left_value;
  int64_t right_value;
  const FccSymbol* symbol;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(value_out != NULL);

  switch (expression->kind) {
    case FCC_AST_EXPRESSION_INTEGER_LITERAL:
      return fcc_sema_integer_literal_to_constant(expression->data.integer_literal.value,
                                                  value_out);
    case FCC_AST_EXPRESSION_IDENTIFIER:
      symbol = fcc_symbol_table_lookup(&sema->symbols, expression->data.identifier.name);
      if ((symbol == NULL) || (symbol->kind != FCC_SYMBOL_ENUMERATOR) ||
          !symbol->has_integer_constant) {
        return false;
      }

      *value_out = symbol->integer_constant_value;
      return true;
    case FCC_AST_EXPRESSION_UNARY:
      if (!fcc_sema_evaluate_enum_constant_expression(sema, expression->data.unary.operand,
                                                      &left_value)) {
        return false;
      }

      return fcc_sema_evaluate_unary_integer_constant(expression->data.unary.op_kind, left_value,
                                                      value_out);
    case FCC_AST_EXPRESSION_BINARY:
      if (!fcc_sema_evaluate_enum_constant_expression(sema, expression->data.binary.left,
                                                      &left_value) ||
          !fcc_sema_evaluate_enum_constant_expression(sema, expression->data.binary.right,
                                                      &right_value)) {
        return false;
      }

      return fcc_sema_evaluate_binary_integer_constant(expression->data.binary.op_kind, left_value,
                                                       right_value, value_out);
    case FCC_AST_EXPRESSION_CONDITIONAL:
      if (!fcc_sema_evaluate_enum_constant_expression(sema, expression->data.conditional.condition,
                                                      &left_value)) {
        return false;
      }

      return fcc_sema_evaluate_enum_constant_expression(
          sema,
          left_value != 0 ? expression->data.conditional.then_expression
                          : expression->data.conditional.else_expression,
          value_out);
    case FCC_AST_EXPRESSION_STRING_LITERAL:
    case FCC_AST_EXPRESSION_INITIALIZER_LIST:
    case FCC_AST_EXPRESSION_ASSIGN:
    case FCC_AST_EXPRESSION_CALL:
    case FCC_AST_EXPRESSION_SIZEOF:
    case FCC_AST_EXPRESSION_ALIGNOF:
    case FCC_AST_EXPRESSION_CAST:
    case FCC_AST_EXPRESSION_SUBSCRIPT:
    case FCC_AST_EXPRESSION_MEMBER:
    case FCC_AST_EXPRESSION_COMPOUND_ASSIGN:
    case FCC_AST_EXPRESSION_UPDATE:
      return false;
  }

  return false;
}

static bool fcc_sema_define_enumerator(FccSema* sema, const FccAstEnumerator* enumerator,
                                       int64_t value) {
  FccSymbol symbol;

  assert(sema != NULL);
  assert(enumerator != NULL);

  memset(&symbol, 0, sizeof(symbol));
  symbol.kind = FCC_SYMBOL_ENUMERATOR;
  symbol.storage_class = FCC_STORAGE_CLASS_NONE;
  symbol.type_id = fcc_sema_int_type(sema);
  symbol.span = enumerator->span;
  symbol.name = enumerator->name;
  symbol.has_integer_constant = true;
  symbol.integer_constant_value = value;
  return fcc_sema_define_named_symbol(sema, &symbol, "enumerator");
}

static FccTypeId fcc_sema_enum_type_from_ast(FccSema* sema, const FccAstType* ast_type) {
  const FccSemaTag* current_tag;
  const FccSemaTag* visible_tag;
  FccTypeId enum_type_id;
  int64_t previous_value;
  size_t enumerator_index;

  assert(sema != NULL);
  assert(ast_type != NULL);
  assert(ast_type->kind == FCC_AST_TYPE_ENUM);

  current_tag = NULL;
  visible_tag = NULL;
  enum_type_id = FCC_TYPE_ID_INVALID;
  if (ast_type->tag_name != NULL) {
    current_tag = fcc_sema_lookup_tag_current_scope(sema, ast_type->tag_name);
    visible_tag = fcc_sema_lookup_tag(sema, ast_type->tag_name);
  }

  if (!ast_type->is_enum_definition) {
    if (ast_type->tag_name == NULL) {
      fcc_sema_emit(sema, ast_type->span, FCC_DIAG_SEVERITY_ERROR,
                    "anonymous enum reference requires a definition");
      return FCC_TYPE_ID_INVALID;
    }

    if ((visible_tag == NULL) || (visible_tag->kind != FCC_TYPE_ENUM)) {
      fcc_sema_emit_named_message(sema, ast_type->span, FCC_DIAG_SEVERITY_ERROR,
                                  "unknown enum tag '", ast_type->tag_name, "'");
      return FCC_TYPE_ID_INVALID;
    }

    return visible_tag->type_id;
  }

  if (current_tag != NULL) {
    if (current_tag->kind != FCC_TYPE_ENUM) {
      fcc_sema_emit_named_message(sema, ast_type->span, FCC_DIAG_SEVERITY_ERROR,
                                  "tag kind mismatch for '", ast_type->tag_name, "'");
      return FCC_TYPE_ID_INVALID;
    }

    return current_tag->type_id;
  }

  enum_type_id = fcc_type_context_create_enum(&sema->result->type_context, ast_type->tag_name);
  if (enum_type_id == FCC_TYPE_ID_INVALID) {
    return FCC_TYPE_ID_INVALID;
  }

  if ((ast_type->tag_name != NULL) &&
      !fcc_sema_define_tag(sema, ast_type->tag_name, FCC_TYPE_ENUM, enum_type_id)) {
    (void)fcc_sema_out_of_memory(sema, ast_type->span);
    return FCC_TYPE_ID_INVALID;
  }

  previous_value = -1;
  for (enumerator_index = 0; enumerator_index < ast_type->enumerator_count; ++enumerator_index) {
    const FccAstEnumerator* enumerator;
    int64_t value;

    enumerator = &ast_type->enumerators[enumerator_index];
    if (enumerator->has_value) {
      if (!fcc_sema_evaluate_enum_constant_expression(sema, enumerator->value, &value)) {
        fcc_sema_emit(sema, enumerator->span, FCC_DIAG_SEVERITY_ERROR,
                      "enumerator value must be an integer constant expression");
        value = 0;
      }
    } else if (enumerator_index == 0) {
      value = 0;
    } else if (!fcc_sema_add_integer_constants(previous_value, 1, &value)) {
      fcc_sema_emit(sema, enumerator->span, FCC_DIAG_SEVERITY_ERROR,
                    "implicit enumerator value overflows int64_t");
      value = 0;
    }

    if (!fcc_sema_define_enumerator(sema, enumerator, value)) {
      return FCC_TYPE_ID_INVALID;
    }

    previous_value = value;
  }

  return enum_type_id;
}

static bool fcc_sema_check_expression(FccSema* sema, const FccAstExpression* expression,
                                      FccSemaExpressionResult* result_out);
static bool fcc_sema_check_static_assert(FccSema* sema,
                                         const FccAstStaticAssert* static_assertion);

static bool fcc_sema_require_int_expression(FccSema* sema, const FccAstExpression* expression,
                                            const char* message) {
  FccSemaExpressionResult result;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(message != NULL);

  if (!fcc_sema_check_expression(sema, expression, &result)) {
    return false;
  }

  if (!result.is_valid) {
    return true;
  }

  if (!fcc_type_is_integer(&sema->result->type_context, result.type_id)) {
    fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR, message);
  }

  return true;
}

static bool fcc_sema_check_identifier_expression(FccSema* sema, const FccAstExpression* expression,
                                                 FccSemaExpressionResult* result_out) {
  const FccSymbol* symbol;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  symbol = fcc_symbol_table_lookup(&sema->symbols, expression->data.identifier.name);
  if (symbol == NULL) {
    fcc_sema_emit_named_message(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                                "use of undeclared identifier '", expression->data.identifier.name,
                                "'");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  if (symbol->kind == FCC_SYMBOL_ENUMERATOR) {
    result_out->type_id = symbol->type_id;
    result_out->is_lvalue = false;
    result_out->is_valid = true;
    result_out->has_integer_constant = symbol->has_integer_constant;
    result_out->integer_constant_value = symbol->integer_constant_value;
    if (result_out->has_integer_constant) {
      return fcc_sema_record_constant_expression_info(sema, expression, result_out->type_id, false,
                                                      true, result_out->integer_constant_value);
    }

    return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
  }

  if (symbol->kind == FCC_SYMBOL_FUNCTION) {
    result_out->type_id = symbol->type_id;
    result_out->is_lvalue = false;
    result_out->is_valid = true;
    result_out->has_integer_constant = false;
    result_out->integer_constant_value = 0;
    result_out->has_address_constant = true;
    result_out->address_constant_symbol_name = symbol->name;
    return fcc_sema_record_address_constant_expression_info(
        sema, expression, result_out->type_id, true, result_out->address_constant_symbol_name);
  }

  result_out->type_id = symbol->type_id;
  result_out->is_lvalue = true;
  result_out->is_valid = true;
  result_out->has_integer_constant = false;
  result_out->integer_constant_value = 0;
  return fcc_sema_record_expression_info(sema, expression, result_out->type_id, true, true);
}

static bool fcc_sema_check_call_expression(FccSema* sema, const FccAstExpression* expression,
                                           FccSemaExpressionResult* result_out) {
  const FccAstExpression* callee;
  FccSemaExpressionResult callee_result;
  const FccType* function_type;
  FccTypeId function_type_id;
  size_t argument_index;
  size_t parameter_count;
  bool is_variadic;
  bool has_invalid_argument;
  const char* callee_name;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  callee = expression->data.call.callee;
  if (!fcc_sema_check_expression(sema, callee, &callee_result)) {
    return false;
  }

  if (!callee_result.is_valid) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  function_type_id = callee_result.type_id;
  function_type = fcc_type_context_get(&sema->result->type_context,
                                       fcc_type_resolve_typedef(&sema->result->type_context,
                                                                function_type_id));
  if ((function_type != NULL) && (function_type->kind == FCC_TYPE_POINTER)) {
    function_type_id = function_type->data.pointer.pointee_type_id;
    function_type = fcc_type_context_get(&sema->result->type_context,
                                         fcc_type_resolve_typedef(&sema->result->type_context,
                                                                  function_type_id));
  }

  if ((function_type == NULL) || (function_type->kind != FCC_TYPE_FUNCTION)) {
    if (callee->kind == FCC_AST_EXPRESSION_IDENTIFIER) {
      fcc_sema_emit_named_message(sema, callee->span, FCC_DIAG_SEVERITY_ERROR, "'",
                                  callee->data.identifier.name,
                                  "' is not a function or function pointer");
    } else {
      fcc_sema_emit(sema, callee->span, FCC_DIAG_SEVERITY_ERROR,
                    "called expression is not a function or function pointer");
    }

    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  is_variadic = function_type->data.function.is_variadic;
  parameter_count =
      fcc_type_get_function_parameter_count(&sema->result->type_context, function_type_id);
  if ((!is_variadic && (expression->data.call.argument_count != parameter_count)) ||
      (is_variadic && (expression->data.call.argument_count < parameter_count))) {
    callee_name =
        (callee->kind == FCC_AST_EXPRESSION_IDENTIFIER) ? callee->data.identifier.name : NULL;
    if (callee_name != NULL) {
      fcc_sema_emit_named_message(sema, expression->span, FCC_DIAG_SEVERITY_ERROR, "call to '",
                                  callee_name, "' has the wrong number of arguments");
    } else {
      fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                    "call through function pointer has the wrong number of arguments");
    }

    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  has_invalid_argument = false;
  for (argument_index = 0; argument_index < expression->data.call.argument_count;
       ++argument_index) {
    FccSemaExpressionResult argument_result;
    FccTypeId parameter_type_id;

    if (!fcc_sema_check_expression(sema, expression->data.call.arguments[argument_index],
                                   &argument_result)) {
      return false;
    }

    if (!argument_result.is_valid) {
      has_invalid_argument = true;
      continue;
    }

    if (argument_index >= parameter_count) {
      continue;
    }

    parameter_type_id = fcc_type_get_function_parameter_type(
        &sema->result->type_context, function_type_id, argument_index);
    if (!fcc_sema_expression_result_can_initialize_type(sema, parameter_type_id,
                                                        &argument_result)) {
      fcc_sema_emit(sema, expression->data.call.arguments[argument_index]->span,
                    FCC_DIAG_SEVERITY_ERROR,
                    "call argument type does not match the parameter type");
      has_invalid_argument = true;
    }
  }

  if (has_invalid_argument) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  result_out->type_id =
      fcc_type_get_function_return_type(&sema->result->type_context, function_type_id);
  result_out->is_lvalue = false;
  result_out->is_valid = true;
  result_out->has_integer_constant = false;
  result_out->integer_constant_value = 0;
  return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
}

static bool fcc_sema_check_sizeof_expression(FccSema* sema, const FccAstExpression* expression,
                                             FccSemaExpressionResult* result_out) {
  FccTypeId operand_type_id;
  FccSemaExpressionResult operand_result;
  size_t operand_size;
  bool size_ok;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  if (expression->data.sizeof_expression.has_type_operand) {
    operand_type_id = fcc_sema_type_from_ast(sema, &expression->data.sizeof_expression.type);
    if (operand_type_id == FCC_TYPE_ID_INVALID) {
      return fcc_sema_handle_invalid_type_id(sema, expression->span);
    }
  } else {
    if (!fcc_sema_check_expression(sema, expression->data.sizeof_expression.operand,
                                   &operand_result)) {
      return false;
    }

    if (!operand_result.is_valid) {
      *result_out = fcc_sema_invalid_expression();
      return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
    }

    operand_type_id = operand_result.type_id;
  }

  operand_size = fcc_type_size_of(&sema->result->type_context, operand_type_id, &size_ok);
  if (!size_ok) {
    fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                  "sizeof(void) is not supported in this phase");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  result_out->type_id = fcc_sema_int_type(sema);
  result_out->is_lvalue = false;
  result_out->is_valid = true;
  if (operand_size <= (size_t)INT64_MAX) {
    result_out->has_integer_constant = true;
    result_out->integer_constant_value = (int64_t)operand_size;
    return fcc_sema_record_constant_expression_info(sema, expression, result_out->type_id, false,
                                                    true, result_out->integer_constant_value);
  }

  result_out->has_integer_constant = false;
  result_out->integer_constant_value = 0;
  return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
}

static bool fcc_sema_check_alignof_expression(FccSema* sema, const FccAstExpression* expression,
                                              FccSemaExpressionResult* result_out) {
  FccTypeId operand_type_id;
  size_t operand_alignment;
  bool alignment_ok;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  operand_type_id = fcc_sema_type_from_ast(sema, &expression->data.alignof_expression.type);
  if (operand_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_handle_invalid_type_id(sema, expression->span);
  }

  operand_alignment =
      fcc_type_alignment_of(&sema->result->type_context, operand_type_id, &alignment_ok);
  if (!alignment_ok) {
    fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                  "_Alignof operand has incomplete or unsupported type");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  result_out->type_id = fcc_sema_int_type(sema);
  result_out->is_lvalue = false;
  result_out->is_valid = true;
  if (operand_alignment <= (size_t)INT64_MAX) {
    result_out->has_integer_constant = true;
    result_out->integer_constant_value = (int64_t)operand_alignment;
    return fcc_sema_record_constant_expression_info(sema, expression, result_out->type_id, false,
                                                    true, result_out->integer_constant_value);
  }

  result_out->has_integer_constant = false;
  result_out->integer_constant_value = 0;
  return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
}

static bool fcc_sema_apply_integer_cast(FccSema* sema, FccTypeId target_type_id, int64_t value,
                                        int64_t* value_out) {
  size_t target_size;
  bool size_ok;

  assert(sema != NULL);
  assert(value_out != NULL);

  target_size = fcc_type_size_of(&sema->result->type_context, target_type_id, &size_ok);
  if (!size_ok) {
    return false;
  }

  if (target_size == 1) {
    *value_out = value & 0xff;
    return true;
  }

  if (target_size == 2) {
    *value_out = value & 0xffff;
    return true;
  }

  if ((target_size == 4) || (target_size == 8)) {
    *value_out = value;
    return true;
  }

  return false;
}

static bool fcc_sema_check_cast_expression(FccSema* sema, const FccAstExpression* expression,
                                           FccSemaExpressionResult* result_out) {
  FccSemaExpressionResult operand_result;
  FccTypeId target_type_id;
  int64_t constant_value;
  bool has_integer_constant;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  target_type_id = fcc_sema_type_from_ast(sema, &expression->data.cast.type);
  if (target_type_id == FCC_TYPE_ID_INVALID) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  if (!fcc_sema_check_expression(sema, expression->data.cast.operand, &operand_result)) {
    return false;
  }

  if (!operand_result.is_valid) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  operand_result.type_id = fcc_type_decay_array(&sema->result->type_context, operand_result.type_id);
  if (operand_result.type_id == FCC_TYPE_ID_INVALID) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  if (fcc_sema_type_is_void(sema, target_type_id)) {
    result_out->type_id = target_type_id;
    result_out->is_lvalue = false;
    result_out->is_valid = true;
    result_out->has_integer_constant = false;
    result_out->integer_constant_value = 0;
    return fcc_sema_record_expression_info(sema, expression, target_type_id, false, true);
  }

  if (!fcc_type_is_scalar(&sema->result->type_context, target_type_id) ||
      !fcc_type_is_scalar(&sema->result->type_context, operand_result.type_id)) {
    fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                  "cast requires scalar source and target types");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  has_integer_constant =
      operand_result.has_integer_constant &&
      fcc_type_is_integer(&sema->result->type_context, target_type_id) &&
      fcc_sema_apply_integer_cast(sema, target_type_id, operand_result.integer_constant_value,
                                  &constant_value);
  result_out->type_id = target_type_id;
  result_out->is_lvalue = false;
  result_out->is_valid = true;
  result_out->has_integer_constant = has_integer_constant;
  result_out->integer_constant_value = has_integer_constant ? constant_value : 0;
  if (has_integer_constant) {
    return fcc_sema_record_constant_expression_info(sema, expression, target_type_id, false, true,
                                                    constant_value);
  }

  return fcc_sema_record_expression_info(sema, expression, target_type_id, false, true);
}

static bool fcc_sema_check_subscript_expression(FccSema* sema, const FccAstExpression* expression,
                                                FccSemaExpressionResult* result_out) {
  FccSemaExpressionResult target_result;
  FccSemaExpressionResult index_result;
  const FccType* target_type;
  FccTypeId element_type_id;
  bool has_error;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  if (!fcc_sema_check_expression(sema, expression->data.subscript.target, &target_result)) {
    return false;
  }

  if (!fcc_sema_check_expression(sema, expression->data.subscript.index, &index_result)) {
    return false;
  }

  if (!target_result.is_valid || !index_result.is_valid) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  has_error = false;
  element_type_id = FCC_TYPE_ID_INVALID;
  target_type = fcc_type_context_get(&sema->result->type_context, target_result.type_id);
  if (target_type == NULL) {
    has_error = true;
  } else if (target_type->kind == FCC_TYPE_ARRAY) {
    element_type_id = target_type->data.array.element_type_id;
  } else if (target_type->kind == FCC_TYPE_POINTER) {
    element_type_id = target_type->data.pointer.pointee_type_id;
  } else {
    fcc_sema_emit(sema, expression->data.subscript.target->span, FCC_DIAG_SEVERITY_ERROR,
                  "subscript requires an array or pointer operand");
    has_error = true;
  }

  if (!fcc_type_is_integer(&sema->result->type_context, index_result.type_id)) {
    fcc_sema_emit(sema, expression->data.subscript.index->span, FCC_DIAG_SEVERITY_ERROR,
                  "subscript index must have integer type");
    has_error = true;
  }

  if ((element_type_id != FCC_TYPE_ID_INVALID) &&
      !fcc_type_is_object(&sema->result->type_context, element_type_id)) {
    fcc_sema_emit(sema, expression->data.subscript.target->span, FCC_DIAG_SEVERITY_ERROR,
                  "subscript element type must be an object type");
    has_error = true;
  }

  if (has_error || (element_type_id == FCC_TYPE_ID_INVALID)) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  result_out->type_id = element_type_id;
  result_out->is_lvalue = fcc_type_is_object(&sema->result->type_context, element_type_id);
  result_out->is_valid = true;
  result_out->has_integer_constant = false;
  result_out->integer_constant_value = 0;
  return fcc_sema_record_expression_info(sema, expression, result_out->type_id,
                                         result_out->is_lvalue, true);
}

static bool fcc_sema_check_member_expression(FccSema* sema, const FccAstExpression* expression,
                                             FccSemaExpressionResult* result_out) {
  FccSemaExpressionResult target_result;
  const FccType* record_type;
  const FccTypeRecordField* field;
  FccTypeId record_type_id;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  if (!fcc_sema_check_expression(sema, expression->data.member.target, &target_result)) {
    return false;
  }

  if (!target_result.is_valid) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  record_type_id = target_result.type_id;
  if (expression->data.member.is_arrow) {
    record_type_id = fcc_type_get_pointee_type(&sema->result->type_context, target_result.type_id);
    if (record_type_id == FCC_TYPE_ID_INVALID) {
      fcc_sema_emit(sema, expression->data.member.target->span, FCC_DIAG_SEVERITY_ERROR,
                    "arrow member access requires a pointer operand");
      *result_out = fcc_sema_invalid_expression();
      return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
    }
  } else if (!target_result.is_lvalue) {
    fcc_sema_emit(sema, expression->data.member.target->span, FCC_DIAG_SEVERITY_ERROR,
                  "dot member access requires a struct or union lvalue operand");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  record_type = fcc_type_context_get(&sema->result->type_context, record_type_id);
  if ((record_type == NULL) ||
      ((record_type->kind != FCC_TYPE_STRUCT) && (record_type->kind != FCC_TYPE_UNION)) ||
      !record_type->data.record.is_complete) {
    fcc_sema_emit(sema, expression->data.member.target->span, FCC_DIAG_SEVERITY_ERROR,
                  "member access requires a complete struct or union operand");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  field = fcc_type_record_find_field(&sema->result->type_context, record_type_id,
                                     expression->data.member.field_name);
  if (field == NULL) {
    fcc_sema_emit_named_message(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                                "record has no member '", expression->data.member.field_name, "'");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  result_out->type_id = field->type_id;
  result_out->is_lvalue = fcc_type_is_object(&sema->result->type_context, field->type_id);
  result_out->is_valid = true;
  result_out->has_integer_constant = false;
  result_out->integer_constant_value = 0;
  return fcc_sema_record_expression_info(sema, expression, result_out->type_id,
                                         result_out->is_lvalue, true);
}

static bool fcc_sema_check_string_literal_expression(FccSema* sema,
                                                     const FccAstExpression* expression,
                                                     FccSemaExpressionResult* result_out) {
  FccTypeId const_char_type_id;
  FccTypeId pointer_type_id;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  const_char_type_id =
      fcc_type_context_get_builtin(&sema->result->type_context, FCC_TYPE_CHAR, true);
  if (const_char_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_out_of_memory(sema, expression->span);
  }

  pointer_type_id = fcc_type_context_get_pointer(&sema->result->type_context, const_char_type_id);
  if (pointer_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_out_of_memory(sema, expression->span);
  }

  result_out->type_id = pointer_type_id;
  result_out->is_lvalue = false;
  result_out->is_valid = true;
  result_out->has_integer_constant = false;
  result_out->integer_constant_value = 0;
  return fcc_sema_record_expression_info(sema, expression, pointer_type_id, false, true);
}

static bool fcc_sema_type_is_char_array(FccSema* sema, FccTypeId type_id, size_t* element_count_out,
                                        bool* is_vla_out) {
  const FccType* type;
  const FccType* element_type;

  assert(sema != NULL);
  assert(element_count_out != NULL);
  assert(is_vla_out != NULL);

  type = fcc_type_context_get(&sema->result->type_context, type_id);
  if ((type == NULL) || (type->kind != FCC_TYPE_ARRAY)) {
    return false;
  }

  element_type = fcc_type_context_get(&sema->result->type_context, type->data.array.element_type_id);
  if ((element_type == NULL) || (element_type->kind != FCC_TYPE_CHAR)) {
    return false;
  }

  *element_count_out = type->data.array.element_count;
  *is_vla_out = type->data.array.is_vla;
  return true;
}

static FccTypeId fcc_sema_complete_unsized_array_from_initializer(
    FccSema* sema, FccTypeId type_id, const FccAstExpression* initializer) {
  const FccType* type;
  size_t element_count;

  assert(sema != NULL);

  if (initializer == NULL) {
    return type_id;
  }

  type_id = fcc_type_resolve_typedef(&sema->result->type_context, type_id);
  type = fcc_type_context_get(&sema->result->type_context, type_id);
  if ((type == NULL) || (type->kind != FCC_TYPE_ARRAY) || !type->data.array.is_vla) {
    return type_id;
  }

  element_count = 0;
  if (initializer->kind == FCC_AST_EXPRESSION_STRING_LITERAL) {
    const FccType* element_type;

    element_type = fcc_type_context_get(&sema->result->type_context,
                                        type->data.array.element_type_id);
    if ((element_type == NULL) || (element_type->kind != FCC_TYPE_CHAR)) {
      return type_id;
    }

    element_count = initializer->data.string_literal.length + 1;
  } else if (initializer->kind == FCC_AST_EXPRESSION_INITIALIZER_LIST) {
    element_count = initializer->data.initializer_list.item_count;
  } else {
    return type_id;
  }

  return fcc_type_context_get_array(&sema->result->type_context, type->data.array.element_type_id,
                                    element_count, false);
}

static bool fcc_sema_check_initializer(FccSema* sema, FccTypeId target_type_id,
                                       const FccAstExpression* initializer,
                                       bool require_constant_expression);

static bool fcc_sema_initializer_is_static_constant(
    FccSema* sema, FccTypeId target_type_id, const FccAstExpression* initializer,
    const FccSemaExpressionResult* initializer_result);

static bool fcc_sema_check_initializer_list(FccSema* sema, FccTypeId target_type_id,
                                            const FccAstExpression* initializer,
                                            bool require_constant_expression) {
  const FccType* target_type;
  size_t item_index;

  assert(sema != NULL);
  assert(initializer != NULL);
  assert(initializer->kind == FCC_AST_EXPRESSION_INITIALIZER_LIST);

  target_type = fcc_type_context_get(&sema->result->type_context, target_type_id);
  if (target_type == NULL) {
    return false;
  }

  if (target_type->kind == FCC_TYPE_ARRAY) {
    if (!target_type->data.array.is_vla &&
        (initializer->data.initializer_list.item_count > target_type->data.array.element_count)) {
      fcc_sema_emit(sema, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                    "too many elements in array initializer");
    }

    for (item_index = 0; item_index < initializer->data.initializer_list.item_count; ++item_index) {
      if (!fcc_sema_check_initializer(sema, target_type->data.array.element_type_id,
                                      initializer->data.initializer_list.items[item_index],
                                      require_constant_expression)) {
        return false;
      }
    }

    return true;
  }

  if (target_type->kind == FCC_TYPE_STRUCT) {
    size_t field_count;

    field_count = fcc_type_record_field_count(&sema->result->type_context, target_type_id);
    if (initializer->data.initializer_list.item_count > field_count) {
      fcc_sema_emit(sema, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                    "too many elements in struct initializer");
    }

    for (item_index = 0;
         (item_index < initializer->data.initializer_list.item_count) && (item_index < field_count);
         ++item_index) {
      const FccTypeRecordField* field;

      field = fcc_type_record_field_at(&sema->result->type_context, target_type_id, item_index);
      if ((field == NULL) ||
          !fcc_sema_check_initializer(sema, field->type_id,
                                      initializer->data.initializer_list.items[item_index],
                                      require_constant_expression)) {
        return false;
      }
    }

    return true;
  }

  if (target_type->kind == FCC_TYPE_UNION) {
    const FccTypeRecordField* field;

    if (initializer->data.initializer_list.item_count > 1) {
      fcc_sema_emit(sema, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                    "too many elements in union initializer");
    }

    if (initializer->data.initializer_list.item_count == 0) {
      return true;
    }

    field = fcc_type_record_field_at(&sema->result->type_context, target_type_id, 0);
    return (field != NULL) &&
           fcc_sema_check_initializer(sema, field->type_id,
                                      initializer->data.initializer_list.items[0],
                                      require_constant_expression);
  }

  if (initializer->data.initializer_list.item_count != 1) {
    fcc_sema_emit(sema, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                  "scalar initializer list must contain one element");
    return true;
  }

  return fcc_sema_check_initializer(sema, target_type_id,
                                    initializer->data.initializer_list.items[0],
                                    require_constant_expression);
}

static bool fcc_sema_check_initializer(FccSema* sema, FccTypeId target_type_id,
                                       const FccAstExpression* initializer,
                                       bool require_constant_expression) {
  FccSemaExpressionResult initializer_result;
  size_t char_array_element_count;
  bool is_char_array_vla;

  assert(sema != NULL);
  assert(initializer != NULL);

  if (initializer->kind == FCC_AST_EXPRESSION_INITIALIZER_LIST) {
    return fcc_sema_check_initializer_list(sema, target_type_id, initializer,
                                           require_constant_expression);
  }

  if (fcc_sema_type_is_char_array(sema, target_type_id, &char_array_element_count,
                                  &is_char_array_vla) &&
      (initializer->kind == FCC_AST_EXPRESSION_STRING_LITERAL)) {
    if (!is_char_array_vla &&
        (initializer->data.string_literal.length + 1 > char_array_element_count)) {
      fcc_sema_emit(sema, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                    "string initializer is too large for char array");
    }

    return true;
  }

  if (!fcc_sema_check_expression(sema, initializer, &initializer_result)) {
    return false;
  }

  if (initializer_result.is_valid &&
      !fcc_sema_expression_result_can_initialize_type(sema, target_type_id,
                                                      &initializer_result)) {
    fcc_sema_emit(sema, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                  "initializer type does not match the declared type");
  }

  if (require_constant_expression && initializer_result.is_valid &&
      !fcc_sema_initializer_is_static_constant(sema, target_type_id, initializer,
                                               &initializer_result)) {
    fcc_sema_emit(sema, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                  "global initializers must be constant expressions in this phase");
  }

  return true;
}

static bool fcc_sema_initializer_is_static_constant(
    FccSema* sema, FccTypeId target_type_id, const FccAstExpression* initializer,
    const FccSemaExpressionResult* initializer_result) {
  FccTypeId decayed_target_type_id;

  assert(sema != NULL);
  assert(initializer != NULL);
  assert(initializer_result != NULL);

  if (initializer_result->has_integer_constant) {
    return true;
  }

  decayed_target_type_id = fcc_type_decay_array(&sema->result->type_context, target_type_id);
  if (decayed_target_type_id == FCC_TYPE_ID_INVALID) {
    return false;
  }

  if (fcc_type_is_pointer(&sema->result->type_context, decayed_target_type_id) &&
      (initializer_result->has_address_constant ||
       (initializer->kind == FCC_AST_EXPRESSION_STRING_LITERAL))) {
    return true;
  }

  return false;
}

static bool fcc_sema_check_unary_expression(FccSema* sema, const FccAstExpression* expression,
                                            FccSemaExpressionResult* result_out) {
  int64_t constant_value;
  FccSemaExpressionResult operand_result;
  FccTypeId pointee_type_id;
  bool has_integer_constant;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  if (!fcc_sema_check_expression(sema, expression->data.unary.operand, &operand_result)) {
    return false;
  }

  if (!operand_result.is_valid) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  switch (expression->data.unary.op_kind) {
    case FCC_AST_UNARY_PLUS:
    case FCC_AST_UNARY_NEGATE:
    case FCC_AST_UNARY_LOGICAL_NOT:
    case FCC_AST_UNARY_BITWISE_NOT:
      if (!fcc_type_is_integer(&sema->result->type_context, operand_result.type_id)) {
        fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                      "unary operator requires an int operand");
        *result_out = fcc_sema_invalid_expression();
        return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
      }

      result_out->type_id = fcc_sema_int_type(sema);
      result_out->is_lvalue = false;
      result_out->is_valid = true;
      has_integer_constant = operand_result.has_integer_constant &&
                             fcc_sema_evaluate_unary_integer_constant(
                                 expression->data.unary.op_kind,
                                 operand_result.integer_constant_value, &constant_value);
      result_out->has_integer_constant = has_integer_constant;
      result_out->integer_constant_value = has_integer_constant ? constant_value : 0;
      if (has_integer_constant) {
        return fcc_sema_record_constant_expression_info(sema, expression, result_out->type_id,
                                                        false, true, constant_value);
      }

      return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
    case FCC_AST_UNARY_ADDRESS_OF:
      if (!operand_result.is_lvalue &&
          !fcc_type_is_function(&sema->result->type_context, operand_result.type_id)) {
        fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                      "address-of operand must be an lvalue");
        *result_out = fcc_sema_invalid_expression();
        return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
      }

      result_out->type_id =
          fcc_type_context_get_pointer(&sema->result->type_context, operand_result.type_id);
      if (result_out->type_id == FCC_TYPE_ID_INVALID) {
        return fcc_sema_out_of_memory(sema, expression->span);
      }

      result_out->is_lvalue = false;
      result_out->is_valid = true;
      result_out->has_integer_constant = false;
      result_out->integer_constant_value = 0;
      if (operand_result.has_address_constant) {
        result_out->has_address_constant = true;
        result_out->address_constant_symbol_name = operand_result.address_constant_symbol_name;
        return fcc_sema_record_address_constant_expression_info(
            sema, expression, result_out->type_id, true,
            result_out->address_constant_symbol_name);
      }

      return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
    case FCC_AST_UNARY_DEREFERENCE:
      pointee_type_id =
          fcc_type_get_pointee_type(&sema->result->type_context, operand_result.type_id);
      if (pointee_type_id == FCC_TYPE_ID_INVALID) {
        fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                      "dereference requires a pointer operand");
        *result_out = fcc_sema_invalid_expression();
        return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
      }

      result_out->type_id = pointee_type_id;
      result_out->is_lvalue = fcc_type_is_object(&sema->result->type_context, pointee_type_id);
      result_out->is_valid = true;
      result_out->has_integer_constant = false;
      result_out->integer_constant_value = 0;
      return fcc_sema_record_expression_info(sema, expression, result_out->type_id,
                                             result_out->is_lvalue, true);
  }

  *result_out = fcc_sema_invalid_expression();
  return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
}

static bool fcc_sema_binary_op_is_relational(FccAstBinaryOpKind op_kind) {
  return (op_kind == FCC_AST_BINARY_LESS) || (op_kind == FCC_AST_BINARY_LESS_EQUAL) ||
         (op_kind == FCC_AST_BINARY_GREATER) || (op_kind == FCC_AST_BINARY_GREATER_EQUAL);
}

static bool fcc_sema_binary_op_is_equality(FccAstBinaryOpKind op_kind) {
  return (op_kind == FCC_AST_BINARY_EQUAL) || (op_kind == FCC_AST_BINARY_NOT_EQUAL);
}

static bool fcc_sema_pointer_arithmetic_operand_is_complete(FccSema* sema,
                                                            FccTypeId pointer_type_id) {
  FccTypeId pointee_type_id;

  assert(sema != NULL);

  pointee_type_id = fcc_type_get_pointee_type(&sema->result->type_context, pointer_type_id);
  if (pointee_type_id == FCC_TYPE_ID_INVALID) {
    return false;
  }

  return fcc_type_is_object(&sema->result->type_context, pointee_type_id) &&
         fcc_type_is_complete(&sema->result->type_context, pointee_type_id);
}

static FccTypeKind fcc_sema_integer_promotion_kind(const FccType* type) {
  assert(type != NULL);

  switch (type->kind) {
    case FCC_TYPE_BOOL:
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_UNSIGNED_CHAR:
    case FCC_TYPE_SHORT:
    case FCC_TYPE_UNSIGNED_SHORT:
    case FCC_TYPE_ENUM:
      return FCC_TYPE_INT;
    default:
      return type->kind;
  }
}

static int fcc_sema_integer_rank(FccTypeKind kind) {
  switch (kind) {
    case FCC_TYPE_BOOL:
      return 0;
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_UNSIGNED_CHAR:
      return 1;
    case FCC_TYPE_SHORT:
    case FCC_TYPE_UNSIGNED_SHORT:
      return 2;
    case FCC_TYPE_INT:
    case FCC_TYPE_UNSIGNED_INT:
    case FCC_TYPE_ENUM:
      return 3;
    case FCC_TYPE_LONG:
    case FCC_TYPE_UNSIGNED_LONG:
      return 4;
    case FCC_TYPE_LONG_LONG:
    case FCC_TYPE_UNSIGNED_LONG_LONG:
      return 5;
    default:
      return -1;
  }
}

static FccTypeKind fcc_sema_unsigned_kind_for_rank(int rank) {
  switch (rank) {
    case 3:
      return FCC_TYPE_UNSIGNED_INT;
    case 4:
      return FCC_TYPE_UNSIGNED_LONG;
    case 5:
      return FCC_TYPE_UNSIGNED_LONG_LONG;
    default:
      return FCC_TYPE_UNSIGNED_INT;
  }
}

static FccTypeKind fcc_sema_signed_kind_for_rank(int rank) {
  switch (rank) {
    case 4:
      return FCC_TYPE_LONG;
    case 5:
      return FCC_TYPE_LONG_LONG;
    default:
      return FCC_TYPE_INT;
  }
}

static FccTypeId fcc_sema_integer_promotion(FccSema* sema, FccTypeId type_id) {
  const FccType* type;
  FccTypeKind promoted_kind;

  assert(sema != NULL);

  type_id = fcc_type_resolve_typedef(&sema->result->type_context, type_id);
  type = fcc_type_context_get(&sema->result->type_context, type_id);
  if (type == NULL) {
    return FCC_TYPE_ID_INVALID;
  }

  promoted_kind = fcc_sema_integer_promotion_kind(type);
  if (promoted_kind == type->kind) {
    return type_id;
  }

  return fcc_type_context_get_builtin(&sema->result->type_context, promoted_kind, false);
}

static FccTypeId fcc_sema_usual_integer_conversion(FccSema* sema, FccTypeId left_type_id,
                                                   FccTypeId right_type_id) {
  const FccType* left_type;
  const FccType* right_type;
  FccTypeKind result_kind;
  bool left_unsigned;
  bool right_unsigned;
  int left_rank;
  int right_rank;

  assert(sema != NULL);

  left_type_id = fcc_sema_integer_promotion(sema, left_type_id);
  right_type_id = fcc_sema_integer_promotion(sema, right_type_id);
  if ((left_type_id == FCC_TYPE_ID_INVALID) || (right_type_id == FCC_TYPE_ID_INVALID)) {
    return FCC_TYPE_ID_INVALID;
  }

  left_type = fcc_type_context_get(&sema->result->type_context, left_type_id);
  right_type = fcc_type_context_get(&sema->result->type_context, right_type_id);
  if ((left_type == NULL) || (right_type == NULL)) {
    return FCC_TYPE_ID_INVALID;
  }

  if (left_type->kind == right_type->kind) {
    return left_type_id;
  }

  left_unsigned = fcc_type_is_unsigned_integer(&sema->result->type_context, left_type_id);
  right_unsigned = fcc_type_is_unsigned_integer(&sema->result->type_context, right_type_id);
  left_rank = fcc_sema_integer_rank(left_type->kind);
  right_rank = fcc_sema_integer_rank(right_type->kind);
  if ((left_rank < 0) || (right_rank < 0)) {
    return FCC_TYPE_ID_INVALID;
  }

  if (left_unsigned == right_unsigned) {
    result_kind =
        (left_rank >= right_rank) ? left_type->kind : right_type->kind;
  } else if (left_unsigned && (left_rank >= right_rank)) {
    result_kind = left_type->kind;
  } else if (right_unsigned && (right_rank >= left_rank)) {
    result_kind = right_type->kind;
  } else {
    int signed_rank;
    int unsigned_rank;
    size_t signed_size;
    size_t unsigned_size;
    bool size_ok;

    signed_rank = left_unsigned ? right_rank : left_rank;
    unsigned_rank = left_unsigned ? left_rank : right_rank;
    signed_size = fcc_type_size_of(
        &sema->result->type_context,
        fcc_type_context_get_builtin(&sema->result->type_context,
                                     fcc_sema_signed_kind_for_rank(signed_rank), false),
        &size_ok);
    if (!size_ok) {
      return FCC_TYPE_ID_INVALID;
    }

    unsigned_size = fcc_type_size_of(
        &sema->result->type_context,
        fcc_type_context_get_builtin(&sema->result->type_context,
                                     fcc_sema_unsigned_kind_for_rank(unsigned_rank), false),
        &size_ok);
    if (!size_ok) {
      return FCC_TYPE_ID_INVALID;
    }

    if (signed_size > unsigned_size) {
      result_kind = fcc_sema_signed_kind_for_rank(signed_rank);
    } else {
      result_kind = fcc_sema_unsigned_kind_for_rank(signed_rank);
    }
  }

  return fcc_type_context_get_builtin(&sema->result->type_context, result_kind, false);
}

static FccTypeId fcc_sema_binary_integer_result_type(FccSema* sema,
                                                     FccAstBinaryOpKind op_kind,
                                                     FccTypeId left_type_id,
                                                     FccTypeId right_type_id) {
  assert(sema != NULL);

  if (fcc_sema_binary_op_is_equality(op_kind) || fcc_sema_binary_op_is_relational(op_kind) ||
      (op_kind == FCC_AST_BINARY_LOGICAL_AND) || (op_kind == FCC_AST_BINARY_LOGICAL_OR)) {
    return fcc_sema_int_type(sema);
  }

  if ((op_kind == FCC_AST_BINARY_LEFT_SHIFT) || (op_kind == FCC_AST_BINARY_RIGHT_SHIFT)) {
    return fcc_sema_integer_promotion(sema, left_type_id);
  }

  return fcc_sema_usual_integer_conversion(sema, left_type_id, right_type_id);
}

static bool fcc_sema_check_binary_expression(FccSema* sema, const FccAstExpression* expression,
                                             FccSemaExpressionResult* result_out) {
  int64_t constant_value;
  FccSemaExpressionResult left_result;
  FccSemaExpressionResult right_result;
  FccTypeId left_type_id;
  FccTypeId right_type_id;
  bool left_is_integer;
  bool right_is_integer;
  bool left_is_pointer;
  bool right_is_pointer;
  bool has_integer_constant;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  if (!fcc_sema_check_expression(sema, expression->data.binary.left, &left_result)) {
    return false;
  }

  if (!fcc_sema_check_expression(sema, expression->data.binary.right, &right_result)) {
    return false;
  }

  if (!left_result.is_valid || !right_result.is_valid) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  left_type_id = fcc_type_decay_array(&sema->result->type_context, left_result.type_id);
  right_type_id = fcc_type_decay_array(&sema->result->type_context, right_result.type_id);
  if ((left_type_id == FCC_TYPE_ID_INVALID) || (right_type_id == FCC_TYPE_ID_INVALID)) {
    return fcc_sema_out_of_memory(sema, expression->span);
  }

  left_is_integer = fcc_type_is_integer(&sema->result->type_context, left_type_id);
  right_is_integer = fcc_type_is_integer(&sema->result->type_context, right_type_id);
  left_is_pointer = fcc_type_is_pointer(&sema->result->type_context, left_type_id);
  right_is_pointer = fcc_type_is_pointer(&sema->result->type_context, right_type_id);

  if (left_is_integer && right_is_integer) {
    result_out->type_id = fcc_sema_binary_integer_result_type(
        sema, expression->data.binary.op_kind, left_type_id, right_type_id);
    if (result_out->type_id == FCC_TYPE_ID_INVALID) {
      return fcc_sema_out_of_memory(sema, expression->span);
    }

    result_out->is_lvalue = false;
    result_out->is_valid = true;
    has_integer_constant = left_result.has_integer_constant && right_result.has_integer_constant &&
                           fcc_sema_evaluate_binary_integer_constant(
                               expression->data.binary.op_kind, left_result.integer_constant_value,
                               right_result.integer_constant_value, &constant_value);
    result_out->has_integer_constant = has_integer_constant;
    result_out->integer_constant_value = has_integer_constant ? constant_value : 0;
    if (has_integer_constant) {
      return fcc_sema_record_constant_expression_info(sema, expression, result_out->type_id, false,
                                                      true, constant_value);
    }

    return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
  }

  if ((expression->data.binary.op_kind == FCC_AST_BINARY_ADD) &&
      ((left_is_pointer && right_is_integer) || (left_is_integer && right_is_pointer))) {
    FccTypeId pointer_type_id;

    pointer_type_id = left_is_pointer ? left_type_id : right_type_id;
    if (!fcc_sema_pointer_arithmetic_operand_is_complete(sema, pointer_type_id)) {
      fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                    "pointer arithmetic requires a pointer to a complete object type");
      *result_out = fcc_sema_invalid_expression();
      return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
    }

    result_out->type_id = pointer_type_id;
    result_out->is_lvalue = false;
    result_out->is_valid = true;
    result_out->has_integer_constant = false;
    result_out->integer_constant_value = 0;
    return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
  }

  if ((expression->data.binary.op_kind == FCC_AST_BINARY_SUBTRACT) && left_is_pointer &&
      right_is_integer) {
    if (!fcc_sema_pointer_arithmetic_operand_is_complete(sema, left_type_id)) {
      fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                    "pointer arithmetic requires a pointer to a complete object type");
      *result_out = fcc_sema_invalid_expression();
      return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
    }

    result_out->type_id = left_type_id;
    result_out->is_lvalue = false;
    result_out->is_valid = true;
    result_out->has_integer_constant = false;
    result_out->integer_constant_value = 0;
    return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
  }

  if ((expression->data.binary.op_kind == FCC_AST_BINARY_SUBTRACT) && left_is_pointer &&
      right_is_pointer && fcc_type_equals(left_type_id, right_type_id)) {
    if (!fcc_sema_pointer_arithmetic_operand_is_complete(sema, left_type_id)) {
      fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                    "pointer subtraction requires a pointer to a complete object type");
      *result_out = fcc_sema_invalid_expression();
      return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
    }

    result_out->type_id = fcc_sema_int_type(sema);
    result_out->is_lvalue = false;
    result_out->is_valid = true;
    result_out->has_integer_constant = false;
    result_out->integer_constant_value = 0;
    return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
  }

  if (fcc_sema_binary_op_is_equality(expression->data.binary.op_kind) &&
      ((left_is_pointer && fcc_sema_result_is_null_pointer_constant(sema, &right_result)) ||
       (right_is_pointer && fcc_sema_result_is_null_pointer_constant(sema, &left_result)))) {
    result_out->type_id = fcc_sema_int_type(sema);
    result_out->is_lvalue = false;
    result_out->is_valid = true;
    result_out->has_integer_constant = false;
    result_out->integer_constant_value = 0;
    return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
  }

  if ((fcc_sema_binary_op_is_equality(expression->data.binary.op_kind) ||
       fcc_sema_binary_op_is_relational(expression->data.binary.op_kind)) &&
      left_is_pointer && right_is_pointer &&
      fcc_sema_pointer_types_are_compatible(sema, left_type_id, right_type_id)) {
    result_out->type_id = fcc_sema_int_type(sema);
    result_out->is_lvalue = false;
    result_out->is_valid = true;
    result_out->has_integer_constant = false;
    result_out->integer_constant_value = 0;
    return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
  }

  if (!left_is_integer || !right_is_integer) {
    fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                  "binary operator requires compatible integer or pointer operands");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  *result_out = fcc_sema_invalid_expression();
  return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
}

static bool fcc_sema_check_assign_expression(FccSema* sema, const FccAstExpression* expression,
                                             FccSemaExpressionResult* result_out) {
  FccSemaExpressionResult target_result;
  FccSemaExpressionResult value_result;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  if (!fcc_sema_check_expression(sema, expression->data.assign.target, &target_result)) {
    return false;
  }

  if (!fcc_sema_check_expression(sema, expression->data.assign.value, &value_result)) {
    return false;
  }

  if (!target_result.is_valid || !value_result.is_valid) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  if (!target_result.is_lvalue) {
    fcc_sema_emit(sema, expression->data.assign.target->span, FCC_DIAG_SEVERITY_ERROR,
                  "assignment target is not an lvalue");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  if (!fcc_sema_expression_result_can_initialize_type(sema, target_result.type_id,
                                                      &value_result)) {
    fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                  "assignment requires matching operand types");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  result_out->type_id = target_result.type_id;
  result_out->is_lvalue = false;
  result_out->is_valid = true;
  result_out->has_integer_constant = false;
  result_out->integer_constant_value = 0;
  return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
}

static bool fcc_sema_op_is_pointer_compound_assignment(FccAstBinaryOpKind op_kind) {
  return (op_kind == FCC_AST_BINARY_ADD) || (op_kind == FCC_AST_BINARY_SUBTRACT);
}

static bool fcc_sema_check_compound_assign_expression(FccSema* sema,
                                                      const FccAstExpression* expression,
                                                      FccSemaExpressionResult* result_out) {
  FccSemaExpressionResult target_result;
  FccSemaExpressionResult value_result;
  FccTypeId value_type_id;
  bool target_is_integer;
  bool target_is_pointer;
  bool value_is_integer;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  if (!fcc_sema_check_expression(sema, expression->data.compound_assign.target, &target_result)) {
    return false;
  }

  if (!fcc_sema_check_expression(sema, expression->data.compound_assign.value, &value_result)) {
    return false;
  }

  if (!target_result.is_valid || !value_result.is_valid) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  if (!target_result.is_lvalue) {
    fcc_sema_emit(sema, expression->data.compound_assign.target->span, FCC_DIAG_SEVERITY_ERROR,
                  "compound assignment target is not an lvalue");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  value_type_id = fcc_type_decay_array(&sema->result->type_context, value_result.type_id);
  if (value_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_out_of_memory(sema, expression->span);
  }

  target_is_integer = fcc_type_is_integer(&sema->result->type_context, target_result.type_id);
  target_is_pointer = fcc_type_is_pointer(&sema->result->type_context, target_result.type_id);
  value_is_integer = fcc_type_is_integer(&sema->result->type_context, value_type_id);

  if (target_is_pointer) {
    if (!fcc_sema_op_is_pointer_compound_assignment(expression->data.compound_assign.op_kind) ||
        !value_is_integer ||
        !fcc_sema_pointer_arithmetic_operand_is_complete(sema, target_result.type_id)) {
      fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                    "pointer compound assignment requires += or -= with an integer operand");
      *result_out = fcc_sema_invalid_expression();
      return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
    }
  } else if (!target_is_integer || !value_is_integer) {
    fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                  "compound assignment requires integer operands or pointer +=/-= integer");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  result_out->type_id = target_result.type_id;
  result_out->is_lvalue = false;
  result_out->is_valid = true;
  result_out->has_integer_constant = false;
  result_out->integer_constant_value = 0;
  return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
}

static bool fcc_sema_check_update_expression(FccSema* sema, const FccAstExpression* expression,
                                             FccSemaExpressionResult* result_out) {
  FccSemaExpressionResult target_result;
  bool target_is_integer;
  bool target_is_pointer;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  if (!fcc_sema_check_expression(sema, expression->data.update.target, &target_result)) {
    return false;
  }

  if (!target_result.is_valid) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  if (!target_result.is_lvalue) {
    fcc_sema_emit(sema, expression->data.update.target->span, FCC_DIAG_SEVERITY_ERROR,
                  "increment/decrement target is not an lvalue");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  target_is_integer = fcc_type_is_integer(&sema->result->type_context, target_result.type_id);
  target_is_pointer = fcc_type_is_pointer(&sema->result->type_context, target_result.type_id);
  if (!target_is_integer && (!target_is_pointer || !fcc_sema_pointer_arithmetic_operand_is_complete(
                                                       sema, target_result.type_id))) {
    fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                  "increment/decrement requires an integer lvalue or complete object pointer");
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  result_out->type_id = target_result.type_id;
  result_out->is_lvalue = false;
  result_out->is_valid = true;
  result_out->has_integer_constant = false;
  result_out->integer_constant_value = 0;
  return fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
}

static bool fcc_sema_check_conditional_expression(FccSema* sema, const FccAstExpression* expression,
                                                  FccSemaExpressionResult* result_out) {
  FccSemaExpressionResult condition_result;
  FccSemaExpressionResult then_result;
  FccSemaExpressionResult else_result;
  FccTypeId result_type_id;
  bool has_error;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  if (!fcc_sema_check_expression(sema, expression->data.conditional.condition, &condition_result) ||
      !fcc_sema_check_expression(sema, expression->data.conditional.then_expression,
                                 &then_result) ||
      !fcc_sema_check_expression(sema, expression->data.conditional.else_expression,
                                 &else_result)) {
    return false;
  }

  has_error = false;
  if (condition_result.is_valid &&
      !fcc_type_is_integer(&sema->result->type_context, condition_result.type_id) &&
      !fcc_type_is_pointer(&sema->result->type_context, condition_result.type_id)) {
    fcc_sema_emit(sema, expression->data.conditional.condition->span, FCC_DIAG_SEVERITY_ERROR,
                  "conditional expression condition must be scalar");
    has_error = true;
  }

  result_type_id = FCC_TYPE_ID_INVALID;
  if (then_result.is_valid && else_result.is_valid) {
    if (fcc_type_equals(then_result.type_id, else_result.type_id) ||
        fcc_sema_expression_result_can_initialize_type(sema, then_result.type_id, &else_result)) {
      result_type_id = then_result.type_id;
    } else if (fcc_sema_expression_result_can_initialize_type(sema, else_result.type_id,
                                                              &then_result)) {
      result_type_id = else_result.type_id;
    } else {
      fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                    "conditional expression arms must have compatible types");
      has_error = true;
    }
  } else {
    has_error = true;
  }

  if (has_error || (result_type_id == FCC_TYPE_ID_INVALID)) {
    *result_out = fcc_sema_invalid_expression();
    return fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
  }

  result_out->type_id = result_type_id;
  result_out->is_lvalue = false;
  result_out->is_valid = true;
  result_out->has_integer_constant = condition_result.has_integer_constant &&
                                     then_result.has_integer_constant &&
                                     else_result.has_integer_constant;
  if (result_out->has_integer_constant) {
    result_out->integer_constant_value = condition_result.integer_constant_value != 0
                                             ? then_result.integer_constant_value
                                             : else_result.integer_constant_value;
    return fcc_sema_record_constant_expression_info(sema, expression, result_type_id, false, true,
                                                    result_out->integer_constant_value);
  }

  result_out->integer_constant_value = 0;
  return fcc_sema_record_expression_info(sema, expression, result_type_id, false, true);
}

static bool fcc_sema_check_expression(FccSema* sema, const FccAstExpression* expression,
                                      FccSemaExpressionResult* result_out) {
  bool ok;

  assert(sema != NULL);
  assert(expression != NULL);
  assert(result_out != NULL);

  *result_out = fcc_sema_invalid_expression();
  if (!fcc_sema_push_recursion(sema, expression->span)) {
    return false;
  }

  ok = true;
  switch (expression->kind) {
    case FCC_AST_EXPRESSION_INTEGER_LITERAL:
      if (expression->data.integer_literal.value <= (uint64_t)INT_MAX) {
        result_out->type_id = fcc_sema_int_type(sema);
      } else if (expression->data.integer_literal.value <= (uint64_t)INT64_MAX) {
        result_out->type_id = fcc_type_context_get_builtin(&sema->result->type_context,
                                                           FCC_TYPE_LONG_LONG, false);
      } else {
        result_out->type_id = fcc_type_context_get_builtin(&sema->result->type_context,
                                                           FCC_TYPE_UNSIGNED_LONG_LONG, false);
      }
      if (result_out->type_id == FCC_TYPE_ID_INVALID) {
        fcc_sema_pop_recursion(sema);
        return fcc_sema_out_of_memory(sema, expression->span);
      }

      result_out->is_lvalue = false;
      result_out->is_valid = true;
      result_out->has_integer_constant = fcc_sema_integer_literal_to_constant(
          expression->data.integer_literal.value, &result_out->integer_constant_value);
      if (result_out->has_integer_constant) {
        ok = fcc_sema_record_constant_expression_info(sema, expression, result_out->type_id, false,
                                                      true, result_out->integer_constant_value);
      } else {
        result_out->integer_constant_value = 0;
        ok = fcc_sema_record_expression_info(sema, expression, result_out->type_id, false, true);
      }
      break;
    case FCC_AST_EXPRESSION_IDENTIFIER:
      ok = fcc_sema_check_identifier_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_STRING_LITERAL:
      ok = fcc_sema_check_string_literal_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_UNARY:
      ok = fcc_sema_check_unary_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_BINARY:
      ok = fcc_sema_check_binary_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_ASSIGN:
      ok = fcc_sema_check_assign_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_COMPOUND_ASSIGN:
      ok = fcc_sema_check_compound_assign_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_CALL:
      ok = fcc_sema_check_call_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_SIZEOF:
      ok = fcc_sema_check_sizeof_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_ALIGNOF:
      ok = fcc_sema_check_alignof_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_CAST:
      ok = fcc_sema_check_cast_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_SUBSCRIPT:
      ok = fcc_sema_check_subscript_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_MEMBER:
      ok = fcc_sema_check_member_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_UPDATE:
      ok = fcc_sema_check_update_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_CONDITIONAL:
      ok = fcc_sema_check_conditional_expression(sema, expression, result_out);
      break;
    case FCC_AST_EXPRESSION_INITIALIZER_LIST:
      fcc_sema_emit(sema, expression->span, FCC_DIAG_SEVERITY_ERROR,
                    "initializer list is only valid in an initializer");
      *result_out = fcc_sema_invalid_expression();
      ok = fcc_sema_record_expression_info(sema, expression, FCC_TYPE_ID_INVALID, false, false);
      break;
  }

  fcc_sema_pop_recursion(sema);
  return ok;
}

static bool fcc_sema_emit_static_assert_failure(FccSema* sema,
                                                const FccAstStaticAssert* static_assertion) {
  char message[FCC_MAX_DIAG_MESSAGE_LENGTH];
  const char* assertion_message;
  int precision;
  int written;

  assert(sema != NULL);
  assert(static_assertion != NULL);

  assertion_message = static_assertion->message != NULL ? static_assertion->message : "";
  precision = (static_assertion->message_length > (size_t)INT_MAX)
                  ? INT_MAX
                  : (int)static_assertion->message_length;
  written = snprintf(message, sizeof(message), "static assertion failed: %.*s", precision,
                     assertion_message);
  if ((written < 0) || ((size_t)written >= sizeof(message))) {
    fcc_sema_emit(sema, static_assertion->span, FCC_DIAG_SEVERITY_ERROR,
                  "static assertion failed");
    return true;
  }

  fcc_sema_emit(sema, static_assertion->span, FCC_DIAG_SEVERITY_ERROR, message);
  return true;
}

static bool fcc_sema_check_static_assert(FccSema* sema,
                                         const FccAstStaticAssert* static_assertion) {
  FccSemaExpressionResult condition_result;

  assert(sema != NULL);
  assert(static_assertion != NULL);

  if (!fcc_sema_check_expression(sema, static_assertion->condition, &condition_result)) {
    return false;
  }

  if (!condition_result.is_valid) {
    return true;
  }

  if (!fcc_type_is_integer(&sema->result->type_context, condition_result.type_id) ||
      !condition_result.has_integer_constant) {
    fcc_sema_emit(sema, static_assertion->condition->span, FCC_DIAG_SEVERITY_ERROR,
                  "_Static_assert expression must be an integer constant expression");
    return true;
  }

  if (condition_result.integer_constant_value == 0) {
    return fcc_sema_emit_static_assert_failure(sema, static_assertion);
  }

  return true;
}

static bool fcc_sema_check_statement(FccSema* sema, const FccAstStatement* statement);

static bool fcc_sema_check_compound_statement(FccSema* sema, const FccAstStatement* statement,
                                              bool push_scope) {
  size_t item_index;

  assert(sema != NULL);
  assert(statement != NULL);
  assert(statement->kind == FCC_AST_STATEMENT_COMPOUND);

  if (!fcc_sema_push_recursion(sema, statement->span)) {
    return false;
  }

  if (push_scope && !fcc_sema_push_scope(sema, statement->span)) {
    fcc_sema_pop_recursion(sema);
    return false;
  }

  for (item_index = 0; item_index < statement->data.compound.item_count; ++item_index) {
    if (!fcc_sema_check_statement(sema, statement->data.compound.items[item_index])) {
      if (push_scope) {
        fcc_sema_pop_scope(sema);
      }

      fcc_sema_pop_recursion(sema);
      return false;
    }
  }

  if (push_scope) {
    fcc_sema_pop_scope(sema);
  }

  fcc_sema_pop_recursion(sema);
  return true;
}

static bool fcc_sema_check_return_statement(FccSema* sema, const FccAstStatement* statement) {
  FccSemaExpressionResult expression_result;

  assert(sema != NULL);
  assert(statement != NULL);

  if (statement->data.return_statement.expression == NULL) {
    if (!fcc_type_equals(sema->current_function_return_type_id, fcc_sema_void_type(sema))) {
      fcc_sema_emit(sema, statement->span, FCC_DIAG_SEVERITY_ERROR,
                    "non-void function must return a value");
    }

    return true;
  }

  if (!fcc_sema_check_expression(sema, statement->data.return_statement.expression,
                                 &expression_result)) {
    return false;
  }

  if (fcc_type_equals(sema->current_function_return_type_id, fcc_sema_void_type(sema))) {
    fcc_sema_emit(sema, statement->span, FCC_DIAG_SEVERITY_ERROR,
                  "void function must not return a value");
    return true;
  }

  if (expression_result.is_valid &&
      !fcc_sema_expression_result_can_initialize_type(sema, sema->current_function_return_type_id,
                                                      &expression_result)) {
    fcc_sema_emit(sema, statement->span, FCC_DIAG_SEVERITY_ERROR,
                  "return expression type does not match function return type");
  }

  return true;
}

static bool fcc_sema_check_declaration_statement(FccSema* sema, const FccAstStatement* statement) {
  FccSymbol symbol;
  FccStorageClass storage_class;
  FccTypeId type_id;

  assert(sema != NULL);
  assert(statement != NULL);

  type_id = fcc_sema_type_from_ast(sema, &statement->data.declaration.type);
  if (type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_handle_invalid_type_id(sema, statement->span);
  }
  type_id = fcc_sema_complete_unsized_array_from_initializer(
      sema, type_id, statement->data.declaration.initializer);
  if (type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_out_of_memory(sema, statement->span);
  }

  if (statement->data.declaration.name == NULL) {
    return true;
  }

  storage_class = fcc_sema_storage_class_from_ast(
      statement->data.declaration.syntax.decl_specifiers.storage_class);
  if ((storage_class != FCC_STORAGE_CLASS_TYPEDEF) &&
      !fcc_sema_is_valid_object_type(sema, type_id)) {
    fcc_sema_emit_named_message(sema, statement->span, FCC_DIAG_SEVERITY_ERROR, "local '",
                                statement->data.declaration.name,
                                "' cannot have type void or incomplete type");
  }

  memset(&symbol, 0, sizeof(symbol));
  symbol.kind =
      (storage_class == FCC_STORAGE_CLASS_TYPEDEF) ? FCC_SYMBOL_TYPEDEF : FCC_SYMBOL_LOCAL;
  symbol.storage_class = storage_class;
  symbol.type_id = type_id;
  symbol.span = statement->span;
  symbol.name = statement->data.declaration.name;
  if (!fcc_sema_define_named_symbol(sema, &symbol, "local")) {
    return false;
  }

  if (!fcc_sema_record_object_info(sema, statement, statement->data.declaration.name, symbol.kind,
                                   storage_class, type_id, false, statement->span)) {
    return false;
  }

  if (storage_class == FCC_STORAGE_CLASS_TYPEDEF) {
    if (statement->data.declaration.initializer != NULL) {
      fcc_sema_emit(sema, statement->data.declaration.initializer->span, FCC_DIAG_SEVERITY_ERROR,
                    "typedef declarations cannot have initializers");
    }

    return true;
  }

  if (statement->data.declaration.initializer == NULL) {
    return true;
  }

  return fcc_sema_check_initializer(sema, type_id, statement->data.declaration.initializer, false);
}

static bool fcc_sema_check_if_statement(FccSema* sema, const FccAstStatement* statement) {
  assert(sema != NULL);
  assert(statement != NULL);

  if (!fcc_sema_require_int_expression(sema, statement->data.if_statement.condition,
                                       "if condition must have type int")) {
    return false;
  }

  if (!fcc_sema_check_statement(sema, statement->data.if_statement.then_statement)) {
    return false;
  }

  if ((statement->data.if_statement.else_statement != NULL) &&
      !fcc_sema_check_statement(sema, statement->data.if_statement.else_statement)) {
    return false;
  }

  return true;
}

static bool fcc_sema_check_while_statement(FccSema* sema, const FccAstStatement* statement) {
  assert(sema != NULL);
  assert(statement != NULL);

  if (!fcc_sema_require_int_expression(sema, statement->data.while_statement.condition,
                                       "while condition must have type int")) {
    return false;
  }

  ++sema->loop_depth;
  if (!fcc_sema_check_statement(sema, statement->data.while_statement.body)) {
    --sema->loop_depth;
    return false;
  }

  --sema->loop_depth;
  return true;
}

static bool fcc_sema_check_do_while_statement(FccSema* sema, const FccAstStatement* statement) {
  assert(sema != NULL);
  assert(statement != NULL);

  ++sema->loop_depth;
  if (!fcc_sema_check_statement(sema, statement->data.do_while_statement.body)) {
    --sema->loop_depth;
    return false;
  }

  --sema->loop_depth;
  return fcc_sema_require_int_expression(sema, statement->data.do_while_statement.condition,
                                         "do-while condition must have type int");
}

static bool fcc_sema_check_for_statement(FccSema* sema, const FccAstStatement* statement) {
  FccSemaExpressionResult update_result;

  assert(sema != NULL);
  assert(statement != NULL);

  if (!fcc_sema_push_scope(sema, statement->span)) {
    return false;
  }

  if ((statement->data.for_statement.init_statement != NULL) &&
      !fcc_sema_check_statement(sema, statement->data.for_statement.init_statement)) {
    fcc_sema_pop_scope(sema);
    return false;
  }

  if ((statement->data.for_statement.condition != NULL) &&
      !fcc_sema_require_int_expression(sema, statement->data.for_statement.condition,
                                       "for condition must have type int")) {
    fcc_sema_pop_scope(sema);
    return false;
  }

  if ((statement->data.for_statement.update != NULL) &&
      !fcc_sema_check_expression(sema, statement->data.for_statement.update, &update_result)) {
    fcc_sema_pop_scope(sema);
    return false;
  }

  ++sema->loop_depth;
  if (!fcc_sema_check_statement(sema, statement->data.for_statement.body)) {
    --sema->loop_depth;
    fcc_sema_pop_scope(sema);
    return false;
  }

  --sema->loop_depth;
  fcc_sema_pop_scope(sema);
  return true;
}

static bool fcc_sema_check_switch_statement(FccSema* sema, const FccAstStatement* statement) {
  assert(sema != NULL);
  assert(statement != NULL);

  if (!fcc_sema_require_int_expression(sema, statement->data.switch_statement.condition,
                                       "switch condition must have integer type")) {
    return false;
  }

  ++sema->switch_depth;
  if (!fcc_sema_check_statement(sema, statement->data.switch_statement.body)) {
    --sema->switch_depth;
    return false;
  }

  --sema->switch_depth;
  return true;
}

static bool fcc_sema_check_case_statement(FccSema* sema, const FccAstStatement* statement) {
  FccSemaExpressionResult value_result;

  assert(sema != NULL);
  assert(statement != NULL);

  if (sema->switch_depth == 0) {
    fcc_sema_emit(sema, statement->span, FCC_DIAG_SEVERITY_ERROR,
                  "case label is only valid inside a switch");
  }

  if (!fcc_sema_check_expression(sema, statement->data.case_statement.value, &value_result)) {
    return false;
  }

  if (value_result.is_valid &&
      (!fcc_type_is_integer(&sema->result->type_context, value_result.type_id) ||
       !value_result.has_integer_constant)) {
    fcc_sema_emit(sema, statement->data.case_statement.value->span, FCC_DIAG_SEVERITY_ERROR,
                  "case label must be an integer constant expression");
  }

  return fcc_sema_check_statement(sema, statement->data.case_statement.statement);
}

static bool fcc_sema_check_default_statement(FccSema* sema, const FccAstStatement* statement) {
  assert(sema != NULL);
  assert(statement != NULL);

  if (sema->switch_depth == 0) {
    fcc_sema_emit(sema, statement->span, FCC_DIAG_SEVERITY_ERROR,
                  "default label is only valid inside a switch");
  }

  return fcc_sema_check_statement(sema, statement->data.default_statement.statement);
}

static bool fcc_sema_check_statement(FccSema* sema, const FccAstStatement* statement) {
  FccSemaExpressionResult expression_result;
  bool ok;

  assert(sema != NULL);
  assert(statement != NULL);

  ok = true;
  switch (statement->kind) {
    case FCC_AST_STATEMENT_COMPOUND:
      ok = fcc_sema_check_compound_statement(sema, statement, true);
      break;
    case FCC_AST_STATEMENT_RETURN:
      ok = fcc_sema_check_return_statement(sema, statement);
      break;
    case FCC_AST_STATEMENT_EXPRESSION:
      if (statement->data.expression_statement.expression != NULL) {
        ok = fcc_sema_check_expression(sema, statement->data.expression_statement.expression,
                                       &expression_result);
      }

      break;
    case FCC_AST_STATEMENT_DECLARATION:
      ok = fcc_sema_check_declaration_statement(sema, statement);
      break;
    case FCC_AST_STATEMENT_STATIC_ASSERT:
      ok = fcc_sema_check_static_assert(sema, &statement->data.static_assertion);
      break;
    case FCC_AST_STATEMENT_IF:
      ok = fcc_sema_check_if_statement(sema, statement);
      break;
    case FCC_AST_STATEMENT_WHILE:
      ok = fcc_sema_check_while_statement(sema, statement);
      break;
    case FCC_AST_STATEMENT_DO_WHILE:
      ok = fcc_sema_check_do_while_statement(sema, statement);
      break;
    case FCC_AST_STATEMENT_FOR:
      ok = fcc_sema_check_for_statement(sema, statement);
      break;
    case FCC_AST_STATEMENT_SWITCH:
      ok = fcc_sema_check_switch_statement(sema, statement);
      break;
    case FCC_AST_STATEMENT_CASE:
      ok = fcc_sema_check_case_statement(sema, statement);
      break;
    case FCC_AST_STATEMENT_DEFAULT:
      ok = fcc_sema_check_default_statement(sema, statement);
      break;
    case FCC_AST_STATEMENT_BREAK:
      if ((sema->loop_depth == 0) && (sema->switch_depth == 0)) {
        fcc_sema_emit(sema, statement->span, FCC_DIAG_SEVERITY_ERROR,
                      "break is only valid inside a loop or switch");
      }

      break;
    case FCC_AST_STATEMENT_CONTINUE:
      if (sema->loop_depth == 0) {
        fcc_sema_emit(sema, statement->span, FCC_DIAG_SEVERITY_ERROR,
                      "continue is only valid inside a loop");
      }

      break;
    case FCC_AST_STATEMENT_GOTO:
      break;
    case FCC_AST_STATEMENT_LABEL:
      ok = fcc_sema_check_statement(sema, statement->data.label_statement.statement);
      break;
  }

  return ok;
}

static bool fcc_sema_declare_function(FccSema* sema,
                                      const FccAstFunctionDefinition* function_definition) {
  FccSymbol symbol;
  FccSymbol* existing_symbol;
  FccTypeId function_type_id;
  FccTypeId parameter_type_ids[FCC_MAX_FUNCTION_PARAMETERS];
  FccTypeId return_type_id;
  size_t parameter_index;
  FccStorageClass storage_class;
  char note_message[FCC_MAX_DIAG_MESSAGE_LENGTH];
  int written;

  assert(sema != NULL);
  assert(function_definition != NULL);

  return_type_id = fcc_sema_type_from_ast(sema, &function_definition->return_type);
  if (return_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_handle_invalid_type_id(sema, function_definition->span);
  }

  if (function_definition->parameter_count > FCC_MAX_FUNCTION_PARAMETERS) {
    fcc_sema_emit(sema, function_definition->span, FCC_DIAG_SEVERITY_FATAL,
                  "parameter count exceeds FCC_MAX_FUNCTION_PARAMETERS");
    return false;
  }

  for (parameter_index = 0; parameter_index < function_definition->parameter_count;
       ++parameter_index) {
    parameter_type_ids[parameter_index] =
        fcc_sema_type_from_ast(sema, &function_definition->parameters[parameter_index].type);
    if (parameter_type_ids[parameter_index] == FCC_TYPE_ID_INVALID) {
      return fcc_sema_handle_invalid_type_id(sema,
                                             function_definition->parameters[parameter_index].span);
    }

    parameter_type_ids[parameter_index] =
        fcc_type_decay_array(&sema->result->type_context, parameter_type_ids[parameter_index]);
    if (parameter_type_ids[parameter_index] == FCC_TYPE_ID_INVALID) {
      return fcc_sema_out_of_memory(sema, function_definition->parameters[parameter_index].span);
    }
  }

  function_type_id = fcc_type_context_get_function(
      &sema->result->type_context, return_type_id, parameter_type_ids,
      function_definition->parameter_count, function_definition->is_variadic);
  if (function_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_out_of_memory(sema, function_definition->span);
  }

  storage_class =
      fcc_sema_storage_class_from_ast(function_definition->syntax.decl_specifiers.storage_class);
  if (storage_class == FCC_STORAGE_CLASS_TYPEDEF) {
    fcc_sema_emit(sema, function_definition->span, FCC_DIAG_SEVERITY_ERROR,
                  "typedef function declarations are not supported in this phase");
    return true;
  }

  existing_symbol =
      fcc_symbol_table_lookup_mutable_current_scope(&sema->symbols, function_definition->name);
  if (existing_symbol != NULL) {
    if ((existing_symbol->kind != FCC_SYMBOL_FUNCTION) ||
        !fcc_sema_function_signature_matches_symbol(sema, function_type_id, existing_symbol)) {
      written = snprintf(note_message, sizeof(note_message),
                         "previous declaration of '%s' was here", function_definition->name);
      if ((written < 0) || ((size_t)written >= sizeof(note_message))) {
        fcc_sema_emit(sema, function_definition->span, FCC_DIAG_SEVERITY_ERROR,
                      "diagnostic formatting failure");
        return true;
      }

      fcc_sema_emit_named_message(sema, function_definition->span, FCC_DIAG_SEVERITY_ERROR,
                                  "conflicting declaration of function '",
                                  function_definition->name, "'");
      fcc_sema_emit(sema, existing_symbol->span, FCC_DIAG_SEVERITY_NOTE, note_message);
      return true;
    }

    if (function_definition->has_body && existing_symbol->has_body) {
      written = snprintf(note_message, sizeof(note_message), "previous definition of '%s' was here",
                         function_definition->name);
      if ((written < 0) || ((size_t)written >= sizeof(note_message))) {
        fcc_sema_emit(sema, function_definition->span, FCC_DIAG_SEVERITY_ERROR,
                      "diagnostic formatting failure");
        return true;
      }

      fcc_sema_emit_named_message(sema, function_definition->span, FCC_DIAG_SEVERITY_ERROR,
                                  "redefinition of function '", function_definition->name, "'");
      fcc_sema_emit(sema, existing_symbol->span, FCC_DIAG_SEVERITY_NOTE, note_message);
      return true;
    }

    if (function_definition->has_body) {
      existing_symbol->has_body = true;
    }
  } else {
    memset(&symbol, 0, sizeof(symbol));
    symbol.kind = FCC_SYMBOL_FUNCTION;
    symbol.storage_class = storage_class;
    symbol.type_id = function_type_id;
    symbol.span = function_definition->span;
    symbol.has_body = function_definition->has_body;
    symbol.name = function_definition->name;
    if (!fcc_sema_define_named_symbol(sema, &symbol, "function")) {
      return false;
    }
  }

  return fcc_sema_record_object_info(sema, function_definition, function_definition->name,
                                     FCC_SYMBOL_FUNCTION, storage_class, function_type_id,
                                     function_definition->has_body, function_definition->span);
}

static bool fcc_sema_declare_global(FccSema* sema, const FccAstGlobalVariable* global_variable) {
  FccSymbol symbol;
  FccStorageClass storage_class;
  FccTypeId type_id;

  assert(sema != NULL);
  assert(global_variable != NULL);

  type_id = fcc_sema_type_from_ast(sema, &global_variable->type);
  if (type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_handle_invalid_type_id(sema, global_variable->span);
  }
  type_id =
      fcc_sema_complete_unsized_array_from_initializer(sema, type_id, global_variable->initializer);
  if (type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_out_of_memory(sema, global_variable->span);
  }

  if (global_variable->name == NULL) {
    return true;
  }

  storage_class =
      fcc_sema_storage_class_from_ast(global_variable->syntax.decl_specifiers.storage_class);
  if ((storage_class != FCC_STORAGE_CLASS_TYPEDEF) &&
      !fcc_sema_is_valid_object_type(sema, type_id)) {
    fcc_sema_emit_named_message(sema, global_variable->span, FCC_DIAG_SEVERITY_ERROR, "global '",
                                global_variable->name,
                                "' cannot have type void or incomplete type");
  }

  memset(&symbol, 0, sizeof(symbol));
  symbol.kind =
      (storage_class == FCC_STORAGE_CLASS_TYPEDEF) ? FCC_SYMBOL_TYPEDEF : FCC_SYMBOL_GLOBAL;
  symbol.storage_class = storage_class;
  symbol.type_id = type_id;
  symbol.span = global_variable->span;
  symbol.name = global_variable->name;
  if (!fcc_sema_define_named_symbol(sema, &symbol, "global")) {
    return false;
  }

  return fcc_sema_record_object_info(sema, global_variable, global_variable->name, symbol.kind,
                                     storage_class, type_id, false, global_variable->span);
}

static bool fcc_sema_check_global_variable(FccSema* sema,
                                           const FccAstGlobalVariable* global_variable) {
  FccTypeId type_id;

  assert(sema != NULL);
  assert(global_variable != NULL);

  if (global_variable->name == NULL) {
    return true;
  }

  if (global_variable->initializer == NULL) {
    return true;
  }

  if (global_variable->syntax.decl_specifiers.storage_class == FCC_AST_STORAGE_CLASS_TYPEDEF) {
    fcc_sema_emit(sema, global_variable->initializer->span, FCC_DIAG_SEVERITY_ERROR,
                  "typedef declarations cannot have initializers");
    return true;
  }

  type_id = fcc_sema_type_from_ast(sema, &global_variable->type);
  if (type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_handle_invalid_type_id(sema, global_variable->span);
  }
  type_id =
      fcc_sema_complete_unsized_array_from_initializer(sema, type_id, global_variable->initializer);
  if (type_id == FCC_TYPE_ID_INVALID) {
    return fcc_sema_out_of_memory(sema, global_variable->span);
  }

  return fcc_sema_check_initializer(sema, type_id, global_variable->initializer, true);
}

static bool
fcc_sema_check_function_definition(FccSema* sema,
                                   const FccAstFunctionDefinition* function_definition) {
  size_t parameter_index;

  assert(sema != NULL);
  assert(function_definition != NULL);

  if (!function_definition->has_body) {
    return true;
  }

  if (!fcc_sema_push_scope(sema, function_definition->span)) {
    return false;
  }

  sema->current_function_return_type_id =
      fcc_sema_type_from_ast(sema, &function_definition->return_type);
  if (sema->current_function_return_type_id == FCC_TYPE_ID_INVALID) {
    fcc_sema_pop_scope(sema);
    return fcc_sema_handle_invalid_type_id(sema, function_definition->span);
  }

  for (parameter_index = 0; parameter_index < function_definition->parameter_count;
       ++parameter_index) {
    FccSymbol symbol;
    FccStorageClass storage_class;
    FccTypeId type_id;
    const FccAstParameter* parameter;

    parameter = &function_definition->parameters[parameter_index];
    type_id = fcc_sema_type_from_ast(sema, &parameter->type);
    if (type_id == FCC_TYPE_ID_INVALID) {
      fcc_sema_pop_scope(sema);
      return fcc_sema_handle_invalid_type_id(sema, parameter->span);
    }

    type_id = fcc_type_decay_array(&sema->result->type_context, type_id);
    if (type_id == FCC_TYPE_ID_INVALID) {
      fcc_sema_pop_scope(sema);
      return fcc_sema_out_of_memory(sema, parameter->span);
    }

    if (parameter->name == NULL) {
      fcc_sema_emit(sema, parameter->span, FCC_DIAG_SEVERITY_ERROR,
                    "function definition parameter requires an identifier");
      continue;
    }

    if (!fcc_sema_is_valid_object_type(sema, type_id)) {
      fcc_sema_emit_named_message(sema, parameter->span, FCC_DIAG_SEVERITY_ERROR, "parameter '",
                                  parameter->name, "' cannot have type void or incomplete type");
      continue;
    }

    storage_class =
        fcc_sema_storage_class_from_ast(parameter->syntax.decl_specifiers.storage_class);
    if (storage_class != FCC_STORAGE_CLASS_NONE) {
      fcc_sema_emit_named_message(sema, parameter->span, FCC_DIAG_SEVERITY_ERROR, "parameter '",
                                  parameter->name,
                                  "' uses an unsupported storage class in this phase");
    }

    memset(&symbol, 0, sizeof(symbol));
    symbol.kind = FCC_SYMBOL_PARAMETER;
    symbol.storage_class = FCC_STORAGE_CLASS_NONE;
    symbol.type_id = type_id;
    symbol.span = parameter->span;
    symbol.name = parameter->name;
    if (!fcc_sema_define_named_symbol(sema, &symbol, "parameter")) {
      fcc_sema_pop_scope(sema);
      return false;
    }

    if (!fcc_sema_record_object_info(sema, parameter, parameter->name, FCC_SYMBOL_PARAMETER,
                                     FCC_STORAGE_CLASS_NONE, type_id, false, parameter->span)) {
      fcc_sema_pop_scope(sema);
      return false;
    }
  }

  if (!fcc_sema_check_compound_statement(sema, function_definition->body, false)) {
    fcc_sema_pop_scope(sema);
    sema->current_function_return_type_id = FCC_TYPE_ID_INVALID;
    return false;
  }

  fcc_sema_pop_scope(sema);
  sema->current_function_return_type_id = FCC_TYPE_ID_INVALID;
  return true;
}

void fcc_sema_result_init(FccSemaResult* result) {
  assert(result != NULL);

  fcc_type_context_init(&result->type_context);
  result->expression_infos = NULL;
  result->expression_count = 0;
  result->expression_capacity = 0;
  result->object_infos = NULL;
  result->object_count = 0;
  result->object_capacity = 0;
}

void fcc_sema_result_dispose(FccSemaResult* result) {
  if (result == NULL) {
    return;
  }

  free(result->expression_infos);
  free(result->object_infos);
  fcc_type_context_dispose(&result->type_context);
  result->expression_infos = NULL;
  result->expression_count = 0;
  result->expression_capacity = 0;
  result->object_infos = NULL;
  result->object_count = 0;
  result->object_capacity = 0;
}

const FccSemaExpressionInfo*
fcc_sema_result_find_expression_info(const FccSemaResult* result,
                                     const FccAstExpression* expression) {
  size_t index;

  assert(result != NULL);
  assert(expression != NULL);

  for (index = result->expression_count; index > 0; --index) {
    if (result->expression_infos[index - 1].expression == expression) {
      return &result->expression_infos[index - 1];
    }
  }

  return NULL;
}

const FccSemaObjectInfo* fcc_sema_result_find_object_info(const FccSemaResult* result,
                                                          const void* node) {
  size_t index;

  assert(result != NULL);
  assert(node != NULL);

  for (index = result->object_count; index > 0; --index) {
    if (result->object_infos[index - 1].node == node) {
      return &result->object_infos[index - 1];
    }
  }

  return NULL;
}

bool fcc_sema_analyze_translation_unit(const FccSourceFile* source_file,
                                       const FccAstTranslationUnit* translation_unit,
                                       FccDiagnostics* diagnostics, FccSemaResult* result) {
  FccSema sema;
  size_t global_index;
  size_t function_index;
  size_t static_assertion_index;
  size_t starting_error_count;

  assert(source_file != NULL);
  assert(translation_unit != NULL);
  assert(diagnostics != NULL);
  assert(result != NULL);

  sema.source_file = source_file;
  sema.diagnostics = diagnostics;
  sema.result = result;
  sema.tags = NULL;
  sema.tag_count = 0;
  sema.tag_capacity = 0;
  sema.tag_scope_offsets = NULL;
  sema.tag_scope_count = 0;
  sema.tag_scope_capacity = 0;
  sema.current_function_return_type_id = FCC_TYPE_ID_INVALID;
  sema.loop_depth = 0;
  sema.switch_depth = 0;
  sema.recursion_depth = 0;
  fcc_symbol_table_init(&sema.symbols);
  starting_error_count = diagnostics->error_count;

  if (!fcc_sema_push_scope(&sema, translation_unit->span)) {
    free(sema.tags);
    free(sema.tag_scope_offsets);
    fcc_symbol_table_dispose(&sema.symbols);
    return false;
  }

  for (global_index = 0; global_index < translation_unit->global_count; ++global_index) {
    if (!fcc_sema_declare_global(&sema, translation_unit->globals[global_index])) {
      fcc_sema_pop_scope(&sema);
      free(sema.tags);
      free(sema.tag_scope_offsets);
      fcc_symbol_table_dispose(&sema.symbols);
      return false;
    }
  }

  for (function_index = 0; function_index < translation_unit->function_count; ++function_index) {
    if (!fcc_sema_declare_function(&sema, translation_unit->functions[function_index])) {
      fcc_sema_pop_scope(&sema);
      free(sema.tags);
      free(sema.tag_scope_offsets);
      fcc_symbol_table_dispose(&sema.symbols);
      return false;
    }
  }

  for (static_assertion_index = 0;
       static_assertion_index < translation_unit->static_assertion_count;
       ++static_assertion_index) {
    if (!fcc_sema_check_static_assert(&sema,
                                      translation_unit->static_assertions[static_assertion_index])) {
      fcc_sema_pop_scope(&sema);
      free(sema.tags);
      free(sema.tag_scope_offsets);
      fcc_symbol_table_dispose(&sema.symbols);
      return false;
    }
  }

  for (global_index = 0; global_index < translation_unit->global_count; ++global_index) {
    if (!fcc_sema_check_global_variable(&sema, translation_unit->globals[global_index])) {
      fcc_sema_pop_scope(&sema);
      free(sema.tags);
      free(sema.tag_scope_offsets);
      fcc_symbol_table_dispose(&sema.symbols);
      return false;
    }
  }

  for (function_index = 0; function_index < translation_unit->function_count; ++function_index) {
    if (!fcc_sema_check_function_definition(&sema, translation_unit->functions[function_index])) {
      fcc_sema_pop_scope(&sema);
      free(sema.tags);
      free(sema.tag_scope_offsets);
      fcc_symbol_table_dispose(&sema.symbols);
      return false;
    }
  }

  fcc_sema_pop_scope(&sema);
  free(sema.tags);
  free(sema.tag_scope_offsets);
  fcc_symbol_table_dispose(&sema.symbols);
  return diagnostics->error_count == starting_error_count;
}

bool fcc_sema_check_translation_unit(const FccSourceFile* source_file,
                                     const FccAstTranslationUnit* translation_unit,
                                     FccDiagnostics* diagnostics) {
  FccSemaResult result;
  bool ok;

  assert(source_file != NULL);
  assert(translation_unit != NULL);
  assert(diagnostics != NULL);

  fcc_sema_result_init(&result);
  ok = fcc_sema_analyze_translation_unit(source_file, translation_unit, diagnostics, &result);
  fcc_sema_result_dispose(&result);
  return ok;
}
