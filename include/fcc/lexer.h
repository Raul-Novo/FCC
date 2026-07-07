// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fcc/diag.h"
#include "fcc/token.h"

typedef struct FccLexer {
  const FccSourceFile* source_file;
  FccDiagnostics* diagnostics;
  size_t next_offset;
} FccLexer;

/*
 * `source_file` and `diagnostics` are borrowed for the lifetime of `lexer`.
 * They must remain valid until lexing is complete.
 */
void fcc_lexer_init(FccLexer* lexer, const FccSourceFile* source_file, FccDiagnostics* diagnostics);

void fcc_lexer_next(FccLexer* lexer, FccToken* token);
