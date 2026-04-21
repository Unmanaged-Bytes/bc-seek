// SPDX-License-Identifier: MIT

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef BC_SEEK_TEST_BINARY_PATH
#error "BC_SEEK_TEST_BINARY_PATH must be defined"
#endif

typedef struct command_result {
    int exit_code;
    char* stdout_buffer;
    size_t stdout_length;
} command_result_t;

static void command_result_free(command_result_t* result)
{
    free(result->stdout_buffer);
    result->stdout_buffer = NULL;
    result->stdout_length = 0;
}

static bool run_capture(char* const argv[], command_result_t* out_result)
{
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return false;
    }
    if (pid == 0) {
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pipe_fd[1]);

    size_t capacity = 4096u;
    char* buffer = malloc(capacity);
    if (buffer == NULL) {
        close(pipe_fd[0]);
        waitpid(pid, NULL, 0);
        return false;
    }
    size_t length = 0;
    for (;;) {
        if (length + 1024 > capacity) {
            capacity *= 2u;
            char* grown = realloc(buffer, capacity);
            if (grown == NULL) {
                free(buffer);
                close(pipe_fd[0]);
                waitpid(pid, NULL, 0);
                return false;
            }
            buffer = grown;
        }
        ssize_t bytes_read = read(pipe_fd[0], buffer + length, capacity - length - 1);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buffer);
            close(pipe_fd[0]);
            waitpid(pid, NULL, 0);
            return false;
        }
        if (bytes_read == 0) {
            break;
        }
        length += (size_t)bytes_read;
    }
    buffer[length] = '\0';
    close(pipe_fd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    out_result->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    out_result->stdout_buffer = buffer;
    out_result->stdout_length = length;
    return true;
}

static size_t count_lines(const char* buffer, size_t length)
{
    size_t count = 0;
    for (size_t index = 0; index < length; index++) {
        if (buffer[index] == '\n') {
            count += 1;
        }
    }
    return count;
}

static size_t count_null_separators(const char* buffer, size_t length)
{
    size_t count = 0;
    for (size_t index = 0; index < length; index++) {
        if (buffer[index] == '\0') {
            count += 1;
        }
    }
    return count;
}

static bool buffer_contains_path(const char* buffer, size_t length, const char* path)
{
    size_t path_length = strlen(path);
    if (path_length == 0 || path_length > length) {
        return false;
    }
    return memmem(buffer, length, path, path_length) != NULL;
}

static bool make_file(const char* path, const char* content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    size_t content_length = strlen(content);
    ssize_t written = write(fd, content, content_length);
    close(fd);
    return written == (ssize_t)content_length;
}

typedef struct tree_fixture {
    char root[256];
} tree_fixture_t;

static int tree_setup(void** state)
{
    tree_fixture_t* fixture = test_calloc(1, sizeof(*fixture));
    snprintf(fixture->root, sizeof(fixture->root), "%s/e2e-XXXXXX", BC_SEEK_TEST_FIXTURES_DIRECTORY);
    if (mkdir(BC_SEEK_TEST_FIXTURES_DIRECTORY, 0755) != 0 && errno != EEXIST) {
        test_free(fixture);
        return -1;
    }
    if (mkdtemp(fixture->root) == NULL) {
        test_free(fixture);
        return -1;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/top.txt", fixture->root);
    if (!make_file(path, "top-level")) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/sub", fixture->root);
    if (mkdir(path, 0755) != 0) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/sub/alpha.c", fixture->root);
    if (!make_file(path, "alpha contents exceeding forty eight bytes for size test ........")) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/sub/beta.h", fixture->root);
    if (!make_file(path, "beta")) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/sub/deep", fixture->root);
    if (mkdir(path, 0755) != 0) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/sub/deep/gamma.c", fixture->root);
    if (!make_file(path, "gamma")) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/.hidden", fixture->root);
    if (!make_file(path, "secret")) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/.git", fixture->root);
    if (mkdir(path, 0755) != 0) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/.git/config", fixture->root);
    if (!make_file(path, "should be ignored")) {
        return -1;
    }

    *state = fixture;
    return 0;
}

static void remove_tree(const char* path)
{
    DIR* directory = opendir(path);
    if (directory != NULL) {
        struct dirent* entry;
        while ((entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char child_path[1024];
            snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
            struct stat child_stat;
            if (lstat(child_path, &child_stat) == 0) {
                if (S_ISDIR(child_stat.st_mode)) {
                    remove_tree(child_path);
                } else {
                    unlink(child_path);
                }
            }
        }
        closedir(directory);
    }
    rmdir(path);
}

static int tree_teardown(void** state)
{
    tree_fixture_t* fixture = *state;
    remove_tree(fixture->root);
    test_free(fixture);
    return 0;
}

static void test_help(void** state)
{
    (void)state;
    char* const argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "--help", NULL};
    command_result_t result;
    assert_true(run_capture(argv, &result));
    assert_int_equal(result.exit_code, 0);
    assert_true(result.stdout_length > 0);
    assert_non_null(strstr(result.stdout_buffer, "USAGE"));
    command_result_free(&result);
}

static void test_version(void** state)
{
    (void)state;
    char* const argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "--version", NULL};
    command_result_t result;
    assert_true(run_capture(argv, &result));
    assert_int_equal(result.exit_code, 0);
    assert_true(result.stdout_length > 0);
    command_result_free(&result);
}

static void test_walk_default_ignores_hidden_and_git(void** state)
{
    tree_fixture_t* fixture = *state;
    char* const argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "find", "--type=f", fixture->root, NULL};
    command_result_t result;
    assert_true(run_capture(argv, &result));
    assert_int_equal(result.exit_code, 0);
    assert_true(buffer_contains_path(result.stdout_buffer, result.stdout_length, "top.txt"));
    assert_true(buffer_contains_path(result.stdout_buffer, result.stdout_length, "alpha.c"));
    assert_true(buffer_contains_path(result.stdout_buffer, result.stdout_length, "beta.h"));
    assert_true(buffer_contains_path(result.stdout_buffer, result.stdout_length, "gamma.c"));
    assert_false(buffer_contains_path(result.stdout_buffer, result.stdout_length, ".hidden"));
    assert_false(buffer_contains_path(result.stdout_buffer, result.stdout_length, ".git/config"));
    command_result_free(&result);
}

static void test_walk_hidden_flag(void** state)
{
    tree_fixture_t* fixture = *state;
    char* const argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "find", "--hidden", "--type=f", fixture->root, NULL};
    command_result_t result;
    assert_true(run_capture(argv, &result));
    assert_int_equal(result.exit_code, 0);
    assert_true(buffer_contains_path(result.stdout_buffer, result.stdout_length, ".hidden"));
    command_result_free(&result);
}

static void test_walk_name_glob(void** state)
{
    tree_fixture_t* fixture = *state;
    char* const argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "find", "--name=*.c", fixture->root, NULL};
    command_result_t result;
    assert_true(run_capture(argv, &result));
    assert_int_equal(result.exit_code, 0);
    assert_int_equal(count_lines(result.stdout_buffer, result.stdout_length), 2u);
    assert_true(buffer_contains_path(result.stdout_buffer, result.stdout_length, "alpha.c"));
    assert_true(buffer_contains_path(result.stdout_buffer, result.stdout_length, "gamma.c"));
    command_result_free(&result);
}

static void test_walk_max_depth(void** state)
{
    tree_fixture_t* fixture = *state;
    char* const argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "find", "--max-depth=1", "--type=f", fixture->root, NULL};
    command_result_t result;
    assert_true(run_capture(argv, &result));
    assert_int_equal(result.exit_code, 0);
    assert_true(buffer_contains_path(result.stdout_buffer, result.stdout_length, "top.txt"));
    assert_false(buffer_contains_path(result.stdout_buffer, result.stdout_length, "alpha.c"));
    assert_false(buffer_contains_path(result.stdout_buffer, result.stdout_length, "gamma.c"));
    command_result_free(&result);
}

static void test_walk_min_depth(void** state)
{
    tree_fixture_t* fixture = *state;
    char* const argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "find", "--min-depth=3", "--type=f", fixture->root, NULL};
    command_result_t result;
    assert_true(run_capture(argv, &result));
    assert_int_equal(result.exit_code, 0);
    assert_true(buffer_contains_path(result.stdout_buffer, result.stdout_length, "gamma.c"));
    assert_false(buffer_contains_path(result.stdout_buffer, result.stdout_length, "top.txt"));
    assert_false(buffer_contains_path(result.stdout_buffer, result.stdout_length, "alpha.c"));
    command_result_free(&result);
}

static void test_walk_size_filter(void** state)
{
    tree_fixture_t* fixture = *state;
    char* const argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "find", "--type=f", "--size=+32c", fixture->root, NULL};
    command_result_t result;
    assert_true(run_capture(argv, &result));
    assert_int_equal(result.exit_code, 0);
    assert_true(buffer_contains_path(result.stdout_buffer, result.stdout_length, "alpha.c"));
    assert_false(buffer_contains_path(result.stdout_buffer, result.stdout_length, "beta.h"));
    command_result_free(&result);
}

static void test_walk_null_terminated(void** state)
{
    tree_fixture_t* fixture = *state;
    char* const argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "find", "--type=f", "--null", fixture->root, NULL};
    command_result_t result;
    assert_true(run_capture(argv, &result));
    assert_int_equal(result.exit_code, 0);
    assert_int_equal(count_lines(result.stdout_buffer, result.stdout_length), 0u);
    assert_true(count_null_separators(result.stdout_buffer, result.stdout_length) >= 3u);
    command_result_free(&result);
}

static void test_walk_no_ignore(void** state)
{
    tree_fixture_t* fixture = *state;
    char* const argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "find", "--no-ignore", "--hidden", "--type=f", fixture->root, NULL};
    command_result_t result;
    assert_true(run_capture(argv, &result));
    assert_int_equal(result.exit_code, 0);
    assert_true(buffer_contains_path(result.stdout_buffer, result.stdout_length, ".git/config"));
    command_result_free(&result);
}

static void test_walk_mono_thread(void** state)
{
    tree_fixture_t* fixture = *state;
    char* const argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "--threads=0", "find", "--type=f", fixture->root, NULL};
    command_result_t result;
    assert_true(run_capture(argv, &result));
    assert_int_equal(result.exit_code, 0);
    assert_true(count_lines(result.stdout_buffer, result.stdout_length) > 0u);
    command_result_free(&result);
}

static int compare_c_strings(const void* left, const void* right)
{
    const char* left_value = *(const char* const*)left;
    const char* right_value = *(const char* const*)right;
    return strcmp(left_value, right_value);
}

static void split_lines_sorted(char* buffer, size_t length, char*** out_lines, size_t* out_count)
{
    size_t line_count = 0;
    for (size_t index = 0; index < length; index++) {
        if (buffer[index] == '\n') {
            buffer[index] = '\0';
            line_count += 1;
        }
    }
    char** lines = test_calloc(line_count > 0 ? line_count : 1, sizeof(char*));
    size_t cursor = 0;
    size_t index = 0;
    size_t start = 0;
    while (index < length && cursor < line_count) {
        if (buffer[index] == '\0') {
            lines[cursor++] = buffer + start;
            start = index + 1;
        }
        index += 1;
    }
    qsort(lines, line_count, sizeof(char*), compare_c_strings);
    *out_lines = lines;
    *out_count = line_count;
}

static void test_walk_follow_symlinks(void** state)
{
    tree_fixture_t* fixture = *state;
    char link_path[512];
    snprintf(link_path, sizeof(link_path), "%s/sub_link", fixture->root);
    char target_path[512];
    snprintf(target_path, sizeof(target_path), "%s/sub", fixture->root);
    if (symlink(target_path, link_path) != 0) {
        skip();
    }

    char* const without[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "find", "--type=f", fixture->root, NULL};
    command_result_t result_without;
    assert_true(run_capture(without, &result_without));
    assert_int_equal(result_without.exit_code, 0);
    assert_false(buffer_contains_path(result_without.stdout_buffer, result_without.stdout_length, "sub_link/alpha.c"));
    command_result_free(&result_without);

    char* const with[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "find", "--follow-symlinks", "--type=f", fixture->root, NULL};
    command_result_t result_with;
    assert_true(run_capture(with, &result_with));
    assert_int_equal(result_with.exit_code, 0);
    assert_true(buffer_contains_path(result_with.stdout_buffer, result_with.stdout_length, "sub_link/alpha.c"));
    command_result_free(&result_with);

    unlink(link_path);
}

static void test_walk_mono_equals_parallel(void** state)
{
    tree_fixture_t* fixture = *state;
    char* const mono_argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "--threads=0", "find", "--hidden", "--no-ignore", fixture->root, NULL};
    char* const multi_argv[] = {(char*)BC_SEEK_TEST_BINARY_PATH, "--threads=4", "find", "--hidden", "--no-ignore", fixture->root, NULL};
    command_result_t mono;
    command_result_t multi;
    assert_true(run_capture(mono_argv, &mono));
    assert_int_equal(mono.exit_code, 0);
    assert_true(run_capture(multi_argv, &multi));
    assert_int_equal(multi.exit_code, 0);

    char** mono_lines = NULL;
    size_t mono_count = 0u;
    char** multi_lines = NULL;
    size_t multi_count = 0u;
    split_lines_sorted(mono.stdout_buffer, mono.stdout_length, &mono_lines, &mono_count);
    split_lines_sorted(multi.stdout_buffer, multi.stdout_length, &multi_lines, &multi_count);

    assert_int_equal(mono_count, multi_count);
    for (size_t index = 0; index < mono_count; index++) {
        assert_string_equal(mono_lines[index], multi_lines[index]);
    }

    test_free(mono_lines);
    test_free(multi_lines);
    command_result_free(&mono);
    command_result_free(&multi);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_help),
        cmocka_unit_test(test_version),
        cmocka_unit_test_setup_teardown(test_walk_default_ignores_hidden_and_git, tree_setup, tree_teardown),
        cmocka_unit_test_setup_teardown(test_walk_hidden_flag, tree_setup, tree_teardown),
        cmocka_unit_test_setup_teardown(test_walk_name_glob, tree_setup, tree_teardown),
        cmocka_unit_test_setup_teardown(test_walk_max_depth, tree_setup, tree_teardown),
        cmocka_unit_test_setup_teardown(test_walk_min_depth, tree_setup, tree_teardown),
        cmocka_unit_test_setup_teardown(test_walk_size_filter, tree_setup, tree_teardown),
        cmocka_unit_test_setup_teardown(test_walk_null_terminated, tree_setup, tree_teardown),
        cmocka_unit_test_setup_teardown(test_walk_no_ignore, tree_setup, tree_teardown),
        cmocka_unit_test_setup_teardown(test_walk_mono_thread, tree_setup, tree_teardown),
        cmocka_unit_test_setup_teardown(test_walk_follow_symlinks, tree_setup, tree_teardown),
        cmocka_unit_test_setup_teardown(test_walk_mono_equals_parallel, tree_setup, tree_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
