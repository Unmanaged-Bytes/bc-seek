// SPDX-License-Identifier: MIT

#include "bc_allocators.h"
#include "bc_runtime.h"
#include "bc_runtime_cli.h"
#include "bc_seek_cli_internal.h"

#include <bc_core_parse.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_CLI_MAX_ARGS 16
#define FUZZ_CLI_MAX_ARG_LENGTH 64
#define FUZZ_CLI_BUFFER_SIZE (FUZZ_CLI_MAX_ARGS * FUZZ_CLI_MAX_ARG_LENGTH)

static size_t build_argv_from_fuzz_buffer(const uint8_t *data, size_t size,
                                          char *buffer, const char **argv,
                                          size_t argv_capacity) {
  argv[0] = "bc-seek";
  size_t argument_count = 1;
  size_t buffer_position = 0;
  size_t data_position = 0;

  while (data_position < size && argument_count < argv_capacity &&
         buffer_position < FUZZ_CLI_BUFFER_SIZE - 1) {
    const size_t argument_start = buffer_position;
    while (data_position < size && buffer_position < FUZZ_CLI_BUFFER_SIZE - 1) {
      const uint8_t byte = data[data_position++];
      if (byte == 0) {
        break;
      }
      buffer[buffer_position++] = (char)byte;
      if (buffer_position - argument_start >= FUZZ_CLI_MAX_ARG_LENGTH - 1) {
        break;
      }
    }
    buffer[buffer_position++] = '\0';
    argv[argument_count++] = &buffer[argument_start];
  }
  return argument_count;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char argv_buffer[FUZZ_CLI_BUFFER_SIZE];
  const char *argv[FUZZ_CLI_MAX_ARGS];
  const size_t argument_count = build_argv_from_fuzz_buffer(
      data, size, argv_buffer, argv, FUZZ_CLI_MAX_ARGS);

  bc_allocators_context_t *memory_context = NULL;
  if (!bc_allocators_context_create(NULL, &memory_context)) {
    return 0;
  }
  bc_runtime_config_store_t *config_store = NULL;
  if (!bc_runtime_config_store_create(memory_context, &config_store)) {
    bc_allocators_context_destroy(memory_context);
    return 0;
  }

  const bc_runtime_cli_program_spec_t *spec = bc_seek_cli_program_spec();
  bc_runtime_cli_parsed_t parsed = {0};
  FILE *error_stream = fopen("/dev/null", "w");
  bc_runtime_cli_parse_status_t status = bc_runtime_cli_parse(
      spec, (int)argument_count, argv, config_store, &parsed, error_stream);

  if (status == BC_RUNTIME_CLI_PARSE_OK) {
    bc_seek_cli_options_t options = {0};
    (void)bc_seek_cli_bind_options(config_store, &parsed, &options);
  }

  if (error_stream != NULL) {
    fclose(error_stream);
  }
  bc_runtime_config_store_destroy(memory_context, config_store);
  bc_allocators_context_destroy(memory_context);
  return 0;
}

#ifndef BC_FUZZ_LIBFUZZER
int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <iterations> [seed]\n", argv[0]);
    return 2;
  }
  uint64_t iterations = 0;
  size_t consumed = 0;
  const size_t iterations_length = strlen(argv[1]);
  if (!bc_core_parse_unsigned_integer_64_decimal(argv[1], iterations_length,
                                                 &iterations, &consumed) ||
      consumed != iterations_length) {
    fprintf(stderr, "invalid iterations: %s\n", argv[1]);
    return 2;
  }
  uint64_t seed = 0;
  if (argc >= 3) {
    const size_t seed_length = strlen(argv[2]);
    if (!bc_core_parse_unsigned_integer_64_decimal(argv[2], seed_length, &seed,
                                                   &consumed) ||
        consumed != seed_length) {
      fprintf(stderr, "invalid seed: %s\n", argv[2]);
      return 2;
    }
  }
  srand((unsigned int)seed);

  uint8_t buffer[2048];
  for (uint64_t i = 0; i < iterations; i++) {
    const size_t length = (size_t)(rand() % (int)sizeof(buffer));
    for (size_t j = 0; j < length; j++) {
      buffer[j] = (uint8_t)(rand() & 0xFF);
    }
    LLVMFuzzerTestOneInput(buffer, length);
  }
  return 0;
}
#endif
