// SPDX-License-Identifier: MIT

#include "bc_seek_cli_internal.h"
#include "bc_seek_strings_internal.h"

#include <bc_core_parse.h>

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool bc_seek_cli_parse_unsigned(const char* value, size_t length, uint64_t* out_value)
{
    if (length == 0) {
        return false;
    }
    uint64_t accumulator = 0;
    for (size_t index = 0; index < length; index++) {
        char character = value[index];
        if (character < '0' || character > '9') {
            return false;
        }
        uint64_t digit = (uint64_t)(character - '0');
        if (accumulator > (UINT64_MAX - digit) / 10u) {
            return false;
        }
        accumulator = accumulator * 10u + digit;
    }
    *out_value = accumulator;
    return true;
}

bool bc_seek_cli_parse_type_filter(const char* value, bc_seek_entry_type_t* out_type)
{
    if (bc_seek_strings_equal(value, "f") || bc_seek_strings_equal(value, "file")) {
        *out_type = BC_SEEK_ENTRY_TYPE_FILE;
        return true;
    }
    if (bc_seek_strings_equal(value, "d") || bc_seek_strings_equal(value, "dir")) {
        *out_type = BC_SEEK_ENTRY_TYPE_DIRECTORY;
        return true;
    }
    if (bc_seek_strings_equal(value, "l") || bc_seek_strings_equal(value, "link") || bc_seek_strings_equal(value, "symlink")) {
        *out_type = BC_SEEK_ENTRY_TYPE_SYMLINK;
        return true;
    }
    return false;
}

bool bc_seek_cli_parse_size_filter(const char* value, bc_seek_compare_op_t* out_op, uint64_t* out_threshold_bytes)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    const char* cursor = value;
    bc_seek_compare_op_t op = BC_SEEK_COMPARE_EQUAL;
    if (cursor[0] == '+') {
        op = BC_SEEK_COMPARE_GREATER;
        cursor += 1;
    } else if (cursor[0] == '-') {
        op = BC_SEEK_COMPARE_LESS;
        cursor += 1;
    }

    size_t cursor_length = bc_seek_strings_length(cursor);
    if (cursor_length == 0) {
        return false;
    }

    char suffix = cursor[cursor_length - 1];
    uint64_t multiplier = 512u;
    size_t digits_length = cursor_length;

    switch (suffix) {
    case 'c':
        multiplier = 1u;
        digits_length -= 1;
        break;
    case 'w':
        multiplier = 2u;
        digits_length -= 1;
        break;
    case 'b':
        multiplier = 512u;
        digits_length -= 1;
        break;
    case 'k':
    case 'K':
        multiplier = 1024u;
        digits_length -= 1;
        break;
    case 'M':
        multiplier = 1024u * 1024u;
        digits_length -= 1;
        break;
    case 'G':
        multiplier = 1024u * 1024u * 1024u;
        digits_length -= 1;
        break;
    case 'T':
        multiplier = 1024ull * 1024ull * 1024ull * 1024ull;
        digits_length -= 1;
        break;
    default:
        if (suffix < '0' || suffix > '9') {
            return false;
        }
        multiplier = 512u;
        break;
    }

    uint64_t numeric_value = 0;
    if (!bc_seek_cli_parse_unsigned(cursor, digits_length, &numeric_value)) {
        return false;
    }
    if (multiplier != 0u && numeric_value > UINT64_MAX / multiplier) {
        return false;
    }
    *out_op = op;
    *out_threshold_bytes = numeric_value * multiplier;
    return true;
}

bool bc_seek_cli_parse_mtime_filter(const char* value, bc_seek_compare_op_t* out_op, int64_t* out_days)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    const char* cursor = value;
    bc_seek_compare_op_t op = BC_SEEK_COMPARE_EQUAL;
    if (cursor[0] == '+') {
        op = BC_SEEK_COMPARE_GREATER;
        cursor += 1;
    } else if (cursor[0] == '-') {
        op = BC_SEEK_COMPARE_LESS;
        cursor += 1;
    }
    size_t cursor_length = bc_seek_strings_length(cursor);
    if (cursor_length == 0) {
        return false;
    }
    uint64_t numeric_value = 0;
    if (!bc_seek_cli_parse_unsigned(cursor, cursor_length, &numeric_value)) {
        return false;
    }
    if (numeric_value > (uint64_t)INT64_MAX) {
        return false;
    }
    *out_op = op;
    *out_days = (int64_t)numeric_value;
    return true;
}

bool bc_seek_cli_parse_perm_filter(const char* value, unsigned int* out_mask)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    size_t length = bc_seek_strings_length(value);
    unsigned int mask = 0u;
    for (size_t index = 0; index < length; index++) {
        char character = value[index];
        if (character < '0' || character > '7') {
            return false;
        }
        if (mask > (UINT_MAX >> 3)) {
            return false;
        }
        mask = (mask << 3) | (unsigned int)(character - '0');
    }
    if (mask > 07777u) {
        return false;
    }
    *out_mask = mask;
    return true;
}

bool bc_seek_cli_parse_threads(const char* value, bc_seek_threads_mode_t* out_mode, size_t* out_worker_count)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    if (bc_seek_strings_equal(value, "mono")) {
        *out_mode = BC_SEEK_THREADS_MODE_MONO;
        *out_worker_count = 0;
        return true;
    }
    if (bc_seek_strings_equal(value, "auto")) {
        *out_mode = BC_SEEK_THREADS_MODE_AUTO;
        *out_worker_count = 0;
        return true;
    }
    if (bc_seek_strings_equal(value, "io")) {
        *out_mode = BC_SEEK_THREADS_MODE_IO;
        *out_worker_count = 0;
        return true;
    }
    const size_t value_length = bc_seek_strings_length(value);
    if (value_length == 0) {
        return false;
    }
    uint64_t parsed_value = 0;
    size_t consumed = 0;
    if (!bc_core_parse_unsigned_integer_64_decimal(value, value_length, &parsed_value, &consumed)) {
        return false;
    }
    if (consumed != value_length) {
        return false;
    }
    if (parsed_value == 0u) {
        *out_mode = BC_SEEK_THREADS_MODE_MONO;
        *out_worker_count = 0;
        return true;
    }
    *out_mode = BC_SEEK_THREADS_MODE_EXPLICIT;
    *out_worker_count = (size_t)parsed_value;
    return true;
}
