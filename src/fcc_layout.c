// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/layout.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool fcc_layout_is_power_of_two(size_t value) {
  return (value != 0) && ((value & (value - 1)) == 0);
}

static bool fcc_layout_align_up(size_t value, size_t alignment, size_t* aligned_out) {
  size_t remainder;
  size_t padding;

  assert(aligned_out != NULL);

  if (!fcc_layout_is_power_of_two(alignment)) {
    return false;
  }

  remainder = value & (alignment - 1);
  if (remainder == 0) {
    *aligned_out = value;
    return true;
  }

  padding = alignment - remainder;
  if (value > (SIZE_MAX - padding)) {
    return false;
  }

  *aligned_out = value + padding;
  return true;
}

static bool fcc_layout_validate_members(const FccLayoutMember* members, size_t member_count) {
  size_t member_index;

  assert((members != NULL) || (member_count == 0));

  for (member_index = 0; member_index < member_count; ++member_index) {
    if (!fcc_layout_is_power_of_two(members[member_index].alignment)) {
      return false;
    }
  }

  return true;
}

bool fcc_layout_compute_struct(FccLayoutMember* members, size_t member_count, size_t* size_out,
                               size_t* alignment_out) {
  size_t current_offset;
  size_t max_alignment;
  size_t member_index;

  assert(size_out != NULL);
  assert(alignment_out != NULL);
  assert((members != NULL) || (member_count == 0));

  if (!fcc_layout_validate_members(members, member_count)) {
    return false;
  }

  current_offset = 0;
  max_alignment = 1;
  for (member_index = 0; member_index < member_count; ++member_index) {
    size_t aligned_offset;

    if (!fcc_layout_align_up(current_offset, members[member_index].alignment, &aligned_offset)) {
      return false;
    }

    members[member_index].offset = aligned_offset;
    if (members[member_index].size > (SIZE_MAX - aligned_offset)) {
      return false;
    }

    current_offset = aligned_offset + members[member_index].size;
    if (members[member_index].alignment > max_alignment) {
      max_alignment = members[member_index].alignment;
    }
  }

  if (!fcc_layout_align_up(current_offset, max_alignment, &current_offset)) {
    return false;
  }

  *size_out = current_offset;
  *alignment_out = max_alignment;
  return true;
}

bool fcc_layout_compute_union(FccLayoutMember* members, size_t member_count, size_t* size_out,
                              size_t* alignment_out) {
  size_t max_size;
  size_t max_alignment;
  size_t member_index;

  assert(size_out != NULL);
  assert(alignment_out != NULL);
  assert((members != NULL) || (member_count == 0));

  if (!fcc_layout_validate_members(members, member_count)) {
    return false;
  }

  max_size = 0;
  max_alignment = 1;
  for (member_index = 0; member_index < member_count; ++member_index) {
    members[member_index].offset = 0;
    if (members[member_index].size > max_size) {
      max_size = members[member_index].size;
    }

    if (members[member_index].alignment > max_alignment) {
      max_alignment = members[member_index].alignment;
    }
  }

  if (!fcc_layout_align_up(max_size, max_alignment, &max_size)) {
    return false;
  }

  *size_out = max_size;
  *alignment_out = max_alignment;
  return true;
}
