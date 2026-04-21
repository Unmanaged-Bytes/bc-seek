// SPDX-License-Identifier: MIT

#include "bc_seek_output_internal.h"

#include "bc_core.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define BC_SEEK_OUTPUT_BUFFER_BYTES ((size_t)(64 * 1024))

static char bc_seek_output_buffer_stdout[BC_SEEK_OUTPUT_BUFFER_BYTES];

static bool bc_seek_output_attach(bc_seek_output_t* output, FILE* stream, bool null_terminated, bool owns_stream, char* buffer, size_t buffer_size)
{
    bc_core_zero(output, sizeof(*output));
    output->stream = stream;
    output->separator = null_terminated ? '\0' : '\n';
    output->owns_stream = owns_stream;
    output->emitted_count = 0;
    if (buffer != NULL && buffer_size > 0) {
        (void)setvbuf(stream, buffer, _IOFBF, buffer_size);
    }
    return true;
}

bool bc_seek_output_open_stdout(bool null_terminated, bc_seek_output_t* out_output)
{
    return bc_seek_output_attach(out_output, stdout, null_terminated, false, bc_seek_output_buffer_stdout, sizeof(bc_seek_output_buffer_stdout));
}

bool bc_seek_output_open_file(const char* path, bool null_terminated, bc_seek_output_t* out_output)
{
    FILE* stream = fopen(path, "w");
    if (stream == NULL) {
        return false;
    }
    return bc_seek_output_attach(out_output, stream, null_terminated, true, NULL, 0);
}

bool bc_seek_output_emit(bc_seek_output_t* output, const char* path, size_t path_length)
{
    if (fwrite(path, 1, path_length, output->stream) != path_length) {
        return false;
    }
    if (fputc((unsigned char)output->separator, output->stream) == EOF) {
        return false;
    }
    output->emitted_count += 1;
    return true;
}

bool bc_seek_output_close(bc_seek_output_t* output)
{
    if (output->stream == NULL) {
        return true;
    }
    bool ok = fflush(output->stream) == 0;
    if (output->owns_stream) {
        if (fclose(output->stream) != 0) {
            ok = false;
        }
    }
    output->stream = NULL;
    return ok;
}
