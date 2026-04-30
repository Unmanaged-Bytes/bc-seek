// SPDX-License-Identifier: MIT

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include <stdbool.h>
#include <stdint.h>

#include "bc_seek_cli_internal.h"

static void test_parse_type_filter_valid(void **state) {
  (void)state;
  bc_seek_entry_type_t out_type = BC_SEEK_ENTRY_TYPE_ANY;
  assert_true(bc_seek_cli_parse_type_filter("f", &out_type));
  assert_int_equal(out_type, BC_SEEK_ENTRY_TYPE_FILE);
  assert_true(bc_seek_cli_parse_type_filter("file", &out_type));
  assert_int_equal(out_type, BC_SEEK_ENTRY_TYPE_FILE);
  assert_true(bc_seek_cli_parse_type_filter("d", &out_type));
  assert_int_equal(out_type, BC_SEEK_ENTRY_TYPE_DIRECTORY);
  assert_true(bc_seek_cli_parse_type_filter("dir", &out_type));
  assert_int_equal(out_type, BC_SEEK_ENTRY_TYPE_DIRECTORY);
  assert_true(bc_seek_cli_parse_type_filter("l", &out_type));
  assert_int_equal(out_type, BC_SEEK_ENTRY_TYPE_SYMLINK);
  assert_true(bc_seek_cli_parse_type_filter("link", &out_type));
  assert_int_equal(out_type, BC_SEEK_ENTRY_TYPE_SYMLINK);
  assert_true(bc_seek_cli_parse_type_filter("symlink", &out_type));
  assert_int_equal(out_type, BC_SEEK_ENTRY_TYPE_SYMLINK);
}

static void test_parse_type_filter_invalid(void **state) {
  (void)state;
  bc_seek_entry_type_t out_type = BC_SEEK_ENTRY_TYPE_ANY;
  assert_false(bc_seek_cli_parse_type_filter("", &out_type));
  assert_false(bc_seek_cli_parse_type_filter("files", &out_type));
  assert_false(bc_seek_cli_parse_type_filter("directory", &out_type));
  assert_false(bc_seek_cli_parse_type_filter("b", &out_type));
}

static void test_parse_size_filter_default_block_unit(void **state) {
  (void)state;
  bc_seek_compare_op_t op = BC_SEEK_COMPARE_EQUAL;
  uint64_t threshold = 0;
  assert_true(bc_seek_cli_parse_size_filter("10", &op, &threshold));
  assert_int_equal(op, BC_SEEK_COMPARE_EQUAL);
  assert_int_equal(threshold, 10u * 512u);
}

static void test_parse_size_filter_suffixes(void **state) {
  (void)state;
  bc_seek_compare_op_t op = BC_SEEK_COMPARE_EQUAL;
  uint64_t threshold = 0;
  assert_true(bc_seek_cli_parse_size_filter("100c", &op, &threshold));
  assert_int_equal(threshold, 100u);
  assert_true(bc_seek_cli_parse_size_filter("1k", &op, &threshold));
  assert_int_equal(threshold, 1024u);
  assert_true(bc_seek_cli_parse_size_filter("2M", &op, &threshold));
  assert_int_equal(threshold, 2u * 1024u * 1024u);
  assert_true(bc_seek_cli_parse_size_filter("1G", &op, &threshold));
  assert_int_equal(threshold, 1024u * 1024u * 1024u);
}

static void test_parse_size_filter_operators(void **state) {
  (void)state;
  bc_seek_compare_op_t op = BC_SEEK_COMPARE_EQUAL;
  uint64_t threshold = 0;
  assert_true(bc_seek_cli_parse_size_filter("+1k", &op, &threshold));
  assert_int_equal(op, BC_SEEK_COMPARE_GREATER);
  assert_int_equal(threshold, 1024u);
  assert_true(bc_seek_cli_parse_size_filter("-1k", &op, &threshold));
  assert_int_equal(op, BC_SEEK_COMPARE_LESS);
  assert_int_equal(threshold, 1024u);
}

static void test_parse_size_filter_invalid(void **state) {
  (void)state;
  bc_seek_compare_op_t op = BC_SEEK_COMPARE_EQUAL;
  uint64_t threshold = 0;
  assert_false(bc_seek_cli_parse_size_filter("", &op, &threshold));
  assert_false(bc_seek_cli_parse_size_filter("+", &op, &threshold));
  assert_false(bc_seek_cli_parse_size_filter("abc", &op, &threshold));
  assert_false(bc_seek_cli_parse_size_filter("1Z", &op, &threshold));
}

static void test_parse_mtime_filter(void **state) {
  (void)state;
  bc_seek_compare_op_t op = BC_SEEK_COMPARE_EQUAL;
  int64_t days = 0;
  assert_true(bc_seek_cli_parse_mtime_filter("1", &op, &days));
  assert_int_equal(op, BC_SEEK_COMPARE_EQUAL);
  assert_int_equal(days, 1);
  assert_true(bc_seek_cli_parse_mtime_filter("+7", &op, &days));
  assert_int_equal(op, BC_SEEK_COMPARE_GREATER);
  assert_int_equal(days, 7);
  assert_true(bc_seek_cli_parse_mtime_filter("-30", &op, &days));
  assert_int_equal(op, BC_SEEK_COMPARE_LESS);
  assert_int_equal(days, 30);
  assert_false(bc_seek_cli_parse_mtime_filter("", &op, &days));
  assert_false(bc_seek_cli_parse_mtime_filter("abc", &op, &days));
}

static void test_parse_perm_filter(void **state) {
  (void)state;
  unsigned int mask = 0u;
  assert_true(bc_seek_cli_parse_perm_filter("644", &mask));
  assert_int_equal(mask, 0644u);
  assert_true(bc_seek_cli_parse_perm_filter("755", &mask));
  assert_int_equal(mask, 0755u);
  assert_true(bc_seek_cli_parse_perm_filter("0", &mask));
  assert_int_equal(mask, 0u);
  assert_true(bc_seek_cli_parse_perm_filter("7777", &mask));
  assert_int_equal(mask, 07777u);
  assert_false(bc_seek_cli_parse_perm_filter("", &mask));
  assert_false(bc_seek_cli_parse_perm_filter("abc", &mask));
  assert_false(bc_seek_cli_parse_perm_filter("999", &mask));
  assert_false(bc_seek_cli_parse_perm_filter("77777", &mask));
}

static void test_parse_threads(void **state) {
  (void)state;
  bc_seek_threads_mode_t mode = BC_SEEK_THREADS_MODE_EXPLICIT;
  size_t count = 99u;
  assert_true(bc_seek_cli_parse_threads("auto", &mode, &count));
  assert_int_equal(mode, BC_SEEK_THREADS_MODE_AUTO);
  assert_int_equal(count, 0u);
  assert_true(bc_seek_cli_parse_threads("mono", &mode, &count));
  assert_int_equal(mode, BC_SEEK_THREADS_MODE_MONO);
  assert_int_equal(count, 0u);
  assert_true(bc_seek_cli_parse_threads("0", &mode, &count));
  assert_int_equal(mode, BC_SEEK_THREADS_MODE_MONO);
  assert_int_equal(count, 0u);
  assert_true(bc_seek_cli_parse_threads("io", &mode, &count));
  assert_int_equal(mode, BC_SEEK_THREADS_MODE_IO);
  assert_int_equal(count, 0u);
  assert_true(bc_seek_cli_parse_threads("4", &mode, &count));
  assert_int_equal(mode, BC_SEEK_THREADS_MODE_EXPLICIT);
  assert_int_equal(count, 4u);
  assert_false(bc_seek_cli_parse_threads("auto-io", &mode, &count));
  assert_false(bc_seek_cli_parse_threads("", &mode, &count));
  assert_false(bc_seek_cli_parse_threads("5x", &mode, &count));
  assert_false(bc_seek_cli_parse_threads("not-a-number", &mode, &count));
  assert_false(
      bc_seek_cli_parse_threads("99999999999999999999", &mode, &count));
  assert_false(bc_seek_cli_parse_threads("-1", &mode, &count));
  assert_false(bc_seek_cli_parse_threads("+1", &mode, &count));
  assert_false(bc_seek_cli_parse_threads(" 4", &mode, &count));
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_parse_type_filter_valid),
      cmocka_unit_test(test_parse_type_filter_invalid),
      cmocka_unit_test(test_parse_size_filter_default_block_unit),
      cmocka_unit_test(test_parse_size_filter_suffixes),
      cmocka_unit_test(test_parse_size_filter_operators),
      cmocka_unit_test(test_parse_size_filter_invalid),
      cmocka_unit_test(test_parse_mtime_filter),
      cmocka_unit_test(test_parse_perm_filter),
      cmocka_unit_test(test_parse_threads),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
