// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/diag.h"

#include <assert.h>

static FILE* fcc_diag_resolve_stream(FILE* stream) {
  if (stream != NULL) {
    return stream;
  }

  return stderr;
}

static void fcc_diag_record_severity(FccDiagnostics* diagnostics, FccDiagSeverity severity) {
  assert(diagnostics != NULL);

  if (severity == FCC_DIAG_SEVERITY_WARNING) {
    ++diagnostics->warning_count;
  }

  if ((severity == FCC_DIAG_SEVERITY_ERROR) || (severity == FCC_DIAG_SEVERITY_FATAL)) {
    ++diagnostics->error_count;
  }
}

void fcc_diag_init(FccDiagnostics* diagnostics, FILE* stream) {
  assert(diagnostics != NULL);

  diagnostics->stream = fcc_diag_resolve_stream(stream);
  diagnostics->warning_count = 0;
  diagnostics->error_count = 0;
}

const char* fcc_diag_severity_name(FccDiagSeverity severity) {
  switch (severity) {
    case FCC_DIAG_SEVERITY_NOTE:
      return "note";
    case FCC_DIAG_SEVERITY_WARNING:
      return "warning";
    case FCC_DIAG_SEVERITY_ERROR:
      return "error";
    case FCC_DIAG_SEVERITY_FATAL:
      return "fatal";
  }

  return "unknown";
}

void fcc_diag_emit(FccDiagnostics* diagnostics, const char* path, FccSourceLocation location,
                   FccDiagSeverity severity, const char* message) {
  const char* printed_path;
  const char* printed_message;

  assert(diagnostics != NULL);

  printed_path = path;
  if (printed_path == NULL) {
    printed_path = "<unknown>";
  }

  printed_message = message;
  if (printed_message == NULL) {
    printed_message = "unspecified diagnostic";
  }

  fprintf(diagnostics->stream, "%s(%zu,%zu): %s: %s\n", printed_path, location.line,
          location.column, fcc_diag_severity_name(severity), printed_message);
  fcc_diag_record_severity(diagnostics, severity);
}

void fcc_diag_emit_source(FccDiagnostics* diagnostics, const FccSourceFile* source_file,
                          FccSourceSpan span, FccDiagSeverity severity, const char* message) {
  FccSourceLocation location;

  assert(diagnostics != NULL);

  if (source_file == NULL) {
    location.offset = 0;
    location.line = 1;
    location.column = 1;
    fcc_diag_emit(diagnostics, NULL, location, severity, message);
    return;
  }

  location = fcc_source_offset_to_location(source_file, span.begin_offset);
  fcc_diag_emit(diagnostics, source_file->path, location, severity, message);
}
