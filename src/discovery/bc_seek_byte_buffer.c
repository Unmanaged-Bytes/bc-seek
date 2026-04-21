// SPDX-License-Identifier: MIT

#include "bc_seek_byte_buffer_internal.h"

#include "bc_allocators.h"
#include "bc_allocators_pool.h"

#include <stdint.h>
#include <string.h>

#define BC_SEEK_BYTE_BUFFER_MIN_CAPACITY ((size_t)(64 * 1024))

bool bc_seek_byte_buffer_reserve(bc_allocators_context_t* memory_context, bc_seek_byte_buffer_t* buffer, size_t requested_capacity)
{
    if (requested_capacity <= buffer->capacity) {
        return true;
    }
    size_t new_capacity = buffer->capacity == 0 ? BC_SEEK_BYTE_BUFFER_MIN_CAPACITY : buffer->capacity;
    while (new_capacity < requested_capacity) {
        if (new_capacity > (SIZE_MAX / 2u)) {
            return false;
        }
        new_capacity *= 2u;
    }
    char* new_data = NULL;
    if (!bc_allocators_pool_allocate(memory_context, new_capacity, (void**)&new_data)) {
        return false;
    }
    if (buffer->length > 0) {
        memcpy(new_data, buffer->data, buffer->length);
    }
    if (buffer->data != NULL) {
        bc_allocators_pool_free(memory_context, buffer->data);
    }
    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return true;
}

bool bc_seek_byte_buffer_append(bc_allocators_context_t* memory_context, bc_seek_byte_buffer_t* buffer, const char* bytes, size_t length)
{
    if (length == 0u) {
        return true;
    }
    size_t required = buffer->length + length;
    if (required < buffer->length) {
        return false;
    }
    if (required > buffer->capacity) {
        if (!bc_seek_byte_buffer_reserve(memory_context, buffer, required)) {
            return false;
        }
    }
    memcpy(buffer->data + buffer->length, bytes, length);
    buffer->length = required;
    return true;
}

bool bc_seek_byte_buffer_append_byte(bc_allocators_context_t* memory_context, bc_seek_byte_buffer_t* buffer, char value)
{
    if (buffer->length == buffer->capacity) {
        if (!bc_seek_byte_buffer_reserve(memory_context, buffer, buffer->length + 1u)) {
            return false;
        }
    }
    buffer->data[buffer->length] = value;
    buffer->length += 1u;
    return true;
}

void bc_seek_byte_buffer_destroy(bc_allocators_context_t* memory_context, bc_seek_byte_buffer_t* buffer)
{
    if (buffer->data != NULL) {
        bc_allocators_pool_free(memory_context, buffer->data);
    }
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}
