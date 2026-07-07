// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdio.h>

typedef struct FccLinkRequest {
  const char* assembly_path;
  const char* output_path;
  bool delete_temporary_files;
} FccLinkRequest;

bool fcc_link_assemble_nasm_to_object(const char* assembly_path, const char* object_path,
                                      FILE* error_stream);

bool fcc_link_assemble_and_link_nasm_internal(const FccLinkRequest* request, FILE* error_stream);
