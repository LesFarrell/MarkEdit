#include <stdio.h>
#include <string.h>

#include "../src/git_status.h"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

static void test_normal_status(void) {
    static const char data[] =
        "# branch.oid abcdef\0"
        "# branch.head feature/test\0"
        "# branch.upstream origin/feature/test\0"
        "# branch.ab +2 -1\0"
        "1 M. N... 100644 100644 100644 abc def staged.txt\0"
        "1 .M N... 100644 100644 100644 abc abc folder/working file.txt\0"
        "2 R. N... 100644 100644 100644 abc def R100 new name.txt\0old name.txt\0"
        "? caf\xc3\xa9 notes.txt\0"
        "! ignored.tmp\0";
    GitStatusResult result;
    CHECK(git_status_parse_v2_z(data, sizeof(data) - 1, &result));
    CHECK(result.head && strcmp(result.head, "feature/test") == 0);
    CHECK(result.upstream && strcmp(result.upstream, "origin/feature/test") == 0);
    CHECK(result.ahead == 2);
    CHECK(result.behind == 1);
    CHECK(!result.detached);
    CHECK(!result.unborn);
    CHECK(result.entryCount == 4);
    if (result.entryCount == 4) {
        CHECK(strcmp(result.entries[0].path, "staged.txt") == 0);
        CHECK(result.entries[0].indexStatus == 'M' && result.entries[0].worktreeStatus == ' ');
        CHECK(strcmp(result.entries[1].path, "folder/working file.txt") == 0);
        CHECK(result.entries[1].indexStatus == ' ' && result.entries[1].worktreeStatus == 'M');
        CHECK(strcmp(result.entries[2].path, "new name.txt") == 0);
        CHECK(result.entries[2].indexStatus == 'R' && result.entries[2].worktreeStatus == ' ');
        CHECK(strcmp(result.entries[3].path, "caf\xc3\xa9 notes.txt") == 0);
        CHECK(result.entries[3].indexStatus == '?' && result.entries[3].worktreeStatus == '?');
    }
    git_status_result_free(&result);
}

static void test_conflict_and_detached_head(void) {
    static const char data[] =
        "# branch.oid 123456\0"
        "# branch.head (detached)\0"
        "u UU N... 100644 100644 100644 100644 a b c conflict.txt\0";
    GitStatusResult result;
    CHECK(git_status_parse_v2_z(data, sizeof(data) - 1, &result));
    CHECK(result.detached);
    CHECK(result.entryCount == 1);
    if (result.entryCount == 1) {
        CHECK(strcmp(result.entries[0].path, "conflict.txt") == 0);
        CHECK(result.entries[0].indexStatus == 'U' && result.entries[0].worktreeStatus == 'U');
    }
    git_status_result_free(&result);
}

static void test_unborn_branch(void) {
    static const char data[] =
        "# branch.oid (initial)\0"
        "# branch.head main\0"
        "? first file.md\0";
    GitStatusResult result;
    CHECK(git_status_parse_v2_z(data, sizeof(data) - 1, &result));
    CHECK(result.unborn);
    CHECK(!result.detached);
    CHECK(result.head && strcmp(result.head, "main") == 0);
    git_status_result_free(&result);
}

static void test_rejects_truncated_records(void) {
    static const char data[] = "# branch.head main";
    GitStatusResult result;
    CHECK(!git_status_parse_v2_z(data, sizeof(data) - 1, &result));
    CHECK(result.head == NULL);
    CHECK(result.entries == NULL);
    CHECK(result.entryCount == 0);
}

static void test_conflict_classification(void) {
    CHECK(git_status_is_conflict('U', 'U'));
    CHECK(git_status_is_conflict('A', 'A'));
    CHECK(git_status_is_conflict('D', 'D'));
    CHECK(git_status_is_conflict(' ', 'U'));
    CHECK(!git_status_is_conflict('A', ' '));
    CHECK(!git_status_is_conflict(' ', 'M'));
    CHECK(!git_status_is_conflict('?', '?'));
}

int main(void) {
    test_normal_status();
    test_conflict_and_detached_head();
    test_unborn_branch();
    test_rejects_truncated_records();
    test_conflict_classification();
    if (failures) return 1;
    puts("All Git status tests passed.");
    return 0;
}
