// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fcc/source.h"
#include "fcc/type.h"

typedef enum FccStorageClass {
  FCC_STORAGE_CLASS_NONE = 0,
  FCC_STORAGE_CLASS_EXTERN = 1,
  FCC_STORAGE_CLASS_STATIC = 2,
  FCC_STORAGE_CLASS_TYPEDEF = 3
} FccStorageClass;

typedef enum FccSymbolKind {
  FCC_SYMBOL_FUNCTION = 0,
  FCC_SYMBOL_GLOBAL = 1,
  FCC_SYMBOL_PARAMETER = 2,
  FCC_SYMBOL_LOCAL = 3,
  FCC_SYMBOL_TYPEDEF = 4,
  FCC_SYMBOL_ENUMERATOR = 5
} FccSymbolKind;

typedef struct FccSymbol {
  FccSymbolKind kind;
  FccStorageClass storage_class;
  FccTypeId type_id;
  FccSourceSpan span;
  size_t scope_depth;
  bool has_body;
  bool has_integer_constant;
  int64_t integer_constant_value;
  const char* name;
} FccSymbol;

typedef struct FccSymbolScope {
  size_t first_symbol_index;
  size_t symbol_count;
} FccSymbolScope;

typedef struct FccSymbolTable {
  FccSymbol* symbols;
  size_t symbol_count;
  size_t symbol_capacity;
  FccSymbolScope* scopes;
  size_t scope_count;
  size_t scope_capacity;
} FccSymbolTable;

void fcc_symbol_table_init(FccSymbolTable* table);

void fcc_symbol_table_dispose(FccSymbolTable* table);

const char* fcc_symbol_kind_name(FccSymbolKind kind);

bool fcc_symbol_table_push_scope(FccSymbolTable* table);

void fcc_symbol_table_pop_scope(FccSymbolTable* table);

size_t fcc_symbol_table_scope_depth(const FccSymbolTable* table);

bool fcc_symbol_table_define(FccSymbolTable* table, const FccSymbol* symbol);

const FccSymbol* fcc_symbol_table_lookup(const FccSymbolTable* table, const char* name);

const FccSymbol* fcc_symbol_table_lookup_current_scope(const FccSymbolTable* table,
                                                       const char* name);

FccSymbol* fcc_symbol_table_lookup_mutable(FccSymbolTable* table, const char* name);

FccSymbol* fcc_symbol_table_lookup_mutable_current_scope(FccSymbolTable* table, const char* name);
