// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct FccLayoutMember {
  size_t size;
  size_t alignment;
  size_t offset;
} FccLayoutMember;

bool fcc_layout_compute_struct(FccLayoutMember* members, size_t member_count, size_t* size_out,
                               size_t* alignment_out);

bool fcc_layout_compute_union(FccLayoutMember* members, size_t member_count, size_t* size_out,
                              size_t* alignment_out);
