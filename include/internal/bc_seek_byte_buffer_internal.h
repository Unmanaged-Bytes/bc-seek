// SPDX-License-Identifier: MIT

#ifndef BC_SEEK_BYTE_BUFFER_INTERNAL_H
#define BC_SEEK_BYTE_BUFFER_INTERNAL_H

#include "bc_allocators.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct bc_seek_byte_buffer {
    char* data;
    size_t length;
    size_t capacity;
} bc_seek_byte_buffer_t;

bool bc_seek_byte_buffer_reserve(bc_allocators_context_t* memory_context, bc_seek_byte_buffer_t* buffer, size_t requested_capacity);

bool bc_seek_byte_buffer_append(bc_allocators_context_t* memory_context, bc_seek_byte_buffer_t* buffer, const char* bytes, size_t length);

bool bc_seek_byte_buffer_append_byte(bc_allocators_context_t* memory_context, bc_seek_byte_buffer_t* buffer, char value);

void bc_seek_byte_buffer_destroy(bc_allocators_context_t* memory_context, bc_seek_byte_buffer_t* buffer);

#endif /* BC_SEEK_BYTE_BUFFER_INTERNAL_H */
