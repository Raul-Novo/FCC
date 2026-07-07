// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>

#include "fcc/ast.h"
#include "fcc/diag.h"
#include "fcc/source.h"

/*
 * The parser uses bounded recursive descent. `FCC_MAX_PARSE_DEPTH` limits
 * syntactic nesting and prevents unbounded stack growth from malformed input.
 */
bool fcc_parser_parse_translation_unit(const FccSourceFile* source_file,
                                       FccDiagnostics* diagnostics, FccAstContext* ast_context,
                                       FccAstTranslationUnit** translation_unit_out);
