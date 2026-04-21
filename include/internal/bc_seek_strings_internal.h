// SPDX-License-Identifier: MIT

#ifndef BC_SEEK_STRINGS_INTERNAL_H
#define BC_SEEK_STRINGS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

bool bc_seek_strings_equal(const char* left, const char* right);

size_t bc_seek_strings_length(const char* value);

#endif /* BC_SEEK_STRINGS_INTERNAL_H */
