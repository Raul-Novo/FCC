// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdio.h>

typedef struct FccSignatureInfo {
  const char* short_name;
  const char* product_name;
  const char* vendor_name;
  const char* version_string;
  const char* host_platform;
  const char* host_architecture;
  const char* object_format;
} FccSignatureInfo;

const FccSignatureInfo* fcc_signature_get_info(void);

const char* fcc_signature_get_embedded_marker(void);

void fcc_signature_print_version(FILE* output_stream);
