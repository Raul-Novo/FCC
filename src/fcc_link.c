// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/link.h"

#include <assert.h>
#include <process.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "fcc/base.h"

enum {
  FCC_LINK_SECTION_KIND_IGNORED = 0,
  FCC_LINK_SECTION_KIND_TEXT = 1,
  FCC_LINK_SECTION_KIND_DATA = 2
};

enum {
  FCC_LINK_MAX_INPUT_SECTIONS = 32,
  FCC_LINK_FILE_ALIGNMENT = 0x200,
  FCC_LINK_SECTION_ALIGNMENT = 0x1000,
  FCC_LINK_TEXT_OUTPUT_ALIGNMENT = 16,
  FCC_LINK_DATA_OUTPUT_ALIGNMENT = 8,
  FCC_LINK_MANIFEST_RESOURCE_TYPE = 24,
  FCC_LINK_MANIFEST_RESOURCE_ID = 1,
  FCC_LINK_MANIFEST_LANGUAGE_ID = 0,
  FCC_LINK_UTF8_CODE_PAGE = 65001
};

static const uint64_t FCC_LINK_IMAGE_BASE = 0x0000000140000000ULL;

static const char FCC_LINK_APPLICATION_MANIFEST[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
    "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">\n"
    "    <security>\n"
    "      <requestedPrivileges>\n"
    "        <requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/>\n"
    "      </requestedPrivileges>\n"
    "    </security>\n"
    "  </trustInfo>\n"
    "</assembly>\n";

/*
 * The internal linker handles the narrow object shape FCC emits today: one NASM
 * COFF object, code/data sections, a main entry, and the relocation forms needed
 * for that object. It is not a general-purpose multi-object linker yet.
 */
typedef struct FccLinkInputSection {
  const IMAGE_SECTION_HEADER* header;
  const IMAGE_RELOCATION* relocations;
  size_t relocation_count;
  size_t output_offset;
  uint32_t output_rva;
  int output_kind;
} FccLinkInputSection;

typedef struct FccLinkContext {
  const BYTE* object_bytes;
  size_t object_size;
  const IMAGE_FILE_HEADER* file_header;
  const IMAGE_SECTION_HEADER* section_headers;
  const IMAGE_SYMBOL* symbols;
  size_t symbol_count;
  const BYTE* string_table;
  size_t string_table_size;
  FccLinkInputSection input_sections[FCC_LINK_MAX_INPUT_SECTIONS];
  size_t section_count;
  BYTE* text_bytes;
  size_t text_size;
  size_t text_capacity;
  BYTE* data_bytes;
  size_t data_size;
  size_t data_capacity;
  uint32_t text_rva;
  uint32_t data_rva;
  uint32_t entry_rva;
} FccLinkContext;

static bool fcc_link_is_add_overflow(size_t left, size_t right) {
  return left > (SIZE_MAX - right);
}

static bool fcc_link_align_up(size_t value, size_t alignment, size_t* aligned_value) {
  size_t remainder;
  size_t padding;

  assert(aligned_value != NULL);
  assert(alignment != 0);

  remainder = value % alignment;
  if (remainder == 0) {
    *aligned_value = value;
    return true;
  }

  padding = alignment - remainder;
  if (fcc_link_is_add_overflow(value, padding)) {
    return false;
  }

  *aligned_value = value + padding;
  return true;
}

static bool fcc_link_write_bytes(FILE* stream, const void* data, size_t byte_count,
                                 FILE* error_stream) {
  assert(stream != NULL);
  assert(error_stream != NULL);

  if (byte_count == 0) {
    return true;
  }

  if (fwrite(data, 1, byte_count, stream) != byte_count) {
    fprintf(error_stream, "fcc: failed to write output executable\n");
    return false;
  }

  return true;
}

static bool fcc_link_write_zeros(FILE* stream, size_t byte_count, FILE* error_stream) {
  static const BYTE ZEROES[FCC_LINK_FILE_ALIGNMENT] = {0};
  size_t remaining_bytes;

  assert(stream != NULL);
  assert(error_stream != NULL);

  remaining_bytes = byte_count;
  while (remaining_bytes > 0) {
    size_t chunk_bytes;

    chunk_bytes = remaining_bytes;
    if (chunk_bytes > sizeof(ZEROES)) {
      chunk_bytes = sizeof(ZEROES);
    }

    if (!fcc_link_write_bytes(stream, ZEROES, chunk_bytes, error_stream)) {
      return false;
    }

    remaining_bytes -= chunk_bytes;
  }

  return true;
}

static bool fcc_link_utf8_to_utf16(const char* utf8_text, wchar_t** wide_text, FILE* error_stream) {
  int wide_length;
  size_t allocation_bytes;
  wchar_t* allocated_text;

  assert(utf8_text != NULL);
  assert(wide_text != NULL);
  assert(error_stream != NULL);

  wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_text, -1, NULL, 0);
  if (wide_length <= 0) {
    fprintf(error_stream, "fcc: path or argument is not valid UTF-8\n");
    return false;
  }

  allocation_bytes = (size_t)wide_length * sizeof(wchar_t);
  allocated_text = (wchar_t*)malloc(allocation_bytes);
  if (allocated_text == NULL) {
    fprintf(error_stream, "fcc: out of memory\n");
    return false;
  }

  wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_text, -1, allocated_text,
                                    wide_length);
  if (wide_length <= 0) {
    free(allocated_text);
    fprintf(error_stream, "fcc: path or argument is not valid UTF-8\n");
    return false;
  }

  *wide_text = allocated_text;
  return true;
}

static bool fcc_link_run_nasm(const char* object_path, const char* assembly_path,
                              FILE* error_stream) {
  wchar_t* wide_arguments[7];
  const wchar_t* spawn_arguments[7];
  intptr_t exit_code;
  size_t argument_index;
  const char* utf8_arguments[6];

  assert(object_path != NULL);
  assert(assembly_path != NULL);
  assert(error_stream != NULL);

  utf8_arguments[0] = "nasm";
  utf8_arguments[1] = "-f";
  utf8_arguments[2] = "win64";
  utf8_arguments[3] = "-o";
  utf8_arguments[4] = object_path;
  utf8_arguments[5] = assembly_path;

  for (argument_index = 0; argument_index < 6; ++argument_index) {
    wide_arguments[argument_index] = NULL;
    if (!fcc_link_utf8_to_utf16(utf8_arguments[argument_index], &wide_arguments[argument_index],
                                error_stream)) {
      size_t cleanup_index;

      for (cleanup_index = 0; cleanup_index < argument_index; ++cleanup_index) {
        free(wide_arguments[cleanup_index]);
      }

      return false;
    }

    spawn_arguments[argument_index] = wide_arguments[argument_index];
  }

  spawn_arguments[6] = NULL;
  exit_code = _wspawnvp(_P_WAIT, wide_arguments[0], spawn_arguments);

  for (argument_index = 0; argument_index < 6; ++argument_index) {
    free(wide_arguments[argument_index]);
  }

  if (exit_code == -1) {
    fprintf(error_stream, "fcc: failed to run 'nasm'; ensure nasm is installed and in PATH\n");
    return false;
  }

  if (exit_code != 0) {
    fprintf(error_stream, "fcc: tool 'nasm' failed with exit code %Id\n", exit_code);
    return false;
  }

  return true;
}

bool fcc_link_assemble_nasm_to_object(const char* assembly_path, const char* object_path,
                                      FILE* error_stream) {
  assert(assembly_path != NULL);
  assert(object_path != NULL);

  if (error_stream == NULL) {
    error_stream = stderr;
  }

  return fcc_link_run_nasm(object_path, assembly_path, error_stream);
}

static void fcc_link_delete_file_if_present(const char* path) {
  wchar_t* wide_path;

  assert(path != NULL);

  wide_path = NULL;
  if (!fcc_link_utf8_to_utf16(path, &wide_path, stderr)) {
    return;
  }

  (void)DeleteFileW(wide_path);
  free(wide_path);
}

static bool fcc_link_replace_extension(const char* input_path, const char* extension,
                                       char* path_buffer, size_t path_buffer_size,
                                       FILE* error_stream) {
  const char* input_basename_end;
  const char* input_extension;
  const char* path_cursor;
  size_t base_length;
  size_t extension_length;

  assert(input_path != NULL);
  assert(extension != NULL);
  assert(path_buffer != NULL);
  assert(path_buffer_size > 0);
  assert(error_stream != NULL);

  input_extension = NULL;
  input_basename_end = input_path + strlen(input_path);
  for (path_cursor = input_path; *path_cursor != '\0'; ++path_cursor) {
    if ((*path_cursor == '\\') || (*path_cursor == '/')) {
      input_extension = NULL;
      continue;
    }

    if (*path_cursor == '.') {
      input_extension = path_cursor;
    }
  }

  if (input_extension != NULL) {
    input_basename_end = input_extension;
  }

  base_length = (size_t)(input_basename_end - input_path);
  extension_length = strlen(extension);
  if (base_length + extension_length + 1 > path_buffer_size) {
    fprintf(error_stream, "fcc: derived output path exceeds FCC_MAX_PATH_LENGTH\n");
    return false;
  }

  (void)memcpy(path_buffer, input_path, base_length);
  (void)memcpy(path_buffer + base_length, extension, extension_length + 1);
  return true;
}

static bool fcc_link_read_file_bytes(const char* path, BYTE** bytes_out, size_t* byte_count_out,
                                     FILE* error_stream) {
  FILE* stream;
  long file_size;
  BYTE* bytes;
  size_t read_bytes;

  assert(path != NULL);
  assert(bytes_out != NULL);
  assert(byte_count_out != NULL);
  assert(error_stream != NULL);

  *bytes_out = NULL;
  *byte_count_out = 0;

  stream = NULL;
  if (fopen_s(&stream, path, "rb") != 0) {
    fprintf(error_stream, "fcc: failed to open '%s' for reading\n", path);
    return false;
  }

  if (fseek(stream, 0, SEEK_END) != 0) {
    (void)fclose(stream);
    fprintf(error_stream, "fcc: failed to seek '%s'\n", path);
    return false;
  }

  file_size = ftell(stream);
  if (file_size < 0) {
    (void)fclose(stream);
    fprintf(error_stream, "fcc: failed to query '%s' size\n", path);
    return false;
  }

  if (fseek(stream, 0, SEEK_SET) != 0) {
    (void)fclose(stream);
    fprintf(error_stream, "fcc: failed to seek '%s'\n", path);
    return false;
  }

  bytes = NULL;
  if ((size_t)file_size > 0) {
    bytes = (BYTE*)malloc((size_t)file_size);
    if (bytes == NULL) {
      (void)fclose(stream);
      fprintf(error_stream, "fcc: out of memory\n");
      return false;
    }

    read_bytes = fread(bytes, 1, (size_t)file_size, stream);
    if (read_bytes != (size_t)file_size) {
      free(bytes);
      (void)fclose(stream);
      fprintf(error_stream, "fcc: failed to read '%s'\n", path);
      return false;
    }
  }

  if (fclose(stream) != 0) {
    free(bytes);
    fprintf(error_stream, "fcc: failed to close '%s'\n", path);
    return false;
  }

  *bytes_out = bytes;
  *byte_count_out = (size_t)file_size;
  return true;
}

static bool fcc_link_parse_object_header(FccLinkContext* context, FILE* error_stream) {
  size_t sections_offset;
  size_t symbol_table_offset;
  size_t symbol_table_size;
  size_t string_table_offset;

  assert(context != NULL);
  assert(error_stream != NULL);

  if (context->object_size < sizeof(IMAGE_FILE_HEADER)) {
    fprintf(error_stream, "fcc: object file is truncated\n");
    return false;
  }

  context->file_header = (const IMAGE_FILE_HEADER*)context->object_bytes;
  if (context->file_header->Machine != IMAGE_FILE_MACHINE_AMD64) {
    fprintf(error_stream, "fcc: object machine is not x86_64\n");
    return false;
  }

  if (context->file_header->NumberOfSections == 0) {
    fprintf(error_stream, "fcc: object file has no sections\n");
    return false;
  }

  if (context->file_header->NumberOfSections > FCC_LINK_MAX_INPUT_SECTIONS) {
    fprintf(error_stream, "fcc: object file has too many sections\n");
    return false;
  }

  sections_offset = sizeof(IMAGE_FILE_HEADER) + context->file_header->SizeOfOptionalHeader;
  if (fcc_link_is_add_overflow(sections_offset, (size_t)context->file_header->NumberOfSections *
                                                    sizeof(IMAGE_SECTION_HEADER)) ||
      (sections_offset +
           ((size_t)context->file_header->NumberOfSections * sizeof(IMAGE_SECTION_HEADER)) >
       context->object_size)) {
    fprintf(error_stream, "fcc: object section table is truncated\n");
    return false;
  }

  context->section_headers = (const IMAGE_SECTION_HEADER*)(context->object_bytes + sections_offset);
  context->section_count = context->file_header->NumberOfSections;

  symbol_table_offset = context->file_header->PointerToSymbolTable;
  symbol_table_size = (size_t)context->file_header->NumberOfSymbols * sizeof(IMAGE_SYMBOL);
  if ((symbol_table_offset == 0) || (symbol_table_size == 0) ||
      fcc_link_is_add_overflow(symbol_table_offset, symbol_table_size) ||
      (symbol_table_offset + symbol_table_size > context->object_size)) {
    fprintf(error_stream, "fcc: object symbol table is missing or truncated\n");
    return false;
  }

  context->symbols = (const IMAGE_SYMBOL*)(context->object_bytes + symbol_table_offset);
  context->symbol_count = context->file_header->NumberOfSymbols;

  string_table_offset = symbol_table_offset + symbol_table_size;
  if (fcc_link_is_add_overflow(string_table_offset, sizeof(uint32_t)) ||
      (string_table_offset + sizeof(uint32_t) > context->object_size)) {
    context->string_table = NULL;
    context->string_table_size = 0;
    return true;
  }

  context->string_table = context->object_bytes + string_table_offset;
  context->string_table_size = *(const uint32_t*)context->string_table;
  if (context->string_table_size < sizeof(uint32_t)) {
    context->string_table_size = sizeof(uint32_t);
  }

  if ((context->string_table_size > 0) &&
      (fcc_link_is_add_overflow(string_table_offset, context->string_table_size) ||
       (string_table_offset + context->string_table_size > context->object_size))) {
    fprintf(error_stream, "fcc: object string table is truncated\n");
    return false;
  }

  return true;
}

static bool fcc_link_initialize_input_sections(FccLinkContext* context, FILE* error_stream) {
  size_t section_index;

  assert(context != NULL);
  assert(error_stream != NULL);

  for (section_index = 0; section_index < context->section_count; ++section_index) {
    const IMAGE_SECTION_HEADER* header;
    FccLinkInputSection* input_section;
    size_t raw_size;
    size_t raw_offset;
    size_t reloc_count;
    size_t reloc_offset;

    header = &context->section_headers[section_index];
    input_section = &context->input_sections[section_index];

    memset(input_section, 0, sizeof(*input_section));
    input_section->header = header;

    raw_size = header->SizeOfRawData;
    raw_offset = header->PointerToRawData;
    if (raw_size > 0) {
      if (fcc_link_is_add_overflow(raw_offset, raw_size) ||
          (raw_offset + raw_size > context->object_size)) {
        fprintf(error_stream, "fcc: section raw data is truncated\n");
        return false;
      }
    }

    reloc_count = header->NumberOfRelocations;
    reloc_offset = header->PointerToRelocations;
    if (reloc_count > 0) {
      size_t reloc_bytes;

      if (reloc_count > (SIZE_MAX / sizeof(IMAGE_RELOCATION))) {
        fprintf(error_stream, "fcc: section relocation table overflow\n");
        return false;
      }

      reloc_bytes = reloc_count * sizeof(IMAGE_RELOCATION);
      if (fcc_link_is_add_overflow(reloc_offset, reloc_bytes) ||
          (reloc_offset + reloc_bytes > context->object_size)) {
        fprintf(error_stream, "fcc: section relocation table is truncated\n");
        return false;
      }

      input_section->relocations = (const IMAGE_RELOCATION*)(context->object_bytes + reloc_offset);
    }

    input_section->relocation_count = reloc_count;

    if ((header->Characteristics & IMAGE_SCN_CNT_CODE) != 0) {
      input_section->output_kind = FCC_LINK_SECTION_KIND_TEXT;
      continue;
    }

    if (((header->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) != 0) ||
        ((header->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) != 0)) {
      input_section->output_kind = FCC_LINK_SECTION_KIND_DATA;
      continue;
    }

    input_section->output_kind = FCC_LINK_SECTION_KIND_IGNORED;
  }

  return true;
}

static bool fcc_link_ensure_capacity(BYTE** bytes, size_t* capacity, size_t required_capacity,
                                     FILE* error_stream) {
  size_t new_capacity;
  BYTE* new_bytes;

  assert(bytes != NULL);
  assert(capacity != NULL);
  assert(error_stream != NULL);

  if (*capacity >= required_capacity) {
    return true;
  }

  new_capacity = *capacity;
  if (new_capacity == 0) {
    new_capacity = 256;
  }

  while (new_capacity < required_capacity) {
    if (new_capacity > (SIZE_MAX / 2)) {
      fprintf(error_stream, "fcc: internal linker buffer overflow\n");
      return false;
    }

    new_capacity *= 2;
  }

  new_bytes = (BYTE*)realloc(*bytes, new_capacity);
  if (new_bytes == NULL) {
    fprintf(error_stream, "fcc: out of memory\n");
    return false;
  }

  *bytes = new_bytes;
  *capacity = new_capacity;
  return true;
}

static bool fcc_link_append_padding(BYTE** bytes, size_t* size, size_t* capacity, size_t alignment,
                                    FILE* error_stream) {
  size_t aligned_size;
  size_t padding_size;

  assert(bytes != NULL);
  assert(size != NULL);
  assert(capacity != NULL);
  assert(error_stream != NULL);

  if (!fcc_link_align_up(*size, alignment, &aligned_size)) {
    fprintf(error_stream, "fcc: internal linker alignment overflow\n");
    return false;
  }

  padding_size = aligned_size - *size;
  if (padding_size == 0) {
    return true;
  }

  if (!fcc_link_ensure_capacity(bytes, capacity, aligned_size, error_stream)) {
    return false;
  }

  memset(*bytes + *size, 0, padding_size);
  *size = aligned_size;
  return true;
}

static bool fcc_link_append_bytes(BYTE** bytes, size_t* size, size_t* capacity, const BYTE* source,
                                  size_t source_size, FILE* error_stream) {
  size_t required_size;

  assert(bytes != NULL);
  assert(size != NULL);
  assert(capacity != NULL);
  assert(error_stream != NULL);

  if (source_size == 0) {
    return true;
  }

  if (fcc_link_is_add_overflow(*size, source_size)) {
    fprintf(error_stream, "fcc: internal linker size overflow\n");
    return false;
  }

  required_size = *size + source_size;
  if (!fcc_link_ensure_capacity(bytes, capacity, required_size, error_stream)) {
    return false;
  }

  memcpy(*bytes + *size, source, source_size);
  *size = required_size;
  return true;
}

static bool fcc_link_append_zero_bytes(BYTE** bytes, size_t* size, size_t* capacity,
                                       size_t zero_size, FILE* error_stream) {
  size_t required_size;

  assert(bytes != NULL);
  assert(size != NULL);
  assert(capacity != NULL);
  assert(error_stream != NULL);

  if (zero_size == 0) {
    return true;
  }

  if (fcc_link_is_add_overflow(*size, zero_size)) {
    fprintf(error_stream, "fcc: internal linker size overflow\n");
    return false;
  }

  required_size = *size + zero_size;
  if (!fcc_link_ensure_capacity(bytes, capacity, required_size, error_stream)) {
    return false;
  }

  memset(*bytes + *size, 0, zero_size);
  *size = required_size;
  return true;
}

static bool fcc_link_build_output_sections(FccLinkContext* context, FILE* error_stream) {
  size_t section_index;

  assert(context != NULL);
  assert(error_stream != NULL);

  for (section_index = 0; section_index < context->section_count; ++section_index) {
    FccLinkInputSection* input_section;
    size_t raw_size;
    size_t raw_offset;
    size_t virtual_size;

    input_section = &context->input_sections[section_index];
    if (input_section->output_kind == FCC_LINK_SECTION_KIND_IGNORED) {
      continue;
    }

    raw_size = input_section->header->SizeOfRawData;
    raw_offset = input_section->header->PointerToRawData;
    virtual_size = input_section->header->Misc.VirtualSize;
    if (virtual_size < raw_size) {
      virtual_size = raw_size;
    }

    if (input_section->output_kind == FCC_LINK_SECTION_KIND_TEXT) {
      if (!fcc_link_append_padding(&context->text_bytes, &context->text_size,
                                   &context->text_capacity, FCC_LINK_TEXT_OUTPUT_ALIGNMENT,
                                   error_stream)) {
        return false;
      }

      input_section->output_offset = context->text_size;
      if (!fcc_link_append_bytes(&context->text_bytes, &context->text_size, &context->text_capacity,
                                 context->object_bytes + raw_offset, raw_size, error_stream)) {
        return false;
      }

      if (virtual_size > raw_size) {
        if (!fcc_link_append_zero_bytes(&context->text_bytes, &context->text_size,
                                        &context->text_capacity, virtual_size - raw_size,
                                        error_stream)) {
          return false;
        }
      }

      continue;
    }

    if (!fcc_link_append_padding(&context->data_bytes, &context->data_size, &context->data_capacity,
                                 FCC_LINK_DATA_OUTPUT_ALIGNMENT, error_stream)) {
      return false;
    }

    input_section->output_offset = context->data_size;
    if (raw_size > 0) {
      if (!fcc_link_append_bytes(&context->data_bytes, &context->data_size, &context->data_capacity,
                                 context->object_bytes + raw_offset, raw_size, error_stream)) {
        return false;
      }
    }

    if (virtual_size > raw_size) {
      if (!fcc_link_append_zero_bytes(&context->data_bytes, &context->data_size,
                                      &context->data_capacity, virtual_size - raw_size,
                                      error_stream)) {
        return false;
      }
    }
  }

  if (context->text_size == 0) {
    fprintf(error_stream, "fcc: object file has no code section\n");
    return false;
  }

  context->text_rva = FCC_LINK_SECTION_ALIGNMENT;
  if (context->data_size > 0) {
    size_t data_rva;

    if (!fcc_link_align_up((size_t)context->text_rva + context->text_size,
                           FCC_LINK_SECTION_ALIGNMENT, &data_rva) ||
        (data_rva > UINT32_MAX)) {
      fprintf(error_stream, "fcc: section layout overflow\n");
      return false;
    }

    context->data_rva = (uint32_t)data_rva;
  } else {
    context->data_rva = 0;
  }

  for (section_index = 0; section_index < context->section_count; ++section_index) {
    FccLinkInputSection* input_section;
    size_t section_rva;

    input_section = &context->input_sections[section_index];
    if (input_section->output_kind == FCC_LINK_SECTION_KIND_IGNORED) {
      continue;
    }

    section_rva = input_section->output_offset;
    if (input_section->output_kind == FCC_LINK_SECTION_KIND_TEXT) {
      if (fcc_link_is_add_overflow(section_rva, context->text_rva) ||
          (section_rva + context->text_rva > UINT32_MAX)) {
        fprintf(error_stream, "fcc: section address overflow\n");
        return false;
      }

      input_section->output_rva = (uint32_t)(section_rva + context->text_rva);
      continue;
    }

    if (fcc_link_is_add_overflow(section_rva, context->data_rva) ||
        (section_rva + context->data_rva > UINT32_MAX)) {
      fprintf(error_stream, "fcc: section address overflow\n");
      return false;
    }

    input_section->output_rva = (uint32_t)(section_rva + context->data_rva);
  }

  return true;
}

static bool fcc_link_copy_symbol_name(const FccLinkContext* context, size_t symbol_index,
                                      char* name_buffer, size_t name_buffer_size,
                                      FILE* error_stream) {
  const IMAGE_SYMBOL* symbol;

  assert(context != NULL);
  assert(name_buffer != NULL);
  assert(name_buffer_size > 0);
  assert(error_stream != NULL);

  if (symbol_index >= context->symbol_count) {
    fprintf(error_stream, "fcc: relocation references an invalid symbol index\n");
    return false;
  }

  symbol = &context->symbols[symbol_index];
  if (symbol->N.Name.Short != 0) {
    size_t copy_length;

    copy_length = 0;
    while ((copy_length < sizeof(symbol->N.ShortName)) &&
           (symbol->N.ShortName[copy_length] != '\0')) {
      ++copy_length;
    }

    if (copy_length + 1 > name_buffer_size) {
      fprintf(error_stream, "fcc: symbol name exceeds internal buffer length\n");
      return false;
    }

    memcpy(name_buffer, symbol->N.ShortName, copy_length);
    name_buffer[copy_length] = '\0';
    return true;
  }

  if ((context->string_table == NULL) || (context->string_table_size < sizeof(uint32_t))) {
    fprintf(error_stream, "fcc: symbol references a missing string table\n");
    return false;
  }

  if (symbol->N.Name.Long >= context->string_table_size) {
    fprintf(error_stream, "fcc: symbol name offset is out of range\n");
    return false;
  }

  {
    const char* symbol_name;
    size_t symbol_name_limit;
    size_t symbol_name_length;

    symbol_name = (const char*)(context->string_table + symbol->N.Name.Long);
    symbol_name_limit = context->string_table_size - symbol->N.Name.Long;
    symbol_name_length = 0;
    while ((symbol_name_length < symbol_name_limit) && (symbol_name[symbol_name_length] != '\0')) {
      ++symbol_name_length;
    }

    if ((symbol_name_length == symbol_name_limit) || (symbol_name_length + 1 > name_buffer_size)) {
      fprintf(error_stream, "fcc: symbol name is invalid\n");
      return false;
    }

    memcpy(name_buffer, symbol_name, symbol_name_length + 1);
  }

  return true;
}

static bool fcc_link_resolve_symbol_rva(const FccLinkContext* context, size_t symbol_index,
                                        uint32_t* symbol_rva, FILE* error_stream) {
  const IMAGE_SYMBOL* symbol;

  assert(context != NULL);
  assert(symbol_rva != NULL);
  assert(error_stream != NULL);

  if (symbol_index >= context->symbol_count) {
    fprintf(error_stream, "fcc: relocation references an invalid symbol index\n");
    return false;
  }

  symbol = &context->symbols[symbol_index];
  if (symbol->SectionNumber > 0) {
    size_t section_index;
    uint32_t section_rva;

    section_index = (size_t)symbol->SectionNumber - 1;
    if (section_index >= context->section_count) {
      fprintf(error_stream, "fcc: symbol references an invalid section\n");
      return false;
    }

    section_rva = context->input_sections[section_index].output_rva;
    if ((context->input_sections[section_index].output_kind == FCC_LINK_SECTION_KIND_IGNORED) ||
        (symbol->Value > UINT32_MAX - section_rva)) {
      fprintf(error_stream, "fcc: symbol references an unsupported section mapping\n");
      return false;
    }

    *symbol_rva = section_rva + symbol->Value;
    return true;
  }

  if (symbol->SectionNumber == IMAGE_SYM_UNDEFINED) {
    char name_buffer[FCC_MAX_IDENTIFIER_LENGTH + 32];

    if (!fcc_link_copy_symbol_name(context, symbol_index, name_buffer, sizeof(name_buffer),
                                   error_stream)) {
      return false;
    }

    fprintf(error_stream, "fcc: unresolved external symbol '%s'\n", name_buffer);
    return false;
  }

  fprintf(error_stream, "fcc: unsupported symbol section kind %d\n", symbol->SectionNumber);
  return false;
}

static bool fcc_link_find_main_entry(FccLinkContext* context, FILE* error_stream) {
  size_t symbol_index;

  assert(context != NULL);
  assert(error_stream != NULL);

  symbol_index = 0;
  while (symbol_index < context->symbol_count) {
    const IMAGE_SYMBOL* symbol;
    size_t aux_count;

    symbol = &context->symbols[symbol_index];
    aux_count = symbol->NumberOfAuxSymbols;

    if ((symbol->StorageClass == IMAGE_SYM_CLASS_EXTERNAL) && (symbol->SectionNumber > 0)) {
      char symbol_name[FCC_MAX_IDENTIFIER_LENGTH + 32];

      if (!fcc_link_copy_symbol_name(context, symbol_index, symbol_name, sizeof(symbol_name),
                                     error_stream)) {
        return false;
      }

      if (strcmp(symbol_name, "main") == 0) {
        if (!fcc_link_resolve_symbol_rva(context, symbol_index, &context->entry_rva,
                                         error_stream)) {
          return false;
        }

        return true;
      }
    }

    symbol_index += aux_count + 1;
  }

  fprintf(error_stream, "fcc: object does not define an external 'main' symbol\n");
  return false;
}

static int32_t fcc_link_read_int32(const BYTE* bytes) {
  int32_t value;

  assert(bytes != NULL);
  memcpy(&value, bytes, sizeof(value));
  return value;
}

static void fcc_link_write_int32(BYTE* bytes, int32_t value) {
  assert(bytes != NULL);
  memcpy(bytes, &value, sizeof(value));
}

static uint32_t fcc_link_read_uint32(const BYTE* bytes) {
  uint32_t value;

  assert(bytes != NULL);
  memcpy(&value, bytes, sizeof(value));
  return value;
}

static void fcc_link_write_uint32(BYTE* bytes, uint32_t value) {
  assert(bytes != NULL);
  memcpy(bytes, &value, sizeof(value));
}

static uint64_t fcc_link_read_uint64(const BYTE* bytes) {
  uint64_t value;

  assert(bytes != NULL);
  memcpy(&value, bytes, sizeof(value));
  return value;
}

static void fcc_link_write_uint64(BYTE* bytes, uint64_t value) {
  assert(bytes != NULL);
  memcpy(bytes, &value, sizeof(value));
}

static bool fcc_link_apply_relocations(FccLinkContext* context, FILE* error_stream) {
  size_t section_index;

  assert(context != NULL);
  assert(error_stream != NULL);

  /*
   * Relocations are patched after input sections have been copied to their final
   * output buffers, so each symbol can be resolved to an image-relative RVA.
   */
  for (section_index = 0; section_index < context->section_count; ++section_index) {
    const FccLinkInputSection* input_section;
    BYTE* output_bytes;
    size_t output_size;
    size_t relocation_index;

    input_section = &context->input_sections[section_index];
    if (input_section->output_kind == FCC_LINK_SECTION_KIND_IGNORED) {
      continue;
    }

    if (input_section->output_kind == FCC_LINK_SECTION_KIND_TEXT) {
      output_bytes = context->text_bytes;
      output_size = context->text_size;
    } else {
      output_bytes = context->data_bytes;
      output_size = context->data_size;
    }

    for (relocation_index = 0; relocation_index < input_section->relocation_count;
         ++relocation_index) {
      const IMAGE_RELOCATION* relocation;
      uint32_t target_rva;
      uint64_t target_va;
      uint64_t place_va;
      size_t patch_offset;

      relocation = &input_section->relocations[relocation_index];
      patch_offset = input_section->output_offset + relocation->VirtualAddress;
      if (patch_offset >= output_size) {
        fprintf(error_stream, "fcc: relocation patch offset exceeds section size\n");
        return false;
      }

      if (!fcc_link_resolve_symbol_rva(context, relocation->SymbolTableIndex, &target_rva,
                                       error_stream)) {
        return false;
      }

      target_va = FCC_LINK_IMAGE_BASE + target_rva;
      place_va = FCC_LINK_IMAGE_BASE + input_section->output_rva + relocation->VirtualAddress;

      switch (relocation->Type) {
        case IMAGE_REL_AMD64_REL32:
        case IMAGE_REL_AMD64_REL32_1:
        case IMAGE_REL_AMD64_REL32_2:
        case IMAGE_REL_AMD64_REL32_3:
        case IMAGE_REL_AMD64_REL32_4:
        case IMAGE_REL_AMD64_REL32_5: {
          int32_t existing_addend;
          int adjustment;
          int64_t displacement;

          if (patch_offset + sizeof(int32_t) > output_size) {
            fprintf(error_stream, "fcc: REL32 relocation exceeds section size\n");
            return false;
          }

          existing_addend = fcc_link_read_int32(output_bytes + patch_offset);
          adjustment = (int)relocation->Type - (int)IMAGE_REL_AMD64_REL32;
          displacement = (int64_t)target_va - (int64_t)(place_va + 4 + (uint64_t)adjustment);
          displacement += existing_addend;
          if ((displacement < INT32_MIN) || (displacement > INT32_MAX)) {
            fprintf(error_stream, "fcc: REL32 relocation is out of range\n");
            return false;
          }

          fcc_link_write_int32(output_bytes + patch_offset, (int32_t)displacement);
          break;
        }
        case IMAGE_REL_AMD64_ADDR32: {
          uint32_t existing_addend;
          uint64_t value;

          if (patch_offset + sizeof(uint32_t) > output_size) {
            fprintf(error_stream, "fcc: ADDR32 relocation exceeds section size\n");
            return false;
          }

          existing_addend = fcc_link_read_uint32(output_bytes + patch_offset);
          value = target_va + existing_addend;
          if (value > UINT32_MAX) {
            fprintf(error_stream, "fcc: ADDR32 relocation overflows 32 bits\n");
            return false;
          }

          fcc_link_write_uint32(output_bytes + patch_offset, (uint32_t)value);
          break;
        }
        case IMAGE_REL_AMD64_ADDR32NB: {
          uint32_t existing_addend;
          uint64_t value;

          if (patch_offset + sizeof(uint32_t) > output_size) {
            fprintf(error_stream, "fcc: ADDR32NB relocation exceeds section size\n");
            return false;
          }

          existing_addend = fcc_link_read_uint32(output_bytes + patch_offset);
          value = (uint64_t)target_rva + existing_addend;
          if (value > UINT32_MAX) {
            fprintf(error_stream, "fcc: ADDR32NB relocation overflows 32 bits\n");
            return false;
          }

          fcc_link_write_uint32(output_bytes + patch_offset, (uint32_t)value);
          break;
        }
        case IMAGE_REL_AMD64_ADDR64: {
          uint64_t existing_addend;

          if (patch_offset + sizeof(uint64_t) > output_size) {
            fprintf(error_stream, "fcc: ADDR64 relocation exceeds section size\n");
            return false;
          }

          existing_addend = fcc_link_read_uint64(output_bytes + patch_offset);
          fcc_link_write_uint64(output_bytes + patch_offset, target_va + existing_addend);
          break;
        }
        default:
          fprintf(error_stream, "fcc: unsupported relocation type 0x%04X\n",
                  (unsigned int)relocation->Type);
          return false;
      }
    }
  }

  return true;
}

static bool fcc_link_build_manifest_resource(uint32_t resource_rva, BYTE** resource_bytes_out,
                                             size_t* resource_size_out, FILE* error_stream) {
  enum {
    ROOT_DIRECTORY_OFFSET = 0,
    ROOT_ENTRY_OFFSET = ROOT_DIRECTORY_OFFSET + sizeof(IMAGE_RESOURCE_DIRECTORY),
    TYPE_DIRECTORY_OFFSET = ROOT_ENTRY_OFFSET + sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY),
    TYPE_ENTRY_OFFSET = TYPE_DIRECTORY_OFFSET + sizeof(IMAGE_RESOURCE_DIRECTORY),
    LANGUAGE_DIRECTORY_OFFSET = TYPE_ENTRY_OFFSET + sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY),
    LANGUAGE_ENTRY_OFFSET = LANGUAGE_DIRECTORY_OFFSET + sizeof(IMAGE_RESOURCE_DIRECTORY),
    DATA_ENTRY_OFFSET = LANGUAGE_ENTRY_OFFSET + sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY),
    MANIFEST_DATA_OFFSET = DATA_ENTRY_OFFSET + sizeof(IMAGE_RESOURCE_DATA_ENTRY)
  };
  IMAGE_RESOURCE_DATA_ENTRY* data_entry;
  IMAGE_RESOURCE_DIRECTORY* language_directory;
  IMAGE_RESOURCE_DIRECTORY* root_directory;
  IMAGE_RESOURCE_DIRECTORY* type_directory;
  IMAGE_RESOURCE_DIRECTORY_ENTRY* language_entry;
  IMAGE_RESOURCE_DIRECTORY_ENTRY* root_entry;
  IMAGE_RESOURCE_DIRECTORY_ENTRY* type_entry;
  BYTE* resource_bytes;
  size_t manifest_size;
  size_t resource_size;

  assert(resource_bytes_out != NULL);
  assert(resource_size_out != NULL);
  assert(error_stream != NULL);

  manifest_size = sizeof(FCC_LINK_APPLICATION_MANIFEST) - 1;
  if ((manifest_size > UINT32_MAX) ||
      (resource_rva > (UINT32_MAX - (uint32_t)MANIFEST_DATA_OFFSET)) ||
      (manifest_size > (SIZE_MAX - MANIFEST_DATA_OFFSET))) {
    fprintf(error_stream, "fcc: manifest resource size overflow\n");
    return false;
  }

  resource_size = MANIFEST_DATA_OFFSET + manifest_size;
  resource_bytes = (BYTE*)calloc(1, resource_size);
  if (resource_bytes == NULL) {
    fprintf(error_stream, "fcc: out of memory\n");
    return false;
  }

  root_directory = (IMAGE_RESOURCE_DIRECTORY*)(void*)(resource_bytes + ROOT_DIRECTORY_OFFSET);
  root_directory->NumberOfIdEntries = 1;

  root_entry = (IMAGE_RESOURCE_DIRECTORY_ENTRY*)(void*)(resource_bytes + ROOT_ENTRY_OFFSET);
  root_entry->Id = FCC_LINK_MANIFEST_RESOURCE_TYPE;
  root_entry->OffsetToData = IMAGE_RESOURCE_DATA_IS_DIRECTORY | (DWORD)TYPE_DIRECTORY_OFFSET;

  type_directory = (IMAGE_RESOURCE_DIRECTORY*)(void*)(resource_bytes + TYPE_DIRECTORY_OFFSET);
  type_directory->NumberOfIdEntries = 1;

  type_entry = (IMAGE_RESOURCE_DIRECTORY_ENTRY*)(void*)(resource_bytes + TYPE_ENTRY_OFFSET);
  type_entry->Id = FCC_LINK_MANIFEST_RESOURCE_ID;
  type_entry->OffsetToData = IMAGE_RESOURCE_DATA_IS_DIRECTORY | (DWORD)LANGUAGE_DIRECTORY_OFFSET;

  language_directory =
      (IMAGE_RESOURCE_DIRECTORY*)(void*)(resource_bytes + LANGUAGE_DIRECTORY_OFFSET);
  language_directory->NumberOfIdEntries = 1;

  language_entry = (IMAGE_RESOURCE_DIRECTORY_ENTRY*)(void*)(resource_bytes + LANGUAGE_ENTRY_OFFSET);
  language_entry->Id = FCC_LINK_MANIFEST_LANGUAGE_ID;
  language_entry->OffsetToData = (DWORD)DATA_ENTRY_OFFSET;

  data_entry = (IMAGE_RESOURCE_DATA_ENTRY*)(void*)(resource_bytes + DATA_ENTRY_OFFSET);
  data_entry->OffsetToData = resource_rva + (DWORD)MANIFEST_DATA_OFFSET;
  data_entry->Size = (DWORD)manifest_size;
  data_entry->CodePage = FCC_LINK_UTF8_CODE_PAGE;
  data_entry->Reserved = 0;

  memcpy(resource_bytes + MANIFEST_DATA_OFFSET, FCC_LINK_APPLICATION_MANIFEST, manifest_size);

  *resource_bytes_out = resource_bytes;
  *resource_size_out = resource_size;
  return true;
}

static bool fcc_link_write_pe_image(const FccLinkContext* context, const char* output_path,
                                    FILE* error_stream) {
  FILE* output_stream;
  bool has_data_section;
  size_t section_count;
  size_t nt_offset;
  size_t headers_unaligned_size;
  size_t headers_size;
  size_t text_raw_size;
  size_t data_raw_size;
  BYTE* resource_bytes;
  size_t resource_size;
  size_t resource_raw_size;
  size_t resource_rva_size;
  uint32_t resource_rva;
  size_t file_offset;
  size_t size_of_image;
  size_t initialized_data_size;
  size_t image_end_rva;
  BYTE* headers;
  IMAGE_DOS_HEADER* dos_header;
  DWORD* nt_signature;
  IMAGE_FILE_HEADER* file_header;
  IMAGE_OPTIONAL_HEADER64* optional_header;
  IMAGE_SECTION_HEADER* section_headers;
  IMAGE_SECTION_HEADER* text_section;
  IMAGE_SECTION_HEADER* data_section;
  IMAGE_SECTION_HEADER* resource_section;

  assert(context != NULL);
  assert(output_path != NULL);
  assert(error_stream != NULL);

  has_data_section = context->data_size > 0;
  section_count = has_data_section ? 3 : 2;
  nt_offset = sizeof(IMAGE_DOS_HEADER);
  headers_unaligned_size = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                           sizeof(IMAGE_OPTIONAL_HEADER64) +
                           (section_count * sizeof(IMAGE_SECTION_HEADER));
  if (!fcc_link_align_up(headers_unaligned_size, FCC_LINK_FILE_ALIGNMENT, &headers_size)) {
    fprintf(error_stream, "fcc: PE header alignment overflow\n");
    return false;
  }

  if (!fcc_link_align_up(context->text_size, FCC_LINK_FILE_ALIGNMENT, &text_raw_size)) {
    fprintf(error_stream, "fcc: text section alignment overflow\n");
    return false;
  }

  data_raw_size = 0;
  if (context->data_size > 0) {
    if (!fcc_link_align_up(context->data_size, FCC_LINK_FILE_ALIGNMENT, &data_raw_size)) {
      fprintf(error_stream, "fcc: data section alignment overflow\n");
      return false;
    }
  }

  resource_rva_size =
      has_data_section ? ((size_t)context->data_rva + context->data_size)
                       : ((size_t)context->text_rva + context->text_size);
  if (!fcc_link_align_up(resource_rva_size, FCC_LINK_SECTION_ALIGNMENT, &resource_rva_size) ||
      (resource_rva_size > UINT32_MAX)) {
    fprintf(error_stream, "fcc: resource section address overflow\n");
    return false;
  }

  resource_rva = (uint32_t)resource_rva_size;
  resource_bytes = NULL;
  resource_size = 0;
  if (!fcc_link_build_manifest_resource(resource_rva, &resource_bytes, &resource_size,
                                        error_stream)) {
    return false;
  }

  if (!fcc_link_align_up(resource_size, FCC_LINK_FILE_ALIGNMENT, &resource_raw_size)) {
    free(resource_bytes);
    fprintf(error_stream, "fcc: resource section alignment overflow\n");
    return false;
  }

  if (fcc_link_is_add_overflow(context->data_size, resource_size) ||
      ((context->data_size + resource_size) > UINT32_MAX)) {
    free(resource_bytes);
    fprintf(error_stream, "fcc: initialized data size overflow\n");
    return false;
  }

  initialized_data_size = context->data_size + resource_size;
  headers = (BYTE*)calloc(1, headers_size);
  if (headers == NULL) {
    free(resource_bytes);
    fprintf(error_stream, "fcc: out of memory\n");
    return false;
  }

  dos_header = (IMAGE_DOS_HEADER*)headers;
  dos_header->e_magic = IMAGE_DOS_SIGNATURE;
  dos_header->e_lfanew = (LONG)nt_offset;

  nt_signature = (DWORD*)(headers + nt_offset);
  *nt_signature = IMAGE_NT_SIGNATURE;

  file_header = (IMAGE_FILE_HEADER*)(headers + nt_offset + sizeof(DWORD));
  file_header->Machine = IMAGE_FILE_MACHINE_AMD64;
  file_header->NumberOfSections = (WORD)section_count;
  file_header->TimeDateStamp = 0;
  file_header->PointerToSymbolTable = 0;
  file_header->NumberOfSymbols = 0;
  file_header->SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
  file_header->Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;

  optional_header =
      (IMAGE_OPTIONAL_HEADER64*)(headers + nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER));
  optional_header->Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  optional_header->MajorLinkerVersion = 1;
  optional_header->MinorLinkerVersion = 0;
  optional_header->SizeOfCode = (DWORD)context->text_size;
  optional_header->SizeOfInitializedData = (DWORD)initialized_data_size;
  optional_header->SizeOfUninitializedData = 0;
  optional_header->AddressOfEntryPoint = context->entry_rva;
  optional_header->BaseOfCode = context->text_rva;
  optional_header->ImageBase = FCC_LINK_IMAGE_BASE;
  optional_header->SectionAlignment = FCC_LINK_SECTION_ALIGNMENT;
  optional_header->FileAlignment = FCC_LINK_FILE_ALIGNMENT;
  optional_header->MajorOperatingSystemVersion = 6;
  optional_header->MinorOperatingSystemVersion = 0;
  optional_header->MajorImageVersion = 0;
  optional_header->MinorImageVersion = 0;
  optional_header->MajorSubsystemVersion = 6;
  optional_header->MinorSubsystemVersion = 0;
  optional_header->Win32VersionValue = 0;
  optional_header->SizeOfHeaders = (DWORD)headers_size;
  optional_header->Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
  optional_header->DllCharacteristics = 0;
  optional_header->SizeOfStackReserve = 1024 * 1024;
  optional_header->SizeOfStackCommit = 4096;
  optional_header->SizeOfHeapReserve = 1024 * 1024;
  optional_header->SizeOfHeapCommit = 4096;
  optional_header->LoaderFlags = 0;
  optional_header->NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
  optional_header->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress = resource_rva;
  optional_header->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size = (DWORD)resource_size;

  section_headers =
      (IMAGE_SECTION_HEADER*)(headers + nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                              sizeof(IMAGE_OPTIONAL_HEADER64));

  text_section = &section_headers[0];
  memset(text_section, 0, sizeof(*text_section));
  memcpy(text_section->Name, ".text", 5);
  text_section->Misc.VirtualSize = (DWORD)context->text_size;
  text_section->VirtualAddress = context->text_rva;
  text_section->SizeOfRawData = (DWORD)text_raw_size;
  text_section->PointerToRawData = (DWORD)headers_size;
  text_section->PointerToRelocations = 0;
  text_section->PointerToLinenumbers = 0;
  text_section->NumberOfRelocations = 0;
  text_section->NumberOfLinenumbers = 0;
  text_section->Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

  file_offset = headers_size + text_raw_size;
  if (has_data_section) {
    data_section = &section_headers[1];
    memset(data_section, 0, sizeof(*data_section));
    memcpy(data_section->Name, ".data", 5);
    data_section->Misc.VirtualSize = (DWORD)context->data_size;
    data_section->VirtualAddress = context->data_rva;
    data_section->SizeOfRawData = (DWORD)data_raw_size;
    data_section->PointerToRawData = (DWORD)file_offset;
    data_section->PointerToRelocations = 0;
    data_section->PointerToLinenumbers = 0;
    data_section->NumberOfRelocations = 0;
    data_section->NumberOfLinenumbers = 0;
    data_section->Characteristics =
        IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

    file_offset += data_raw_size;
  }

  resource_section = &section_headers[has_data_section ? 2 : 1];
  memset(resource_section, 0, sizeof(*resource_section));
  memcpy(resource_section->Name, ".rsrc", 6);
  resource_section->Misc.VirtualSize = (DWORD)resource_size;
  resource_section->VirtualAddress = resource_rva;
  resource_section->SizeOfRawData = (DWORD)resource_raw_size;
  resource_section->PointerToRawData = (DWORD)file_offset;
  resource_section->PointerToRelocations = 0;
  resource_section->PointerToLinenumbers = 0;
  resource_section->NumberOfRelocations = 0;
  resource_section->NumberOfLinenumbers = 0;
  resource_section->Characteristics =
      IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

  if (resource_rva_size > (SIZE_MAX - resource_size)) {
    free(resource_bytes);
    free(headers);
    fprintf(error_stream, "fcc: image size overflow\n");
    return false;
  }

  image_end_rva = resource_rva_size + resource_size;
  if (!fcc_link_align_up(image_end_rva, FCC_LINK_SECTION_ALIGNMENT, &size_of_image) ||
      (size_of_image > UINT32_MAX)) {
    free(resource_bytes);
    free(headers);
    fprintf(error_stream, "fcc: image size overflow\n");
    return false;
  }

  optional_header->SizeOfImage = (DWORD)size_of_image;

  output_stream = NULL;
  if (fopen_s(&output_stream, output_path, "wb") != 0) {
    free(resource_bytes);
    free(headers);
    fprintf(error_stream, "fcc: failed to open '%s' for writing\n", output_path);
    return false;
  }

  if (!fcc_link_write_bytes(output_stream, headers, headers_size, error_stream) ||
      !fcc_link_write_bytes(output_stream, context->text_bytes, context->text_size, error_stream) ||
      !fcc_link_write_zeros(output_stream, text_raw_size - context->text_size, error_stream) ||
      (has_data_section &&
       (!fcc_link_write_bytes(output_stream, context->data_bytes, context->data_size,
                              error_stream) ||
        !fcc_link_write_zeros(output_stream, data_raw_size - context->data_size, error_stream))) ||
      !fcc_link_write_bytes(output_stream, resource_bytes, resource_size, error_stream) ||
      !fcc_link_write_zeros(output_stream, resource_raw_size - resource_size, error_stream)) {
    free(resource_bytes);
    free(headers);
    (void)fclose(output_stream);
    return false;
  }

  free(resource_bytes);
  free(headers);
  if (fclose(output_stream) != 0) {
    fprintf(error_stream, "fcc: failed to close '%s'\n", output_path);
    return false;
  }

  return true;
}

static void fcc_link_dispose_context(FccLinkContext* context) {
  if (context == NULL) {
    return;
  }

  free(context->text_bytes);
  free(context->data_bytes);
  context->text_bytes = NULL;
  context->data_bytes = NULL;
  context->text_size = 0;
  context->data_size = 0;
  context->text_capacity = 0;
  context->data_capacity = 0;
}

bool fcc_link_assemble_and_link_nasm_internal(const FccLinkRequest* request, FILE* error_stream) {
  FccLinkContext context;
  BYTE* object_bytes;
  size_t object_size;
  char object_path[FCC_MAX_PATH_LENGTH];
  bool cleanup_object;
  bool ok;

  assert(request != NULL);
  assert(request->assembly_path != NULL);
  assert(request->output_path != NULL);

  if (error_stream == NULL) {
    error_stream = stderr;
  }

  memset(&context, 0, sizeof(context));
  object_bytes = NULL;
  object_size = 0;
  cleanup_object = false;
  ok = false;

  if (!fcc_link_replace_extension(request->output_path, ".fcc_stage.obj", object_path,
                                  sizeof(object_path), error_stream)) {
    goto cleanup;
  }

  if (!fcc_link_run_nasm(object_path, request->assembly_path, error_stream)) {
    goto cleanup;
  }

  cleanup_object = request->delete_temporary_files;
  if (!fcc_link_read_file_bytes(object_path, &object_bytes, &object_size, error_stream)) {
    goto cleanup;
  }

  context.object_bytes = object_bytes;
  context.object_size = object_size;
  if (!fcc_link_parse_object_header(&context, error_stream) ||
      !fcc_link_initialize_input_sections(&context, error_stream) ||
      !fcc_link_build_output_sections(&context, error_stream) ||
      !fcc_link_find_main_entry(&context, error_stream) ||
      !fcc_link_apply_relocations(&context, error_stream) ||
      !fcc_link_write_pe_image(&context, request->output_path, error_stream)) {
    goto cleanup;
  }

  ok = true;

cleanup:
  fcc_link_dispose_context(&context);
  free(object_bytes);

  if (cleanup_object) {
    fcc_link_delete_file_if_present(object_path);
  }

  return ok;
}
