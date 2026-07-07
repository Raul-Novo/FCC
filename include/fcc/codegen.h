// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdio.h>

#include "fcc/ast.h"
#include "fcc/diag.h"
#include "fcc/sema.h"
#include "fcc/source.h"

/*
 * Codegen consumes a semantically checked tree. The `_with_sema` entry point is
 * the normal path; the wrapper exists for tests and stages that want codegen to
 * run semantic analysis internally first.
 */
bool fcc_codegen_emit_nasm_x64_with_sema(FILE* stream, const FccSourceFile* source_file,
                                         const FccAstTranslationUnit* translation_unit,
                                         FccSemaResult* sema_result,
                                         FccDiagnostics* diagnostics);

bool fcc_codegen_emit_nasm_x64(FILE* stream, const FccSourceFile* source_file,
                               const FccAstTranslationUnit* translation_unit,
                               FccDiagnostics* diagnostics);
