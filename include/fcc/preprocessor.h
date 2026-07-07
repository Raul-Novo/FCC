// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "fcc/diag.h"
#include "fcc/source.h"

typedef struct FccPreprocessorOptions {
  const char* const* include_directories;
  size_t include_directory_count;
  const char* sysroot_directory;
} FccPreprocessorOptions;

/*
 * The standalone preprocessor writes text to a stream. The normal driver path
 * then reloads that text as a synthetic source file before lexing/parsing.
 */
bool fcc_preprocessor_run(const FccSourceFile* source_file, const FccPreprocessorOptions* options,
                          FccDiagnostics* diagnostics, FILE* output_stream);

bool fcc_preprocessor_run_to_source(const FccSourceFile* source_file,
                                    const FccPreprocessorOptions* options,
                                    FccDiagnostics* diagnostics,
                                    FccSourceFile* preprocessed_source_file,
                                    char* error_buffer, size_t error_buffer_size);
