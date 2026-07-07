// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/ast.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void fcc_ast_dump_static_assert(FILE* stream, const FccAstStaticAssert* static_assertion,
                                       size_t depth);

static bool fcc_ast_context_append_allocation(FccAstContext* context, void* allocation) {
  size_t new_capacity;
  void** new_allocations;

  assert(context != NULL);
  assert(allocation != NULL);

  if (context->allocation_count == context->allocation_capacity) {
    new_capacity = context->allocation_capacity;
    if (new_capacity == 0) {
      new_capacity = 16;
    } else {
      new_capacity *= 2;
    }

    if (new_capacity < context->allocation_count) {
      return false;
    }

    if (new_capacity > (SIZE_MAX / sizeof(void*))) {
      return false;
    }

    new_allocations = (void**)realloc(context->owned_allocations, new_capacity * sizeof(void*));
    if (new_allocations == NULL) {
      return false;
    }

    context->owned_allocations = new_allocations;
    context->allocation_capacity = new_capacity;
  }

  context->owned_allocations[context->allocation_count] = allocation;
  ++context->allocation_count;
  return true;
}

static FILE* fcc_ast_resolve_stream(FILE* stream) {
  if (stream != NULL) {
    return stream;
  }

  return stdout;
}

static const char* fcc_ast_safe_name(const char* name) {
  if (name != NULL) {
    return name;
  }

  return "<null-name>";
}

static void fcc_ast_dump_indent(FILE* stream, size_t depth) {
  size_t indent_index;

  for (indent_index = 0; indent_index < depth; ++indent_index) {
    fputs("  ", stream);
  }
}

static void fcc_ast_dump_type(FILE* stream, const FccAstType* type) {
  size_t pointer_index;
  size_t array_index;

  assert(stream != NULL);
  assert(type != NULL);

  if (type->is_const_qualified) {
    fputs("const ", stream);
  }

  if ((type->kind == FCC_AST_TYPE_TYPEDEF_NAME) && (type->typedef_name != NULL)) {
    fputs(type->typedef_name, stream);
  } else if ((type->kind == FCC_AST_TYPE_STRUCT) || (type->kind == FCC_AST_TYPE_UNION) ||
             (type->kind == FCC_AST_TYPE_ENUM)) {
    fputs(fcc_ast_type_name(type->kind), stream);
    if (type->tag_name != NULL) {
      fputc(' ', stream);
      fputs(type->tag_name, stream);
    } else {
      fputs(" <anonymous>", stream);
    }

    if (type->is_record_definition) {
      fprintf(stream, "{field_count=%zu}", type->record_field_count);
    } else if (type->is_enum_definition) {
      fprintf(stream, "{enumerator_count=%zu}", type->enumerator_count);
    }
  } else {
    fputs(fcc_ast_type_name(type->kind), stream);
  }

  for (pointer_index = 0; pointer_index < type->pointer_depth; ++pointer_index) {
    fputc('*', stream);
  }

  if (type->is_function_pointer) {
    size_t parameter_index;

    fputs(" (", stream);
    for (pointer_index = 0; pointer_index < type->function_pointer_depth; ++pointer_index) {
      fputc('*', stream);
    }

    fputs(")(", stream);
    for (parameter_index = 0; parameter_index < type->function_pointer_parameter_count;
         ++parameter_index) {
      if (parameter_index != 0) {
        fputs(", ", stream);
      }

      fcc_ast_dump_type(stream, &type->function_pointer_parameters[parameter_index].type);
    }

    if (type->function_pointer_is_variadic) {
      if (type->function_pointer_parameter_count != 0) {
        fputs(", ", stream);
      }

      fputs("...", stream);
    }

    fputc(')', stream);
  }

  for (array_index = 0; array_index < type->array_count; ++array_index) {
    const FccAstArrayBound* array_bound;

    if (type->array_bounds == NULL) {
      fputs("[<missing-bound>]", stream);
      continue;
    }

    array_bound = &type->array_bounds[array_index];
    if (array_bound->is_vla) {
      fputs("[]", stream);
    } else {
      fprintf(stream, "[%zu]", array_bound->element_count);
    }
  }
}

static const char* fcc_ast_unary_op_name(FccAstUnaryOpKind op_kind) {
  switch (op_kind) {
    case FCC_AST_UNARY_PLUS:
      return "plus";
    case FCC_AST_UNARY_NEGATE:
      return "negate";
    case FCC_AST_UNARY_LOGICAL_NOT:
      return "logical_not";
    case FCC_AST_UNARY_BITWISE_NOT:
      return "bitwise_not";
    case FCC_AST_UNARY_ADDRESS_OF:
      return "address_of";
    case FCC_AST_UNARY_DEREFERENCE:
      return "dereference";
  }

  return "unknown_unary";
}

static const char* fcc_ast_update_op_name(FccAstUpdateOpKind op_kind) {
  switch (op_kind) {
    case FCC_AST_UPDATE_INCREMENT:
      return "increment";
    case FCC_AST_UPDATE_DECREMENT:
      return "decrement";
  }

  return "unknown_update";
}

static const char* fcc_ast_binary_op_name(FccAstBinaryOpKind op_kind) {
  switch (op_kind) {
    case FCC_AST_BINARY_ADD:
      return "add";
    case FCC_AST_BINARY_SUBTRACT:
      return "subtract";
    case FCC_AST_BINARY_MULTIPLY:
      return "multiply";
    case FCC_AST_BINARY_DIVIDE:
      return "divide";
    case FCC_AST_BINARY_MODULO:
      return "modulo";
    case FCC_AST_BINARY_LESS:
      return "less";
    case FCC_AST_BINARY_LESS_EQUAL:
      return "less_equal";
    case FCC_AST_BINARY_GREATER:
      return "greater";
    case FCC_AST_BINARY_GREATER_EQUAL:
      return "greater_equal";
    case FCC_AST_BINARY_EQUAL:
      return "equal";
    case FCC_AST_BINARY_NOT_EQUAL:
      return "not_equal";
    case FCC_AST_BINARY_BITWISE_AND:
      return "bitwise_and";
    case FCC_AST_BINARY_BITWISE_XOR:
      return "bitwise_xor";
    case FCC_AST_BINARY_BITWISE_OR:
      return "bitwise_or";
    case FCC_AST_BINARY_LOGICAL_AND:
      return "logical_and";
    case FCC_AST_BINARY_LOGICAL_OR:
      return "logical_or";
    case FCC_AST_BINARY_LEFT_SHIFT:
      return "left_shift";
    case FCC_AST_BINARY_RIGHT_SHIFT:
      return "right_shift";
  }

  return "unknown_binary";
}

static void fcc_ast_dump_expression(FILE* stream, const FccAstExpression* expression,
                                    size_t depth) {
  size_t item_index;

  if (expression == NULL) {
    fcc_ast_dump_indent(stream, depth);
    fputs("<null-expression>\n", stream);
    return;
  }

  if (depth > FCC_MAX_PARSE_DEPTH) {
    fcc_ast_dump_indent(stream, depth);
    fputs("<expression-depth-limit>\n", stream);
    return;
  }

  switch (expression->kind) {
    case FCC_AST_EXPRESSION_INTEGER_LITERAL:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "integer_literal value=%llu\n",
              (unsigned long long)expression->data.integer_literal.value);
      return;
    case FCC_AST_EXPRESSION_IDENTIFIER:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "identifier name=\"%s\"\n",
              fcc_ast_safe_name(expression->data.identifier.name));
      return;
    case FCC_AST_EXPRESSION_STRING_LITERAL:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "string_literal length=%zu text=\"%s\"\n",
              expression->data.string_literal.length,
              expression->data.string_literal.bytes != NULL ? expression->data.string_literal.bytes
                                                            : "");
      return;
    case FCC_AST_EXPRESSION_UNARY:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "unary op=%s\n", fcc_ast_unary_op_name(expression->data.unary.op_kind));
      fcc_ast_dump_expression(stream, expression->data.unary.operand, depth + 1);
      return;
    case FCC_AST_EXPRESSION_BINARY:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "binary op=%s\n", fcc_ast_binary_op_name(expression->data.binary.op_kind));
      fcc_ast_dump_expression(stream, expression->data.binary.left, depth + 1);
      fcc_ast_dump_expression(stream, expression->data.binary.right, depth + 1);
      return;
    case FCC_AST_EXPRESSION_ASSIGN:
      fcc_ast_dump_indent(stream, depth);
      fputs("assign\n", stream);
      fcc_ast_dump_expression(stream, expression->data.assign.target, depth + 1);
      fcc_ast_dump_expression(stream, expression->data.assign.value, depth + 1);
      return;
    case FCC_AST_EXPRESSION_COMPOUND_ASSIGN:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "compound_assign op=%s\n",
              fcc_ast_binary_op_name(expression->data.compound_assign.op_kind));
      fcc_ast_dump_expression(stream, expression->data.compound_assign.target, depth + 1);
      fcc_ast_dump_expression(stream, expression->data.compound_assign.value, depth + 1);
      return;
    case FCC_AST_EXPRESSION_CALL: {
      size_t argument_index;

      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "call argument_count=%zu\n", expression->data.call.argument_count);
      fcc_ast_dump_expression(stream, expression->data.call.callee, depth + 1);
      for (argument_index = 0; argument_index < expression->data.call.argument_count;
           ++argument_index) {
        fcc_ast_dump_expression(stream, expression->data.call.arguments[argument_index], depth + 1);
      }

      return;
    }
    case FCC_AST_EXPRESSION_SIZEOF:
      fcc_ast_dump_indent(stream, depth);
      if (expression->data.sizeof_expression.has_type_operand) {
        fputs("sizeof type=", stream);
        fcc_ast_dump_type(stream, &expression->data.sizeof_expression.type);
        fputc('\n', stream);
      } else {
        fputs("sizeof expression\n", stream);
        fcc_ast_dump_expression(stream, expression->data.sizeof_expression.operand, depth + 1);
      }

      return;
    case FCC_AST_EXPRESSION_ALIGNOF:
      fcc_ast_dump_indent(stream, depth);
      fputs("alignof type=", stream);
      fcc_ast_dump_type(stream, &expression->data.alignof_expression.type);
      fputc('\n', stream);
      return;
    case FCC_AST_EXPRESSION_CAST:
      fcc_ast_dump_indent(stream, depth);
      fputs("cast type=", stream);
      fcc_ast_dump_type(stream, &expression->data.cast.type);
      fputc('\n', stream);
      fcc_ast_dump_expression(stream, expression->data.cast.operand, depth + 1);
      return;
    case FCC_AST_EXPRESSION_SUBSCRIPT:
      fcc_ast_dump_indent(stream, depth);
      fputs("subscript\n", stream);
      fcc_ast_dump_expression(stream, expression->data.subscript.target, depth + 1);
      fcc_ast_dump_expression(stream, expression->data.subscript.index, depth + 1);
      return;
    case FCC_AST_EXPRESSION_MEMBER:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "member op=%s field=\"%s\"\n",
              expression->data.member.is_arrow ? "arrow" : "dot",
              fcc_ast_safe_name(expression->data.member.field_name));
      fcc_ast_dump_expression(stream, expression->data.member.target, depth + 1);
      return;
    case FCC_AST_EXPRESSION_UPDATE:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "update op=%s form=%s\n",
              fcc_ast_update_op_name(expression->data.update.op_kind),
              expression->data.update.is_postfix ? "postfix" : "prefix");
      fcc_ast_dump_expression(stream, expression->data.update.target, depth + 1);
      return;
    case FCC_AST_EXPRESSION_CONDITIONAL:
      fcc_ast_dump_indent(stream, depth);
      fputs("conditional\n", stream);
      fcc_ast_dump_expression(stream, expression->data.conditional.condition, depth + 1);
      fcc_ast_dump_expression(stream, expression->data.conditional.then_expression, depth + 1);
      fcc_ast_dump_expression(stream, expression->data.conditional.else_expression, depth + 1);
      return;
    case FCC_AST_EXPRESSION_INITIALIZER_LIST:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "initializer_list item_count=%zu\n",
              expression->data.initializer_list.item_count);
      for (item_index = 0; item_index < expression->data.initializer_list.item_count;
           ++item_index) {
        fcc_ast_dump_expression(stream, expression->data.initializer_list.items[item_index],
                                depth + 1);
      }

      return;
  }

  fcc_ast_dump_indent(stream, depth);
  fputs("<unknown-expression>\n", stream);
}

static void fcc_ast_dump_statement(FILE* stream, const FccAstStatement* statement, size_t depth) {
  size_t item_index;

  if (statement == NULL) {
    fcc_ast_dump_indent(stream, depth);
    fputs("<null-statement>\n", stream);
    return;
  }

  if (depth > FCC_MAX_PARSE_DEPTH) {
    fcc_ast_dump_indent(stream, depth);
    fputs("<statement-depth-limit>\n", stream);
    return;
  }

  switch (statement->kind) {
    case FCC_AST_STATEMENT_COMPOUND:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "compound item_count=%zu\n", statement->data.compound.item_count);
      for (item_index = 0; item_index < statement->data.compound.item_count; ++item_index) {
        fcc_ast_dump_statement(stream, statement->data.compound.items[item_index], depth + 1);
      }

      return;
    case FCC_AST_STATEMENT_RETURN:
      fcc_ast_dump_indent(stream, depth);
      fputs("return\n", stream);
      if (statement->data.return_statement.expression != NULL) {
        fcc_ast_dump_expression(stream, statement->data.return_statement.expression, depth + 1);
      }

      return;
    case FCC_AST_STATEMENT_EXPRESSION:
      fcc_ast_dump_indent(stream, depth);
      fputs("expression_statement\n", stream);
      if (statement->data.expression_statement.expression != NULL) {
        fcc_ast_dump_expression(stream, statement->data.expression_statement.expression, depth + 1);
      }

      return;
    case FCC_AST_STATEMENT_DECLARATION:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream,
              "declaration name=\"%s\" type=", fcc_ast_safe_name(statement->data.declaration.name));
      fcc_ast_dump_type(stream, &statement->data.declaration.type);
      fputc('\n', stream);
      if (statement->data.declaration.initializer != NULL) {
        fcc_ast_dump_expression(stream, statement->data.declaration.initializer, depth + 1);
      }

      return;
    case FCC_AST_STATEMENT_STATIC_ASSERT:
      fcc_ast_dump_static_assert(stream, &statement->data.static_assertion, depth);
      return;
    case FCC_AST_STATEMENT_IF:
      fcc_ast_dump_indent(stream, depth);
      fputs("if\n", stream);
      fcc_ast_dump_expression(stream, statement->data.if_statement.condition, depth + 1);
      fcc_ast_dump_statement(stream, statement->data.if_statement.then_statement, depth + 1);
      if (statement->data.if_statement.else_statement != NULL) {
        fcc_ast_dump_indent(stream, depth);
        fputs("else\n", stream);
        fcc_ast_dump_statement(stream, statement->data.if_statement.else_statement, depth + 1);
      }

      return;
    case FCC_AST_STATEMENT_WHILE:
      fcc_ast_dump_indent(stream, depth);
      fputs("while\n", stream);
      fcc_ast_dump_expression(stream, statement->data.while_statement.condition, depth + 1);
      fcc_ast_dump_statement(stream, statement->data.while_statement.body, depth + 1);
      return;
    case FCC_AST_STATEMENT_DO_WHILE:
      fcc_ast_dump_indent(stream, depth);
      fputs("do_while\n", stream);
      fcc_ast_dump_statement(stream, statement->data.do_while_statement.body, depth + 1);
      fcc_ast_dump_expression(stream, statement->data.do_while_statement.condition, depth + 1);
      return;
    case FCC_AST_STATEMENT_FOR:
      fcc_ast_dump_indent(stream, depth);
      fputs("for\n", stream);
      if (statement->data.for_statement.init_statement != NULL) {
        fcc_ast_dump_statement(stream, statement->data.for_statement.init_statement, depth + 1);
      } else {
        fcc_ast_dump_indent(stream, depth + 1);
        fputs("<empty-init>\n", stream);
      }

      if (statement->data.for_statement.condition != NULL) {
        fcc_ast_dump_expression(stream, statement->data.for_statement.condition, depth + 1);
      } else {
        fcc_ast_dump_indent(stream, depth + 1);
        fputs("<empty-condition>\n", stream);
      }

      if (statement->data.for_statement.update != NULL) {
        fcc_ast_dump_expression(stream, statement->data.for_statement.update, depth + 1);
      } else {
        fcc_ast_dump_indent(stream, depth + 1);
        fputs("<empty-update>\n", stream);
      }

      fcc_ast_dump_statement(stream, statement->data.for_statement.body, depth + 1);
      return;
    case FCC_AST_STATEMENT_BREAK:
      fcc_ast_dump_indent(stream, depth);
      fputs("break\n", stream);
      return;
    case FCC_AST_STATEMENT_CONTINUE:
      fcc_ast_dump_indent(stream, depth);
      fputs("continue\n", stream);
      return;
    case FCC_AST_STATEMENT_SWITCH:
      fcc_ast_dump_indent(stream, depth);
      fputs("switch\n", stream);
      fcc_ast_dump_expression(stream, statement->data.switch_statement.condition, depth + 1);
      fcc_ast_dump_statement(stream, statement->data.switch_statement.body, depth + 1);
      return;
    case FCC_AST_STATEMENT_CASE:
      fcc_ast_dump_indent(stream, depth);
      fputs("case\n", stream);
      fcc_ast_dump_expression(stream, statement->data.case_statement.value, depth + 1);
      fcc_ast_dump_statement(stream, statement->data.case_statement.statement, depth + 1);
      return;
    case FCC_AST_STATEMENT_DEFAULT:
      fcc_ast_dump_indent(stream, depth);
      fputs("default\n", stream);
      fcc_ast_dump_statement(stream, statement->data.default_statement.statement, depth + 1);
      return;
    case FCC_AST_STATEMENT_GOTO:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "goto name=\"%s\"\n", fcc_ast_safe_name(statement->data.goto_statement.name));
      return;
    case FCC_AST_STATEMENT_LABEL:
      fcc_ast_dump_indent(stream, depth);
      fprintf(stream, "label name=\"%s\"\n", fcc_ast_safe_name(statement->data.label_statement.name));
      fcc_ast_dump_statement(stream, statement->data.label_statement.statement, depth + 1);
      return;
  }

  fcc_ast_dump_indent(stream, depth);
  fputs("<unknown-statement>\n", stream);
}

void fcc_ast_context_init(FccAstContext* context) {
  assert(context != NULL);

  context->owned_allocations = NULL;
  context->allocation_count = 0;
  context->allocation_capacity = 0;
}

void* fcc_ast_context_allocate(FccAstContext* context, size_t allocation_size) {
  void* allocation;

  assert(context != NULL);

  if (allocation_size == 0) {
    return NULL;
  }

  allocation = calloc(1, allocation_size);
  if (allocation == NULL) {
    return NULL;
  }

  if (!fcc_ast_context_append_allocation(context, allocation)) {
    free(allocation);
    return NULL;
  }

  return allocation;
}

void fcc_ast_context_dispose(FccAstContext* context) {
  size_t allocation_index;

  if (context == NULL) {
    return;
  }

  for (allocation_index = 0; allocation_index < context->allocation_count; ++allocation_index) {
    free(context->owned_allocations[allocation_index]);
  }

  free(context->owned_allocations);
  context->owned_allocations = NULL;
  context->allocation_count = 0;
  context->allocation_capacity = 0;
}

const char* fcc_ast_type_name(FccAstTypeKind kind) {
  switch (kind) {
    case FCC_AST_TYPE_INT:
      return "int";
    case FCC_AST_TYPE_VOID:
      return "void";
    case FCC_AST_TYPE_CHAR:
      return "char";
    case FCC_AST_TYPE_UNSIGNED_INT:
      return "unsigned int";
    case FCC_AST_TYPE_UNSIGNED_CHAR:
      return "unsigned char";
    case FCC_AST_TYPE_SIGNED_CHAR:
      return "signed char";
    case FCC_AST_TYPE_SHORT:
      return "short";
    case FCC_AST_TYPE_UNSIGNED_SHORT:
      return "unsigned short";
    case FCC_AST_TYPE_LONG:
      return "long";
    case FCC_AST_TYPE_UNSIGNED_LONG:
      return "unsigned long";
    case FCC_AST_TYPE_LONG_LONG:
      return "long long";
    case FCC_AST_TYPE_UNSIGNED_LONG_LONG:
      return "unsigned long long";
    case FCC_AST_TYPE_BOOL:
      return "_Bool";
    case FCC_AST_TYPE_TYPEDEF_NAME:
      return "typedef-name";
    case FCC_AST_TYPE_STRUCT:
      return "struct";
    case FCC_AST_TYPE_UNION:
      return "union";
    case FCC_AST_TYPE_ENUM:
      return "enum";
  }

  return "unknown_type";
}

const char* fcc_ast_storage_class_name(FccAstStorageClass storage_class) {
  switch (storage_class) {
    case FCC_AST_STORAGE_CLASS_NONE:
      return "none";
    case FCC_AST_STORAGE_CLASS_EXTERN:
      return "extern";
    case FCC_AST_STORAGE_CLASS_STATIC:
      return "static";
    case FCC_AST_STORAGE_CLASS_TYPEDEF:
      return "typedef";
  }

  return "unknown_storage_class";
}

static void fcc_ast_dump_static_assert(FILE* stream, const FccAstStaticAssert* static_assertion,
                                       size_t depth) {
  fcc_ast_dump_indent(stream, depth);
  fprintf(stream, "static_assert message_length=%zu\n", static_assertion->message_length);
  fcc_ast_dump_expression(stream, static_assertion->condition, depth + 1);
}

void fcc_ast_dump_translation_unit(FILE* stream, const FccAstTranslationUnit* translation_unit) {
  FILE* output_stream;
  size_t static_assertion_index;
  size_t global_index;
  size_t function_index;

  output_stream = fcc_ast_resolve_stream(stream);
  if (translation_unit == NULL) {
    fputs("<null-translation-unit>\n", output_stream);
    return;
  }

  fprintf(output_stream, "translation_unit function_count=%zu global_count=%zu "
                         "static_assert_count=%zu\n",
          translation_unit->function_count, translation_unit->global_count,
          translation_unit->static_assertion_count);
  for (static_assertion_index = 0; static_assertion_index < translation_unit->static_assertion_count;
       ++static_assertion_index) {
    fcc_ast_dump_static_assert(output_stream,
                               translation_unit->static_assertions[static_assertion_index], 1);
  }

  for (global_index = 0; global_index < translation_unit->global_count; ++global_index) {
    const FccAstGlobalVariable* global_variable;

    global_variable = translation_unit->globals[global_index];
    if (global_variable == NULL) {
      fcc_ast_dump_indent(output_stream, 1);
      fputs("<null-global>\n", output_stream);
      continue;
    }

    fcc_ast_dump_indent(output_stream, 1);
    fprintf(output_stream, "global name=\"%s\" type=", fcc_ast_safe_name(global_variable->name));
    fcc_ast_dump_type(output_stream, &global_variable->type);
    fprintf(output_stream, " has_initializer=%s\n",
            global_variable->initializer != NULL ? "true" : "false");
    if (global_variable->initializer != NULL) {
      fcc_ast_dump_expression(output_stream, global_variable->initializer, 2);
    }
  }

  for (function_index = 0; function_index < translation_unit->function_count; ++function_index) {
    const FccAstFunctionDefinition* function_definition;
    size_t parameter_index;

    function_definition = translation_unit->functions[function_index];
    if (function_definition == NULL) {
      fcc_ast_dump_indent(output_stream, 1);
      fputs("<null-function>\n", output_stream);
      continue;
    }

    fcc_ast_dump_indent(output_stream, 1);
    fprintf(output_stream,
            "function name=\"%s\" return_type=", fcc_ast_safe_name(function_definition->name));
    fcc_ast_dump_type(output_stream, &function_definition->return_type);
    fprintf(output_stream, " parameter_count=%zu has_body=%s\n",
            function_definition->parameter_count, function_definition->has_body ? "true" : "false");
    for (parameter_index = 0; parameter_index < function_definition->parameter_count;
         ++parameter_index) {
      const FccAstParameter* parameter;

      parameter = &function_definition->parameters[parameter_index];
      fcc_ast_dump_indent(output_stream, 2);
      fprintf(output_stream, "parameter name=\"%s\" type=", fcc_ast_safe_name(parameter->name));
      fcc_ast_dump_type(output_stream, &parameter->type);
      fputc('\n', output_stream);
    }

    if (function_definition->has_body) {
      fcc_ast_dump_statement(output_stream, function_definition->body, 2);
    } else {
      fcc_ast_dump_indent(output_stream, 2);
      fputs("<declaration-only>\n", output_stream);
    }
  }
}
