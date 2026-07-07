// SPDX-License-Identifier: GPL-3.0-or-later
#define _CRT_SECURE_NO_WARNINGS

#include "fcc/driver.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "fcc/ast.h"
#include "fcc/base.h"
#include "fcc/codegen.h"
#include "fcc/diag.h"
#include "fcc/lexer.h"
#include "fcc/link.h"
#include "fcc/parser.h"
#include "fcc/preprocessor.h"
#include "fcc/sema.h"
#include "fcc/signature.h"
#include "fcc/source.h"
#include "fcc/token.h"

typedef enum FccDriverStage {
  FCC_DRIVER_STAGE_CHECK = 0,
  FCC_DRIVER_STAGE_LEX_ONLY = 1,
  FCC_DRIVER_STAGE_PARSE_ONLY = 2,
  FCC_DRIVER_STAGE_PREPROCESS = 3,
  FCC_DRIVER_STAGE_EMIT_ASM = 4,
  FCC_DRIVER_STAGE_EMIT_OBJECT = 5,
  FCC_DRIVER_STAGE_COMPILE_AND_LINK = 6
} FccDriverStage;

typedef struct FccDriverOptions {
  const char* output_path;
  const char* input_path;
  const char* sysroot_path;
  const char* include_directories[FCC_MAX_INCLUDE_DIRECTORIES];
  size_t include_directory_count;
  FccDriverStage stage;
  bool has_explicit_stage;
  bool should_dump_tokens;
  bool should_dump_ast;
  bool should_show_help;
  bool should_show_version;
  bool should_use_asm_extension;
} FccDriverOptions;

static void fcc_driver_print_usage(FILE* output_stream) {
  assert(output_stream != NULL);

  fcc_signature_print_version(output_stream);
  fprintf(output_stream,
          "Usage: fcc [--help] [--version] [--dump-tokens] [--dump-ast] [--lex-only | "
          "--parse-only\n");
  fprintf(output_stream, "           | --check | --preprocess | --emit-asm | --emit-obj | "
                         "--compile-and-link]\n");
  fprintf(output_stream, "           [-I <dir> | -I<dir>] [--output <path>]\n");
  fprintf(output_stream, "           [--asm-extension=asm] [--sysroot <dir>] <input.c>\n");
  fprintf(output_stream,
          "Current phase: source loading, diagnostics, lexer, parser, semantic analysis,\n");
  fprintf(output_stream, "initial NASM x86_64 code generation, and Windows toolchain driving.\n");
  fprintf(output_stream, "Available flags:\n");
  fprintf(output_stream, "  --help              Show this usage text.\n");
  fprintf(output_stream, "  --version           Show the compiler identity and version.\n");
  fprintf(output_stream, "  --dump-tokens       Lex the input and dump tokens.\n");
  fprintf(output_stream, "  --dump-ast          Parse the input and dump the AST.\n");
  fprintf(output_stream, "  --lex-only          Stop after lexing.\n");
  fprintf(output_stream, "  --parse-only        Stop after parsing.\n");
  fprintf(output_stream, "  --check             Stop after semantic analysis.\n");
  fprintf(output_stream, "  --preprocess        Emit preprocessed source text.\n");
  fprintf(output_stream, "  --emit-asm          Emit NASM x86_64 assembly.\n");
  fprintf(output_stream, "  --emit-obj          Emit a NASM-assembled COFF object (.obj).\n");
  fprintf(output_stream,
          "  --compile-and-link  Emit assembly, assemble with nasm, and use FCC internal "
          "linking.\n");
  fprintf(output_stream,
          "  -I PATH             Add a quoted-include search directory for preprocessing.\n");
  fprintf(output_stream, "  --sysroot PATH      Resolve system includes from PATH\\include.\n");
  fprintf(output_stream, "  --output PATH       Write the stage output to PATH.\n");
  fprintf(output_stream, "  --asm-extension=asm Use '.asm' instead of the default '.s'.\n");
  fprintf(output_stream,
          "Compile-and-link uses only nasm plus FCC internal PE/COFF linking and does not\n");
  fprintf(output_stream, "invoke external C compilers or external linkers.\n");
}

static bool fcc_driver_is_help_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--help") == 0;
}

static bool fcc_driver_is_version_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--version") == 0;
}

static bool fcc_driver_is_dump_tokens_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--dump-tokens") == 0;
}

static bool fcc_driver_is_dump_ast_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--dump-ast") == 0;
}

static bool fcc_driver_is_lex_only_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--lex-only") == 0;
}

static bool fcc_driver_is_parse_only_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--parse-only") == 0;
}

static bool fcc_driver_is_check_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--check") == 0;
}

static bool fcc_driver_is_preprocess_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--preprocess") == 0;
}

static bool fcc_driver_is_emit_asm_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--emit-asm") == 0;
}

static bool fcc_driver_is_emit_obj_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--emit-obj") == 0;
}

static bool fcc_driver_is_compile_and_link_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--compile-and-link") == 0;
}

static bool fcc_driver_is_output_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--output") == 0;
}

static bool fcc_driver_is_sysroot_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "--sysroot") == 0;
}

static bool fcc_driver_is_include_directory_argument(const char* argument) {
  assert(argument != NULL);
  return strcmp(argument, "-I") == 0;
}

static bool fcc_driver_parse_asm_extension_argument(const char* argument,
                                                    bool* should_use_asm_extension) {
  static const char PREFIX[] = "--asm-extension=";
  const char* value;

  assert(argument != NULL);
  assert(should_use_asm_extension != NULL);

  if (strncmp(argument, PREFIX, sizeof(PREFIX) - 1) != 0) {
    return false;
  }

  value = argument + (sizeof(PREFIX) - 1);
  if (strcmp(value, "asm") == 0) {
    *should_use_asm_extension = true;
    return true;
  }

  if (strcmp(value, "s") == 0) {
    *should_use_asm_extension = false;
    return true;
  }

  return false;
}

static bool fcc_driver_select_stage(FccDriverOptions* options, FccDriverStage stage,
                                    const char* argument, FILE* error_stream) {
  assert(options != NULL);
  assert(argument != NULL);
  assert(error_stream != NULL);

  if (options->has_explicit_stage) {
    fprintf(error_stream, "fcc: multiple terminal stage flags were provided; '%s' conflicts\n",
            argument);
    return false;
  }

  options->stage = stage;
  options->has_explicit_stage = true;
  return true;
}

static bool fcc_driver_replace_extension(const char* input_path, const char* extension,
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

static bool fcc_driver_build_output_path(const FccDriverOptions* options, const char* extension,
                                         char* path_buffer, size_t path_buffer_size,
                                         FILE* error_stream) {
  assert(options != NULL);
  assert(extension != NULL);
  assert(path_buffer != NULL);
  assert(path_buffer_size > 0);
  assert(error_stream != NULL);

  if (options->output_path != NULL) {
    if (strlen(options->output_path) + 1 > path_buffer_size) {
      fprintf(error_stream, "fcc: output path exceeds FCC_MAX_PATH_LENGTH\n");
      return false;
    }

    (void)memcpy(path_buffer, options->output_path, strlen(options->output_path) + 1);
    return true;
  }

  return fcc_driver_replace_extension(options->input_path, extension, path_buffer, path_buffer_size,
                                      error_stream);
}

static void fcc_driver_build_preprocessor_options(const FccDriverOptions* options,
                                                  FccPreprocessorOptions* preprocess_options) {
  assert(options != NULL);
  assert(preprocess_options != NULL);

  preprocess_options->include_directories = options->include_directories;
  preprocess_options->include_directory_count = options->include_directory_count;
  preprocess_options->sysroot_directory = options->sysroot_path;
}

static bool fcc_driver_validate_options(const FccDriverOptions* options, FILE* error_stream) {
  assert(options != NULL);
  assert(error_stream != NULL);

  if (options->output_path != NULL && (options->stage != FCC_DRIVER_STAGE_PREPROCESS) &&
      (options->stage != FCC_DRIVER_STAGE_EMIT_ASM) &&
      (options->stage != FCC_DRIVER_STAGE_EMIT_OBJECT) &&
      (options->stage != FCC_DRIVER_STAGE_COMPILE_AND_LINK)) {
    fprintf(error_stream, "fcc: --output is only valid with --preprocess, --emit-asm, "
                          "--emit-obj, or --compile-and-link\n");
    return false;
  }

  if (options->should_use_asm_extension && (options->output_path != NULL)) {
    fprintf(error_stream,
            "fcc: --asm-extension cannot be combined with an explicit --output path\n");
    return false;
  }

  if (options->should_use_asm_extension && (options->stage != FCC_DRIVER_STAGE_EMIT_ASM)) {
    fprintf(error_stream, "fcc: --asm-extension is only valid with --emit-asm\n");
    return false;
  }

  if (options->should_dump_ast && (options->stage == FCC_DRIVER_STAGE_LEX_ONLY)) {
    fprintf(error_stream, "fcc: --dump-ast cannot be combined with --lex-only\n");
    return false;
  }

  if ((options->stage == FCC_DRIVER_STAGE_PREPROCESS) &&
      (options->should_dump_tokens || options->should_dump_ast)) {
    fprintf(error_stream,
            "fcc: --preprocess cannot be combined with --dump-tokens or --dump-ast\n");
    return false;
  }

  return true;
}

static bool fcc_driver_add_include_directory(FccDriverOptions* options, const char* path,
                                             FILE* error_stream) {
  assert(options != NULL);
  assert(path != NULL);
  assert(error_stream != NULL);

  if (*path == '\0') {
    fprintf(error_stream, "fcc: -I requires a non-empty path\n");
    return false;
  }

  if (options->include_directory_count >= FCC_MAX_INCLUDE_DIRECTORIES) {
    fprintf(error_stream, "fcc: include directory count exceeds FCC_MAX_INCLUDE_DIRECTORIES\n");
    return false;
  }

  options->include_directories[options->include_directory_count] = path;
  ++options->include_directory_count;
  return true;
}

static bool fcc_driver_parse_arguments(int argc, const char* const* argv, FccDriverOptions* options,
                                       FILE* error_stream) {
  int argument_index;

  assert(argv != NULL);
  assert(options != NULL);
  assert(error_stream != NULL);

  options->output_path = NULL;
  options->input_path = NULL;
  options->sysroot_path = NULL;
  options->include_directory_count = 0;
  options->stage = FCC_DRIVER_STAGE_CHECK;
  options->has_explicit_stage = false;
  options->should_dump_tokens = false;
  options->should_dump_ast = false;
  options->should_show_help = false;
  options->should_show_version = false;
  options->should_use_asm_extension = false;

  for (argument_index = 1; argument_index < argc; ++argument_index) {
    const char* argument;

    argument = argv[argument_index];
    if (fcc_driver_is_help_argument(argument)) {
      options->should_show_help = true;
      continue;
    }

    if (fcc_driver_is_version_argument(argument)) {
      options->should_show_version = true;
      continue;
    }

    if (fcc_driver_is_dump_tokens_argument(argument)) {
      options->should_dump_tokens = true;
      continue;
    }

    if (fcc_driver_is_dump_ast_argument(argument)) {
      options->should_dump_ast = true;
      continue;
    }

    if (fcc_driver_is_lex_only_argument(argument)) {
      if (!fcc_driver_select_stage(options, FCC_DRIVER_STAGE_LEX_ONLY, argument, error_stream)) {
        return false;
      }

      continue;
    }

    if (fcc_driver_is_parse_only_argument(argument)) {
      if (!fcc_driver_select_stage(options, FCC_DRIVER_STAGE_PARSE_ONLY, argument, error_stream)) {
        return false;
      }

      continue;
    }

    if (fcc_driver_is_check_argument(argument)) {
      if (!fcc_driver_select_stage(options, FCC_DRIVER_STAGE_CHECK, argument, error_stream)) {
        return false;
      }

      continue;
    }

    if (fcc_driver_is_preprocess_argument(argument)) {
      if (!fcc_driver_select_stage(options, FCC_DRIVER_STAGE_PREPROCESS, argument, error_stream)) {
        return false;
      }

      continue;
    }

    if (fcc_driver_is_emit_asm_argument(argument)) {
      if (!fcc_driver_select_stage(options, FCC_DRIVER_STAGE_EMIT_ASM, argument, error_stream)) {
        return false;
      }

      continue;
    }

    if (fcc_driver_is_emit_obj_argument(argument)) {
      if (!fcc_driver_select_stage(options, FCC_DRIVER_STAGE_EMIT_OBJECT, argument, error_stream)) {
        return false;
      }

      continue;
    }

    if (fcc_driver_is_compile_and_link_argument(argument)) {
      if (!fcc_driver_select_stage(options, FCC_DRIVER_STAGE_COMPILE_AND_LINK, argument,
                                   error_stream)) {
        return false;
      }

      continue;
    }

    if (fcc_driver_is_output_argument(argument)) {
      ++argument_index;
      if (argument_index >= argc) {
        fprintf(error_stream, "fcc: --output requires a path argument\n");
        return false;
      }

      options->output_path = argv[argument_index];
      continue;
    }

    if (fcc_driver_is_sysroot_argument(argument)) {
      ++argument_index;
      if (argument_index >= argc) {
        fprintf(error_stream, "fcc: --sysroot requires a path argument\n");
        return false;
      }

      options->sysroot_path = argv[argument_index];
      continue;
    }

    if (fcc_driver_is_include_directory_argument(argument)) {
      ++argument_index;
      if (argument_index >= argc) {
        fprintf(error_stream, "fcc: -I requires a path argument\n");
        return false;
      }

      if (!fcc_driver_add_include_directory(options, argv[argument_index], error_stream)) {
        return false;
      }

      continue;
    }

    if ((argument[0] == '-') && (argument[1] == 'I') && (argument[2] != '\0')) {
      if (!fcc_driver_add_include_directory(options, argument + 2, error_stream)) {
        return false;
      }

      continue;
    }

    if (strncmp(argument, "--asm-extension=", 16) == 0) {
      if (!fcc_driver_parse_asm_extension_argument(argument, &options->should_use_asm_extension)) {
        fprintf(error_stream, "fcc: unsupported asm extension '%s'\n", argument + 16);
        return false;
      }

      continue;
    }

    if ((argument[0] == '-') && (argument[1] == '-')) {
      fprintf(error_stream, "fcc: unknown option '%s'\n", argument);
      return false;
    }

    if (options->input_path != NULL) {
      fprintf(error_stream, "fcc: expected exactly one input file\n");
      return false;
    }

    options->input_path = argument;
  }

  return fcc_driver_validate_options(options, error_stream);
}

static bool fcc_driver_emit_assembly_file(const FccSourceFile* source_file,
                                          const FccAstTranslationUnit* translation_unit,
                                          FccSemaResult* sema_result, FccDiagnostics* diagnostics,
                                          const char* output_path) {
  FILE* assembly_stream;
  FccSourceLocation location;

  assert(source_file != NULL);
  assert(translation_unit != NULL);
  assert(diagnostics != NULL);
  assert(output_path != NULL);

  assembly_stream = NULL;
  assembly_stream = fopen(output_path, "wb");
  if (assembly_stream == NULL) {
    location.offset = 0;
    location.line = 1;
    location.column = 1;
    fcc_diag_emit(diagnostics, output_path, location, FCC_DIAG_SEVERITY_ERROR,
                  "failed to open assembly output file");
    return false;
  }

  if (!fcc_codegen_emit_nasm_x64_with_sema(assembly_stream, source_file, translation_unit,
                                           sema_result, diagnostics)) {
    (void)fclose(assembly_stream);
    return false;
  }

  if (fclose(assembly_stream) != 0) {
    location.offset = 0;
    location.line = 1;
    location.column = 1;
    fcc_diag_emit(diagnostics, output_path, location, FCC_DIAG_SEVERITY_ERROR,
                  "failed to close assembly output file");
    return false;
  }

  return true;
}

static int fcc_driver_preprocess_source(const FccDriverOptions* options,
                                        const FccSourceFile* source_file,
                                        FccDiagnostics* diagnostics, FILE* output_stream,
                                        FILE* error_stream) {
  FccPreprocessorOptions preprocess_options;
  FILE* preprocess_stream;

  assert(options != NULL);
  assert(source_file != NULL);
  assert(diagnostics != NULL);
  assert(output_stream != NULL);
  assert(error_stream != NULL);

  fcc_driver_build_preprocessor_options(options, &preprocess_options);
  preprocess_stream = output_stream;
  if (options->output_path != NULL) {
    preprocess_stream = fopen(options->output_path, "wb");
    if (preprocess_stream == NULL) {
      fprintf(error_stream, "fcc: failed to open '%s' for writing\n", options->output_path);
      return 1;
    }
  }

  if (!fcc_preprocessor_run(source_file, &preprocess_options, diagnostics, preprocess_stream)) {
    if (preprocess_stream != output_stream) {
      (void)fclose(preprocess_stream);
    }

    return (int)diagnostics->error_count;
  }

  if (preprocess_stream != output_stream) {
    if (fclose(preprocess_stream) != 0) {
      fprintf(error_stream, "fcc: failed to close '%s'\n", options->output_path);
      return 1;
    }
  }

  return (int)diagnostics->error_count;
}

static int fcc_driver_compile_and_link(const FccDriverOptions* options,
                                       const FccSourceFile* source_file,
                                       const FccAstTranslationUnit* translation_unit,
                                       FccSemaResult* sema_result, FccDiagnostics* diagnostics,
                                       FILE* error_stream) {
  FccLinkRequest link_request;
  char asm_path[FCC_MAX_PATH_LENGTH];
  char exe_path[FCC_MAX_PATH_LENGTH];
  bool cleanup_asm;
  int exit_code;

  assert(options != NULL);
  assert(source_file != NULL);
  assert(translation_unit != NULL);
  assert(diagnostics != NULL);
  assert(error_stream != NULL);

  cleanup_asm = false;
  exit_code = 1;
  if (!fcc_driver_build_output_path(options, ".exe", exe_path, sizeof(exe_path), error_stream)) {
    return 1;
  }

  if (!fcc_driver_replace_extension(exe_path, ".fcc_stage.s", asm_path, sizeof(asm_path),
                                    error_stream)) {
    return 1;
  }

  if (!fcc_driver_emit_assembly_file(source_file, translation_unit, sema_result, diagnostics,
                                     asm_path)) {
    return (int)diagnostics->error_count;
  }

  cleanup_asm = true;
  link_request.assembly_path = asm_path;
  link_request.output_path = exe_path;
  link_request.delete_temporary_files = true;
  if (!fcc_link_assemble_and_link_nasm_internal(&link_request, error_stream)) {
    goto cleanup;
  }

  exit_code = 0;

cleanup:
  if (cleanup_asm) {
    (void)remove(asm_path);
  }

  return exit_code;
}

static int fcc_driver_emit_object_file(const FccDriverOptions* options,
                                       const FccSourceFile* source_file,
                                       const FccAstTranslationUnit* translation_unit,
                                       FccSemaResult* sema_result, FccDiagnostics* diagnostics,
                                       FILE* error_stream) {
  char asm_path[FCC_MAX_PATH_LENGTH];
  char object_path[FCC_MAX_PATH_LENGTH];
  bool cleanup_asm;
  int exit_code;

  assert(options != NULL);
  assert(source_file != NULL);
  assert(translation_unit != NULL);
  assert(diagnostics != NULL);
  assert(error_stream != NULL);

  cleanup_asm = false;
  exit_code = 1;
  if (!fcc_driver_build_output_path(options, ".obj", object_path, sizeof(object_path),
                                    error_stream)) {
    return 1;
  }

  if (!fcc_driver_replace_extension(object_path, ".fcc_stage.s", asm_path, sizeof(asm_path),
                                    error_stream)) {
    return 1;
  }

  if (!fcc_driver_emit_assembly_file(source_file, translation_unit, sema_result, diagnostics,
                                     asm_path)) {
    return (int)diagnostics->error_count;
  }

  cleanup_asm = true;
  if (!fcc_link_assemble_nasm_to_object(asm_path, object_path, error_stream)) {
    goto cleanup;
  }

  exit_code = 0;

cleanup:
  if (cleanup_asm) {
    (void)remove(asm_path);
  }

  return exit_code;
}

static int fcc_driver_process_source(const FccDriverOptions* options, FILE* output_stream,
                                     FILE* error_stream) {
  FccAstContext ast_context;
  FccAstTranslationUnit* translation_unit;
  FccDiagnostics diagnostics;
  FccLexer lexer;
  FccPreprocessorOptions preprocess_options;
  FccSemaResult sema_result;
  FccSourceFile preprocessed_source_file;
  FccSourceFile source_file;
  const FccSourceFile* active_source_file;
  FccSourceLocation location;
  FccToken token;
  char error_buffer[FCC_MAX_DIAG_MESSAGE_LENGTH];
  char output_path[FCC_MAX_PATH_LENGTH];
  int exit_code;

  assert(options != NULL);
  assert(options->input_path != NULL);
  assert(output_stream != NULL);
  assert(error_stream != NULL);

  fcc_diag_init(&diagnostics, error_stream);
  fcc_ast_context_init(&ast_context);
  fcc_sema_result_init(&sema_result);
  translation_unit = NULL;
  exit_code = 0;
  memset(&source_file, 0, sizeof(source_file));
  memset(&preprocessed_source_file, 0, sizeof(preprocessed_source_file));
  active_source_file = &source_file;
  if (!fcc_source_file_load(&source_file, options->input_path, error_buffer,
                            sizeof(error_buffer))) {
    location.offset = 0;
    location.line = 1;
    location.column = 1;
    fcc_diag_emit(&diagnostics, options->input_path, location, FCC_DIAG_SEVERITY_ERROR,
                  error_buffer);
    exit_code = (int)diagnostics.error_count;
    goto cleanup;
  }

  if (options->stage == FCC_DRIVER_STAGE_PREPROCESS) {
    exit_code = fcc_driver_preprocess_source(options, &source_file, &diagnostics, output_stream,
                                             error_stream);
    goto cleanup;
  }

  fcc_driver_build_preprocessor_options(options, &preprocess_options);
  if (!fcc_preprocessor_run_to_source(&source_file, &preprocess_options, &diagnostics,
                                      &preprocessed_source_file, error_buffer,
                                      sizeof(error_buffer))) {
    if (diagnostics.error_count == 0) {
      exit_code = 1;
      goto cleanup;
    }

    exit_code = (int)diagnostics.error_count;
    goto cleanup;
  }

  active_source_file = &preprocessed_source_file;
  if (options->should_dump_tokens || (options->stage == FCC_DRIVER_STAGE_LEX_ONLY)) {
    fcc_lexer_init(&lexer, active_source_file, &diagnostics);
    do {
      fcc_lexer_next(&lexer, &token);
      if (options->should_dump_tokens) {
        fcc_token_dump_source(output_stream, &token, active_source_file);
      }
    } while (token.kind != FCC_TOKEN_END_OF_FILE);

    if ((diagnostics.error_count != 0) || (options->stage == FCC_DRIVER_STAGE_LEX_ONLY)) {
      exit_code = (int)diagnostics.error_count;
      goto cleanup;
    }
  }

  if (!fcc_parser_parse_translation_unit(active_source_file, &diagnostics, &ast_context,
                                         &translation_unit)) {
    exit_code = (int)diagnostics.error_count;
    goto cleanup;
  }

  if (options->should_dump_ast) {
    fcc_ast_dump_translation_unit(output_stream, translation_unit);
  }

  if ((diagnostics.error_count != 0) || (options->stage == FCC_DRIVER_STAGE_PARSE_ONLY)) {
    exit_code = (int)diagnostics.error_count;
    goto cleanup;
  }

  if (!fcc_sema_analyze_translation_unit(active_source_file, translation_unit, &diagnostics,
                                         &sema_result)) {
    exit_code = (int)diagnostics.error_count;
    goto cleanup;
  }

  if ((diagnostics.error_count != 0) || (options->stage == FCC_DRIVER_STAGE_CHECK)) {
    exit_code = (int)diagnostics.error_count;
    goto cleanup;
  }

  if (options->stage == FCC_DRIVER_STAGE_EMIT_ASM) {
    if (!fcc_driver_build_output_path(options, options->should_use_asm_extension ? ".asm" : ".s",
                                      output_path, sizeof(output_path), error_stream)) {
      exit_code = 1;
      goto cleanup;
    }

    if (!fcc_driver_emit_assembly_file(active_source_file, translation_unit, &sema_result,
                                       &diagnostics, output_path)) {
      exit_code = (int)diagnostics.error_count;
      goto cleanup;
    }

    exit_code = 0;
    goto cleanup;
  }

  if (options->stage == FCC_DRIVER_STAGE_EMIT_OBJECT) {
    exit_code = fcc_driver_emit_object_file(options, active_source_file, translation_unit,
                                            &sema_result, &diagnostics, error_stream);
    goto cleanup;
  }

  if (options->stage == FCC_DRIVER_STAGE_COMPILE_AND_LINK) {
    exit_code = fcc_driver_compile_and_link(options, active_source_file, translation_unit,
                                            &sema_result, &diagnostics, error_stream);
    goto cleanup;
  }

  exit_code = (int)diagnostics.error_count;

cleanup:
  fcc_source_file_dispose(&source_file);
  fcc_source_file_dispose(&preprocessed_source_file);
  fcc_sema_result_dispose(&sema_result);
  fcc_ast_context_dispose(&ast_context);
  return exit_code;
}

int fcc_driver_run(int argc, const char* const* argv, FILE* output_stream, FILE* error_stream) {
  FccDriverOptions options;

  assert(argv != NULL);

  if (output_stream == NULL) {
    output_stream = stdout;
  }

  if (error_stream == NULL) {
    error_stream = stderr;
  }

  if (!fcc_driver_parse_arguments(argc, argv, &options, error_stream)) {
    return 1;
  }

  if (options.should_show_help) {
    fcc_driver_print_usage(output_stream);
    return 0;
  }

  if (options.should_show_version) {
    fcc_signature_print_version(output_stream);
    return 0;
  }

  if (options.input_path == NULL) {
    fprintf(error_stream, "fcc: expected exactly one input file, --help, or --version\n");
    return 1;
  }

  return fcc_driver_process_source(&options, output_stream, error_stream);
}
