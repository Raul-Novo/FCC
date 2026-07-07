// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fcc/ast.h"
#include "fcc/diag.h"
#include "fcc/source.h"
#include "fcc/symbol.h"
#include "fcc/type.h"

typedef struct FccSemaExpressionInfo {
  const FccAstExpression* expression;
  FccTypeId type_id;
  bool is_lvalue;
  bool is_valid;
  bool has_integer_constant;
  int64_t integer_constant_value;
  bool has_address_constant;
  const char* address_constant_symbol_name;
} FccSemaExpressionInfo;

typedef struct FccSemaObjectInfo {
  const void* node;
  const char* name;
  FccSymbolKind symbol_kind;
  FccStorageClass storage_class;
  FccTypeId type_id;
  bool has_body;
} FccSemaObjectInfo;

/*
 * Semantic analysis keeps typed metadata outside the AST. Codegen must query
 * these tables instead of re-inferring language rules from syntax nodes.
 */
typedef struct FccSemaResult {
  FccTypeContext type_context;
  FccSemaExpressionInfo* expression_infos;
  size_t expression_count;
  size_t expression_capacity;
  FccSemaObjectInfo* object_infos;
  size_t object_count;
  size_t object_capacity;
} FccSemaResult;

void fcc_sema_result_init(FccSemaResult* result);

void fcc_sema_result_dispose(FccSemaResult* result);

const FccSemaExpressionInfo* fcc_sema_result_find_expression_info(
    const FccSemaResult* result, const FccAstExpression* expression);

const FccSemaObjectInfo* fcc_sema_result_find_object_info(const FccSemaResult* result,
                                                          const void* node);

bool fcc_sema_analyze_translation_unit(const FccSourceFile* source_file,
                                       const FccAstTranslationUnit* translation_unit,
                                       FccDiagnostics* diagnostics, FccSemaResult* result);

bool fcc_sema_check_translation_unit(const FccSourceFile* source_file,
                                     const FccAstTranslationUnit* translation_unit,
                                     FccDiagnostics* diagnostics);
