// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "fcc/base.h"
#include "fcc/source.h"

/*
 * The AST is intentionally syntax-oriented. Parser-owned nodes describe what
 * was written, while semantic facts such as resolved type IDs and lvalue status
 * live in FccSemaResult side tables keyed by AST pointers.
 */
typedef enum FccAstStorageClass {
  FCC_AST_STORAGE_CLASS_NONE = 0,
  FCC_AST_STORAGE_CLASS_EXTERN = 1,
  FCC_AST_STORAGE_CLASS_STATIC = 2,
  FCC_AST_STORAGE_CLASS_TYPEDEF = 3
} FccAstStorageClass;

typedef enum FccAstTypeKind {
  FCC_AST_TYPE_INT = 0,
  FCC_AST_TYPE_VOID = 1,
  FCC_AST_TYPE_CHAR = 2,
  FCC_AST_TYPE_UNSIGNED_INT = 3,
  FCC_AST_TYPE_UNSIGNED_CHAR = 4,
  FCC_AST_TYPE_SIGNED_CHAR = 5,
  FCC_AST_TYPE_SHORT = 6,
  FCC_AST_TYPE_UNSIGNED_SHORT = 7,
  FCC_AST_TYPE_LONG = 8,
  FCC_AST_TYPE_UNSIGNED_LONG = 9,
  FCC_AST_TYPE_LONG_LONG = 10,
  FCC_AST_TYPE_UNSIGNED_LONG_LONG = 11,
  FCC_AST_TYPE_BOOL = 12,
  FCC_AST_TYPE_TYPEDEF_NAME = 13,
  FCC_AST_TYPE_STRUCT = 14,
  FCC_AST_TYPE_UNION = 15,
  FCC_AST_TYPE_ENUM = 16
} FccAstTypeKind;

struct FccAstExpression;
struct FccAstParameter;

typedef struct FccAstEnumerator {
  FccSourceSpan span;
  const char* name;
  bool has_value;
  struct FccAstExpression* value;
} FccAstEnumerator;

typedef struct FccAstArrayBound {
  FccSourceSpan span;
  struct FccAstExpression* expression;
  size_t element_count;
  bool is_vla;
} FccAstArrayBound;

typedef struct FccAstType {
  FccAstTypeKind kind;
  FccSourceSpan span;
  size_t pointer_depth;
  FccAstArrayBound* array_bounds;
  size_t array_count;
  bool is_function_pointer;
  size_t function_pointer_depth;
  struct FccAstParameter* function_pointer_parameters;
  size_t function_pointer_parameter_count;
  bool function_pointer_is_variadic;
  bool is_const_qualified;
  const char* typedef_name;
  const char* tag_name;
  struct FccAstRecordField* record_fields;
  size_t record_field_count;
  bool is_record_definition;
  FccAstEnumerator* enumerators;
  size_t enumerator_count;
  bool is_enum_definition;
} FccAstType;

typedef struct FccAstDeclSpecifiers {
  FccSourceSpan span;
  FccAstStorageClass storage_class;
  FccAstTypeKind type_kind;
  bool is_const_qualified;
  const char* typedef_name;
} FccAstDeclSpecifiers;

typedef struct FccAstDeclarator {
  FccSourceSpan span;
  const char* name;
  size_t pointer_depth;
  FccAstArrayBound* array_bounds;
  size_t array_count;
  bool is_function_pointer;
  size_t function_pointer_depth;
  struct FccAstParameter* function_pointer_parameters;
  size_t function_pointer_parameter_count;
  bool function_pointer_is_variadic;
} FccAstDeclarator;

typedef struct FccAstDeclarationSyntax {
  FccAstDeclSpecifiers decl_specifiers;
  FccAstDeclarator declarator;
} FccAstDeclarationSyntax;

typedef struct FccAstStaticAssert {
  FccSourceSpan span;
  struct FccAstExpression* condition;
  const char* message;
  size_t message_length;
} FccAstStaticAssert;

typedef struct FccAstRecordField {
  FccSourceSpan span;
  bool is_static_assert;
  FccAstStaticAssert static_assertion;
  FccAstDeclarationSyntax syntax;
  FccAstType type;
  const char* name;
} FccAstRecordField;

typedef enum FccAstUnaryOpKind {
  FCC_AST_UNARY_PLUS = 0,
  FCC_AST_UNARY_NEGATE = 1,
  FCC_AST_UNARY_LOGICAL_NOT = 2,
  FCC_AST_UNARY_BITWISE_NOT = 3,
  FCC_AST_UNARY_ADDRESS_OF = 4,
  FCC_AST_UNARY_DEREFERENCE = 5
} FccAstUnaryOpKind;

typedef enum FccAstUpdateOpKind {
  FCC_AST_UPDATE_INCREMENT = 0,
  FCC_AST_UPDATE_DECREMENT = 1
} FccAstUpdateOpKind;

typedef enum FccAstBinaryOpKind {
  FCC_AST_BINARY_ADD = 0,
  FCC_AST_BINARY_SUBTRACT = 1,
  FCC_AST_BINARY_MULTIPLY = 2,
  FCC_AST_BINARY_DIVIDE = 3,
  FCC_AST_BINARY_MODULO = 4,
  FCC_AST_BINARY_LESS = 5,
  FCC_AST_BINARY_LESS_EQUAL = 6,
  FCC_AST_BINARY_GREATER = 7,
  FCC_AST_BINARY_GREATER_EQUAL = 8,
  FCC_AST_BINARY_EQUAL = 9,
  FCC_AST_BINARY_NOT_EQUAL = 10,
  FCC_AST_BINARY_BITWISE_AND = 11,
  FCC_AST_BINARY_BITWISE_XOR = 12,
  FCC_AST_BINARY_BITWISE_OR = 13,
  FCC_AST_BINARY_LOGICAL_AND = 14,
  FCC_AST_BINARY_LOGICAL_OR = 15,
  FCC_AST_BINARY_LEFT_SHIFT = 16,
  FCC_AST_BINARY_RIGHT_SHIFT = 17
} FccAstBinaryOpKind;

typedef enum FccAstExpressionKind {
  FCC_AST_EXPRESSION_INTEGER_LITERAL = 0,
  FCC_AST_EXPRESSION_IDENTIFIER = 1,
  FCC_AST_EXPRESSION_STRING_LITERAL = 2,
  FCC_AST_EXPRESSION_UNARY = 3,
  FCC_AST_EXPRESSION_BINARY = 4,
  FCC_AST_EXPRESSION_ASSIGN = 5,
  FCC_AST_EXPRESSION_CALL = 6,
  FCC_AST_EXPRESSION_SIZEOF = 7,
  FCC_AST_EXPRESSION_ALIGNOF = 8,
  FCC_AST_EXPRESSION_CAST = 9,
  FCC_AST_EXPRESSION_SUBSCRIPT = 10,
  FCC_AST_EXPRESSION_MEMBER = 11,
  FCC_AST_EXPRESSION_COMPOUND_ASSIGN = 12,
  FCC_AST_EXPRESSION_UPDATE = 13,
  FCC_AST_EXPRESSION_CONDITIONAL = 14,
  FCC_AST_EXPRESSION_INITIALIZER_LIST = 15
} FccAstExpressionKind;

typedef enum FccAstStatementKind {
  FCC_AST_STATEMENT_COMPOUND = 0,
  FCC_AST_STATEMENT_RETURN = 1,
  FCC_AST_STATEMENT_EXPRESSION = 2,
  FCC_AST_STATEMENT_DECLARATION = 3,
  FCC_AST_STATEMENT_IF = 4,
  FCC_AST_STATEMENT_WHILE = 5,
  FCC_AST_STATEMENT_FOR = 6,
  FCC_AST_STATEMENT_BREAK = 7,
  FCC_AST_STATEMENT_CONTINUE = 8,
  FCC_AST_STATEMENT_SWITCH = 9,
  FCC_AST_STATEMENT_CASE = 10,
  FCC_AST_STATEMENT_DEFAULT = 11,
  FCC_AST_STATEMENT_DO_WHILE = 12,
  FCC_AST_STATEMENT_GOTO = 13,
  FCC_AST_STATEMENT_LABEL = 14,
  FCC_AST_STATEMENT_STATIC_ASSERT = 15
} FccAstStatementKind;

typedef struct FccAstExpression FccAstExpression;
typedef struct FccAstStatement FccAstStatement;
typedef struct FccAstFunctionDefinition FccAstFunctionDefinition;
typedef struct FccAstGlobalVariable FccAstGlobalVariable;
typedef struct FccAstTranslationUnit FccAstTranslationUnit;

typedef struct FccAstParameter {
  FccAstDeclarationSyntax syntax;
  FccAstType type;
  FccSourceSpan span;
  const char* name;
} FccAstParameter;

struct FccAstExpression {
  FccAstExpressionKind kind;
  FccSourceSpan span;

  union {
    struct {
      uint64_t value;
    } integer_literal;

    struct {
      const char* name;
    } identifier;

    struct {
      const char* bytes;
      size_t length;
    } string_literal;

    struct {
      FccAstUnaryOpKind op_kind;
      FccAstExpression* operand;
    } unary;

    struct {
      FccAstBinaryOpKind op_kind;
      FccAstExpression* left;
      FccAstExpression* right;
    } binary;

    struct {
      FccAstExpression* target;
      FccAstExpression* value;
    } assign;

    struct {
      FccAstBinaryOpKind op_kind;
      FccAstExpression* target;
      FccAstExpression* value;
    } compound_assign;

    struct {
      FccAstExpression* callee;
      FccAstExpression** arguments;
      size_t argument_count;
    } call;

    struct {
      bool has_type_operand;
      FccAstType type;
      FccAstExpression* operand;
    } sizeof_expression;

    struct {
      FccAstType type;
    } alignof_expression;

    struct {
      FccAstType type;
      FccAstExpression* operand;
    } cast;

    struct {
      FccAstExpression* target;
      FccAstExpression* index;
    } subscript;

    struct {
      FccAstExpression* target;
      const char* field_name;
      bool is_arrow;
    } member;

    struct {
      FccAstUpdateOpKind op_kind;
      FccAstExpression* target;
      bool is_postfix;
    } update;

    struct {
      FccAstExpression* condition;
      FccAstExpression* then_expression;
      FccAstExpression* else_expression;
    } conditional;

    struct {
      FccAstExpression** items;
      size_t item_count;
    } initializer_list;
  } data;
};

struct FccAstStatement {
  FccAstStatementKind kind;
  FccSourceSpan span;

  union {
    struct {
      FccAstStatement** items;
      size_t item_count;
    } compound;

    struct {
      FccAstExpression* expression;
    } return_statement;

    struct {
      FccAstExpression* expression;
    } expression_statement;

    struct {
      FccAstDeclarationSyntax syntax;
      FccAstType type;
      const char* name;
      FccAstExpression* initializer;
    } declaration;

    struct {
      FccAstExpression* condition;
      FccAstStatement* then_statement;
      FccAstStatement* else_statement;
    } if_statement;

    struct {
      FccAstExpression* condition;
      FccAstStatement* body;
    } while_statement;

    struct {
      FccAstStatement* body;
      FccAstExpression* condition;
    } do_while_statement;

    struct {
      FccAstStatement* init_statement;
      FccAstExpression* condition;
      FccAstExpression* update;
      FccAstStatement* body;
    } for_statement;

    struct {
      FccAstExpression* condition;
      FccAstStatement* body;
    } switch_statement;

    struct {
      FccAstExpression* value;
      FccAstStatement* statement;
    } case_statement;

    struct {
      FccAstStatement* statement;
    } default_statement;

    struct {
      const char* name;
    } goto_statement;

    struct {
      const char* name;
      FccAstStatement* statement;
    } label_statement;

    FccAstStaticAssert static_assertion;
  } data;
};

struct FccAstFunctionDefinition {
  FccSourceSpan span;
  FccAstDeclarationSyntax syntax;
  FccAstType return_type;
  const char* name;
  FccAstParameter* parameters;
  size_t parameter_count;
  bool is_variadic;
  bool has_body;
  FccAstStatement* body;
};

struct FccAstGlobalVariable {
  FccSourceSpan span;
  FccAstDeclarationSyntax syntax;
  FccAstType type;
  const char* name;
  FccAstExpression* initializer;
};

struct FccAstTranslationUnit {
  FccSourceSpan span;
  FccAstStaticAssert** static_assertions;
  size_t static_assertion_count;
  FccAstGlobalVariable** globals;
  size_t global_count;
  FccAstFunctionDefinition** functions;
  size_t function_count;
};

typedef struct FccAstContext {
  void** owned_allocations;
  size_t allocation_count;
  size_t allocation_capacity;
} FccAstContext;

void fcc_ast_context_init(FccAstContext* context);

void* fcc_ast_context_allocate(FccAstContext* context, size_t allocation_size);

void fcc_ast_context_dispose(FccAstContext* context);

const char* fcc_ast_type_name(FccAstTypeKind kind);

const char* fcc_ast_storage_class_name(FccAstStorageClass storage_class);

void fcc_ast_dump_translation_unit(FILE* stream, const FccAstTranslationUnit* translation_unit);
