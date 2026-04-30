// SPDX-License-Identifier: MIT

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include <stdbool.h>
#include <string.h>

#include "bc_seek_filter_internal.h"

static void make_empty_predicate(bc_seek_predicate_t *predicate) {
  memset(predicate, 0, sizeof(*predicate));
  predicate->respect_ignore_defaults = true;
}

static void make_candidate(bc_seek_candidate_t *candidate, const char *path,
                           const char *basename, size_t depth,
                           bc_seek_entry_type_t type) {
  memset(candidate, 0, sizeof(*candidate));
  candidate->path = path;
  candidate->path_length = strlen(path);
  candidate->basename = basename;
  candidate->basename_length = strlen(basename);
  candidate->depth = depth;
  candidate->entry_type = type;
  candidate->type_resolved = type != BC_SEEK_ENTRY_TYPE_ANY;
  candidate->parent_directory_fd = -1;
}

static void test_ignored_directory_names(void **state) {
  (void)state;
  assert_true(bc_seek_filter_ignored_directory_name(".git", 4));
  assert_true(bc_seek_filter_ignored_directory_name("node_modules", 12));
  assert_true(bc_seek_filter_ignored_directory_name("__pycache__", 11));
  assert_true(bc_seek_filter_ignored_directory_name(".cache", 6));
  assert_true(bc_seek_filter_ignored_directory_name("target", 6));
  assert_false(bc_seek_filter_ignored_directory_name("src", 3));
  assert_false(bc_seek_filter_ignored_directory_name("tests", 5));
  assert_false(bc_seek_filter_ignored_directory_name(".gitignore", 10));
}

static void test_glob_matches(void **state) {
  (void)state;
  assert_true(bc_seek_filter_glob_matches("*.c", "foo.c", false));
  assert_false(bc_seek_filter_glob_matches("*.c", "foo.h", false));
  assert_true(bc_seek_filter_glob_matches("foo*", "foobar", false));
  assert_false(bc_seek_filter_glob_matches("foo*", "barfoo", false));
  assert_false(bc_seek_filter_glob_matches("FOO*", "foobar", false));
  assert_true(bc_seek_filter_glob_matches("FOO*", "foobar", true));
  assert_true(bc_seek_filter_glob_matches("*.[ch]", "main.c", false));
  assert_true(bc_seek_filter_glob_matches("*.[ch]", "main.h", false));
  assert_false(bc_seek_filter_glob_matches("*.[ch]", "main.cpp", false));
}

static void test_evaluate_no_filters_accepts(void **state) {
  (void)state;
  bc_seek_predicate_t predicate;
  make_empty_predicate(&predicate);
  bc_seek_candidate_t candidate;
  make_candidate(&candidate, "/tmp/foo", "foo", 1u, BC_SEEK_ENTRY_TYPE_FILE);
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &candidate),
                   BC_SEEK_FILTER_DECISION_ACCEPT);
}

static void test_evaluate_name_filter(void **state) {
  (void)state;
  bc_seek_predicate_t predicate;
  make_empty_predicate(&predicate);
  predicate.has_name_glob = true;
  predicate.name_glob = "*.c";

  bc_seek_candidate_t match;
  make_candidate(&match, "/tmp/foo.c", "foo.c", 1u, BC_SEEK_ENTRY_TYPE_FILE);
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &match),
                   BC_SEEK_FILTER_DECISION_ACCEPT);

  bc_seek_candidate_t no_match;
  make_candidate(&no_match, "/tmp/foo.h", "foo.h", 1u, BC_SEEK_ENTRY_TYPE_FILE);
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &no_match),
                   BC_SEEK_FILTER_DECISION_REJECT);
}

static void test_evaluate_type_filter(void **state) {
  (void)state;
  bc_seek_predicate_t predicate;
  make_empty_predicate(&predicate);
  predicate.has_type_filter = true;
  predicate.type_filter = BC_SEEK_ENTRY_TYPE_DIRECTORY;

  bc_seek_candidate_t dir_candidate;
  make_candidate(&dir_candidate, "/tmp/d", "d", 1u,
                 BC_SEEK_ENTRY_TYPE_DIRECTORY);
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &dir_candidate),
                   BC_SEEK_FILTER_DECISION_ACCEPT);

  bc_seek_candidate_t file_candidate;
  make_candidate(&file_candidate, "/tmp/f", "f", 1u, BC_SEEK_ENTRY_TYPE_FILE);
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &file_candidate),
                   BC_SEEK_FILTER_DECISION_REJECT);
}

static void test_evaluate_depth_filter(void **state) {
  (void)state;
  bc_seek_predicate_t predicate;
  make_empty_predicate(&predicate);
  predicate.has_max_depth = true;
  predicate.max_depth = 3u;
  predicate.min_depth = 2u;

  bc_seek_candidate_t too_shallow;
  make_candidate(&too_shallow, "/a", "a", 1u, BC_SEEK_ENTRY_TYPE_FILE);
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &too_shallow),
                   BC_SEEK_FILTER_DECISION_REJECT);

  bc_seek_candidate_t too_deep;
  make_candidate(&too_deep, "/a/b/c/d", "d", 4u, BC_SEEK_ENTRY_TYPE_FILE);
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &too_deep),
                   BC_SEEK_FILTER_DECISION_REJECT);

  bc_seek_candidate_t in_range;
  make_candidate(&in_range, "/a/b/c", "c", 3u, BC_SEEK_ENTRY_TYPE_FILE);
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &in_range),
                   BC_SEEK_FILTER_DECISION_ACCEPT);
}

static void test_evaluate_size_requires_stat(void **state) {
  (void)state;
  bc_seek_predicate_t predicate;
  make_empty_predicate(&predicate);
  predicate.has_size_filter = true;
  predicate.size_op = BC_SEEK_COMPARE_GREATER;
  predicate.size_threshold_bytes = 100u;
  predicate.require_stat = true;

  bc_seek_candidate_t stat_populated;
  make_candidate(&stat_populated, "/a", "a", 1u, BC_SEEK_ENTRY_TYPE_FILE);
  stat_populated.stat_populated = true;
  stat_populated.size_bytes = 500u;
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &stat_populated),
                   BC_SEEK_FILTER_DECISION_ACCEPT);

  bc_seek_candidate_t stat_populated_small;
  make_candidate(&stat_populated_small, "/a", "a", 1u, BC_SEEK_ENTRY_TYPE_FILE);
  stat_populated_small.stat_populated = true;
  stat_populated_small.size_bytes = 50u;
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &stat_populated_small),
                   BC_SEEK_FILTER_DECISION_REJECT);
}

static void test_evaluate_path_filter(void **state) {
  (void)state;
  bc_seek_predicate_t predicate;
  make_empty_predicate(&predicate);
  predicate.has_path_glob = true;
  predicate.path_glob = "*tests*";

  bc_seek_candidate_t match;
  make_candidate(&match, "/a/tests/foo.c", "foo.c", 2u,
                 BC_SEEK_ENTRY_TYPE_FILE);
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &match),
                   BC_SEEK_FILTER_DECISION_ACCEPT);

  bc_seek_candidate_t no_match;
  make_candidate(&no_match, "/a/src/foo.c", "foo.c", 2u,
                 BC_SEEK_ENTRY_TYPE_FILE);
  assert_int_equal(bc_seek_filter_evaluate(&predicate, &no_match),
                   BC_SEEK_FILTER_DECISION_REJECT);
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_ignored_directory_names),
      cmocka_unit_test(test_glob_matches),
      cmocka_unit_test(test_evaluate_no_filters_accepts),
      cmocka_unit_test(test_evaluate_name_filter),
      cmocka_unit_test(test_evaluate_type_filter),
      cmocka_unit_test(test_evaluate_depth_filter),
      cmocka_unit_test(test_evaluate_size_requires_stat),
      cmocka_unit_test(test_evaluate_path_filter),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
