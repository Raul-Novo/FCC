// SPDX-License-Identifier: GPL-3.0-or-later
#define _CRT_SECURE_NO_WARNINGS

#include "fcc/source.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fcc_source_reset(FccSourceFile* source_file) {
  assert(source_file != NULL);

  source_file->path = NULL;
  source_file->bytes = NULL;
  source_file->byte_count = 0;
  source_file->line_starts = NULL;
  source_file->line_count = 0;
}

static void fcc_source_set_error(char* error_buffer, size_t error_buffer_size,
                                 const char* message) {
  if ((error_buffer == NULL) || (error_buffer_size == 0)) {
    return;
  }

  (void)snprintf(error_buffer, error_buffer_size, "%s", message);
}

static bool fcc_source_duplicate_path(const char* path, char** duplicated_path, char* error_buffer,
                                      size_t error_buffer_size) {
  size_t path_length;
  char* allocated_path;

  assert(path != NULL);
  assert(duplicated_path != NULL);

  path_length = strlen(path);
  if (path_length >= FCC_MAX_PATH_LENGTH) {
    fcc_source_set_error(error_buffer, error_buffer_size, "path exceeds FCC_MAX_PATH_LENGTH");
    return false;
  }

  allocated_path = (char*)malloc(path_length + 1);
  if (allocated_path == NULL) {
    fcc_source_set_error(error_buffer, error_buffer_size, "out of memory");
    return false;
  }

  memcpy(allocated_path, path, path_length + 1);
  *duplicated_path = allocated_path;
  return true;
}

static bool fcc_source_allocate_buffer(size_t byte_count, BYTE** buffer, char* error_buffer,
                                       size_t error_buffer_size) {
  BYTE* allocated_buffer;

  assert(buffer != NULL);

  if (byte_count >= SIZE_MAX) {
    fcc_source_set_error(error_buffer, error_buffer_size, "source file is too large");
    return false;
  }

  allocated_buffer = (BYTE*)malloc(byte_count + 1);
  if (allocated_buffer == NULL) {
    fcc_source_set_error(error_buffer, error_buffer_size, "out of memory");
    return false;
  }

  allocated_buffer[byte_count] = 0;
  *buffer = allocated_buffer;
  return true;
}

static bool fcc_source_build_line_map(const BYTE* bytes, size_t byte_count, size_t** line_starts,
                                      size_t* line_count, char* error_buffer,
                                      size_t error_buffer_size) {
  size_t newline_count;
  size_t required_line_count;
  size_t* allocated_line_starts;
  size_t line_index;
  size_t offset;

  assert(bytes != NULL);
  assert(line_starts != NULL);
  assert(line_count != NULL);

  newline_count = 0;
  for (offset = 0; offset < byte_count; ++offset) {
    if (bytes[offset] == (BYTE)'\n') {
      ++newline_count;
    }
  }

  required_line_count = newline_count + 1;
  if (required_line_count > (SIZE_MAX / sizeof(size_t))) {
    fcc_source_set_error(error_buffer, error_buffer_size, "source line map overflow");
    return false;
  }

  allocated_line_starts = (size_t*)malloc(required_line_count * sizeof(size_t));
  if (allocated_line_starts == NULL) {
    fcc_source_set_error(error_buffer, error_buffer_size, "out of memory");
    return false;
  }

  allocated_line_starts[0] = 0;
  line_index = 1;
  for (offset = 0; offset < byte_count; ++offset) {
    if (bytes[offset] == (BYTE)'\n') {
      allocated_line_starts[line_index] = offset + 1;
      ++line_index;
    }
  }

  *line_starts = allocated_line_starts;
  *line_count = required_line_count;
  return true;
}

bool fcc_source_file_init_take_bytes(FccSourceFile* source_file, const char* path, BYTE* bytes,
                                     size_t byte_count, char* error_buffer,
                                     size_t error_buffer_size) {
  char* owned_path;
  size_t* line_starts;
  size_t line_count;

  assert(source_file != NULL);
  assert(path != NULL);

  fcc_source_reset(source_file);
  owned_path = NULL;
  line_starts = NULL;
  line_count = 0;

  if ((bytes == NULL) && (byte_count != 0)) {
    fcc_source_set_error(error_buffer, error_buffer_size, "source bytes are missing");
    return false;
  }

  if (bytes == NULL) {
    if (!fcc_source_allocate_buffer(0, &bytes, error_buffer, error_buffer_size)) {
      return false;
    }
  } else {
    bytes[byte_count] = 0;
  }

  if (!fcc_source_duplicate_path(path, &owned_path, error_buffer, error_buffer_size)) {
    free(bytes);
    return false;
  }

  if (!fcc_source_build_line_map(bytes, byte_count, &line_starts, &line_count, error_buffer,
                                 error_buffer_size)) {
    free(owned_path);
    free(bytes);
    return false;
  }

  source_file->path = owned_path;
  source_file->bytes = bytes;
  source_file->byte_count = byte_count;
  source_file->line_starts = line_starts;
  source_file->line_count = line_count;
  return true;
}

bool fcc_source_file_load(FccSourceFile* source_file, const char* path, char* error_buffer,
                          size_t error_buffer_size) {
  FILE* file_stream;
  char* owned_path;
  BYTE* buffer;
  long file_size;
  size_t byte_count;
  size_t bytes_read;
  size_t* line_starts;
  size_t line_count;
  bool load_ok;

  assert(source_file != NULL);
  assert(path != NULL);

  fcc_source_reset(source_file);
  file_stream = NULL;
  owned_path = NULL;
  buffer = NULL;
  line_starts = NULL;
  line_count = 0;
  load_ok = false;

  if (!fcc_source_duplicate_path(path, &owned_path, error_buffer, error_buffer_size)) {
    goto cleanup;
  }

  file_stream = fopen(path, "rb");
  if (file_stream == NULL) {
    fcc_source_set_error(error_buffer, error_buffer_size, "file not found");
    goto cleanup;
  }

  if (fseek(file_stream, 0, SEEK_END) != 0) {
    fcc_source_set_error(error_buffer, error_buffer_size, "unable to read source file");
    goto cleanup;
  }

  file_size = ftell(file_stream);
  if (file_size < 0) {
    fcc_source_set_error(error_buffer, error_buffer_size, "source file size is invalid");
    goto cleanup;
  }

  if (fseek(file_stream, 0, SEEK_SET) != 0) {
    fcc_source_set_error(error_buffer, error_buffer_size, "unable to read source file");
    goto cleanup;
  }

  if ((unsigned long)file_size > (unsigned long)(SIZE_MAX - 1)) {
    fcc_source_set_error(error_buffer, error_buffer_size, "source file is too large");
    goto cleanup;
  }

  byte_count = (size_t)file_size;
  if (!fcc_source_allocate_buffer(byte_count, &buffer, error_buffer, error_buffer_size)) {
    goto cleanup;
  }

  bytes_read = fread(buffer, 1, byte_count, file_stream);
  if (bytes_read != byte_count) {
    fcc_source_set_error(error_buffer, error_buffer_size, "unable to read source file");
    goto cleanup;
  }

  if (!fcc_source_build_line_map(buffer, byte_count, &line_starts, &line_count, error_buffer,
                                 error_buffer_size)) {
    goto cleanup;
  }

  source_file->path = owned_path;
  source_file->bytes = buffer;
  source_file->byte_count = byte_count;
  source_file->line_starts = line_starts;
  source_file->line_count = line_count;
  load_ok = true;

cleanup:
  if (file_stream != NULL) {
    (void)fclose(file_stream);
  }

  if (!load_ok) {
    free(owned_path);
    free(buffer);
    free(line_starts);
    fcc_source_reset(source_file);
  }

  return load_ok;
}

void fcc_source_file_dispose(FccSourceFile* source_file) {
  if (source_file == NULL) {
    return;
  }

  free(source_file->path);
  free(source_file->bytes);
  free(source_file->line_starts);
  fcc_source_reset(source_file);
}

FccSourceLocation fcc_source_offset_to_location(const FccSourceFile* source_file, size_t offset) {
  FccSourceLocation location;
  size_t clamped_offset;
  size_t low;
  size_t high;

  assert(source_file != NULL);
  assert(source_file->line_count > 0);
  assert(source_file->line_starts != NULL);

  clamped_offset = offset;
  if (clamped_offset > source_file->byte_count) {
    clamped_offset = source_file->byte_count;
  }

  low = 0;
  high = source_file->line_count;
  while ((low + 1) < high) {
    size_t mid;

    mid = low + ((high - low) / 2);
    if (source_file->line_starts[mid] <= clamped_offset) {
      low = mid;
    } else {
      high = mid;
    }
  }

  location.offset = clamped_offset;
  location.line = low + 1;
  location.column = (clamped_offset - source_file->line_starts[low]) + 1;
  return location;
}
