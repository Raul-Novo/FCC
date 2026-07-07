// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdio.h>

#include "fcc/source.h"

typedef enum FccDiagSeverity {
  FCC_DIAG_SEVERITY_NOTE = 0,
  FCC_DIAG_SEVERITY_WARNING = 1,
  FCC_DIAG_SEVERITY_ERROR = 2,
  FCC_DIAG_SEVERITY_FATAL = 3
} FccDiagSeverity;

typedef struct FccDiagnostics {
  FILE* stream;
  size_t warning_count;
  size_t error_count;
} FccDiagnostics;

void fcc_diag_init(FccDiagnostics* diagnostics, FILE* stream);

const char* fcc_diag_severity_name(FccDiagSeverity severity);

void fcc_diag_emit(FccDiagnostics* diagnostics, const char* path, FccSourceLocation location,
                   FccDiagSeverity severity, const char* message);

void fcc_diag_emit_source(FccDiagnostics* diagnostics, const FccSourceFile* source_file,
                          FccSourceSpan span, FccDiagSeverity severity, const char* message);
