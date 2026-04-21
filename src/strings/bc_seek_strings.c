// SPDX-License-Identifier: MIT

#include "bc_seek_strings_internal.h"

#include <string.h>

bool bc_seek_strings_equal(const char* left, const char* right)
{
    return strcmp(left, right) == 0;
}

size_t bc_seek_strings_length(const char* value)
{
    return strlen(value);
}

