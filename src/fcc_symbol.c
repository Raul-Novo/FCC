// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/symbol.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static bool fcc_symbol_table_ensure_symbol_capacity(FccSymbolTable* table, size_t capacity) {
  size_t new_capacity;
  FccSymbol* new_symbols;

  assert(table != NULL);

  if (capacity <= table->symbol_capacity) {
    return true;
  }

  new_capacity = table->symbol_capacity;
  if (new_capacity == 0) {
    new_capacity = 16;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(FccSymbol))) {
    return false;
  }

  new_symbols = (FccSymbol*)realloc(table->symbols, new_capacity * sizeof(FccSymbol));
  if (new_symbols == NULL) {
    return false;
  }

  table->symbols = new_symbols;
  table->symbol_capacity = new_capacity;
  return true;
}

static bool fcc_symbol_table_ensure_scope_capacity(FccSymbolTable* table, size_t capacity) {
  size_t new_capacity;
  FccSymbolScope* new_scopes;

  assert(table != NULL);

  if (capacity <= table->scope_capacity) {
    return true;
  }

  new_capacity = table->scope_capacity;
  if (new_capacity == 0) {
    new_capacity = 8;
  }

  while (new_capacity < capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      return false;
    }

    new_capacity *= 2;
  }

  if (new_capacity > (SIZE_MAX / sizeof(FccSymbolScope))) {
    return false;
  }

  new_scopes = (FccSymbolScope*)realloc(table->scopes, new_capacity * sizeof(FccSymbolScope));
  if (new_scopes == NULL) {
    return false;
  }

  table->scopes = new_scopes;
  table->scope_capacity = new_capacity;
  return true;
}

void fcc_symbol_table_init(FccSymbolTable* table) {
  assert(table != NULL);

  table->symbols = NULL;
  table->symbol_count = 0;
  table->symbol_capacity = 0;
  table->scopes = NULL;
  table->scope_count = 0;
  table->scope_capacity = 0;
}

void fcc_symbol_table_dispose(FccSymbolTable* table) {
  if (table == NULL) {
    return;
  }

  free(table->symbols);
  free(table->scopes);
  table->symbols = NULL;
  table->symbol_count = 0;
  table->symbol_capacity = 0;
  table->scopes = NULL;
  table->scope_count = 0;
  table->scope_capacity = 0;
}

const char* fcc_symbol_kind_name(FccSymbolKind kind) {
  switch (kind) {
    case FCC_SYMBOL_FUNCTION:
      return "function";
    case FCC_SYMBOL_GLOBAL:
      return "global";
    case FCC_SYMBOL_PARAMETER:
      return "parameter";
    case FCC_SYMBOL_LOCAL:
      return "local";
    case FCC_SYMBOL_TYPEDEF:
      return "typedef";
    case FCC_SYMBOL_ENUMERATOR:
      return "enumerator";
  }

  return "unknown_symbol";
}

bool fcc_symbol_table_push_scope(FccSymbolTable* table) {
  FccSymbolScope* scope;

  assert(table != NULL);

  if (!fcc_symbol_table_ensure_scope_capacity(table, table->scope_count + 1)) {
    return false;
  }

  scope = &table->scopes[table->scope_count];
  scope->first_symbol_index = table->symbol_count;
  scope->symbol_count = 0;
  ++table->scope_count;
  return true;
}

void fcc_symbol_table_pop_scope(FccSymbolTable* table) {
  const FccSymbolScope* scope;

  assert(table != NULL);
  assert(table->scope_count > 0);

  scope = &table->scopes[table->scope_count - 1];
  table->symbol_count = scope->first_symbol_index;
  --table->scope_count;
}

size_t fcc_symbol_table_scope_depth(const FccSymbolTable* table) {
  assert(table != NULL);

  return table->scope_count;
}

bool fcc_symbol_table_define(FccSymbolTable* table, const FccSymbol* symbol) {
  FccSymbol* destination;

  assert(table != NULL);
  assert(symbol != NULL);
  assert(table->scope_count > 0);

  if (!fcc_symbol_table_ensure_symbol_capacity(table, table->symbol_count + 1)) {
    return false;
  }

  destination = &table->symbols[table->symbol_count];
  *destination = *symbol;
  destination->scope_depth = table->scope_count;
  ++table->symbol_count;
  ++table->scopes[table->scope_count - 1].symbol_count;
  return true;
}

const FccSymbol* fcc_symbol_table_lookup(const FccSymbolTable* table, const char* name) {
  size_t symbol_index;

  assert(table != NULL);
  assert(name != NULL);

  symbol_index = table->symbol_count;
  while (symbol_index > 0) {
    --symbol_index;
    if ((table->symbols[symbol_index].name == name) ||
        ((table->symbols[symbol_index].name != NULL) &&
         (strcmp(table->symbols[symbol_index].name, name) == 0))) {
      return &table->symbols[symbol_index];
    }
  }

  return NULL;
}

FccSymbol* fcc_symbol_table_lookup_mutable(FccSymbolTable* table, const char* name) {
  size_t symbol_index;

  assert(table != NULL);
  assert(name != NULL);

  symbol_index = table->symbol_count;
  while (symbol_index > 0) {
    --symbol_index;
    if ((table->symbols[symbol_index].name == name) ||
        ((table->symbols[symbol_index].name != NULL) &&
         (strcmp(table->symbols[symbol_index].name, name) == 0))) {
      return &table->symbols[symbol_index];
    }
  }

  return NULL;
}

const FccSymbol* fcc_symbol_table_lookup_current_scope(const FccSymbolTable* table,
                                                       const char* name) {
  const FccSymbolScope* scope;
  size_t symbol_index;

  assert(table != NULL);
  assert(name != NULL);
  assert(table->scope_count > 0);

  scope = &table->scopes[table->scope_count - 1];
  symbol_index = table->symbol_count;
  while (symbol_index > scope->first_symbol_index) {
    --symbol_index;
    if ((table->symbols[symbol_index].name == name) ||
        ((table->symbols[symbol_index].name != NULL) &&
         (strcmp(table->symbols[symbol_index].name, name) == 0))) {
      return &table->symbols[symbol_index];
    }
  }

  return NULL;
}

FccSymbol* fcc_symbol_table_lookup_mutable_current_scope(FccSymbolTable* table, const char* name) {
  const FccSymbolScope* scope;
  size_t symbol_index;

  assert(table != NULL);
  assert(name != NULL);
  assert(table->scope_count > 0);

  scope = &table->scopes[table->scope_count - 1];
  symbol_index = table->symbol_count;
  while (symbol_index > scope->first_symbol_index) {
    --symbol_index;
    if ((table->symbols[symbol_index].name == name) ||
        ((table->symbols[symbol_index].name != NULL) &&
         (strcmp(table->symbols[symbol_index].name, name) == 0))) {
      return &table->symbols[symbol_index];
    }
  }

  return NULL;
}
