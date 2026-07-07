// SPDX-License-Identifier: GPL-3.0-or-later

typedef struct SourceLocation {
  unsigned long offset;
  unsigned long line;
  unsigned long column;
} SourceLocation;

int preserve_register_after_aggregate(int first, int second, SourceLocation location,
                                      int severity) {
  return first + second + (int)location.line + severity;
}
