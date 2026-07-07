// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/signature.h"

#include <assert.h>

#pragma section(".fccsig", read)

typedef struct FccEmbeddedSignature {
  char marker[16];
  char product_name[48];
  char vendor_name[32];
  char version_string[16];
  char host_platform[16];
  char host_architecture[16];
  char object_format[16];
} FccEmbeddedSignature;

/* Keep a compact immutable compiler identity block in a dedicated section for binary inspection. */
__declspec(allocate(".fccsig")) const FccEmbeddedSignature fcc_signature_embedded_block = {
    "FCCSIG_V1",        "Flintine Studios C Compiler",
    "Flintine Studios", "0.8.0-dev",
    "Windows 11",       "x86_64",
    "PE/COFF",
};

static const FccSignatureInfo FCC_SIGNATURE_INFO = {
    "FCC",
    "Flintine C Compiler",
    "Flintine",
    "0.8.0-dev",
    "Windows 11",
    "x86_64",
    "PE/COFF",
};

const FccSignatureInfo* fcc_signature_get_info(void) {
  return &FCC_SIGNATURE_INFO;
}

const char* fcc_signature_get_embedded_marker(void) {
  return fcc_signature_embedded_block.marker;
}

void fcc_signature_print_version(FILE* output_stream) {
  const FccSignatureInfo* info;

  assert(output_stream != NULL);

  info = fcc_signature_get_info();
  fprintf(output_stream, "%s %s (%s, %s %s, %s)\n", info->short_name, info->version_string,
          info->product_name, info->host_platform, info->host_architecture, info->object_format);
}
