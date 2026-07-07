// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/codegen.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fcc/base.h"

typedef struct FccCodegenBinding {
  int stack_offset;
  const char* name;
  FccTypeId type_id;
} FccCodegenBinding;

typedef struct FccCodegenScope {
  size_t first_binding_index;
} FccCodegenScope;

typedef struct FccCodegenLoopTarget {
  size_t break_label_id;
  size_t continue_label_id;
} FccCodegenLoopTarget;

typedef struct FccCodegenSwitchCase {
  const FccAstStatement* statement;
  int64_t value;
  size_t label_id;
  bool is_default;
} FccCodegenSwitchCase;

typedef struct FccCodegenStringLiteral {
  const FccAstExpression* expression;
  size_t label_id;
} FccCodegenStringLiteral;

/*
 * Per-function codegen state is deliberately local. It contains the computed
 * stack layout, lexical bindings, break/continue targets, switch cases, and
 * label allocator needed to lower one typed function to NASM.
 */
typedef struct FccCodegenFunction {
  FILE* stream;
  const FccSourceFile* source_file;
  const FccAstTranslationUnit* translation_unit;
  FccSemaResult* sema_result;
  const FccCodegenStringLiteral* string_literals;
  size_t string_literal_count;
  const FccAstFunctionDefinition* function_definition;
  FccDiagnostics* diagnostics;
  FccCodegenBinding* bindings;
  size_t binding_count;
  size_t binding_capacity;
  FccCodegenScope* scopes;
  size_t scope_count;
  size_t scope_capacity;
  FccCodegenLoopTarget loop_targets[FCC_MAX_CODEGEN_DEPTH];
  size_t break_targets[FCC_MAX_CODEGEN_DEPTH];
  const FccCodegenSwitchCase* switch_cases;
  size_t switch_case_count;
  size_t loop_depth;
  size_t break_depth;
  size_t next_stack_offset;
  size_t total_stack_size;
  size_t next_label_id;
  size_t recursion_depth;
  size_t return_label_id;
} FccCodegenFunction;

static bool fcc_codegen_push_recursion(FccCodegenFunction* function, FccSourceSpan span) {
  assert(function != NULL);

  ++function->recursion_depth;
  if (function->recursion_depth > FCC_MAX_CODEGEN_DEPTH) {
    --function->recursion_depth;
    fcc_diag_emit_source(function->diagnostics, function->source_file, span,
                         FCC_DIAG_SEVERITY_FATAL,
                         "code generation nesting exceeds FCC_MAX_CODEGEN_DEPTH");
    return false;
  }

  return true;
}

static void fcc_codegen_pop_recursion(FccCodegenFunction* function) {
  assert(function != NULL);
  assert(function->recursion_depth > 0);

  --function->recursion_depth;
}

static bool fcc_codegen_out_of_memory(FccCodegenFunction* function, FccSourceSpan span) {
  assert(function != NULL);

  fcc_diag_emit_source(function->diagnostics, function->source_file, span, FCC_DIAG_SEVERITY_FATAL,
                       "out of memory");
  return false;
}

static bool fcc_codegen_emit_error(FccCodegenFunction* function, FccSourceSpan span,
                                   const char* message) {
  assert(function != NULL);
  assert(message != NULL);

  fcc_diag_emit_source(function->diagnostics, function->source_file, span, FCC_DIAG_SEVERITY_ERROR,
                       message);
  return false;
}

static bool fcc_codegen_check_stream(FccCodegenFunction* function) {
  assert(function != NULL);

  if (ferror(function->stream) != 0) {
    fcc_diag_emit_source(function->diagnostics, function->source_file,
                         function->function_definition->span, FCC_DIAG_SEVERITY_FATAL,
                         "assembly output stream write failed");
    return false;
  }

  return true;
}

static const FccSemaExpressionInfo*
fcc_codegen_find_expression_info(const FccCodegenFunction* function,
                                 const FccAstExpression* expression) {
  assert(function != NULL);
  assert(function->sema_result != NULL);
  assert(expression != NULL);

  return fcc_sema_result_find_expression_info(function->sema_result, expression);
}

static const FccSemaObjectInfo* fcc_codegen_find_object_info(const FccCodegenFunction* function,
                                                             const void* node) {
  assert(function != NULL);
  assert(function->sema_result != NULL);
  assert(node != NULL);

  return fcc_sema_result_find_object_info(function->sema_result, node);
}

static bool fcc_codegen_object_requires_storage(const FccSemaObjectInfo* object_info) {
  assert(object_info != NULL);

  if (object_info->symbol_kind == FCC_SYMBOL_TYPEDEF) {
    return false;
  }

  if ((object_info->storage_class == FCC_STORAGE_CLASS_EXTERN) &&
      ((object_info->symbol_kind == FCC_SYMBOL_LOCAL) ||
       (object_info->symbol_kind == FCC_SYMBOL_GLOBAL))) {
    return false;
  }

  return true;
}

static FccTypeId fcc_codegen_expression_type_id(const FccCodegenFunction* function,
                                                const FccAstExpression* expression) {
  const FccSemaExpressionInfo* expression_info;

  expression_info = fcc_codegen_find_expression_info(function, expression);
  if (expression_info == NULL) {
    return FCC_TYPE_ID_INVALID;
  }

  return expression_info->type_id;
}

static bool fcc_codegen_expression_is_lvalue(const FccCodegenFunction* function,
                                             const FccAstExpression* expression) {
  const FccSemaExpressionInfo* expression_info;

  expression_info = fcc_codegen_find_expression_info(function, expression);
  return (expression_info != NULL) && expression_info->is_lvalue;
}

static size_t fcc_codegen_storage_size(const FccCodegenFunction* function, FccTypeId type_id) {
  bool size_ok;
  size_t size;

  assert(function != NULL);

  size = fcc_type_size_of(&function->sema_result->type_context, type_id, &size_ok);
  if (!size_ok) {
    return 0;
  }

  return size;
}

static bool fcc_codegen_type_is_aggregate(const FccCodegenFunction* function, FccTypeId type_id) {
  const FccType* type;

  assert(function != NULL);

  type_id = fcc_type_resolve_typedef(&function->sema_result->type_context, type_id);
  type = fcc_type_context_get(&function->sema_result->type_context, type_id);
  return (type != NULL) && ((type->kind == FCC_TYPE_ARRAY) || (type->kind == FCC_TYPE_STRUCT) ||
                            (type->kind == FCC_TYPE_UNION));
}

static bool fcc_codegen_type_is_void(const FccCodegenFunction* function, FccTypeId type_id) {
  const FccType* type;

  assert(function != NULL);

  type_id = fcc_type_resolve_typedef(&function->sema_result->type_context, type_id);
  type = fcc_type_context_get(&function->sema_result->type_context, type_id);
  return (type != NULL) && (type->kind == FCC_TYPE_VOID);
}

static bool fcc_codegen_type_is_signed_integer(const FccCodegenFunction* function,
                                               FccTypeId type_id) {
  const FccType* type;

  assert(function != NULL);

  type_id = fcc_type_resolve_typedef(&function->sema_result->type_context, type_id);
  type = fcc_type_context_get(&function->sema_result->type_context, type_id);
  if (type == NULL) {
    return false;
  }

  switch (type->kind) {
    case FCC_TYPE_CHAR:
    case FCC_TYPE_SIGNED_CHAR:
    case FCC_TYPE_SHORT:
    case FCC_TYPE_INT:
    case FCC_TYPE_LONG:
    case FCC_TYPE_LONG_LONG:
    case FCC_TYPE_ENUM:
      return true;
    default:
      return false;
  }
}

static bool fcc_codegen_emit_coerce_scalar(FccCodegenFunction* function, FccTypeId source_type_id,
                                           FccTypeId target_type_id, FccSourceSpan span) {
  size_t source_size;
  size_t target_size;
  bool source_is_signed;
  bool target_is_signed;

  assert(function != NULL);

  if (fcc_codegen_type_is_void(function, target_type_id)) {
    return true;
  }

  if (fcc_codegen_type_is_aggregate(function, target_type_id) ||
      fcc_codegen_type_is_aggregate(function, source_type_id)) {
    return true;
  }

  target_size = fcc_codegen_storage_size(function, target_type_id);
  if (target_size == 0) {
    return fcc_codegen_emit_error(function, span, "unsupported scalar conversion target type");
  }

  source_size = fcc_codegen_storage_size(function, source_type_id);
  if (source_size == 0) {
    return true;
  }

  source_is_signed = fcc_codegen_type_is_signed_integer(function, source_type_id);
  target_is_signed = fcc_codegen_type_is_signed_integer(function, target_type_id);
  if (target_size == 1) {
    fputs(target_is_signed ? "  movsx rax, al\n" : "  movzx eax, al\n", function->stream);
    return fcc_codegen_check_stream(function);
  }

  if (target_size == 2) {
    fputs(target_is_signed ? "  movsx rax, ax\n" : "  movzx eax, ax\n", function->stream);
    return fcc_codegen_check_stream(function);
  }

  if (target_size == 4) {
    fputs(target_is_signed ? "  movsxd rax, eax\n" : "  mov eax, eax\n", function->stream);
    return fcc_codegen_check_stream(function);
  }

  if (target_size == 8) {
    if (source_size == 1) {
      fputs(source_is_signed ? "  movsx rax, al\n" : "  movzx eax, al\n", function->stream);
    } else if (source_size == 2) {
      fputs(source_is_signed ? "  movsx rax, ax\n" : "  movzx eax, ax\n", function->stream);
    } else if (source_size == 4) {
      fputs(source_is_signed ? "  movsxd rax, eax\n" : "  mov eax, eax\n", function->stream);
    }

    return fcc_codegen_check_stream(function);
  }

  return fcc_codegen_emit_error(function, span, "unsupported scalar conversion target width");
}

static size_t fcc_codegen_allocate_label(FccCodegenFunction* function);
static void fcc_codegen_emit_local_label(FILE* stream, size_t label_id);
static void fcc_codegen_emit_jump(FILE* stream, const char* mnemonic, size_t label_id);

static bool fcc_codegen_emit_load_from_memory(FccCodegenFunction* function, const char* memory_text,
                                              FccTypeId type_id) {
  size_t storage_size;

  assert(function != NULL);
  assert(memory_text != NULL);

  if (fcc_codegen_type_is_aggregate(function, type_id)) {
    fprintf(function->stream, "  lea rax, %s\n", memory_text);
    return fcc_codegen_check_stream(function);
  }

  storage_size = fcc_codegen_storage_size(function, type_id);
  if (storage_size == 1) {
    fprintf(function->stream, "  movzx eax, byte %s\n", memory_text);
    return fcc_codegen_check_stream(function);
  }

  if (storage_size == 2) {
    fprintf(function->stream, "  movzx eax, word %s\n", memory_text);
    return fcc_codegen_check_stream(function);
  }

  if (storage_size == 4) {
    fprintf(function->stream, "  mov eax, dword %s\n", memory_text);
    return fcc_codegen_check_stream(function);
  }

  if (storage_size == 8) {
    fprintf(function->stream, "  mov rax, qword %s\n", memory_text);
    return fcc_codegen_check_stream(function);
  }

  return fcc_codegen_emit_error(function, function->function_definition->span,
                                "unsupported load width in code generation");
}

static bool fcc_codegen_emit_store_to_memory(FccCodegenFunction* function, const char* memory_text,
                                             FccTypeId type_id) {
  size_t storage_size;

  assert(function != NULL);
  assert(memory_text != NULL);

  storage_size = fcc_codegen_storage_size(function, type_id);
  if (storage_size == 1) {
    fprintf(function->stream, "  mov byte %s, al\n", memory_text);
    return fcc_codegen_check_stream(function);
  }

  if (storage_size == 2) {
    fprintf(function->stream, "  mov word %s, ax\n", memory_text);
    return fcc_codegen_check_stream(function);
  }

  if (storage_size == 4) {
    fprintf(function->stream, "  mov dword %s, eax\n", memory_text);
    return fcc_codegen_check_stream(function);
  }

  if (storage_size == 8) {
    fprintf(function->stream, "  mov qword %s, rax\n", memory_text);
    return fcc_codegen_check_stream(function);
  }

  return fcc_codegen_emit_error(function, function->function_definition->span,
                                "unsupported store width in code generation");
}

static bool fcc_codegen_emit_memory_copy(FccCodegenFunction* function, size_t byte_count) {
  size_t loop_label_id;
  size_t done_label_id;

  assert(function != NULL);

  loop_label_id = fcc_codegen_allocate_label(function);
  done_label_id = fcc_codegen_allocate_label(function);
  fprintf(function->stream, "  mov rdx, rax\n");
  fprintf(function->stream, "  mov r8, rcx\n");
  fprintf(function->stream, "  mov r9, %zu\n", byte_count);
  fcc_codegen_emit_local_label(function->stream, loop_label_id);
  fputs("  cmp r9, 0\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "je", done_label_id);
  fputs("  mov al, byte [rdx]\n", function->stream);
  fputs("  mov byte [r8], al\n", function->stream);
  fputs("  inc rdx\n", function->stream);
  fputs("  inc r8\n", function->stream);
  fputs("  dec r9\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "jmp", loop_label_id);
  fcc_codegen_emit_local_label(function->stream, done_label_id);
  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_memory_zero(FccCodegenFunction* function, size_t byte_count) {
  size_t loop_label_id;
  size_t done_label_id;

  assert(function != NULL);

  loop_label_id = fcc_codegen_allocate_label(function);
  done_label_id = fcc_codegen_allocate_label(function);
  fprintf(function->stream, "  mov r8, rax\n");
  fprintf(function->stream, "  mov r9, %zu\n", byte_count);
  fcc_codegen_emit_local_label(function->stream, loop_label_id);
  fputs("  cmp r9, 0\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "je", done_label_id);
  fputs("  mov byte [r8], 0\n", function->stream);
  fputs("  inc r8\n", function->stream);
  fputs("  dec r9\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "jmp", loop_label_id);
  fcc_codegen_emit_local_label(function->stream, done_label_id);
  return fcc_codegen_check_stream(function);
}

static void fcc_codegen_format_stack_memory(char* buffer, size_t buffer_size, int stack_offset,
                                            size_t member_offset) {
  assert(buffer != NULL);
  assert(buffer_size > 0);

  if (member_offset == 0) {
    (void)snprintf(buffer, buffer_size, "[rbp - %d]", stack_offset);
    return;
  }

  (void)snprintf(buffer, buffer_size, "[rbp - %d + %zu]", stack_offset, member_offset);
}

static const FccCodegenStringLiteral*
fcc_codegen_find_string_literal_in_table(const FccCodegenStringLiteral* string_literals,
                                         size_t string_literal_count,
                                         const FccAstExpression* expression) {
  size_t string_index;

  assert((string_literals != NULL) || (string_literal_count == 0));
  assert(expression != NULL);

  for (string_index = 0; string_index < string_literal_count; ++string_index) {
    if (string_literals[string_index].expression == expression) {
      return &string_literals[string_index];
    }
  }

  return NULL;
}

static const FccCodegenStringLiteral*
fcc_codegen_find_string_literal(const FccCodegenFunction* function,
                                const FccAstExpression* expression) {
  assert(function != NULL);
  assert(expression != NULL);

  return fcc_codegen_find_string_literal_in_table(function->string_literals,
                                                 function->string_literal_count, expression);
}

static bool fcc_codegen_ensure_binding_capacity(FccCodegenFunction* function, size_t capacity) {
  size_t new_capacity;
  FccCodegenBinding* new_bindings;

  assert(function != NULL);

  if (capacity <= function->binding_capacity) {
    return true;
  }

  new_capacity = function->binding_capacity;
  if (new_capacity == 0) {
    new_capacity = 16;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(FccCodegenBinding))) {
    return false;
  }

  new_bindings =
      (FccCodegenBinding*)realloc(function->bindings, new_capacity * sizeof(FccCodegenBinding));
  if (new_bindings == NULL) {
    return false;
  }

  function->bindings = new_bindings;
  function->binding_capacity = new_capacity;
  return true;
}

static bool fcc_codegen_ensure_scope_capacity(FccCodegenFunction* function, size_t capacity) {
  size_t new_capacity;
  FccCodegenScope* new_scopes;

  assert(function != NULL);

  if (capacity <= function->scope_capacity) {
    return true;
  }

  new_capacity = function->scope_capacity;
  if (new_capacity == 0) {
    new_capacity = 8;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(FccCodegenScope))) {
    return false;
  }

  new_scopes = (FccCodegenScope*)realloc(function->scopes, new_capacity * sizeof(FccCodegenScope));
  if (new_scopes == NULL) {
    return false;
  }

  function->scopes = new_scopes;
  function->scope_capacity = new_capacity;
  return true;
}

static bool fcc_codegen_push_scope(FccCodegenFunction* function, FccSourceSpan span) {
  FccCodegenScope* scope;

  assert(function != NULL);

  if (!fcc_codegen_ensure_scope_capacity(function, function->scope_count + 1)) {
    return fcc_codegen_out_of_memory(function, span);
  }

  scope = &function->scopes[function->scope_count];
  scope->first_binding_index = function->binding_count;
  ++function->scope_count;
  return true;
}

static void fcc_codegen_pop_scope(FccCodegenFunction* function) {
  assert(function != NULL);
  assert(function->scope_count > 0);

  function->binding_count = function->scopes[function->scope_count - 1].first_binding_index;
  --function->scope_count;
}

static const FccCodegenBinding* fcc_codegen_lookup_binding(const FccCodegenFunction* function,
                                                           const char* name) {
  size_t binding_index;

  assert(function != NULL);
  assert(name != NULL);

  binding_index = function->binding_count;
  while (binding_index > 0) {
    --binding_index;
    if ((function->bindings[binding_index].name == name) ||
        ((function->bindings[binding_index].name != NULL) &&
         (strcmp(function->bindings[binding_index].name, name) == 0))) {
      return &function->bindings[binding_index];
    }
  }

  return NULL;
}

static const FccAstGlobalVariable* fcc_codegen_find_global(const FccCodegenFunction* function,
                                                           const char* name) {
  size_t global_index;

  assert(function != NULL);
  assert(function->translation_unit != NULL);
  assert(name != NULL);

  for (global_index = 0; global_index < function->translation_unit->global_count; ++global_index) {
    if ((function->translation_unit->globals[global_index]->name == name) ||
        ((function->translation_unit->globals[global_index]->name != NULL) &&
         (strcmp(function->translation_unit->globals[global_index]->name, name) == 0))) {
      return function->translation_unit->globals[global_index];
    }
  }

  return NULL;
}

static const FccAstFunctionDefinition* fcc_codegen_find_function(
    const FccCodegenFunction* function, const char* name) {
  size_t function_index;

  assert(function != NULL);
  assert(name != NULL);

  for (function_index = 0; function_index < function->translation_unit->function_count;
       ++function_index) {
    if ((function->translation_unit->functions[function_index]->name == name) ||
        ((function->translation_unit->functions[function_index]->name != NULL) &&
         (strcmp(function->translation_unit->functions[function_index]->name, name) == 0))) {
      return function->translation_unit->functions[function_index];
    }
  }

  return NULL;
}

static size_t fcc_codegen_align_to_8(size_t value) {
  if ((value & 7U) == 0U) {
    return value;
  }

  return value + (8U - (value & 7U));
}

static size_t fcc_codegen_stack_allocation_size(const FccCodegenFunction* function,
                                                FccTypeId type_id) {
  size_t storage_size;

  storage_size = fcc_codegen_storage_size(function, type_id);
  if (storage_size == 0) {
    return 0;
  }

  return fcc_codegen_align_to_8(storage_size);
}

static bool fcc_codegen_define_binding(FccCodegenFunction* function, FccSourceSpan span,
                                       const char* name, FccTypeId type_id, int* stack_offset_out) {
  FccCodegenBinding* binding;
  size_t allocation_size;
  size_t raw_offset;

  assert(function != NULL);
  assert(name != NULL);
  assert(stack_offset_out != NULL);
  assert(function->scope_count > 0);

  if (!fcc_codegen_ensure_binding_capacity(function, function->binding_count + 1)) {
    return fcc_codegen_out_of_memory(function, span);
  }

  allocation_size = fcc_codegen_stack_allocation_size(function, type_id);
  if (allocation_size == 0) {
    return fcc_codegen_emit_error(function, span, "unsupported object type for stack allocation");
  }

  if (function->next_stack_offset > (SIZE_MAX - allocation_size)) {
    return fcc_codegen_emit_error(function, span, "stack frame exceeds backend offset range");
  }

  raw_offset = function->next_stack_offset + allocation_size;
  if (raw_offset > function->total_stack_size) {
    fcc_diag_emit_source(function->diagnostics, function->source_file, span,
                         FCC_DIAG_SEVERITY_FATAL, "internal stack allocation overflow");
    return false;
  }

  if (raw_offset > (size_t)INT_MAX) {
    return fcc_codegen_emit_error(function, span, "stack frame exceeds backend offset range");
  }

  binding = &function->bindings[function->binding_count];
  binding->stack_offset = (int)raw_offset;
  binding->name = name;
  binding->type_id = type_id;
  ++function->binding_count;
  function->next_stack_offset = raw_offset;
  *stack_offset_out = binding->stack_offset;
  return true;
}

static size_t fcc_codegen_align_to_16(size_t value) {
  if ((value & 15U) == 0U) {
    return value;
  }

  return value + (16U - (value & 15U));
}

static bool fcc_codegen_count_statement_locals(FccCodegenFunction* function,
                                               const FccAstStatement* statement,
                                               size_t* local_stack_size) {
  size_t item_index;

  assert(function != NULL);
  assert(statement != NULL);
  assert(local_stack_size != NULL);

  if (!fcc_codegen_push_recursion(function, statement->span)) {
    return false;
  }

  switch (statement->kind) {
    case FCC_AST_STATEMENT_COMPOUND:
      for (item_index = 0; item_index < statement->data.compound.item_count; ++item_index) {
        if (!fcc_codegen_count_statement_locals(
                function, statement->data.compound.items[item_index], local_stack_size)) {
          fcc_codegen_pop_recursion(function);
          return false;
        }
      }

      break;
    case FCC_AST_STATEMENT_DECLARATION: {
      const FccSemaObjectInfo* object_info;

      object_info = fcc_codegen_find_object_info(function, statement);
      if ((object_info != NULL) && fcc_codegen_object_requires_storage(object_info)) {
        size_t allocation_size;

        allocation_size = fcc_codegen_stack_allocation_size(function, object_info->type_id);
        if (allocation_size == 0) {
          return fcc_codegen_emit_error(function, statement->span,
                                        "unsupported local object type for stack allocation");
        }

        if (*local_stack_size > (SIZE_MAX - allocation_size)) {
          return fcc_codegen_emit_error(function, statement->span,
                                        "stack frame exceeds backend offset range");
        }

        *local_stack_size += allocation_size;
      }
    }

    break;
    case FCC_AST_STATEMENT_IF:
      if (!fcc_codegen_count_statement_locals(function, statement->data.if_statement.then_statement,
                                              local_stack_size)) {
        fcc_codegen_pop_recursion(function);
        return false;
      }

      if ((statement->data.if_statement.else_statement != NULL) &&
          !fcc_codegen_count_statement_locals(function, statement->data.if_statement.else_statement,
                                              local_stack_size)) {
        fcc_codegen_pop_recursion(function);
        return false;
      }

      break;
    case FCC_AST_STATEMENT_WHILE:
      if (!fcc_codegen_count_statement_locals(function, statement->data.while_statement.body,
                                              local_stack_size)) {
        fcc_codegen_pop_recursion(function);
        return false;
      }

      break;
    case FCC_AST_STATEMENT_DO_WHILE:
      if (!fcc_codegen_count_statement_locals(function, statement->data.do_while_statement.body,
                                              local_stack_size)) {
        fcc_codegen_pop_recursion(function);
        return false;
      }

      break;
    case FCC_AST_STATEMENT_FOR:
      if ((statement->data.for_statement.init_statement != NULL) &&
          !fcc_codegen_count_statement_locals(
              function, statement->data.for_statement.init_statement, local_stack_size)) {
        fcc_codegen_pop_recursion(function);
        return false;
      }

      if (!fcc_codegen_count_statement_locals(function, statement->data.for_statement.body,
                                              local_stack_size)) {
        fcc_codegen_pop_recursion(function);
        return false;
      }

      break;
    case FCC_AST_STATEMENT_SWITCH:
      if (!fcc_codegen_count_statement_locals(function, statement->data.switch_statement.body,
                                              local_stack_size)) {
        fcc_codegen_pop_recursion(function);
        return false;
      }

      break;
    case FCC_AST_STATEMENT_CASE:
      if (!fcc_codegen_count_statement_locals(function, statement->data.case_statement.statement,
                                              local_stack_size)) {
        fcc_codegen_pop_recursion(function);
        return false;
      }

      break;
    case FCC_AST_STATEMENT_DEFAULT:
      if (!fcc_codegen_count_statement_locals(function, statement->data.default_statement.statement,
                                              local_stack_size)) {
        fcc_codegen_pop_recursion(function);
        return false;
      }

      break;
    case FCC_AST_STATEMENT_LABEL:
      if (!fcc_codegen_count_statement_locals(function, statement->data.label_statement.statement,
                                              local_stack_size)) {
        fcc_codegen_pop_recursion(function);
        return false;
      }

      break;
    case FCC_AST_STATEMENT_RETURN:
    case FCC_AST_STATEMENT_EXPRESSION:
    case FCC_AST_STATEMENT_STATIC_ASSERT:
    case FCC_AST_STATEMENT_BREAK:
    case FCC_AST_STATEMENT_CONTINUE:
    case FCC_AST_STATEMENT_GOTO:
      break;
  }

  fcc_codegen_pop_recursion(function);
  return true;
}

static size_t fcc_codegen_allocate_label(FccCodegenFunction* function) {
  size_t label_id;

  assert(function != NULL);
  assert(function->next_label_id < SIZE_MAX);

  label_id = function->next_label_id;
  ++function->next_label_id;
  return label_id;
}

static void fcc_codegen_emit_local_label(FILE* stream, size_t label_id) {
  assert(stream != NULL);

  fprintf(stream, ".L%zu:\n", label_id);
}

static void fcc_codegen_emit_jump(FILE* stream, const char* mnemonic, size_t label_id) {
  assert(stream != NULL);
  assert(mnemonic != NULL);

  fprintf(stream, "  %s .L%zu\n", mnemonic, label_id);
}

static void fcc_codegen_emit_user_label(FILE* stream, const char* label_name) {
  assert(stream != NULL);
  assert(label_name != NULL);

  fprintf(stream, ".Luser_%s:\n", label_name);
}

static void fcc_codegen_emit_user_jump(FILE* stream, const char* label_name) {
  assert(stream != NULL);
  assert(label_name != NULL);

  fprintf(stream, "  jmp .Luser_%s\n", label_name);
}

static bool fcc_codegen_emit_expression(FccCodegenFunction* function,
                                        const FccAstExpression* expression);

static bool fcc_codegen_type_is_char_array(const FccTypeContext* type_context,
                                           FccTypeId type_id,
                                           size_t* element_count_out);

static bool fcc_codegen_emit_integer_literal(FccCodegenFunction* function,
                                             const FccAstExpression* expression) {
  FccTypeId type_id;
  size_t storage_size;
  uint64_t value;

  assert(function != NULL);
  assert(expression != NULL);

  type_id = fcc_codegen_expression_type_id(function, expression);
  storage_size = fcc_codegen_storage_size(function, type_id);
  value = expression->data.integer_literal.value;
  if ((storage_size != 8) && (value > UINT32_MAX)) {
    return fcc_codegen_emit_error(function, expression->span,
                                  "integer literal exceeds the backend's 32-bit range");
  }

  if (value == 0) {
    fputs("  xor eax, eax\n", function->stream);
  } else if (storage_size == 8) {
    fprintf(function->stream, "  mov rax, %llu\n", (unsigned long long)value);
  } else {
    fprintf(function->stream, "  mov eax, %u\n", (unsigned int)value);
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_identifier(FccCodegenFunction* function,
                                        const FccAstExpression* expression) {
  const FccCodegenBinding* binding;
  const FccSemaExpressionInfo* expression_info;
  const FccAstFunctionDefinition* function_definition;
  const FccAstGlobalVariable* global_variable;
  char memory_text[64];

  assert(function != NULL);
  assert(expression != NULL);

  expression_info = fcc_codegen_find_expression_info(function, expression);
  if ((expression_info != NULL) && expression_info->has_integer_constant) {
    FccTypeId type_id;
    size_t storage_size;

    type_id = fcc_codegen_expression_type_id(function, expression);
    storage_size = fcc_codegen_storage_size(function, type_id);
    if ((storage_size != 8) && ((expression_info->integer_constant_value < INT_MIN) ||
                                (expression_info->integer_constant_value > UINT32_MAX))) {
      char message[FCC_MAX_DIAG_MESSAGE_LENGTH];
      int written;

      written = snprintf(message, sizeof(message),
                         "integer constant '%s' exceeds the backend's 32-bit range",
                         expression->data.identifier.name);
      if ((written < 0) || ((size_t)written >= sizeof(message))) {
        return fcc_codegen_emit_error(function, expression->span,
                                      "integer constant exceeds the backend's 32-bit range");
      }

      return fcc_codegen_emit_error(function, expression->span, message);
    }

    if (expression_info->integer_constant_value == 0) {
      fputs("  xor eax, eax\n", function->stream);
    } else if (storage_size == 8) {
      fprintf(function->stream, "  mov rax, %lld\n",
              (long long)expression_info->integer_constant_value);
    } else {
      fprintf(function->stream, "  mov eax, %lld\n",
              (long long)expression_info->integer_constant_value);
    }

    return fcc_codegen_check_stream(function);
  }

  if ((expression_info != NULL) &&
      fcc_type_is_function(&function->sema_result->type_context, expression_info->type_id)) {
    function_definition = fcc_codegen_find_function(function, expression->data.identifier.name);
    if (function_definition != NULL) {
      fprintf(function->stream, "  lea rax, [rel %s]\n", function_definition->name);
      return fcc_codegen_check_stream(function);
    }

    fprintf(function->stream, "  lea rax, [rel %s]\n", expression->data.identifier.name);
    return fcc_codegen_check_stream(function);
  }

  binding = fcc_codegen_lookup_binding(function, expression->data.identifier.name);
  if (binding != NULL) {
    (void)snprintf(memory_text, sizeof(memory_text), "[rbp - %d]", binding->stack_offset);
    return fcc_codegen_emit_load_from_memory(function, memory_text, binding->type_id);
  }

  global_variable = fcc_codegen_find_global(function, expression->data.identifier.name);
  if (global_variable != NULL) {
    const FccSemaObjectInfo* object_info;

    object_info = fcc_codegen_find_object_info(function, global_variable);
    if (object_info == NULL) {
      return fcc_codegen_emit_error(function, expression->span,
                                    "internal global semantic metadata lookup failed");
    }

    (void)snprintf(memory_text, sizeof(memory_text), "[rel %s]", global_variable->name);
    return fcc_codegen_emit_load_from_memory(function, memory_text, object_info->type_id);
  }

  return fcc_codegen_emit_error(function, expression->span,
                                "internal codegen binding lookup failed");
}

static bool fcc_codegen_emit_string_literal(FccCodegenFunction* function,
                                            const FccAstExpression* expression) {
  const FccCodegenStringLiteral* string_literal;

  assert(function != NULL);
  assert(expression != NULL);

  string_literal = fcc_codegen_find_string_literal(function, expression);
  if (string_literal == NULL) {
    return fcc_codegen_emit_error(function, expression->span,
                                  "internal string literal label lookup failed");
  }

  fprintf(function->stream, "  lea rax, [rel FCC_STR%zu]\n", string_literal->label_id);
  return fcc_codegen_check_stream(function);
}

static FccTypeId fcc_codegen_subscript_element_type(const FccCodegenFunction* function,
                                                    const FccAstExpression* expression) {
  const FccType* target_type;
  FccTypeId target_type_id;

  assert(function != NULL);
  assert(expression != NULL);
  assert(expression->kind == FCC_AST_EXPRESSION_SUBSCRIPT);

  target_type_id = fcc_codegen_expression_type_id(function, expression->data.subscript.target);
  target_type = fcc_type_context_get(&function->sema_result->type_context, target_type_id);
  if (target_type == NULL) {
    return FCC_TYPE_ID_INVALID;
  }

  if (target_type->kind == FCC_TYPE_ARRAY) {
    return target_type->data.array.element_type_id;
  }

  if (target_type->kind == FCC_TYPE_POINTER) {
    return target_type->data.pointer.pointee_type_id;
  }

  return FCC_TYPE_ID_INVALID;
}

static const FccTypeRecordField* fcc_codegen_member_field(const FccCodegenFunction* function,
                                                          const FccAstExpression* expression) {
  FccTypeId record_type_id;

  assert(function != NULL);
  assert(expression != NULL);
  assert(expression->kind == FCC_AST_EXPRESSION_MEMBER);

  record_type_id = fcc_codegen_expression_type_id(function, expression->data.member.target);
  if (expression->data.member.is_arrow) {
    record_type_id =
        fcc_type_get_pointee_type(&function->sema_result->type_context, record_type_id);
  }

  if (record_type_id == FCC_TYPE_ID_INVALID) {
    return NULL;
  }

  return fcc_type_record_find_field(&function->sema_result->type_context, record_type_id,
                                    expression->data.member.field_name);
}

static bool fcc_codegen_emit_address_of_lvalue(FccCodegenFunction* function,
                                               const FccAstExpression* expression) {
  const FccCodegenBinding* binding;
  const FccAstFunctionDefinition* function_definition;
  const FccAstGlobalVariable* global_variable;
  const FccTypeRecordField* field;
  FccTypeId element_type_id;
  FccTypeId expression_type_id;
  size_t element_size;

  assert(function != NULL);
  assert(expression != NULL);

  switch (expression->kind) {
    case FCC_AST_EXPRESSION_IDENTIFIER:
      expression_type_id = fcc_codegen_expression_type_id(function, expression);
      if (fcc_type_is_function(&function->sema_result->type_context, expression_type_id)) {
        function_definition = fcc_codegen_find_function(function, expression->data.identifier.name);
        if (function_definition != NULL) {
          fprintf(function->stream, "  lea rax, [rel %s]\n", function_definition->name);
          return fcc_codegen_check_stream(function);
        }

        fprintf(function->stream, "  lea rax, [rel %s]\n", expression->data.identifier.name);
        return fcc_codegen_check_stream(function);
      }

      binding = fcc_codegen_lookup_binding(function, expression->data.identifier.name);
      if (binding != NULL) {
        fprintf(function->stream, "  lea rax, [rbp - %d]\n", binding->stack_offset);
        return fcc_codegen_check_stream(function);
      }

      global_variable = fcc_codegen_find_global(function, expression->data.identifier.name);
      if (global_variable != NULL) {
        fprintf(function->stream, "  lea rax, [rel %s]\n", global_variable->name);
        return fcc_codegen_check_stream(function);
      }

      return fcc_codegen_emit_error(function, expression->span,
                                    "internal lvalue binding lookup failed");
    case FCC_AST_EXPRESSION_UNARY:
      if (expression->data.unary.op_kind == FCC_AST_UNARY_DEREFERENCE) {
        return fcc_codegen_emit_expression(function, expression->data.unary.operand);
      }

      break;
    case FCC_AST_EXPRESSION_SUBSCRIPT:
      element_type_id = fcc_codegen_subscript_element_type(function, expression);
      if (element_type_id == FCC_TYPE_ID_INVALID) {
        return fcc_codegen_emit_error(function, expression->span,
                                      "missing subscript element type metadata in code generation");
      }

      element_size = fcc_codegen_storage_size(function, element_type_id);
      if (element_size == 0) {
        return fcc_codegen_emit_error(function, expression->span,
                                      "unsupported subscript element width in code generation");
      }

      {
        FccTypeId target_type_id;
        const FccType* target_type;

        target_type_id =
            fcc_codegen_expression_type_id(function, expression->data.subscript.target);
        target_type = fcc_type_context_get(&function->sema_result->type_context, target_type_id);
        if ((target_type != NULL) && (target_type->kind == FCC_TYPE_ARRAY)) {
          if (!fcc_codegen_emit_address_of_lvalue(function, expression->data.subscript.target)) {
            return false;
          }
        } else if (!fcc_codegen_emit_expression(function, expression->data.subscript.target)) {
          return false;
        }
      }

      fputs("  push rax\n", function->stream);
      if (!fcc_codegen_emit_expression(function, expression->data.subscript.index)) {
        return false;
      }

      if (element_size != 1) {
        fprintf(function->stream, "  imul rax, rax, %zu\n", element_size);
      }

      fputs("  pop rcx\n", function->stream);
      fputs("  add rax, rcx\n", function->stream);
      return fcc_codegen_check_stream(function);
    case FCC_AST_EXPRESSION_MEMBER:
      field = fcc_codegen_member_field(function, expression);
      if (field == NULL) {
        return fcc_codegen_emit_error(function, expression->span,
                                      "missing member field metadata in code generation");
      }

      if (expression->data.member.is_arrow) {
        if (!fcc_codegen_emit_expression(function, expression->data.member.target)) {
          return false;
        }
      } else if (!fcc_codegen_emit_address_of_lvalue(function, expression->data.member.target)) {
        return false;
      }

      if (field->offset != 0) {
        fprintf(function->stream, "  add rax, %zu\n", field->offset);
      }

      return fcc_codegen_check_stream(function);
    default:
      break;
  }

  return fcc_codegen_emit_error(function, expression->span,
                                "unsupported lvalue form in code generation");
}

static bool fcc_codegen_emit_call(FccCodegenFunction* function,
                                  const FccAstExpression* expression) {
  static const char* const PARAMETER_REGISTERS[FCC_MAX_WINDOWS_X64_REGISTER_ARGUMENTS] = {
      "rcx",
      "rdx",
      "r8",
      "r9",
  };
  const FccAstExpression* callee;
  size_t argument_index;
  size_t callee_stack_offset;
  size_t cleanup_size;
  size_t register_argument_count;
  size_t stack_argument_count;
  bool needs_alignment_pad;
  bool is_direct_call;
  FccTypeId callee_type_id;

  assert(function != NULL);
  assert(expression != NULL);

  callee = expression->data.call.callee;
  callee_type_id = fcc_codegen_expression_type_id(function, callee);
  is_direct_call = (callee->kind == FCC_AST_EXPRESSION_IDENTIFIER) &&
                   fcc_type_is_function(&function->sema_result->type_context, callee_type_id);

  register_argument_count = expression->data.call.argument_count;
  if (register_argument_count > FCC_MAX_WINDOWS_X64_REGISTER_ARGUMENTS) {
    register_argument_count = FCC_MAX_WINDOWS_X64_REGISTER_ARGUMENTS;
  }

  stack_argument_count = expression->data.call.argument_count - register_argument_count;
  needs_alignment_pad =
      (((expression->data.call.argument_count + (is_direct_call ? 0U : 1U)) & 1U) != 0U);
  if (needs_alignment_pad) {
    fputs("  sub rsp, 8\n", function->stream);
  }

  if (!is_direct_call) {
    if (!fcc_codegen_emit_expression(function, callee)) {
      return false;
    }

    fputs("  push rax\n", function->stream);
  }

  /*
   * Windows x64 requires 32 bytes of shadow space before every call. Arguments
   * are first evaluated right-to-left onto the stack, then shuffled into the
   * register and stack locations expected by the ABI.
   */
  for (argument_index = expression->data.call.argument_count; argument_index > 0;
       --argument_index) {
    if (!fcc_codegen_emit_expression(function,
                                     expression->data.call.arguments[argument_index - 1])) {
      return false;
    }

    fputs("  push rax\n", function->stream);
  }

  fputs("  sub rsp, 32\n", function->stream);
  for (argument_index = 0; argument_index < register_argument_count; ++argument_index) {
    fprintf(function->stream, "  mov %s, qword [rsp + %zu]\n", PARAMETER_REGISTERS[argument_index],
            32 + (argument_index * 8));
  }

  for (argument_index = 0; argument_index < stack_argument_count; ++argument_index) {
    fprintf(function->stream, "  mov rax, qword [rsp + %zu]\n",
            32 + ((register_argument_count + argument_index) * 8));
    fprintf(function->stream, "  mov qword [rsp + %zu], rax\n", 32 + (argument_index * 8));
  }

  if (is_direct_call) {
    fprintf(function->stream, "  call %s\n", callee->data.identifier.name);
  } else {
    callee_stack_offset = 32 + (expression->data.call.argument_count * 8);
    fprintf(function->stream, "  call qword [rsp + %zu]\n", callee_stack_offset);
  }

  cleanup_size = 32 + (expression->data.call.argument_count * 8);
  if (!is_direct_call) {
    cleanup_size += 8;
  }

  if (needs_alignment_pad) {
    cleanup_size += 8;
  }

  if (cleanup_size != 0) {
    fprintf(function->stream, "  add rsp, %zu\n", cleanup_size);
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_integer_constant_metadata(FccCodegenFunction* function,
                                                       const FccAstExpression* expression,
                                                       const char* missing_message) {
  const FccSemaExpressionInfo* expression_info;

  assert(function != NULL);
  assert(expression != NULL);
  assert(missing_message != NULL);

  expression_info = fcc_codegen_find_expression_info(function, expression);
  if ((expression_info == NULL) || !expression_info->has_integer_constant ||
      (expression_info->integer_constant_value < 0) ||
      (expression_info->integer_constant_value > INT_MAX)) {
    return fcc_codegen_emit_error(function, expression->span, missing_message);
  }

  fprintf(function->stream, "  mov eax, %lld\n",
          (long long)expression_info->integer_constant_value);
  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_sizeof(FccCodegenFunction* function,
                                    const FccAstExpression* expression) {
  return fcc_codegen_emit_integer_constant_metadata(
      function, expression, "missing sizeof constant metadata in code generation");
}

static bool fcc_codegen_emit_alignof(FccCodegenFunction* function,
                                     const FccAstExpression* expression) {
  return fcc_codegen_emit_integer_constant_metadata(
      function, expression, "missing alignof constant metadata in code generation");
}

static bool fcc_codegen_emit_cast(FccCodegenFunction* function,
                                  const FccAstExpression* expression) {
  FccTypeId source_type_id;
  FccTypeId target_type_id;

  assert(function != NULL);
  assert(expression != NULL);

  if (!fcc_codegen_emit_expression(function, expression->data.cast.operand)) {
    return false;
  }

  target_type_id = fcc_codegen_expression_type_id(function, expression);
  if (target_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_codegen_emit_error(function, expression->span,
                                  "missing cast type metadata in code generation");
  }

  if (fcc_codegen_type_is_void(function, target_type_id)) {
    return true;
  }

  source_type_id = fcc_codegen_expression_type_id(function, expression->data.cast.operand);
  return fcc_codegen_emit_coerce_scalar(function, source_type_id, target_type_id,
                                        expression->span);
}

static bool fcc_codegen_emit_lvalue_load(FccCodegenFunction* function,
                                         const FccAstExpression* expression) {
  FccTypeId type_id;

  assert(function != NULL);
  assert(expression != NULL);

  type_id = fcc_codegen_expression_type_id(function, expression);
  if (type_id == FCC_TYPE_ID_INVALID) {
    return fcc_codegen_emit_error(function, expression->span,
                                  "missing lvalue type metadata in code generation");
  }

  if (!fcc_codegen_emit_address_of_lvalue(function, expression)) {
    return false;
  }

  return fcc_codegen_emit_load_from_memory(function, "[rax]", type_id);
}

static bool fcc_codegen_emit_unary(FccCodegenFunction* function,
                                   const FccAstExpression* expression) {
  FccTypeId result_type_id;

  assert(function != NULL);
  assert(expression != NULL);

  if (expression->data.unary.op_kind == FCC_AST_UNARY_ADDRESS_OF) {
    return fcc_codegen_emit_address_of_lvalue(function, expression->data.unary.operand);
  }

  if (!fcc_codegen_emit_expression(function, expression->data.unary.operand)) {
    return false;
  }

  switch (expression->data.unary.op_kind) {
    case FCC_AST_UNARY_PLUS:
      return true;
    case FCC_AST_UNARY_NEGATE:
      result_type_id = fcc_codegen_expression_type_id(function, expression);
      if ((result_type_id != FCC_TYPE_ID_INVALID) &&
          (fcc_codegen_storage_size(function, result_type_id) == 8)) {
        fputs("  neg rax\n", function->stream);
      } else {
        fputs("  neg eax\n", function->stream);
      }
      return fcc_codegen_check_stream(function);
    case FCC_AST_UNARY_LOGICAL_NOT:
      fputs("  cmp eax, 0\n", function->stream);
      fputs("  sete al\n", function->stream);
      fputs("  movzx eax, al\n", function->stream);
      return fcc_codegen_check_stream(function);
    case FCC_AST_UNARY_BITWISE_NOT:
      fputs("  not eax\n", function->stream);
      return fcc_codegen_check_stream(function);
    case FCC_AST_UNARY_ADDRESS_OF:
      return fcc_codegen_emit_address_of_lvalue(function, expression->data.unary.operand);
    case FCC_AST_UNARY_DEREFERENCE:
      result_type_id = fcc_codegen_expression_type_id(function, expression);
      if (result_type_id == FCC_TYPE_ID_INVALID) {
        return fcc_codegen_emit_error(function, expression->span,
                                      "missing dereference type metadata in code generation");
      }

      if (fcc_type_is_function(&function->sema_result->type_context, result_type_id)) {
        return true;
      }

      return fcc_codegen_emit_load_from_memory(function, "[rax]", result_type_id);
  }

  return fcc_codegen_emit_error(function, expression->span,
                                "unknown unary operator in code generation");
}

static bool fcc_codegen_emit_logical_and(FccCodegenFunction* function,
                                         const FccAstExpression* expression) {
  size_t false_label_id;
  size_t end_label_id;

  assert(function != NULL);
  assert(expression != NULL);

  false_label_id = fcc_codegen_allocate_label(function);
  end_label_id = fcc_codegen_allocate_label(function);
  if (!fcc_codegen_emit_expression(function, expression->data.binary.left)) {
    return false;
  }

  fputs("  cmp eax, 0\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "je", false_label_id);
  if (!fcc_codegen_emit_expression(function, expression->data.binary.right)) {
    return false;
  }

  fputs("  cmp eax, 0\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "je", false_label_id);
  fputs("  mov eax, 1\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "jmp", end_label_id);
  fcc_codegen_emit_local_label(function->stream, false_label_id);
  fputs("  xor eax, eax\n", function->stream);
  fcc_codegen_emit_local_label(function->stream, end_label_id);
  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_logical_or(FccCodegenFunction* function,
                                        const FccAstExpression* expression) {
  size_t true_label_id;
  size_t end_label_id;

  assert(function != NULL);
  assert(expression != NULL);

  true_label_id = fcc_codegen_allocate_label(function);
  end_label_id = fcc_codegen_allocate_label(function);
  if (!fcc_codegen_emit_expression(function, expression->data.binary.left)) {
    return false;
  }

  fputs("  cmp eax, 0\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "jne", true_label_id);
  if (!fcc_codegen_emit_expression(function, expression->data.binary.right)) {
    return false;
  }

  fputs("  cmp eax, 0\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "jne", true_label_id);
  fputs("  xor eax, eax\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "jmp", end_label_id);
  fcc_codegen_emit_local_label(function->stream, true_label_id);
  fputs("  mov eax, 1\n", function->stream);
  fcc_codegen_emit_local_label(function->stream, end_label_id);
  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_binary_op_is_relational(FccAstBinaryOpKind op_kind) {
  return (op_kind == FCC_AST_BINARY_LESS) || (op_kind == FCC_AST_BINARY_LESS_EQUAL) ||
         (op_kind == FCC_AST_BINARY_GREATER) || (op_kind == FCC_AST_BINARY_GREATER_EQUAL);
}

static bool fcc_codegen_binary_op_is_equality(FccAstBinaryOpKind op_kind) {
  return (op_kind == FCC_AST_BINARY_EQUAL) || (op_kind == FCC_AST_BINARY_NOT_EQUAL);
}

static size_t fcc_codegen_integer_binary_size(const FccCodegenFunction* function,
                                              FccTypeId left_type_id, FccTypeId right_type_id) {
  size_t left_size;
  size_t right_size;

  assert(function != NULL);

  left_size = fcc_codegen_storage_size(function, left_type_id);
  right_size = fcc_codegen_storage_size(function, right_type_id);
  if ((left_size == 8) || (right_size == 8)) {
    return 8;
  }

  return 4;
}

static bool fcc_codegen_integer_binary_is_unsigned(const FccCodegenFunction* function,
                                                   FccTypeId left_type_id,
                                                   FccTypeId right_type_id) {
  assert(function != NULL);

  return fcc_type_is_unsigned_integer(&function->sema_result->type_context, left_type_id) ||
         fcc_type_is_unsigned_integer(&function->sema_result->type_context, right_type_id);
}

static FccTypeId fcc_codegen_integer_binary_operation_type(FccCodegenFunction* function,
                                                           size_t operation_size,
                                                           bool operation_is_unsigned) {
  FccTypeKind kind;

  assert(function != NULL);

  if (operation_size == 8) {
    kind = operation_is_unsigned ? FCC_TYPE_UNSIGNED_LONG_LONG : FCC_TYPE_LONG_LONG;
  } else {
    kind = operation_is_unsigned ? FCC_TYPE_UNSIGNED_INT : FCC_TYPE_INT;
  }

  return fcc_type_context_get_builtin(&function->sema_result->type_context, kind, false);
}

static FccTypeId fcc_codegen_decay_array_type(FccCodegenFunction* function, FccTypeId type_id) {
  assert(function != NULL);

  return fcc_type_decay_array(&function->sema_result->type_context, type_id);
}

static bool fcc_codegen_pointer_element_size(FccCodegenFunction* function,
                                             FccTypeId pointer_type_id, FccSourceSpan span,
                                             size_t* size_out) {
  FccTypeId pointee_type_id;
  size_t element_size;

  assert(function != NULL);
  assert(size_out != NULL);

  pointee_type_id =
      fcc_type_get_pointee_type(&function->sema_result->type_context, pointer_type_id);
  if (pointee_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_codegen_emit_error(function, span,
                                  "missing pointer element type in code generation");
  }

  element_size = fcc_codegen_storage_size(function, pointee_type_id);
  if (element_size == 0) {
    return fcc_codegen_emit_error(function, span,
                                  "unsupported pointer element width in code generation");
  }

  *size_out = element_size;
  return true;
}

static bool fcc_codegen_emit_scale_rax(FccCodegenFunction* function, size_t element_size) {
  assert(function != NULL);

  if (element_size != 1) {
    fprintf(function->stream, "  imul rax, rax, %zu\n", element_size);
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_pointer_arithmetic(FccCodegenFunction* function,
                                                const FccAstExpression* expression,
                                                FccTypeId left_type_id, FccTypeId right_type_id) {
  FccAstBinaryOpKind op_kind;
  bool left_is_pointer;
  bool right_is_pointer;
  bool left_is_integer;
  bool right_is_integer;
  size_t element_size;

  assert(function != NULL);
  assert(expression != NULL);

  op_kind = expression->data.binary.op_kind;
  left_is_pointer = fcc_type_is_pointer(&function->sema_result->type_context, left_type_id);
  right_is_pointer = fcc_type_is_pointer(&function->sema_result->type_context, right_type_id);
  left_is_integer = fcc_type_is_integer(&function->sema_result->type_context, left_type_id);
  right_is_integer = fcc_type_is_integer(&function->sema_result->type_context, right_type_id);

  if ((op_kind == FCC_AST_BINARY_ADD) && left_is_pointer && right_is_integer) {
    if (!fcc_codegen_pointer_element_size(function, left_type_id, expression->span,
                                          &element_size)) {
      return false;
    }

    if (!fcc_codegen_emit_expression(function, expression->data.binary.left)) {
      return false;
    }

    fputs("  push rax\n", function->stream);
    if (!fcc_codegen_emit_expression(function, expression->data.binary.right)) {
      return false;
    }

    if (!fcc_codegen_emit_scale_rax(function, element_size)) {
      return false;
    }

    fputs("  pop rcx\n", function->stream);
    fputs("  add rax, rcx\n", function->stream);
    return fcc_codegen_check_stream(function);
  }

  if ((op_kind == FCC_AST_BINARY_ADD) && left_is_integer && right_is_pointer) {
    if (!fcc_codegen_pointer_element_size(function, right_type_id, expression->span,
                                          &element_size)) {
      return false;
    }

    if (!fcc_codegen_emit_expression(function, expression->data.binary.left)) {
      return false;
    }

    if (!fcc_codegen_emit_scale_rax(function, element_size)) {
      return false;
    }

    fputs("  push rax\n", function->stream);
    if (!fcc_codegen_emit_expression(function, expression->data.binary.right)) {
      return false;
    }

    fputs("  pop rcx\n", function->stream);
    fputs("  add rax, rcx\n", function->stream);
    return fcc_codegen_check_stream(function);
  }

  if ((op_kind == FCC_AST_BINARY_SUBTRACT) && left_is_pointer && right_is_integer) {
    if (!fcc_codegen_pointer_element_size(function, left_type_id, expression->span,
                                          &element_size)) {
      return false;
    }

    if (!fcc_codegen_emit_expression(function, expression->data.binary.left)) {
      return false;
    }

    fputs("  push rax\n", function->stream);
    if (!fcc_codegen_emit_expression(function, expression->data.binary.right)) {
      return false;
    }

    if (!fcc_codegen_emit_scale_rax(function, element_size)) {
      return false;
    }

    fputs("  pop rcx\n", function->stream);
    fputs("  sub rcx, rax\n", function->stream);
    fputs("  mov rax, rcx\n", function->stream);
    return fcc_codegen_check_stream(function);
  }

  if ((op_kind == FCC_AST_BINARY_SUBTRACT) && left_is_pointer && right_is_pointer) {
    if (!fcc_codegen_pointer_element_size(function, left_type_id, expression->span,
                                          &element_size)) {
      return false;
    }

    if (!fcc_codegen_emit_expression(function, expression->data.binary.left)) {
      return false;
    }

    fputs("  push rax\n", function->stream);
    if (!fcc_codegen_emit_expression(function, expression->data.binary.right)) {
      return false;
    }

    fputs("  pop rcx\n", function->stream);
    fputs("  sub rcx, rax\n", function->stream);
    fputs("  mov rax, rcx\n", function->stream);
    if (element_size != 1) {
      fputs("  cqo\n", function->stream);
      fprintf(function->stream, "  mov rcx, %zu\n", element_size);
      fputs("  idiv rcx\n", function->stream);
    }

    return fcc_codegen_check_stream(function);
  }

  return fcc_codegen_emit_error(function, expression->span,
                                "unsupported pointer arithmetic in code generation");
}

static bool fcc_codegen_emit_pointer_comparison(FccCodegenFunction* function,
                                                const FccAstExpression* expression) {
  FccAstBinaryOpKind op_kind;

  assert(function != NULL);
  assert(expression != NULL);

  op_kind = expression->data.binary.op_kind;
  if (!fcc_codegen_emit_expression(function, expression->data.binary.left)) {
    return false;
  }

  fputs("  push rax\n", function->stream);
  if (!fcc_codegen_emit_expression(function, expression->data.binary.right)) {
    return false;
  }

  fputs("  mov rcx, rax\n", function->stream);
  fputs("  pop rax\n", function->stream);
  fputs("  cmp rax, rcx\n", function->stream);
  switch (op_kind) {
    case FCC_AST_BINARY_LESS:
      fputs("  setb al\n", function->stream);
      break;
    case FCC_AST_BINARY_LESS_EQUAL:
      fputs("  setbe al\n", function->stream);
      break;
    case FCC_AST_BINARY_GREATER:
      fputs("  seta al\n", function->stream);
      break;
    case FCC_AST_BINARY_GREATER_EQUAL:
      fputs("  setae al\n", function->stream);
      break;
    case FCC_AST_BINARY_EQUAL:
      fputs("  sete al\n", function->stream);
      break;
    case FCC_AST_BINARY_NOT_EQUAL:
      fputs("  setne al\n", function->stream);
      break;
    case FCC_AST_BINARY_ADD:
    case FCC_AST_BINARY_SUBTRACT:
    case FCC_AST_BINARY_MULTIPLY:
    case FCC_AST_BINARY_DIVIDE:
    case FCC_AST_BINARY_MODULO:
    case FCC_AST_BINARY_BITWISE_AND:
    case FCC_AST_BINARY_BITWISE_XOR:
    case FCC_AST_BINARY_BITWISE_OR:
    case FCC_AST_BINARY_LOGICAL_AND:
    case FCC_AST_BINARY_LOGICAL_OR:
    case FCC_AST_BINARY_LEFT_SHIFT:
    case FCC_AST_BINARY_RIGHT_SHIFT:
      return fcc_codegen_emit_error(function, expression->span,
                                    "unsupported pointer comparison operator");
  }

  fputs("  movzx eax, al\n", function->stream);
  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_binary(FccCodegenFunction* function,
                                    const FccAstExpression* expression) {
  FccAstBinaryOpKind op_kind;
  FccTypeId left_type_id;
  FccTypeId right_type_id;
  bool left_is_pointer;
  bool right_is_pointer;
  bool operation_is_unsigned;
  size_t operation_size;
  FccTypeId operation_type_id;

  assert(function != NULL);
  assert(expression != NULL);

  op_kind = expression->data.binary.op_kind;
  if (op_kind == FCC_AST_BINARY_LOGICAL_AND) {
    return fcc_codegen_emit_logical_and(function, expression);
  }

  if (op_kind == FCC_AST_BINARY_LOGICAL_OR) {
    return fcc_codegen_emit_logical_or(function, expression);
  }

  left_type_id = fcc_codegen_decay_array_type(
      function, fcc_codegen_expression_type_id(function, expression->data.binary.left));
  right_type_id = fcc_codegen_decay_array_type(
      function, fcc_codegen_expression_type_id(function, expression->data.binary.right));
  if ((left_type_id == FCC_TYPE_ID_INVALID) || (right_type_id == FCC_TYPE_ID_INVALID)) {
    return fcc_codegen_emit_error(function, expression->span,
                                  "missing binary operand type metadata in code generation");
  }

  left_is_pointer = fcc_type_is_pointer(&function->sema_result->type_context, left_type_id);
  right_is_pointer = fcc_type_is_pointer(&function->sema_result->type_context, right_type_id);
  if ((op_kind == FCC_AST_BINARY_ADD) || (op_kind == FCC_AST_BINARY_SUBTRACT)) {
    if (left_is_pointer || right_is_pointer) {
      return fcc_codegen_emit_pointer_arithmetic(function, expression, left_type_id, right_type_id);
    }
  }

  if ((fcc_codegen_binary_op_is_equality(op_kind) ||
       fcc_codegen_binary_op_is_relational(op_kind)) &&
      left_is_pointer && right_is_pointer) {
    return fcc_codegen_emit_pointer_comparison(function, expression);
  }

  operation_size = fcc_codegen_integer_binary_size(function, left_type_id, right_type_id);
  operation_is_unsigned =
      fcc_codegen_integer_binary_is_unsigned(function, left_type_id, right_type_id);
  operation_type_id =
      fcc_codegen_integer_binary_operation_type(function, operation_size, operation_is_unsigned);
  if (operation_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_codegen_emit_error(function, expression->span,
                                  "missing binary operation type in code generation");
  }

  if (!fcc_codegen_emit_expression(function, expression->data.binary.left)) {
    return false;
  }

  if (!fcc_codegen_emit_coerce_scalar(function, left_type_id, operation_type_id,
                                      expression->data.binary.left->span)) {
    return false;
  }

  fputs("  push rax\n", function->stream);
  if (!fcc_codegen_emit_expression(function, expression->data.binary.right)) {
    return false;
  }

  if (!fcc_codegen_emit_coerce_scalar(function, right_type_id, operation_type_id,
                                      expression->data.binary.right->span)) {
    return false;
  }

  if (operation_size == 8) {
    fputs("  mov rcx, rax\n", function->stream);
    fputs("  pop rax\n", function->stream);
    switch (op_kind) {
      case FCC_AST_BINARY_ADD:
        fputs("  add rax, rcx\n", function->stream);
        break;
      case FCC_AST_BINARY_SUBTRACT:
        fputs("  sub rax, rcx\n", function->stream);
        break;
      case FCC_AST_BINARY_MULTIPLY:
        fputs("  imul rax, rcx\n", function->stream);
        break;
      case FCC_AST_BINARY_DIVIDE:
        if (operation_is_unsigned) {
          fputs("  xor rdx, rdx\n", function->stream);
          fputs("  div rcx\n", function->stream);
        } else {
          fputs("  cqo\n", function->stream);
          fputs("  idiv rcx\n", function->stream);
        }
        break;
      case FCC_AST_BINARY_MODULO:
        if (operation_is_unsigned) {
          fputs("  xor rdx, rdx\n", function->stream);
          fputs("  div rcx\n", function->stream);
        } else {
          fputs("  cqo\n", function->stream);
          fputs("  idiv rcx\n", function->stream);
        }
        fputs("  mov rax, rdx\n", function->stream);
        break;
      case FCC_AST_BINARY_LESS:
        fputs("  cmp rax, rcx\n", function->stream);
        fputs(operation_is_unsigned ? "  setb al\n" : "  setl al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_LESS_EQUAL:
        fputs("  cmp rax, rcx\n", function->stream);
        fputs(operation_is_unsigned ? "  setbe al\n" : "  setle al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_GREATER:
        fputs("  cmp rax, rcx\n", function->stream);
        fputs(operation_is_unsigned ? "  seta al\n" : "  setg al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_GREATER_EQUAL:
        fputs("  cmp rax, rcx\n", function->stream);
        fputs(operation_is_unsigned ? "  setae al\n" : "  setge al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_EQUAL:
        fputs("  cmp rax, rcx\n", function->stream);
        fputs("  sete al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_NOT_EQUAL:
        fputs("  cmp rax, rcx\n", function->stream);
        fputs("  setne al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_BITWISE_AND:
        fputs("  and rax, rcx\n", function->stream);
        break;
      case FCC_AST_BINARY_BITWISE_XOR:
        fputs("  xor rax, rcx\n", function->stream);
        break;
      case FCC_AST_BINARY_BITWISE_OR:
        fputs("  or rax, rcx\n", function->stream);
        break;
      case FCC_AST_BINARY_LEFT_SHIFT:
        fputs("  shl rax, cl\n", function->stream);
        break;
      case FCC_AST_BINARY_RIGHT_SHIFT:
        fputs(operation_is_unsigned ? "  shr rax, cl\n" : "  sar rax, cl\n", function->stream);
        break;
      case FCC_AST_BINARY_LOGICAL_AND:
      case FCC_AST_BINARY_LOGICAL_OR:
        break;
    }
  } else {
    fputs("  mov ecx, eax\n", function->stream);
    fputs("  pop rax\n", function->stream);
    switch (op_kind) {
      case FCC_AST_BINARY_ADD:
        fputs("  add eax, ecx\n", function->stream);
        break;
      case FCC_AST_BINARY_SUBTRACT:
        fputs("  sub eax, ecx\n", function->stream);
        break;
      case FCC_AST_BINARY_MULTIPLY:
        fputs("  imul eax, ecx\n", function->stream);
        break;
      case FCC_AST_BINARY_DIVIDE:
        if (operation_is_unsigned) {
          fputs("  xor edx, edx\n", function->stream);
          fputs("  div ecx\n", function->stream);
        } else {
          fputs("  cdq\n", function->stream);
          fputs("  idiv ecx\n", function->stream);
        }
        break;
      case FCC_AST_BINARY_MODULO:
        if (operation_is_unsigned) {
          fputs("  xor edx, edx\n", function->stream);
          fputs("  div ecx\n", function->stream);
        } else {
          fputs("  cdq\n", function->stream);
          fputs("  idiv ecx\n", function->stream);
        }
        fputs("  mov eax, edx\n", function->stream);
        break;
      case FCC_AST_BINARY_LESS:
        fputs("  cmp eax, ecx\n", function->stream);
        fputs(operation_is_unsigned ? "  setb al\n" : "  setl al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_LESS_EQUAL:
        fputs("  cmp eax, ecx\n", function->stream);
        fputs(operation_is_unsigned ? "  setbe al\n" : "  setle al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_GREATER:
        fputs("  cmp eax, ecx\n", function->stream);
        fputs(operation_is_unsigned ? "  seta al\n" : "  setg al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_GREATER_EQUAL:
        fputs("  cmp eax, ecx\n", function->stream);
        fputs(operation_is_unsigned ? "  setae al\n" : "  setge al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_EQUAL:
        fputs("  cmp eax, ecx\n", function->stream);
        fputs("  sete al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_NOT_EQUAL:
        fputs("  cmp eax, ecx\n", function->stream);
        fputs("  setne al\n", function->stream);
        fputs("  movzx eax, al\n", function->stream);
        break;
      case FCC_AST_BINARY_BITWISE_AND:
        fputs("  and eax, ecx\n", function->stream);
        break;
      case FCC_AST_BINARY_BITWISE_XOR:
        fputs("  xor eax, ecx\n", function->stream);
        break;
      case FCC_AST_BINARY_BITWISE_OR:
        fputs("  or eax, ecx\n", function->stream);
        break;
      case FCC_AST_BINARY_LEFT_SHIFT:
        fputs("  shl eax, cl\n", function->stream);
        break;
      case FCC_AST_BINARY_RIGHT_SHIFT:
        fputs(operation_is_unsigned ? "  shr eax, cl\n" : "  sar eax, cl\n", function->stream);
        break;
      case FCC_AST_BINARY_LOGICAL_AND:
      case FCC_AST_BINARY_LOGICAL_OR:
        break;
    }
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_integer_compound_operation(FccCodegenFunction* function,
                                                        FccAstBinaryOpKind op_kind) {
  assert(function != NULL);

  switch (op_kind) {
    case FCC_AST_BINARY_ADD:
      fputs("  add eax, ecx\n", function->stream);
      break;
    case FCC_AST_BINARY_SUBTRACT:
      fputs("  sub eax, ecx\n", function->stream);
      break;
    case FCC_AST_BINARY_MULTIPLY:
      fputs("  imul eax, ecx\n", function->stream);
      break;
    case FCC_AST_BINARY_DIVIDE:
      fputs("  cdq\n", function->stream);
      fputs("  idiv ecx\n", function->stream);
      break;
    case FCC_AST_BINARY_MODULO:
      fputs("  cdq\n", function->stream);
      fputs("  idiv ecx\n", function->stream);
      fputs("  mov eax, edx\n", function->stream);
      break;
    case FCC_AST_BINARY_BITWISE_AND:
      fputs("  and eax, ecx\n", function->stream);
      break;
    case FCC_AST_BINARY_BITWISE_XOR:
      fputs("  xor eax, ecx\n", function->stream);
      break;
    case FCC_AST_BINARY_BITWISE_OR:
      fputs("  or eax, ecx\n", function->stream);
      break;
    case FCC_AST_BINARY_LEFT_SHIFT:
      fputs("  shl eax, cl\n", function->stream);
      break;
    case FCC_AST_BINARY_RIGHT_SHIFT:
      fputs("  sar eax, cl\n", function->stream);
      break;
    case FCC_AST_BINARY_LESS:
    case FCC_AST_BINARY_LESS_EQUAL:
    case FCC_AST_BINARY_GREATER:
    case FCC_AST_BINARY_GREATER_EQUAL:
    case FCC_AST_BINARY_EQUAL:
    case FCC_AST_BINARY_NOT_EQUAL:
    case FCC_AST_BINARY_LOGICAL_AND:
    case FCC_AST_BINARY_LOGICAL_OR:
      return fcc_codegen_emit_error(function, function->function_definition->span,
                                    "invalid compound assignment operator in code generation");
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_pointer_compound_operation(FccCodegenFunction* function,
                                                        FccAstBinaryOpKind op_kind,
                                                        FccTypeId target_type_id,
                                                        FccSourceSpan span) {
  size_t element_size;

  assert(function != NULL);

  if ((op_kind != FCC_AST_BINARY_ADD) && (op_kind != FCC_AST_BINARY_SUBTRACT)) {
    return fcc_codegen_emit_error(function, span,
                                  "invalid pointer compound assignment in code generation");
  }

  if (!fcc_codegen_pointer_element_size(function, target_type_id, span, &element_size)) {
    return false;
  }

  if (element_size != 1) {
    fprintf(function->stream, "  imul rcx, rcx, %zu\n", element_size);
  }

  if (op_kind == FCC_AST_BINARY_ADD) {
    fputs("  add rax, rcx\n", function->stream);
  } else {
    fputs("  sub rax, rcx\n", function->stream);
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_assign(FccCodegenFunction* function,
                                    const FccAstExpression* expression) {
  const FccAstExpression* target;
  FccTypeId target_type_id;

  assert(function != NULL);
  assert(expression != NULL);

  target = expression->data.assign.target;
  target_type_id = fcc_codegen_expression_type_id(function, target);
  if (target_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_codegen_emit_error(function, target->span,
                                  "missing assignment target type metadata in code generation");
  }

  if (fcc_codegen_type_is_aggregate(function, target_type_id)) {
    size_t copy_size;

    copy_size = fcc_codegen_storage_size(function, target_type_id);
    if (copy_size == 0) {
      return fcc_codegen_emit_error(function, expression->span,
                                    "unsupported aggregate assignment size");
    }

    if (fcc_codegen_expression_is_lvalue(function, expression->data.assign.value)) {
      if (!fcc_codegen_emit_address_of_lvalue(function, expression->data.assign.value)) {
        return false;
      }
    } else if (!fcc_codegen_emit_expression(function, expression->data.assign.value)) {
      return false;
    }

    fputs("  push rax\n", function->stream);
    if (!fcc_codegen_emit_address_of_lvalue(function, target)) {
      return false;
    }

    fputs("  mov rcx, rax\n", function->stream);
    fputs("  pop rax\n", function->stream);
    return fcc_codegen_emit_memory_copy(function, copy_size);
  }

  if (!fcc_codegen_emit_expression(function, expression->data.assign.value)) {
    return false;
  }

  if (!fcc_codegen_emit_coerce_scalar(
          function, fcc_codegen_expression_type_id(function, expression->data.assign.value),
          target_type_id, expression->span)) {
    return false;
  }

  fputs("  push rax\n", function->stream);
  if (!fcc_codegen_emit_address_of_lvalue(function, target)) {
    return false;
  }

  fputs("  mov rcx, rax\n", function->stream);
  fputs("  pop rax\n", function->stream);
  if (!fcc_codegen_emit_store_to_memory(function, "[rcx]", target_type_id)) {
    return false;
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_compound_assign(FccCodegenFunction* function,
                                             const FccAstExpression* expression) {
  const FccAstExpression* target;
  FccTypeId target_type_id;
  bool target_is_pointer;

  assert(function != NULL);
  assert(expression != NULL);

  target = expression->data.compound_assign.target;
  target_type_id = fcc_codegen_expression_type_id(function, target);
  if (target_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_codegen_emit_error(function, target->span,
                                  "missing compound assignment target type metadata");
  }

  target_is_pointer = fcc_type_is_pointer(&function->sema_result->type_context, target_type_id);
  if (!fcc_codegen_emit_address_of_lvalue(function, target)) {
    return false;
  }

  fputs("  push rax\n", function->stream);
  if (!fcc_codegen_emit_load_from_memory(function, "[rax]", target_type_id)) {
    return false;
  }

  fputs("  push rax\n", function->stream);
  if (!fcc_codegen_emit_expression(function, expression->data.compound_assign.value)) {
    return false;
  }

  fputs("  mov rcx, rax\n", function->stream);
  fputs("  pop rax\n", function->stream);
  if (target_is_pointer) {
    if (!fcc_codegen_emit_pointer_compound_operation(
            function, expression->data.compound_assign.op_kind, target_type_id, expression->span)) {
      return false;
    }
  } else if (!fcc_codegen_emit_integer_compound_operation(
                 function, expression->data.compound_assign.op_kind)) {
    return false;
  }

  fputs("  pop rcx\n", function->stream);
  if (!fcc_codegen_emit_store_to_memory(function, "[rcx]", target_type_id)) {
    return false;
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_update_value(FccCodegenFunction* function, FccTypeId target_type_id,
                                          FccAstUpdateOpKind op_kind, FccSourceSpan span) {
  bool target_is_pointer;

  assert(function != NULL);

  target_is_pointer = fcc_type_is_pointer(&function->sema_result->type_context, target_type_id);
  if (target_is_pointer) {
    size_t element_size;

    if (!fcc_codegen_pointer_element_size(function, target_type_id, span, &element_size)) {
      return false;
    }

    if (op_kind == FCC_AST_UPDATE_INCREMENT) {
      fprintf(function->stream, "  add rax, %zu\n", element_size);
    } else {
      fprintf(function->stream, "  sub rax, %zu\n", element_size);
    }

    return fcc_codegen_check_stream(function);
  }

  if (op_kind == FCC_AST_UPDATE_INCREMENT) {
    fputs("  add eax, 1\n", function->stream);
  } else {
    fputs("  sub eax, 1\n", function->stream);
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_update(FccCodegenFunction* function,
                                    const FccAstExpression* expression) {
  const FccAstExpression* target;
  FccTypeId target_type_id;

  assert(function != NULL);
  assert(expression != NULL);

  target = expression->data.update.target;
  target_type_id = fcc_codegen_expression_type_id(function, target);
  if (target_type_id == FCC_TYPE_ID_INVALID) {
    return fcc_codegen_emit_error(function, target->span,
                                  "missing update target type metadata in code generation");
  }

  if (!fcc_codegen_emit_address_of_lvalue(function, target)) {
    return false;
  }

  fputs("  push rax\n", function->stream);
  if (!fcc_codegen_emit_load_from_memory(function, "[rax]", target_type_id)) {
    return false;
  }

  if (expression->data.update.is_postfix) {
    fputs("  push rax\n", function->stream);
  }

  if (!fcc_codegen_emit_update_value(function, target_type_id, expression->data.update.op_kind,
                                     expression->span)) {
    return false;
  }

  if (expression->data.update.is_postfix) {
    fputs("  pop rdx\n", function->stream);
  }

  fputs("  pop rcx\n", function->stream);
  if (!fcc_codegen_emit_store_to_memory(function, "[rcx]", target_type_id)) {
    return false;
  }

  if (expression->data.update.is_postfix) {
    fputs("  mov rax, rdx\n", function->stream);
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_conditional(FccCodegenFunction* function,
                                         const FccAstExpression* expression) {
  size_t else_label_id;
  size_t end_label_id;

  assert(function != NULL);
  assert(expression != NULL);

  else_label_id = fcc_codegen_allocate_label(function);
  end_label_id = fcc_codegen_allocate_label(function);

  if (!fcc_codegen_emit_expression(function, expression->data.conditional.condition)) {
    return false;
  }

  fputs("  cmp eax, 0\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "je", else_label_id);
  if (!fcc_codegen_emit_expression(function, expression->data.conditional.then_expression)) {
    return false;
  }

  fcc_codegen_emit_jump(function->stream, "jmp", end_label_id);
  fcc_codegen_emit_local_label(function->stream, else_label_id);
  if (!fcc_codegen_emit_expression(function, expression->data.conditional.else_expression)) {
    return false;
  }

  fcc_codegen_emit_local_label(function->stream, end_label_id);
  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_expression(FccCodegenFunction* function,
                                        const FccAstExpression* expression) {
  bool ok;

  assert(function != NULL);
  assert(expression != NULL);

  if (!fcc_codegen_push_recursion(function, expression->span)) {
    return false;
  }

  ok = true;
  switch (expression->kind) {
    case FCC_AST_EXPRESSION_INTEGER_LITERAL:
      ok = fcc_codegen_emit_integer_literal(function, expression);
      break;
    case FCC_AST_EXPRESSION_IDENTIFIER:
      ok = fcc_codegen_emit_identifier(function, expression);
      break;
    case FCC_AST_EXPRESSION_STRING_LITERAL:
      ok = fcc_codegen_emit_string_literal(function, expression);
      break;
    case FCC_AST_EXPRESSION_UNARY:
      ok = fcc_codegen_emit_unary(function, expression);
      break;
    case FCC_AST_EXPRESSION_BINARY:
      ok = fcc_codegen_emit_binary(function, expression);
      break;
    case FCC_AST_EXPRESSION_ASSIGN:
      ok = fcc_codegen_emit_assign(function, expression);
      break;
    case FCC_AST_EXPRESSION_COMPOUND_ASSIGN:
      ok = fcc_codegen_emit_compound_assign(function, expression);
      break;
    case FCC_AST_EXPRESSION_CALL:
      ok = fcc_codegen_emit_call(function, expression);
      break;
    case FCC_AST_EXPRESSION_SIZEOF:
      ok = fcc_codegen_emit_sizeof(function, expression);
      break;
    case FCC_AST_EXPRESSION_ALIGNOF:
      ok = fcc_codegen_emit_alignof(function, expression);
      break;
    case FCC_AST_EXPRESSION_CAST:
      ok = fcc_codegen_emit_cast(function, expression);
      break;
    case FCC_AST_EXPRESSION_SUBSCRIPT:
    case FCC_AST_EXPRESSION_MEMBER:
      ok = fcc_codegen_emit_lvalue_load(function, expression);
      break;
    case FCC_AST_EXPRESSION_UPDATE:
      ok = fcc_codegen_emit_update(function, expression);
      break;
    case FCC_AST_EXPRESSION_CONDITIONAL:
      ok = fcc_codegen_emit_conditional(function, expression);
      break;
    case FCC_AST_EXPRESSION_INITIALIZER_LIST:
      ok = fcc_codegen_emit_error(function, expression->span,
                                  "initializer list is unsupported in expression code generation");
      break;
  }

  fcc_codegen_pop_recursion(function);
  return ok;
}

static bool fcc_codegen_push_loop(FccCodegenFunction* function, FccSourceSpan span,
                                  size_t break_label_id, size_t continue_label_id) {
  assert(function != NULL);

  if (function->loop_depth >= FCC_MAX_CODEGEN_DEPTH) {
    return fcc_codegen_emit_error(function, span, "loop nesting exceeds FCC_MAX_CODEGEN_DEPTH");
  }

  if (function->break_depth >= FCC_MAX_CODEGEN_DEPTH) {
    return fcc_codegen_emit_error(function, span, "break nesting exceeds FCC_MAX_CODEGEN_DEPTH");
  }

  function->loop_targets[function->loop_depth].break_label_id = break_label_id;
  function->loop_targets[function->loop_depth].continue_label_id = continue_label_id;
  function->break_targets[function->break_depth] = break_label_id;
  ++function->loop_depth;
  ++function->break_depth;
  return true;
}

static void fcc_codegen_pop_loop(FccCodegenFunction* function) {
  assert(function != NULL);
  assert(function->loop_depth > 0);

  --function->loop_depth;
  --function->break_depth;
}

static bool fcc_codegen_push_break_target(FccCodegenFunction* function, FccSourceSpan span,
                                          size_t break_label_id) {
  assert(function != NULL);

  if (function->break_depth >= FCC_MAX_CODEGEN_DEPTH) {
    return fcc_codegen_emit_error(function, span, "break nesting exceeds FCC_MAX_CODEGEN_DEPTH");
  }

  function->break_targets[function->break_depth] = break_label_id;
  ++function->break_depth;
  return true;
}

static void fcc_codegen_pop_break_target(FccCodegenFunction* function) {
  assert(function != NULL);
  assert(function->break_depth > 0);

  --function->break_depth;
}

static bool fcc_codegen_emit_statement(FccCodegenFunction* function,
                                       const FccAstStatement* statement);

static bool fcc_codegen_emit_compound_statement(FccCodegenFunction* function,
                                                const FccAstStatement* statement, bool push_scope) {
  size_t item_index;

  assert(function != NULL);
  assert(statement != NULL);
  assert(statement->kind == FCC_AST_STATEMENT_COMPOUND);

  if (!fcc_codegen_push_recursion(function, statement->span)) {
    return false;
  }

  if (push_scope && !fcc_codegen_push_scope(function, statement->span)) {
    fcc_codegen_pop_recursion(function);
    return false;
  }

  for (item_index = 0; item_index < statement->data.compound.item_count; ++item_index) {
    if (!fcc_codegen_emit_statement(function, statement->data.compound.items[item_index])) {
      if (push_scope) {
        fcc_codegen_pop_scope(function);
      }

      fcc_codegen_pop_recursion(function);
      return false;
    }
  }

  if (push_scope) {
    fcc_codegen_pop_scope(function);
  }

  fcc_codegen_pop_recursion(function);
  return true;
}

static bool fcc_codegen_emit_local_initializer_to_stack(
    FccCodegenFunction* function, FccTypeId target_type_id, const FccAstExpression* initializer,
    int base_stack_offset, size_t member_offset);

static bool fcc_codegen_emit_local_initializer_list_to_stack(
    FccCodegenFunction* function, FccTypeId target_type_id, const FccAstExpression* initializer,
    int base_stack_offset, size_t member_offset) {
  const FccTypeContext* type_context;
  const FccType* target_type;
  FccTypeId resolved_type_id;

  assert(function != NULL);
  assert(initializer != NULL);
  assert(initializer->kind == FCC_AST_EXPRESSION_INITIALIZER_LIST);

  type_context = &function->sema_result->type_context;
  resolved_type_id = fcc_type_resolve_typedef(type_context, target_type_id);
  target_type = fcc_type_context_get(type_context, resolved_type_id);
  if (target_type == NULL) {
    return fcc_codegen_emit_error(function, initializer->span,
                                  "local initializer target type is invalid");
  }

  if (initializer->data.initializer_list.item_count == 0) {
    return true;
  }

  if (target_type->kind == FCC_TYPE_ARRAY) {
    size_t element_size;
    size_t item_index;
    bool size_ok;

    element_size =
        fcc_type_size_of(type_context, target_type->data.array.element_type_id, &size_ok);
    if (!size_ok) {
      return fcc_codegen_emit_error(function, initializer->span,
                                    "local array initializer element size is unsupported");
    }

    for (item_index = 0; item_index < initializer->data.initializer_list.item_count;
         ++item_index) {
      if (!fcc_codegen_emit_local_initializer_to_stack(
              function, target_type->data.array.element_type_id,
              initializer->data.initializer_list.items[item_index], base_stack_offset,
              member_offset + (item_index * element_size))) {
        return false;
      }
    }

    return true;
  }

  if (target_type->kind == FCC_TYPE_STRUCT) {
    size_t field_count;
    size_t item_index;

    field_count = fcc_type_record_field_count(type_context, resolved_type_id);
    for (item_index = 0;
         (item_index < initializer->data.initializer_list.item_count) && (item_index < field_count);
         ++item_index) {
      const FccTypeRecordField* field;

      field = fcc_type_record_field_at(type_context, resolved_type_id, item_index);
      if (field == NULL) {
        return fcc_codegen_emit_error(function, initializer->span,
                                      "local struct initializer field metadata is missing");
      }

      if (!fcc_codegen_emit_local_initializer_to_stack(
              function, field->type_id, initializer->data.initializer_list.items[item_index],
              base_stack_offset, member_offset + field->offset)) {
        return false;
      }
    }

    return true;
  }

  if (target_type->kind == FCC_TYPE_UNION) {
    const FccTypeRecordField* field;

    field = fcc_type_record_field_at(type_context, resolved_type_id, 0);
    if (field == NULL) {
      return true;
    }

    return fcc_codegen_emit_local_initializer_to_stack(
        function, field->type_id, initializer->data.initializer_list.items[0], base_stack_offset,
        member_offset);
  }

  if (initializer->data.initializer_list.item_count != 1) {
    return fcc_codegen_emit_error(function, initializer->span,
                                  "local scalar initializer list must contain one element");
  }

  return fcc_codegen_emit_local_initializer_to_stack(
      function, resolved_type_id, initializer->data.initializer_list.items[0], base_stack_offset,
      member_offset);
}

static bool fcc_codegen_emit_local_initializer_to_stack(
    FccCodegenFunction* function, FccTypeId target_type_id, const FccAstExpression* initializer,
    int base_stack_offset, size_t member_offset) {
  const FccTypeContext* type_context;
  size_t char_array_element_count;
  char memory_text[64];

  assert(function != NULL);
  assert(initializer != NULL);

  type_context = &function->sema_result->type_context;
  if (initializer->kind == FCC_AST_EXPRESSION_INITIALIZER_LIST) {
    return fcc_codegen_emit_local_initializer_list_to_stack(function, target_type_id, initializer,
                                                           base_stack_offset, member_offset);
  }

  if ((initializer->kind == FCC_AST_EXPRESSION_STRING_LITERAL) &&
      fcc_codegen_type_is_char_array(type_context, target_type_id, &char_array_element_count)) {
    if (initializer->data.string_literal.length + 1 > char_array_element_count) {
      return fcc_codegen_emit_error(function, initializer->span,
                                    "string initializer is too large for local char array");
    }

    if (!fcc_codegen_emit_string_literal(function, initializer)) {
      return false;
    }

    fcc_codegen_format_stack_memory(memory_text, sizeof(memory_text), base_stack_offset,
                                    member_offset);
    fprintf(function->stream, "  lea rcx, %s\n", memory_text);
    return fcc_codegen_emit_memory_copy(function, initializer->data.string_literal.length + 1);
  }

  if (fcc_codegen_type_is_aggregate(function, target_type_id)) {
    return fcc_codegen_emit_error(function, initializer->span,
                                  "local aggregate initializer is unsupported in code generation");
  }

  if (!fcc_codegen_emit_expression(function, initializer)) {
    return false;
  }

  if (!fcc_codegen_emit_coerce_scalar(function, fcc_codegen_expression_type_id(function, initializer),
                                      target_type_id, initializer->span)) {
    return false;
  }

  fcc_codegen_format_stack_memory(memory_text, sizeof(memory_text), base_stack_offset,
                                  member_offset);
  return fcc_codegen_emit_store_to_memory(function, memory_text, target_type_id);
}

static bool fcc_codegen_emit_return_statement(FccCodegenFunction* function,
                                              const FccAstStatement* statement) {
  FccTypeId return_type_id;

  assert(function != NULL);
  assert(statement != NULL);

  if (statement->data.return_statement.expression != NULL) {
    const FccSemaObjectInfo* function_info;
    FccTypeId expression_type_id;

    if (!fcc_codegen_emit_expression(function, statement->data.return_statement.expression)) {
      return false;
    }

    function_info = fcc_codegen_find_object_info(function, function->function_definition);
    if (function_info == NULL) {
      return fcc_codegen_emit_error(function, statement->span,
                                    "missing function type metadata in return code generation");
    }

    return_type_id =
        fcc_type_get_function_return_type(&function->sema_result->type_context,
                                          function_info->type_id);
    expression_type_id =
        fcc_codegen_expression_type_id(function, statement->data.return_statement.expression);
    if (!fcc_codegen_emit_coerce_scalar(function, expression_type_id, return_type_id,
                                        statement->span)) {
      return false;
    }
  } else {
    fputs("  xor eax, eax\n", function->stream);
  }

  fcc_codegen_emit_jump(function->stream, "jmp", function->return_label_id);
  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_declaration_statement(FccCodegenFunction* function,
                                                   const FccAstStatement* statement) {
  const FccSemaObjectInfo* object_info;
  int stack_offset;

  assert(function != NULL);
  assert(statement != NULL);

  if (statement->data.declaration.name == NULL) {
    return true;
  }

  object_info = fcc_codegen_find_object_info(function, statement);
  if (object_info == NULL) {
    return fcc_codegen_emit_error(function, statement->span,
                                  "internal declaration semantic metadata lookup failed");
  }

  if (!fcc_codegen_object_requires_storage(object_info)) {
    return true;
  }

  if (!fcc_codegen_define_binding(function, statement->span, statement->data.declaration.name,
                                  object_info->type_id, &stack_offset)) {
    return false;
  }

  if (statement->data.declaration.initializer != NULL) {
    if (fcc_codegen_type_is_aggregate(function, object_info->type_id)) {
      size_t initializer_size;
      char memory_text[64];

      initializer_size = fcc_codegen_storage_size(function, object_info->type_id);
      if (initializer_size == 0) {
        return fcc_codegen_emit_error(function, statement->span,
                                      "unsupported aggregate initializer size");
      }

      (void)snprintf(memory_text, sizeof(memory_text), "[rbp - %d]", stack_offset);
      fprintf(function->stream, "  lea rax, %s\n", memory_text);
      if ((statement->data.declaration.initializer->kind ==
           FCC_AST_EXPRESSION_INITIALIZER_LIST) ||
          (statement->data.declaration.initializer->kind == FCC_AST_EXPRESSION_STRING_LITERAL)) {
        if (!fcc_codegen_emit_memory_zero(function, initializer_size)) {
          return false;
        }

        return fcc_codegen_emit_local_initializer_to_stack(
            function, object_info->type_id, statement->data.declaration.initializer, stack_offset,
            0);
      }

      fputs("  push rax\n", function->stream);
      if (statement->data.declaration.initializer->kind == FCC_AST_EXPRESSION_STRING_LITERAL) {
        if (!fcc_codegen_emit_string_literal(function, statement->data.declaration.initializer)) {
          return false;
        }
      } else if (fcc_codegen_expression_is_lvalue(function, statement->data.declaration.initializer)) {
        if (!fcc_codegen_emit_address_of_lvalue(function,
                                                statement->data.declaration.initializer)) {
          return false;
        }
      } else if (!fcc_codegen_emit_expression(function, statement->data.declaration.initializer)) {
        return false;
      }

      fputs("  pop rcx\n", function->stream);
      return fcc_codegen_emit_memory_copy(function, initializer_size);
    }

    if (!fcc_codegen_emit_expression(function, statement->data.declaration.initializer)) {
      return false;
    }

    if (!fcc_codegen_emit_coerce_scalar(
            function,
            fcc_codegen_expression_type_id(function, statement->data.declaration.initializer),
            object_info->type_id, statement->data.declaration.initializer->span)) {
      return false;
    }

    {
      char memory_text[64];

      (void)snprintf(memory_text, sizeof(memory_text), "[rbp - %d]", stack_offset);
      return fcc_codegen_emit_store_to_memory(function, memory_text, object_info->type_id);
    }
  }

  return true;
}

static bool fcc_codegen_append_switch_case(FccCodegenFunction* function,
                                           FccCodegenSwitchCase** cases, size_t* case_count,
                                           size_t* case_capacity,
                                           const FccAstStatement* statement) {
  FccCodegenSwitchCase* new_cases;
  size_t new_capacity;
  const FccSemaExpressionInfo* expression_info;

  assert(function != NULL);
  assert(cases != NULL);
  assert(case_count != NULL);
  assert(case_capacity != NULL);
  assert(statement != NULL);

  if (*case_count == *case_capacity) {
    new_capacity = *case_capacity;
    if (new_capacity == 0) {
      new_capacity = 8;
    } else {
      new_capacity *= 2;
    }

    if ((new_capacity < *case_count) ||
        (new_capacity > (SIZE_MAX / sizeof(FccCodegenSwitchCase)))) {
      return fcc_codegen_emit_error(function, statement->span, "too many switch cases");
    }

    new_cases = (FccCodegenSwitchCase*)realloc(*cases, new_capacity * sizeof(FccCodegenSwitchCase));
    if (new_cases == NULL) {
      return fcc_codegen_out_of_memory(function, statement->span);
    }

    *cases = new_cases;
    *case_capacity = new_capacity;
  }

  (*cases)[*case_count].statement = statement;
  (*cases)[*case_count].value = 0;
  (*cases)[*case_count].label_id = fcc_codegen_allocate_label(function);
  (*cases)[*case_count].is_default = statement->kind == FCC_AST_STATEMENT_DEFAULT;
  if (statement->kind == FCC_AST_STATEMENT_CASE) {
    expression_info =
        fcc_codegen_find_expression_info(function, statement->data.case_statement.value);
    if ((expression_info == NULL) || !expression_info->has_integer_constant ||
        (expression_info->integer_constant_value < INT_MIN) ||
        (expression_info->integer_constant_value > INT_MAX)) {
      return fcc_codegen_emit_error(function, statement->span,
                                    "missing case constant metadata in code generation");
    }

    (*cases)[*case_count].value = expression_info->integer_constant_value;
  }

  ++(*case_count);
  return true;
}

static bool fcc_codegen_collect_switch_cases(FccCodegenFunction* function,
                                             const FccAstStatement* statement,
                                             FccCodegenSwitchCase** cases, size_t* case_count,
                                             size_t* case_capacity) {
  size_t item_index;

  assert(function != NULL);
  assert(cases != NULL);
  assert(case_count != NULL);
  assert(case_capacity != NULL);

  if (statement == NULL) {
    return true;
  }

  switch (statement->kind) {
    case FCC_AST_STATEMENT_COMPOUND:
      for (item_index = 0; item_index < statement->data.compound.item_count; ++item_index) {
        if (!fcc_codegen_collect_switch_cases(function, statement->data.compound.items[item_index],
                                              cases, case_count, case_capacity)) {
          return false;
        }
      }

      return true;
    case FCC_AST_STATEMENT_CASE:
      return fcc_codegen_append_switch_case(function, cases, case_count, case_capacity,
                                            statement) &&
             fcc_codegen_collect_switch_cases(function, statement->data.case_statement.statement,
                                              cases, case_count, case_capacity);
    case FCC_AST_STATEMENT_DEFAULT:
      return fcc_codegen_append_switch_case(function, cases, case_count, case_capacity,
                                            statement) &&
             fcc_codegen_collect_switch_cases(function, statement->data.default_statement.statement,
                                              cases, case_count, case_capacity);
    case FCC_AST_STATEMENT_IF:
      return fcc_codegen_collect_switch_cases(function, statement->data.if_statement.then_statement,
                                              cases, case_count, case_capacity) &&
             fcc_codegen_collect_switch_cases(function, statement->data.if_statement.else_statement,
                                              cases, case_count, case_capacity);
    case FCC_AST_STATEMENT_WHILE:
      return fcc_codegen_collect_switch_cases(function, statement->data.while_statement.body, cases,
                                              case_count, case_capacity);
    case FCC_AST_STATEMENT_DO_WHILE:
      return fcc_codegen_collect_switch_cases(function, statement->data.do_while_statement.body,
                                              cases, case_count, case_capacity);
    case FCC_AST_STATEMENT_FOR:
      return fcc_codegen_collect_switch_cases(function, statement->data.for_statement.body, cases,
                                              case_count, case_capacity);
    case FCC_AST_STATEMENT_LABEL:
      return fcc_codegen_collect_switch_cases(function, statement->data.label_statement.statement,
                                              cases, case_count, case_capacity);
    case FCC_AST_STATEMENT_SWITCH:
    case FCC_AST_STATEMENT_RETURN:
    case FCC_AST_STATEMENT_EXPRESSION:
    case FCC_AST_STATEMENT_DECLARATION:
    case FCC_AST_STATEMENT_STATIC_ASSERT:
    case FCC_AST_STATEMENT_BREAK:
    case FCC_AST_STATEMENT_CONTINUE:
    case FCC_AST_STATEMENT_GOTO:
      return true;
  }

  return true;
}

static const FccCodegenSwitchCase*
fcc_codegen_find_active_switch_case(const FccCodegenFunction* function,
                                    const FccAstStatement* statement) {
  size_t case_index;

  assert(function != NULL);
  assert(statement != NULL);

  for (case_index = 0; case_index < function->switch_case_count; ++case_index) {
    if (function->switch_cases[case_index].statement == statement) {
      return &function->switch_cases[case_index];
    }
  }

  return NULL;
}

static bool fcc_codegen_emit_if_statement(FccCodegenFunction* function,
                                          const FccAstStatement* statement) {
  size_t false_label_id;
  size_t end_label_id;

  assert(function != NULL);
  assert(statement != NULL);

  false_label_id = fcc_codegen_allocate_label(function);
  end_label_id = fcc_codegen_allocate_label(function);
  if (!fcc_codegen_emit_expression(function, statement->data.if_statement.condition)) {
    return false;
  }

  fputs("  cmp eax, 0\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "je", false_label_id);
  if (!fcc_codegen_emit_statement(function, statement->data.if_statement.then_statement)) {
    return false;
  }

  if (statement->data.if_statement.else_statement != NULL) {
    fcc_codegen_emit_jump(function->stream, "jmp", end_label_id);
    fcc_codegen_emit_local_label(function->stream, false_label_id);
    if (!fcc_codegen_emit_statement(function, statement->data.if_statement.else_statement)) {
      return false;
    }

    fcc_codegen_emit_local_label(function->stream, end_label_id);
  } else {
    fcc_codegen_emit_local_label(function->stream, false_label_id);
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_while_statement(FccCodegenFunction* function,
                                             const FccAstStatement* statement) {
  size_t condition_label_id;
  size_t end_label_id;

  assert(function != NULL);
  assert(statement != NULL);

  condition_label_id = fcc_codegen_allocate_label(function);
  end_label_id = fcc_codegen_allocate_label(function);
  fcc_codegen_emit_local_label(function->stream, condition_label_id);
  if (!fcc_codegen_emit_expression(function, statement->data.while_statement.condition)) {
    return false;
  }

  fputs("  cmp eax, 0\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "je", end_label_id);
  if (!fcc_codegen_push_loop(function, statement->span, end_label_id, condition_label_id)) {
    return false;
  }

  if (!fcc_codegen_emit_statement(function, statement->data.while_statement.body)) {
    fcc_codegen_pop_loop(function);
    return false;
  }

  fcc_codegen_pop_loop(function);
  fcc_codegen_emit_jump(function->stream, "jmp", condition_label_id);
  fcc_codegen_emit_local_label(function->stream, end_label_id);
  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_do_while_statement(FccCodegenFunction* function,
                                                const FccAstStatement* statement) {
  size_t body_label_id;
  size_t condition_label_id;
  size_t end_label_id;

  assert(function != NULL);
  assert(statement != NULL);

  body_label_id = fcc_codegen_allocate_label(function);
  condition_label_id = fcc_codegen_allocate_label(function);
  end_label_id = fcc_codegen_allocate_label(function);
  fcc_codegen_emit_local_label(function->stream, body_label_id);
  if (!fcc_codegen_push_loop(function, statement->span, end_label_id, condition_label_id)) {
    return false;
  }

  if (!fcc_codegen_emit_statement(function, statement->data.do_while_statement.body)) {
    fcc_codegen_pop_loop(function);
    return false;
  }

  fcc_codegen_pop_loop(function);
  fcc_codegen_emit_local_label(function->stream, condition_label_id);
  if (!fcc_codegen_emit_expression(function, statement->data.do_while_statement.condition)) {
    return false;
  }

  fputs("  cmp eax, 0\n", function->stream);
  fcc_codegen_emit_jump(function->stream, "jne", body_label_id);
  fcc_codegen_emit_local_label(function->stream, end_label_id);
  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_for_statement(FccCodegenFunction* function,
                                           const FccAstStatement* statement) {
  size_t condition_label_id;
  size_t update_label_id;
  size_t end_label_id;

  assert(function != NULL);
  assert(statement != NULL);

  if (!fcc_codegen_push_scope(function, statement->span)) {
    return false;
  }

  if ((statement->data.for_statement.init_statement != NULL) &&
      !fcc_codegen_emit_statement(function, statement->data.for_statement.init_statement)) {
    fcc_codegen_pop_scope(function);
    return false;
  }

  condition_label_id = fcc_codegen_allocate_label(function);
  update_label_id = fcc_codegen_allocate_label(function);
  end_label_id = fcc_codegen_allocate_label(function);
  fcc_codegen_emit_local_label(function->stream, condition_label_id);
  if (statement->data.for_statement.condition != NULL) {
    if (!fcc_codegen_emit_expression(function, statement->data.for_statement.condition)) {
      fcc_codegen_pop_scope(function);
      return false;
    }

    fputs("  cmp eax, 0\n", function->stream);
    fcc_codegen_emit_jump(function->stream, "je", end_label_id);
  }

  if (!fcc_codegen_push_loop(function, statement->span, end_label_id, update_label_id)) {
    fcc_codegen_pop_scope(function);
    return false;
  }

  if (!fcc_codegen_emit_statement(function, statement->data.for_statement.body)) {
    fcc_codegen_pop_loop(function);
    fcc_codegen_pop_scope(function);
    return false;
  }

  fcc_codegen_pop_loop(function);
  fcc_codegen_emit_local_label(function->stream, update_label_id);
  if (statement->data.for_statement.update != NULL) {
    if (!fcc_codegen_emit_expression(function, statement->data.for_statement.update)) {
      fcc_codegen_pop_scope(function);
      return false;
    }
  }

  fcc_codegen_emit_jump(function->stream, "jmp", condition_label_id);
  fcc_codegen_emit_local_label(function->stream, end_label_id);
  fcc_codegen_pop_scope(function);
  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_switch_statement(FccCodegenFunction* function,
                                              const FccAstStatement* statement) {
  const FccCodegenSwitchCase* saved_switch_cases;
  FccCodegenSwitchCase* cases;
  size_t saved_switch_case_count;
  size_t case_count;
  size_t case_capacity;
  size_t case_index;
  size_t default_label_id;
  size_t end_label_id;
  bool has_default;
  bool ok;

  assert(function != NULL);
  assert(statement != NULL);

  cases = NULL;
  case_count = 0;
  case_capacity = 0;
  if (!fcc_codegen_collect_switch_cases(function, statement->data.switch_statement.body, &cases,
                                        &case_count, &case_capacity)) {
    free(cases);
    return false;
  }

  end_label_id = fcc_codegen_allocate_label(function);
  default_label_id = end_label_id;
  has_default = false;
  for (case_index = 0; case_index < case_count; ++case_index) {
    if (cases[case_index].is_default) {
      default_label_id = cases[case_index].label_id;
      has_default = true;
      break;
    }
  }

  if (!fcc_codegen_emit_expression(function, statement->data.switch_statement.condition)) {
    free(cases);
    return false;
  }

  for (case_index = 0; case_index < case_count; ++case_index) {
    if (cases[case_index].is_default) {
      continue;
    }

    fprintf(function->stream, "  cmp eax, %lld\n", (long long)cases[case_index].value);
    fcc_codegen_emit_jump(function->stream, "je", cases[case_index].label_id);
  }

  if (has_default) {
    fcc_codegen_emit_jump(function->stream, "jmp", default_label_id);
  } else {
    fcc_codegen_emit_jump(function->stream, "jmp", end_label_id);
  }

  saved_switch_cases = function->switch_cases;
  saved_switch_case_count = function->switch_case_count;
  function->switch_cases = cases;
  function->switch_case_count = case_count;
  if (!fcc_codegen_push_break_target(function, statement->span, end_label_id)) {
    function->switch_cases = saved_switch_cases;
    function->switch_case_count = saved_switch_case_count;
    free(cases);
    return false;
  }

  ok = fcc_codegen_emit_statement(function, statement->data.switch_statement.body);
  fcc_codegen_pop_break_target(function);
  function->switch_cases = saved_switch_cases;
  function->switch_case_count = saved_switch_case_count;
  fcc_codegen_emit_local_label(function->stream, end_label_id);
  free(cases);
  return ok && fcc_codegen_check_stream(function);
}

static bool fcc_codegen_emit_case_statement(FccCodegenFunction* function,
                                            const FccAstStatement* statement) {
  const FccCodegenSwitchCase* switch_case;

  assert(function != NULL);
  assert(statement != NULL);

  switch_case = fcc_codegen_find_active_switch_case(function, statement);
  if (switch_case == NULL) {
    return fcc_codegen_emit_error(function, statement->span, "internal case label lookup failed");
  }

  fcc_codegen_emit_local_label(function->stream, switch_case->label_id);
  return fcc_codegen_emit_statement(function, statement->data.case_statement.statement);
}

static bool fcc_codegen_emit_default_statement(FccCodegenFunction* function,
                                               const FccAstStatement* statement) {
  const FccCodegenSwitchCase* switch_case;

  assert(function != NULL);
  assert(statement != NULL);

  switch_case = fcc_codegen_find_active_switch_case(function, statement);
  if (switch_case == NULL) {
    return fcc_codegen_emit_error(function, statement->span,
                                  "internal default label lookup failed");
  }

  fcc_codegen_emit_local_label(function->stream, switch_case->label_id);
  return fcc_codegen_emit_statement(function, statement->data.default_statement.statement);
}

static bool fcc_codegen_emit_statement(FccCodegenFunction* function,
                                       const FccAstStatement* statement) {
  bool ok;

  assert(function != NULL);
  assert(statement != NULL);

  ok = true;
  switch (statement->kind) {
    case FCC_AST_STATEMENT_COMPOUND:
      ok = fcc_codegen_emit_compound_statement(function, statement, true);
      break;
    case FCC_AST_STATEMENT_RETURN:
      ok = fcc_codegen_emit_return_statement(function, statement);
      break;
    case FCC_AST_STATEMENT_EXPRESSION:
      if (statement->data.expression_statement.expression != NULL) {
        ok = fcc_codegen_emit_expression(function, statement->data.expression_statement.expression);
      }

      break;
    case FCC_AST_STATEMENT_DECLARATION:
      ok = fcc_codegen_emit_declaration_statement(function, statement);
      break;
    case FCC_AST_STATEMENT_STATIC_ASSERT:
      break;
    case FCC_AST_STATEMENT_IF:
      ok = fcc_codegen_emit_if_statement(function, statement);
      break;
    case FCC_AST_STATEMENT_WHILE:
      ok = fcc_codegen_emit_while_statement(function, statement);
      break;
    case FCC_AST_STATEMENT_DO_WHILE:
      ok = fcc_codegen_emit_do_while_statement(function, statement);
      break;
    case FCC_AST_STATEMENT_FOR:
      ok = fcc_codegen_emit_for_statement(function, statement);
      break;
    case FCC_AST_STATEMENT_SWITCH:
      ok = fcc_codegen_emit_switch_statement(function, statement);
      break;
    case FCC_AST_STATEMENT_CASE:
      ok = fcc_codegen_emit_case_statement(function, statement);
      break;
    case FCC_AST_STATEMENT_DEFAULT:
      ok = fcc_codegen_emit_default_statement(function, statement);
      break;
    case FCC_AST_STATEMENT_BREAK:
      if (function->break_depth == 0) {
        ok = fcc_codegen_emit_error(function, statement->span,
                                    "internal break target stack underflow for break");
      } else {
        fcc_codegen_emit_jump(function->stream, "jmp",
                              function->break_targets[function->break_depth - 1]);
        ok = fcc_codegen_check_stream(function);
      }

      break;
    case FCC_AST_STATEMENT_CONTINUE:
      if (function->loop_depth == 0) {
        ok = fcc_codegen_emit_error(function, statement->span,
                                    "internal loop target stack underflow for continue");
      } else {
        fcc_codegen_emit_jump(function->stream, "jmp",
                              function->loop_targets[function->loop_depth - 1].continue_label_id);
        ok = fcc_codegen_check_stream(function);
      }

      break;
    case FCC_AST_STATEMENT_GOTO:
      fcc_codegen_emit_user_jump(function->stream, statement->data.goto_statement.name);
      ok = fcc_codegen_check_stream(function);
      break;
    case FCC_AST_STATEMENT_LABEL:
      fcc_codegen_emit_user_label(function->stream, statement->data.label_statement.name);
      ok = fcc_codegen_emit_statement(function, statement->data.label_statement.statement);
      break;
  }

  return ok;
}

static bool fcc_codegen_emit_parameter_homes(FccCodegenFunction* function) {
  static const char* const PARAMETER_REGISTERS[FCC_MAX_WINDOWS_X64_REGISTER_ARGUMENTS] = {
      "rcx",
      "rdx",
      "r8",
      "r9",
  };
  size_t parameter_index;
  size_t register_parameter_count;

  assert(function != NULL);

  register_parameter_count = function->function_definition->parameter_count;
  if (register_parameter_count > FCC_MAX_WINDOWS_X64_REGISTER_ARGUMENTS) {
    register_parameter_count = FCC_MAX_WINDOWS_X64_REGISTER_ARGUMENTS;
  }

  /*
   * Parameters are copied into local stack homes before the body runs. This keeps
   * later lvalue/address-of handling uniform: every named parameter has a normal
   * binding, even if it originally arrived in a Windows x64 register.
   */
  for (parameter_index = 0; parameter_index < register_parameter_count; ++parameter_index) {
    fprintf(function->stream, "  push %s\n", PARAMETER_REGISTERS[parameter_index]);
  }

  for (parameter_index = 0; parameter_index < function->function_definition->parameter_count;
       ++parameter_index) {
    const FccSemaObjectInfo* object_info;
    int stack_offset;
    char memory_text[64];

    object_info = fcc_codegen_find_object_info(
        function, &function->function_definition->parameters[parameter_index]);
    if (object_info == NULL) {
      return fcc_codegen_emit_error(function,
                                    function->function_definition->parameters[parameter_index].span,
                                    "internal parameter semantic metadata lookup failed");
    }

    if (!fcc_codegen_define_binding(function,
                                    function->function_definition->parameters[parameter_index].span,
                                    function->function_definition->parameters[parameter_index].name,
                                    object_info->type_id, &stack_offset)) {
      return false;
    }

    (void)snprintf(memory_text, sizeof(memory_text), "[rbp - %d]", stack_offset);
    if (parameter_index < FCC_MAX_WINDOWS_X64_REGISTER_ARGUMENTS) {
      fprintf(function->stream, "  mov rax, qword [rsp + %zu]\n",
              (register_parameter_count - parameter_index - 1) * 8);
      if (fcc_codegen_type_is_aggregate(function, object_info->type_id)) {
        size_t copy_size;

        copy_size = fcc_codegen_storage_size(function, object_info->type_id);
        if (copy_size == 0) {
          return fcc_codegen_emit_error(
              function, function->function_definition->parameters[parameter_index].span,
              "unsupported aggregate parameter size");
        }

        fputs("  push rax\n", function->stream);
        fprintf(function->stream, "  lea rcx, %s\n", memory_text);
        fputs("  pop rax\n", function->stream);
        if (!fcc_codegen_emit_memory_copy(function, copy_size)) {
          return false;
        }
      } else if (!fcc_codegen_emit_store_to_memory(function, memory_text, object_info->type_id)) {
        return false;
      }
    } else {
      fprintf(function->stream, "  mov rax, qword [rbp + %zu]\n",
              48 + ((parameter_index - FCC_MAX_WINDOWS_X64_REGISTER_ARGUMENTS) * 8));
      if (fcc_codegen_type_is_aggregate(function, object_info->type_id)) {
        size_t copy_size;

        copy_size = fcc_codegen_storage_size(function, object_info->type_id);
        if (copy_size == 0) {
          return fcc_codegen_emit_error(
              function, function->function_definition->parameters[parameter_index].span,
              "unsupported aggregate parameter size");
        }

        fputs("  push rax\n", function->stream);
        fprintf(function->stream, "  lea rcx, %s\n", memory_text);
        fputs("  pop rax\n", function->stream);
        if (!fcc_codegen_emit_memory_copy(function, copy_size)) {
          return false;
        }
      } else if (!fcc_codegen_emit_store_to_memory(function, memory_text, object_info->type_id)) {
        return false;
      }
    }
  }

  if (register_parameter_count != 0) {
    fprintf(function->stream, "  add rsp, %zu\n", register_parameter_count * 8);
  }

  return fcc_codegen_check_stream(function);
}

static bool fcc_codegen_count_parameter_stack(FccCodegenFunction* function,
                                              size_t* parameter_stack_size) {
  size_t parameter_index;

  assert(function != NULL);
  assert(parameter_stack_size != NULL);

  *parameter_stack_size = 0;
  for (parameter_index = 0; parameter_index < function->function_definition->parameter_count;
       ++parameter_index) {
    const FccSemaObjectInfo* object_info;
    size_t allocation_size;

    object_info = fcc_codegen_find_object_info(
        function, &function->function_definition->parameters[parameter_index]);
    if (object_info == NULL) {
      return fcc_codegen_emit_error(function,
                                    function->function_definition->parameters[parameter_index].span,
                                    "internal parameter semantic metadata lookup failed");
    }

    allocation_size = fcc_codegen_stack_allocation_size(function, object_info->type_id);
    if (allocation_size == 0) {
      return fcc_codegen_emit_error(function,
                                    function->function_definition->parameters[parameter_index].span,
                                    "unsupported parameter type for stack allocation");
    }

    if (*parameter_stack_size > (SIZE_MAX - allocation_size)) {
      return fcc_codegen_emit_error(function,
                                    function->function_definition->parameters[parameter_index].span,
                                    "stack frame exceeds backend offset range");
    }

    *parameter_stack_size += allocation_size;
  }

  return true;
}

static bool fcc_codegen_ensure_string_literal_capacity(FccCodegenStringLiteral** string_literals,
                                                       size_t* string_literal_capacity,
                                                       size_t capacity) {
  size_t new_capacity;
  FccCodegenStringLiteral* new_string_literals;

  assert(string_literals != NULL);
  assert(string_literal_capacity != NULL);

  if (capacity <= *string_literal_capacity) {
    return true;
  }

  new_capacity = *string_literal_capacity;
  if (new_capacity == 0) {
    new_capacity = 8;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(FccCodegenStringLiteral))) {
    return false;
  }

  new_string_literals = (FccCodegenStringLiteral*)realloc(
      *string_literals, new_capacity * sizeof(FccCodegenStringLiteral));
  if (new_string_literals == NULL) {
    return false;
  }

  *string_literals = new_string_literals;
  *string_literal_capacity = new_capacity;
  return true;
}

static bool fcc_codegen_append_string_literal(FccCodegenStringLiteral** string_literals,
                                              size_t* string_literal_count,
                                              size_t* string_literal_capacity,
                                              const FccAstExpression* expression) {
  size_t string_index;

  assert(string_literals != NULL);
  assert(string_literal_count != NULL);
  assert(string_literal_capacity != NULL);
  assert(expression != NULL);

  for (string_index = 0; string_index < *string_literal_count; ++string_index) {
    if ((*string_literals)[string_index].expression == expression) {
      return true;
    }
  }

  if (!fcc_codegen_ensure_string_literal_capacity(string_literals, string_literal_capacity,
                                                  *string_literal_count + 1)) {
    return false;
  }

  (*string_literals)[*string_literal_count].expression = expression;
  (*string_literals)[*string_literal_count].label_id = *string_literal_count;
  ++(*string_literal_count);
  return true;
}

static bool fcc_codegen_collect_expression_string_literals(
    FccCodegenStringLiteral** string_literals, size_t* string_literal_count,
    size_t* string_literal_capacity, const FccAstExpression* expression, size_t depth) {
  size_t argument_index;

  assert(string_literals != NULL);
  assert(string_literal_count != NULL);
  assert(string_literal_capacity != NULL);

  if (expression == NULL) {
    return true;
  }

  if (depth > FCC_MAX_CODEGEN_DEPTH) {
    return false;
  }

  switch (expression->kind) {
    case FCC_AST_EXPRESSION_INTEGER_LITERAL:
    case FCC_AST_EXPRESSION_IDENTIFIER:
      return true;
    case FCC_AST_EXPRESSION_STRING_LITERAL:
      return fcc_codegen_append_string_literal(string_literals, string_literal_count,
                                               string_literal_capacity, expression);
    case FCC_AST_EXPRESSION_UNARY:
      return fcc_codegen_collect_expression_string_literals(
          string_literals, string_literal_count, string_literal_capacity,
          expression->data.unary.operand, depth + 1);
    case FCC_AST_EXPRESSION_BINARY:
      return fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 expression->data.binary.left, depth + 1) &&
             fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 expression->data.binary.right, depth + 1);
    case FCC_AST_EXPRESSION_ASSIGN:
      return fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 expression->data.assign.target, depth + 1) &&
             fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 expression->data.assign.value, depth + 1);
    case FCC_AST_EXPRESSION_COMPOUND_ASSIGN:
      return fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 expression->data.compound_assign.target, depth + 1) &&
             fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 expression->data.compound_assign.value, depth + 1);
    case FCC_AST_EXPRESSION_CALL:
      if (!fcc_codegen_collect_expression_string_literals(
              string_literals, string_literal_count, string_literal_capacity,
              expression->data.call.callee, depth + 1)) {
        return false;
      }

      for (argument_index = 0; argument_index < expression->data.call.argument_count;
           ++argument_index) {
        if (!fcc_codegen_collect_expression_string_literals(
                string_literals, string_literal_count, string_literal_capacity,
                expression->data.call.arguments[argument_index], depth + 1)) {
          return false;
        }
      }

      return true;
    case FCC_AST_EXPRESSION_SIZEOF:
      if (expression->data.sizeof_expression.has_type_operand) {
        return true;
      }

      return fcc_codegen_collect_expression_string_literals(
          string_literals, string_literal_count, string_literal_capacity,
          expression->data.sizeof_expression.operand, depth + 1);
    case FCC_AST_EXPRESSION_ALIGNOF:
      return true;
    case FCC_AST_EXPRESSION_CAST:
      return fcc_codegen_collect_expression_string_literals(
          string_literals, string_literal_count, string_literal_capacity,
          expression->data.cast.operand, depth + 1);
    case FCC_AST_EXPRESSION_SUBSCRIPT:
      return fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 expression->data.subscript.target, depth + 1) &&
             fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 expression->data.subscript.index, depth + 1);
    case FCC_AST_EXPRESSION_MEMBER:
      return fcc_codegen_collect_expression_string_literals(
          string_literals, string_literal_count, string_literal_capacity,
          expression->data.member.target, depth + 1);
    case FCC_AST_EXPRESSION_UPDATE:
      return fcc_codegen_collect_expression_string_literals(
          string_literals, string_literal_count, string_literal_capacity,
          expression->data.update.target, depth + 1);
    case FCC_AST_EXPRESSION_CONDITIONAL:
      return fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 expression->data.conditional.condition, depth + 1) &&
             fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 expression->data.conditional.then_expression, depth + 1) &&
             fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 expression->data.conditional.else_expression, depth + 1);
    case FCC_AST_EXPRESSION_INITIALIZER_LIST:
      for (argument_index = 0; argument_index < expression->data.initializer_list.item_count;
           ++argument_index) {
        if (!fcc_codegen_collect_expression_string_literals(
                string_literals, string_literal_count, string_literal_capacity,
                expression->data.initializer_list.items[argument_index], depth + 1)) {
          return false;
        }
      }

      return true;
  }

  return false;
}

static bool fcc_codegen_collect_statement_string_literals(FccCodegenStringLiteral** string_literals,
                                                          size_t* string_literal_count,
                                                          size_t* string_literal_capacity,
                                                          const FccAstStatement* statement,
                                                          size_t depth) {
  size_t item_index;

  assert(string_literals != NULL);
  assert(string_literal_count != NULL);
  assert(string_literal_capacity != NULL);

  if (statement == NULL) {
    return true;
  }

  if (depth > FCC_MAX_CODEGEN_DEPTH) {
    return false;
  }

  switch (statement->kind) {
    case FCC_AST_STATEMENT_COMPOUND:
      for (item_index = 0; item_index < statement->data.compound.item_count; ++item_index) {
        if (!fcc_codegen_collect_statement_string_literals(
                string_literals, string_literal_count, string_literal_capacity,
                statement->data.compound.items[item_index], depth + 1)) {
          return false;
        }
      }

      return true;
    case FCC_AST_STATEMENT_RETURN:
      return fcc_codegen_collect_expression_string_literals(
          string_literals, string_literal_count, string_literal_capacity,
          statement->data.return_statement.expression, depth + 1);
    case FCC_AST_STATEMENT_EXPRESSION:
      return fcc_codegen_collect_expression_string_literals(
          string_literals, string_literal_count, string_literal_capacity,
          statement->data.expression_statement.expression, depth + 1);
    case FCC_AST_STATEMENT_DECLARATION:
      return fcc_codegen_collect_expression_string_literals(
          string_literals, string_literal_count, string_literal_capacity,
          statement->data.declaration.initializer, depth + 1);
    case FCC_AST_STATEMENT_STATIC_ASSERT:
      return true;
    case FCC_AST_STATEMENT_IF:
      return fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.if_statement.condition, depth + 1) &&
             fcc_codegen_collect_statement_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.if_statement.then_statement, depth + 1) &&
             fcc_codegen_collect_statement_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.if_statement.else_statement, depth + 1);
    case FCC_AST_STATEMENT_WHILE:
      return fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.while_statement.condition, depth + 1) &&
             fcc_codegen_collect_statement_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.while_statement.body, depth + 1);
    case FCC_AST_STATEMENT_DO_WHILE:
      return fcc_codegen_collect_statement_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.do_while_statement.body, depth + 1) &&
             fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.do_while_statement.condition, depth + 1);
    case FCC_AST_STATEMENT_FOR:
      return fcc_codegen_collect_statement_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.for_statement.init_statement, depth + 1) &&
             fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.for_statement.condition, depth + 1) &&
             fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.for_statement.update, depth + 1) &&
             fcc_codegen_collect_statement_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.for_statement.body, depth + 1);
    case FCC_AST_STATEMENT_SWITCH:
      return fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.switch_statement.condition, depth + 1) &&
             fcc_codegen_collect_statement_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.switch_statement.body, depth + 1);
    case FCC_AST_STATEMENT_CASE:
      return fcc_codegen_collect_expression_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.case_statement.value, depth + 1) &&
             fcc_codegen_collect_statement_string_literals(
                 string_literals, string_literal_count, string_literal_capacity,
                 statement->data.case_statement.statement, depth + 1);
    case FCC_AST_STATEMENT_DEFAULT:
      return fcc_codegen_collect_statement_string_literals(
          string_literals, string_literal_count, string_literal_capacity,
          statement->data.default_statement.statement, depth + 1);
    case FCC_AST_STATEMENT_LABEL:
      return fcc_codegen_collect_statement_string_literals(
          string_literals, string_literal_count, string_literal_capacity,
          statement->data.label_statement.statement, depth + 1);
    case FCC_AST_STATEMENT_BREAK:
    case FCC_AST_STATEMENT_CONTINUE:
    case FCC_AST_STATEMENT_GOTO:
      return true;
  }

  return false;
}

static bool fcc_codegen_try_evaluate_global_initializer(const FccSemaResult* sema_result,
                                                        const FccAstExpression* expression,
                                                        long long* value_out) {
  const FccSemaExpressionInfo* expression_info;

  assert(sema_result != NULL);
  assert(expression != NULL);
  assert(value_out != NULL);

  expression_info = fcc_sema_result_find_expression_info(sema_result, expression);
  if ((expression_info == NULL) || !expression_info->has_integer_constant) {
    return false;
  }

  *value_out = (long long)expression_info->integer_constant_value;
  return true;
}

static const char* fcc_codegen_try_get_global_address_initializer(
    const FccSemaResult* sema_result, const FccAstExpression* expression) {
  const FccSemaExpressionInfo* expression_info;

  assert(sema_result != NULL);
  assert(expression != NULL);

  expression_info = fcc_sema_result_find_expression_info(sema_result, expression);
  if ((expression_info == NULL) || !expression_info->has_address_constant) {
    return NULL;
  }

  return expression_info->address_constant_symbol_name;
}

static const char* fcc_codegen_nasm_data_directive(size_t storage_size) {
  switch (storage_size) {
    case 1:
      return "db";
    case 2:
      return "dw";
    case 4:
      return "dd";
    case 8:
      return "dq";
  }

  return NULL;
}

static const char* fcc_codegen_nasm_bss_directive(size_t storage_size) {
  switch (storage_size) {
    case 1:
      return "resb";
    case 2:
      return "resw";
    case 4:
      return "resd";
    case 8:
      return "resq";
  }

  return NULL;
}

static bool fcc_codegen_emit_global_zero_bytes(FILE* stream, size_t byte_count) {
  assert(stream != NULL);

  if (byte_count == 0) {
    return true;
  }

  fprintf(stream, "  times %zu db 0\n", byte_count);
  return ferror(stream) == 0;
}

static bool fcc_codegen_emit_global_integer_value(FILE* stream, size_t storage_size,
                                                  int64_t value) {
  const char* directive;

  assert(stream != NULL);

  directive = fcc_codegen_nasm_data_directive(storage_size);
  if (directive == NULL) {
    return false;
  }

  fprintf(stream, "  %s %lld\n", directive, (long long)value);
  return ferror(stream) == 0;
}

static bool fcc_codegen_type_is_char_array(const FccTypeContext* type_context,
                                           FccTypeId type_id,
                                           size_t* element_count_out) {
  const FccType* element_type;
  const FccType* type;

  assert(type_context != NULL);
  assert(element_count_out != NULL);

  type_id = fcc_type_resolve_typedef(type_context, type_id);
  type = fcc_type_context_get(type_context, type_id);
  if ((type == NULL) || (type->kind != FCC_TYPE_ARRAY) || type->data.array.is_vla) {
    return false;
  }

  element_type = fcc_type_context_get(type_context,
                                      fcc_type_resolve_typedef(type_context,
                                                               type->data.array.element_type_id));
  if ((element_type == NULL) || (element_type->kind != FCC_TYPE_CHAR)) {
    return false;
  }

  *element_count_out = type->data.array.element_count;
  return true;
}

static bool fcc_codegen_emit_global_string_to_char_array(FILE* stream,
                                                         const FccSourceFile* source_file,
                                                         const FccAstExpression* initializer,
                                                         size_t element_count,
                                                         FccDiagnostics* diagnostics) {
  const char* bytes;
  size_t byte_index;
  size_t emitted_count;

  assert(stream != NULL);
  assert(source_file != NULL);
  assert(initializer != NULL);
  assert(initializer->kind == FCC_AST_EXPRESSION_STRING_LITERAL);
  assert(diagnostics != NULL);

  if (initializer->data.string_literal.length + 1 > element_count) {
    fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                         "string initializer is too large for global char array");
    return false;
  }

  bytes = initializer->data.string_literal.bytes;
  fputs("  db ", stream);
  for (byte_index = 0; byte_index < initializer->data.string_literal.length; ++byte_index) {
    if (byte_index != 0) {
      fputs(", ", stream);
    }

    fprintf(stream, "%u", (unsigned int)(unsigned char)bytes[byte_index]);
  }

  if (initializer->data.string_literal.length != 0) {
    fputs(", ", stream);
  }

  fputs("0\n", stream);
  emitted_count = initializer->data.string_literal.length + 1;
  return (ferror(stream) == 0) &&
         fcc_codegen_emit_global_zero_bytes(stream, element_count - emitted_count);
}

static bool fcc_codegen_emit_global_initializer(
    FILE* stream, const FccSourceFile* source_file, const FccSemaResult* sema_result,
    const FccAstExpression* initializer, FccTypeId target_type_id,
    const FccCodegenStringLiteral* string_literals, size_t string_literal_count,
    FccDiagnostics* diagnostics);

static bool fcc_codegen_emit_global_initializer_list(
    FILE* stream, const FccSourceFile* source_file, const FccSemaResult* sema_result,
    const FccAstExpression* initializer, FccTypeId target_type_id,
    const FccCodegenStringLiteral* string_literals, size_t string_literal_count,
    FccDiagnostics* diagnostics) {
  const FccTypeContext* type_context;
  const FccType* target_type;
  FccTypeId resolved_type_id;
  size_t target_size;
  size_t bytes_written;
  bool size_ok;

  assert(stream != NULL);
  assert(source_file != NULL);
  assert(sema_result != NULL);
  assert(initializer != NULL);
  assert(initializer->kind == FCC_AST_EXPRESSION_INITIALIZER_LIST);
  assert(diagnostics != NULL);

  type_context = &sema_result->type_context;
  resolved_type_id = fcc_type_resolve_typedef(type_context, target_type_id);
  target_type = fcc_type_context_get(type_context, resolved_type_id);
  if (target_type == NULL) {
    fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                         "global initializer target type is invalid");
    return false;
  }

  target_size = fcc_type_size_of(type_context, resolved_type_id, &size_ok);
  if (!size_ok) {
    fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                         "global initializer target type has unsupported size");
    return false;
  }

  if (initializer->data.initializer_list.item_count == 0) {
    return fcc_codegen_emit_global_zero_bytes(stream, target_size);
  }

  if (target_type->kind == FCC_TYPE_ARRAY) {
    size_t element_size;
    size_t item_index;

    element_size =
        fcc_type_size_of(type_context, target_type->data.array.element_type_id, &size_ok);
    if (!size_ok) {
      fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                           "global array initializer element size is unsupported");
      return false;
    }

    for (item_index = 0; item_index < initializer->data.initializer_list.item_count;
         ++item_index) {
      if (!fcc_codegen_emit_global_initializer(
              stream, source_file, sema_result,
              initializer->data.initializer_list.items[item_index],
              target_type->data.array.element_type_id, string_literals, string_literal_count,
              diagnostics)) {
        return false;
      }
    }

    return fcc_codegen_emit_global_zero_bytes(
        stream, (target_type->data.array.element_count -
                 initializer->data.initializer_list.item_count) *
                    element_size);
  }

  if (target_type->kind == FCC_TYPE_STRUCT) {
    size_t field_count;
    size_t item_index;

    bytes_written = 0;
    field_count = fcc_type_record_field_count(type_context, resolved_type_id);
    for (item_index = 0;
         (item_index < initializer->data.initializer_list.item_count) && (item_index < field_count);
         ++item_index) {
      const FccTypeRecordField* field;
      size_t field_size;

      field = fcc_type_record_field_at(type_context, resolved_type_id, item_index);
      if (field == NULL) {
        fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                             "global struct initializer field metadata is missing");
        return false;
      }

      if (field->offset < bytes_written) {
        fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                             "global struct initializer layout is inconsistent");
        return false;
      }

      if (!fcc_codegen_emit_global_zero_bytes(stream, field->offset - bytes_written)) {
        return false;
      }

      if (!fcc_codegen_emit_global_initializer(
              stream, source_file, sema_result,
              initializer->data.initializer_list.items[item_index], field->type_id, string_literals,
              string_literal_count, diagnostics)) {
        return false;
      }

      field_size = fcc_type_size_of(type_context, field->type_id, &size_ok);
      if (!size_ok) {
        fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                             "global struct initializer field size is unsupported");
        return false;
      }

      bytes_written = field->offset + field_size;
    }

    return fcc_codegen_emit_global_zero_bytes(stream, target_size - bytes_written);
  }

  if (target_type->kind == FCC_TYPE_UNION) {
    const FccTypeRecordField* field;
    size_t field_size;

    field = fcc_type_record_field_at(type_context, resolved_type_id, 0);
    if (field == NULL) {
      return fcc_codegen_emit_global_zero_bytes(stream, target_size);
    }

    if (!fcc_codegen_emit_global_initializer(stream, source_file, sema_result,
                                             initializer->data.initializer_list.items[0],
                                             field->type_id, string_literals,
                                             string_literal_count, diagnostics)) {
      return false;
    }

    field_size = fcc_type_size_of(type_context, field->type_id, &size_ok);
    if (!size_ok) {
      fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                           "global union initializer field size is unsupported");
      return false;
    }

    return fcc_codegen_emit_global_zero_bytes(stream, target_size - field_size);
  }

  if (initializer->data.initializer_list.item_count != 1) {
    fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                         "global scalar initializer list must contain one element");
    return false;
  }

  return fcc_codegen_emit_global_initializer(stream, source_file, sema_result,
                                             initializer->data.initializer_list.items[0],
                                             resolved_type_id, string_literals,
                                             string_literal_count, diagnostics);
}

static bool fcc_codegen_emit_global_initializer(
    FILE* stream, const FccSourceFile* source_file, const FccSemaResult* sema_result,
    const FccAstExpression* initializer, FccTypeId target_type_id,
    const FccCodegenStringLiteral* string_literals, size_t string_literal_count,
    FccDiagnostics* diagnostics) {
  const FccTypeContext* type_context;
  const FccType* target_type;
  FccTypeId resolved_type_id;
  size_t storage_size;
  size_t char_array_element_count;
  bool size_ok;

  assert(stream != NULL);
  assert(source_file != NULL);
  assert(sema_result != NULL);
  assert(initializer != NULL);
  assert(diagnostics != NULL);

  type_context = &sema_result->type_context;
  resolved_type_id = fcc_type_resolve_typedef(type_context, target_type_id);
  target_type = fcc_type_context_get(type_context, resolved_type_id);
  if (target_type == NULL) {
    fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                         "global initializer target type is invalid");
    return false;
  }

  storage_size = fcc_type_size_of(type_context, resolved_type_id, &size_ok);
  if (!size_ok) {
    fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                         "global initializer target type has unsupported size");
    return false;
  }

  if (initializer->kind == FCC_AST_EXPRESSION_INITIALIZER_LIST) {
    return fcc_codegen_emit_global_initializer_list(
        stream, source_file, sema_result, initializer, resolved_type_id, string_literals,
        string_literal_count, diagnostics);
  }

  if ((initializer->kind == FCC_AST_EXPRESSION_STRING_LITERAL) &&
      fcc_codegen_type_is_char_array(type_context, resolved_type_id, &char_array_element_count)) {
    return fcc_codegen_emit_global_string_to_char_array(stream, source_file, initializer,
                                                        char_array_element_count, diagnostics);
  }

  if ((initializer->kind == FCC_AST_EXPRESSION_STRING_LITERAL) &&
      (target_type->kind == FCC_TYPE_POINTER)) {
    const FccCodegenStringLiteral* string_literal;

    string_literal =
        fcc_codegen_find_string_literal_in_table(string_literals, string_literal_count, initializer);
    if (string_literal == NULL) {
      fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                           "internal string literal label lookup failed");
      return false;
    }

    fprintf(stream, "  dq FCC_STR%zu\n", string_literal->label_id);
    return ferror(stream) == 0;
  }

  if (target_type->kind == FCC_TYPE_POINTER) {
    const char* address_symbol_name;

    address_symbol_name = fcc_codegen_try_get_global_address_initializer(sema_result, initializer);
    if (address_symbol_name != NULL) {
      if (storage_size != 8) {
        fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                             "global address initializer width is unsupported in code generation");
        return false;
      }

      fprintf(stream, "  dq %s\n", address_symbol_name);
      return ferror(stream) == 0;
    }
  }

  if (fcc_type_is_integer(type_context, resolved_type_id) || (target_type->kind == FCC_TYPE_ENUM) ||
      (target_type->kind == FCC_TYPE_POINTER)) {
    long long initializer_value;

    if (!fcc_codegen_try_evaluate_global_initializer(sema_result, initializer,
                                                     &initializer_value)) {
      fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                           "global initializer is unsupported in code generation");
      return false;
    }

    if (!fcc_codegen_emit_global_integer_value(stream, storage_size, initializer_value)) {
      fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                           "global initializer width is unsupported in code generation");
      return false;
    }

    return true;
  }

  fcc_diag_emit_source(diagnostics, source_file, initializer->span, FCC_DIAG_SEVERITY_ERROR,
                       "global initializer is unsupported in code generation");
  return false;
}

static bool
fcc_codegen_collect_translation_unit_string_literals(const FccAstTranslationUnit* translation_unit,
                                                     FccCodegenStringLiteral** string_literals_out,
                                                     size_t* string_literal_count_out) {
  FccCodegenStringLiteral* string_literals;
  size_t function_index;
  size_t global_index;
  size_t string_literal_count;
  size_t string_literal_capacity;

  assert(translation_unit != NULL);
  assert(string_literals_out != NULL);
  assert(string_literal_count_out != NULL);

  string_literals = NULL;
  string_literal_count = 0;
  string_literal_capacity = 0;
  for (global_index = 0; global_index < translation_unit->global_count; ++global_index) {
    if (!fcc_codegen_collect_expression_string_literals(
            &string_literals, &string_literal_count, &string_literal_capacity,
            translation_unit->globals[global_index]->initializer, 0)) {
      free(string_literals);
      return false;
    }
  }

  for (function_index = 0; function_index < translation_unit->function_count; ++function_index) {
    if (!fcc_codegen_collect_statement_string_literals(
            &string_literals, &string_literal_count, &string_literal_capacity,
            translation_unit->functions[function_index]->body, 0)) {
      free(string_literals);
      return false;
    }
  }

  *string_literals_out = string_literals;
  *string_literal_count_out = string_literal_count;
  return true;
}

static bool fcc_codegen_emit_string_literals(FILE* stream,
                                             const FccCodegenStringLiteral* string_literals,
                                             size_t string_literal_count) {
  size_t byte_index;
  size_t string_index;

  assert(stream != NULL);

  for (string_index = 0; string_index < string_literal_count; ++string_index) {
    const char* bytes;
    size_t length;

    bytes = string_literals[string_index].expression->data.string_literal.bytes;
    length = string_literals[string_index].expression->data.string_literal.length;
    fprintf(stream, "FCC_STR%zu:\n", string_literals[string_index].label_id);
    fputs("  db ", stream);
    for (byte_index = 0; byte_index < length; ++byte_index) {
      if (byte_index != 0) {
        fputs(", ", stream);
      }

      fprintf(stream, "%u", (unsigned int)(unsigned char)bytes[byte_index]);
    }

    if (length != 0) {
      fputs(", ", stream);
    }

    fputs("0\n\n", stream);
  }

  return true;
}

static bool fcc_codegen_emit_global_variable(FILE* stream, const FccSourceFile* source_file,
                                             const FccAstGlobalVariable* global_variable,
                                             const FccSemaObjectInfo* object_info,
                                             FccSemaResult* sema_result,
                                             const FccCodegenStringLiteral* string_literals,
                                             size_t string_literal_count,
                                             FccDiagnostics* diagnostics) {
  const char* directive;
  size_t storage_size;
  bool size_ok;

  assert(stream != NULL);
  assert(source_file != NULL);
  assert(global_variable != NULL);
  assert(object_info != NULL);
  assert(sema_result != NULL);
  assert(diagnostics != NULL);

  storage_size = fcc_type_size_of(&sema_result->type_context, object_info->type_id, &size_ok);
  if (!size_ok) {
    fcc_diag_emit_source(diagnostics, source_file, global_variable->span, FCC_DIAG_SEVERITY_ERROR,
                         "global type is unsupported in code generation");
    return false;
  }

  fprintf(stream, "%s:\n", global_variable->name);
  if (global_variable->initializer != NULL) {
    if (!fcc_codegen_emit_global_initializer(stream, source_file, sema_result,
                                             global_variable->initializer, object_info->type_id,
                                             string_literals, string_literal_count, diagnostics)) {
      return false;
    }

    fputc('\n', stream);
  } else {
    directive = fcc_codegen_nasm_bss_directive(storage_size);
    if (directive != NULL) {
      fprintf(stream, "  %s 1\n\n", directive);
    } else {
      fprintf(stream, "  resb %zu\n\n", storage_size);
    }
  }

  return ferror(stream) == 0;
}

static bool fcc_codegen_emit_function(FILE* stream, const FccSourceFile* source_file,
                                      const FccAstTranslationUnit* translation_unit,
                                      FccSemaResult* sema_result,
                                      const FccCodegenStringLiteral* string_literals,
                                      size_t string_literal_count,
                                      const FccAstFunctionDefinition* function_definition,
                                      FccDiagnostics* diagnostics) {
  FccCodegenFunction function;
  size_t frame_size;
  size_t parameter_stack_size;
  size_t local_stack_size;
  bool ok;

  assert(stream != NULL);
  assert(source_file != NULL);
  assert(function_definition != NULL);
  assert(diagnostics != NULL);

  memset(&function, 0, sizeof(function));
  function.stream = stream;
  function.source_file = source_file;
  function.translation_unit = translation_unit;
  function.sema_result = sema_result;
  function.string_literals = string_literals;
  function.string_literal_count = string_literal_count;
  function.function_definition = function_definition;
  function.diagnostics = diagnostics;
  function.return_label_id = 0;
  parameter_stack_size = 0;
  local_stack_size = 0;
  ok = false;

  if (!function_definition->has_body) {
    return true;
  }

  /*
   * The backend computes the full frame size before emitting the prologue so
   * local bindings and parameter homes can use fixed rbp-relative offsets.
   */
  if (!fcc_codegen_count_parameter_stack(&function, &parameter_stack_size)) {
    goto cleanup;
  }

  if (!fcc_codegen_count_statement_locals(&function, function_definition->body,
                                          &local_stack_size)) {
    goto cleanup;
  }

  if (parameter_stack_size > (SIZE_MAX - local_stack_size)) {
    (void)fcc_codegen_emit_error(&function, function_definition->span,
                                 "stack frame exceeds backend offset range");
    goto cleanup;
  }

  function.total_stack_size = parameter_stack_size + local_stack_size;
  frame_size = fcc_codegen_align_to_16(function.total_stack_size);
  function.recursion_depth = 0;
  function.return_label_id = fcc_codegen_allocate_label(&function);
  fprintf(stream, "%s:\n", function_definition->name);
  fputs("  push rbp\n", stream);
  fputs("  mov rbp, rsp\n", stream);
  if (frame_size != 0) {
    fprintf(stream, "  sub rsp, %zu\n", frame_size);
  }

  if (!fcc_codegen_push_scope(&function, function_definition->span)) {
    goto cleanup;
  }

  if (!fcc_codegen_emit_parameter_homes(&function)) {
    goto cleanup;
  }

  if (!fcc_codegen_emit_compound_statement(&function, function_definition->body, false)) {
    goto cleanup;
  }

  fcc_codegen_pop_scope(&function);
  fputs("  xor eax, eax\n", stream);
  fcc_codegen_emit_local_label(stream, function.return_label_id);
  fputs("  mov rsp, rbp\n", stream);
  fputs("  pop rbp\n", stream);
  fputs("  ret\n\n", stream);
  ok = fcc_codegen_check_stream(&function);

cleanup:
  free(function.bindings);
  free(function.scopes);
  return ok;
}

static bool
fcc_codegen_translation_unit_has_function_definition(const FccAstTranslationUnit* translation_unit,
                                                     const char* name) {
  size_t function_index;

  assert(translation_unit != NULL);
  assert(name != NULL);

  for (function_index = 0; function_index < translation_unit->function_count; ++function_index) {
    if (translation_unit->functions[function_index]->has_body &&
        (strcmp(translation_unit->functions[function_index]->name, name) == 0)) {
      return true;
    }
  }

  return false;
}

static bool fcc_codegen_function_name_seen_before(const FccAstTranslationUnit* translation_unit,
                                                  size_t function_limit, const char* name) {
  size_t function_index;

  assert(translation_unit != NULL);
  assert(name != NULL);

  for (function_index = 0; function_index < function_limit; ++function_index) {
    if (strcmp(translation_unit->functions[function_index]->name, name) == 0) {
      return true;
    }
  }

  return false;
}

bool fcc_codegen_emit_nasm_x64_with_sema(FILE* stream, const FccSourceFile* source_file,
                                         const FccAstTranslationUnit* translation_unit,
                                         FccSemaResult* sema_result, FccDiagnostics* diagnostics) {
  bool emitted_bss_section;
  bool emitted_data_section;
  FccCodegenStringLiteral* string_literals;
  size_t function_index;
  size_t global_index;
  size_t string_literal_count;

  assert(stream != NULL);
  assert(source_file != NULL);
  assert(translation_unit != NULL);
  assert(sema_result != NULL);
  assert(diagnostics != NULL);

  string_literals = NULL;
  string_literal_count = 0;
  emitted_bss_section = false;
  emitted_data_section = false;
  if (!fcc_codegen_collect_translation_unit_string_literals(translation_unit, &string_literals,
                                                            &string_literal_count)) {
    fcc_diag_emit_source(diagnostics, source_file, translation_unit->span, FCC_DIAG_SEVERITY_FATAL,
                         "out of memory while collecting string literals");
    return false;
  }

  fputs("; FCC Phase 5 NASM x86_64 output for Windows 11\n", stream);
  fputs("bits 64\n", stream);
  fputs("default rel\n\n", stream);
  for (global_index = 0; global_index < translation_unit->global_count; ++global_index) {
    const FccSemaObjectInfo* object_info;

    object_info =
        fcc_sema_result_find_object_info(sema_result, translation_unit->globals[global_index]);
    if ((object_info == NULL) || (object_info->symbol_kind == FCC_SYMBOL_TYPEDEF)) {
      continue;
    }

    if ((object_info->storage_class == FCC_STORAGE_CLASS_EXTERN) &&
        (translation_unit->globals[global_index]->initializer == NULL)) {
      fprintf(stream, "extern %s\n", translation_unit->globals[global_index]->name);
    } else if (object_info->storage_class != FCC_STORAGE_CLASS_STATIC) {
      fprintf(stream, "global %s\n", translation_unit->globals[global_index]->name);
    }
  }

  for (function_index = 0; function_index < translation_unit->function_count; ++function_index) {
    const FccAstFunctionDefinition* function_definition;
    const FccSemaObjectInfo* object_info;

    function_definition = translation_unit->functions[function_index];
    if (fcc_codegen_function_name_seen_before(translation_unit, function_index,
                                              function_definition->name)) {
      continue;
    }

    object_info = fcc_sema_result_find_object_info(sema_result, function_definition);
    if ((object_info != NULL) && (object_info->storage_class == FCC_STORAGE_CLASS_STATIC)) {
      continue;
    }

    if (fcc_codegen_translation_unit_has_function_definition(translation_unit,
                                                             function_definition->name)) {
      fprintf(stream, "global %s\n", function_definition->name);
    } else {
      fprintf(stream, "extern %s\n", function_definition->name);
    }
  }

  fputc('\n', stream);
  if (string_literal_count != 0) {
    fputs("section .rdata\n", stream);
    if (!fcc_codegen_emit_string_literals(stream, string_literals, string_literal_count)) {
      free(string_literals);
      return false;
    }
  }

  for (global_index = 0; global_index < translation_unit->global_count; ++global_index) {
    const FccAstGlobalVariable* global_variable;
    const FccSemaObjectInfo* object_info;

    global_variable = translation_unit->globals[global_index];
    object_info = fcc_sema_result_find_object_info(sema_result, global_variable);
    if ((object_info == NULL) || (object_info->symbol_kind == FCC_SYMBOL_TYPEDEF) ||
        ((object_info->storage_class == FCC_STORAGE_CLASS_EXTERN) &&
         (global_variable->initializer == NULL))) {
      continue;
    }

    if (global_variable->initializer != NULL) {
      continue;
    }

    if (!emitted_bss_section) {
      fputs("section .bss\n", stream);
      emitted_bss_section = true;
    }

    if (!fcc_codegen_emit_global_variable(stream, source_file, global_variable, object_info,
                                          sema_result, string_literals, string_literal_count,
                                          diagnostics)) {
      free(string_literals);
      return false;
    }
  }

  for (global_index = 0; global_index < translation_unit->global_count; ++global_index) {
    const FccAstGlobalVariable* global_variable;
    const FccSemaObjectInfo* object_info;

    global_variable = translation_unit->globals[global_index];
    object_info = fcc_sema_result_find_object_info(sema_result, global_variable);
    if ((object_info == NULL) || (object_info->symbol_kind == FCC_SYMBOL_TYPEDEF) ||
        ((object_info->storage_class == FCC_STORAGE_CLASS_EXTERN) &&
         (global_variable->initializer == NULL)) ||
        (global_variable->initializer == NULL)) {
      continue;
    }

    if (!emitted_data_section) {
      fputs("section .data\n", stream);
      emitted_data_section = true;
    }

    if (!fcc_codegen_emit_global_variable(stream, source_file, global_variable, object_info,
                                          sema_result, string_literals, string_literal_count,
                                          diagnostics)) {
      free(string_literals);
      return false;
    }
  }

  fputs("section .text\n", stream);
  for (function_index = 0; function_index < translation_unit->function_count; ++function_index) {
    if (!fcc_codegen_emit_function(stream, source_file, translation_unit, sema_result,
                                   string_literals, string_literal_count,
                                   translation_unit->functions[function_index], diagnostics)) {
      free(string_literals);
      return false;
    }
  }

  free(string_literals);
  if (ferror(stream) != 0) {
    fcc_diag_emit_source(diagnostics, source_file, translation_unit->span, FCC_DIAG_SEVERITY_FATAL,
                         "assembly output stream write failed");
    return false;
  }

  return true;
}

bool fcc_codegen_emit_nasm_x64(FILE* stream, const FccSourceFile* source_file,
                               const FccAstTranslationUnit* translation_unit,
                               FccDiagnostics* diagnostics) {
  FccSemaResult sema_result;
  bool ok;

  assert(stream != NULL);
  assert(source_file != NULL);
  assert(translation_unit != NULL);
  assert(diagnostics != NULL);

  fcc_sema_result_init(&sema_result);
  ok = fcc_sema_analyze_translation_unit(source_file, translation_unit, diagnostics, &sema_result);
  if (ok) {
    ok = fcc_codegen_emit_nasm_x64_with_sema(stream, source_file, translation_unit, &sema_result,
                                             diagnostics);
  }

  fcc_sema_result_dispose(&sema_result);
  return ok;
}
