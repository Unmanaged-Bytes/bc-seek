// SPDX-License-Identifier: MIT

#include "bc_seek_strings_internal.h"

#include "bc_core_memory.h"

bool bc_seek_strings_equal(const char *left, const char *right) {
  size_t left_length = 0;
  size_t right_length = 0;
  if (!bc_core_length(left, '\0', &left_length) ||
      !bc_core_length(right, '\0', &right_length)) {
    return false;
  }
  if (left_length != right_length) {
    return false;
  }
  bool result = false;
  if (!bc_core_equal(left, right, left_length, &result)) {
    return false;
  }
  return result;
}

size_t bc_seek_strings_length(const char *value) {
  size_t length = 0;
  (void)bc_core_length(value, '\0', &length);
  return length;
}
