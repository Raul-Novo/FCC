// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>

#include "fcc/base.h"

typedef struct FccSourceLocation {
  size_t offset;
  size_t line;
  size_t column;
} FccSourceLocation;

typedef struct FccSourceSpan {
  size_t begin_offset;
  size_t end_offset;
} FccSourceSpan;

typedef struct FccSourceFile {
  char* path;
  BYTE* bytes;
  size_t byte_count;
  size_t* line_starts;
  size_t line_count;
} FccSourceFile;

/*
 * Loads a source file and transfers ownership of the resulting buffers to `source_file` on
 * success. On failure, `source_file` is reset to an empty state.
 */
bool fcc_source_file_load(FccSourceFile* source_file, const char* path, char* error_buffer,
                          size_t error_buffer_size);

bool fcc_source_file_init_take_bytes(FccSourceFile* source_file, const char* path, BYTE* bytes,
                                     size_t byte_count, char* error_buffer,
                                     size_t error_buffer_size);

void fcc_source_file_dispose(FccSourceFile* source_file);

FccSourceLocation fcc_source_offset_to_location(const FccSourceFile* source_file, size_t offset);
