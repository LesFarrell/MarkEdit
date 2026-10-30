#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

static DWORD run_git(const WCHAR *directory, const WCHAR *arguments) {
    WCHAR command[2048];
    if (swprintf(command, 2048, L"git.exe %ls", arguments) < 0) return (DWORD)-1;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, directory, &startup, &process)) return (DWORD)-1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = (DWORD)-1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exitCode;
}

/* GitHub's Windows runners have shown a transient failure immediately after
   a command that just wrote ref/lock state - observed with --abort right
   after a conflicting merge/rebase, not reproducible locally across repeated
   runs - consistent with filesystem-cache or antivirus-scan contention
   rather than a real git error. One short retry absorbs it without
   weakening the assertion: the command still has to succeed, just not
   necessarily on the first try. */
static DWORD run_git_retrying(const WCHAR *directory, const WCHAR *arguments) {
    DWORD exitCode = run_git(directory, arguments);
    if (exitCode != 0) {
        Sleep(200);
        exitCode = run_git(directory, arguments);
    }
    return exitCode;
}

static int write_file(const WCHAR *path, const char *text) {
    FILE *file = _wfopen(path, L"wb");
    if (!file) return 0;
    size_t length = strlen(text);
    int wrote = fwrite(text, 1, length, file) == length;
    int closed = fclose(file) == 0;
    return wrote && closed;
}

static int read_file_equals(const WCHAR *path, const char *expected) {
    FILE *file = _wfopen(path, L"rb");
    if (!file) return 0;
    char buffer[128];
    size_t length = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    return length == strlen(expected) && memcmp(buffer, expected, length) == 0;
}

static void remove_tree(const WCHAR *directory) {
    WCHAR pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", directory);
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            WCHAR path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%ls\\%ls", directory, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path);
            else {
                SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path);
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    SetFileAttributesW(directory, FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(directory);
}

int main(void) {
    if (run_git(NULL, L"--version") == (DWORD)-1) {
        puts("Git not found; Git workflow tests skipped.");
        return 0;
    }

    WCHAR tempRoot[MAX_PATH], sandbox[MAX_PATH], repository[MAX_PATH], remote[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, tempRoot) > 0);
    CHECK(GetTempFileNameW(tempRoot, L"mge", 0, sandbox));
    CHECK(DeleteFileW(sandbox));
    CHECK(CreateDirectoryW(sandbox, NULL));
    swprintf(repository, MAX_PATH, L"%ls\\work", sandbox);
    swprintf(remote, MAX_PATH, L"%ls\\remote.git", sandbox);
    CHECK(CreateDirectoryW(repository, NULL));
    if (failures) return 1;

    WCHAR tracked[MAX_PATH], untracked[MAX_PATH];
    swprintf(tracked, MAX_PATH, L"%ls\\tracked.txt", repository);
    swprintf(untracked, MAX_PATH, L"%ls\\new file.txt", repository);

    CHECK(run_git(repository, L"init -q -b main") == 0);
    CHECK(run_git(repository, L"config core.autocrlf false") == 0);
    CHECK(write_file(tracked, "base\n"));
    CHECK(run_git(repository, L"add -- tracked.txt") == 0);
    CHECK(run_git(repository,
        L"-c user.name=TinkerEditor -c user.email=tinkeredit-tests@example.invalid "
        L"-c commit.gpgsign=false commit --no-verify -q -m initial") == 0);

    /* Staged and working-tree diffs must remain distinct. */
    CHECK(write_file(tracked, "staged\n"));
    CHECK(run_git(repository, L"add -- tracked.txt") == 0);
    CHECK(write_file(tracked, "unstaged\n"));
    CHECK(run_git(repository, L"diff --quiet -- tracked.txt") == 1);
    CHECK(run_git(repository, L"diff --cached --quiet -- tracked.txt") == 1);

    /* An untracked file needs --no-index; ordinary git diff omits it. */
    CHECK(write_file(untracked, "new\n"));
    CHECK(run_git(repository, L"diff --quiet -- \"new file.txt\"") == 0);
    CHECK(run_git(repository, L"diff --no-index --quiet -- /dev/null \"new file.txt\"") == 1);

    /* Discard All resets both index and worktree, then removes untracked files. */
    CHECK(run_git(repository, L"reset --hard -q HEAD") == 0);
    CHECK(run_git(repository, L"clean -fdq") == 0);
    CHECK(read_file_equals(tracked, "base\n"));
    CHECK(GetFileAttributesW(untracked) == INVALID_FILE_ATTRIBUTES);

    /* Safe deletion refuses an unmerged branch; force is a separate action. */
    CHECK(run_git(repository, L"switch -q -c topic") == 0);
    CHECK(write_file(tracked, "topic\n"));
    CHECK(run_git(repository, L"add -- tracked.txt") == 0);
    CHECK(run_git(repository,
        L"-c user.name=TinkerEditor -c user.email=tinkeredit-tests@example.invalid "
        L"-c commit.gpgsign=false commit --no-verify -q -m topic") == 0);
    CHECK(run_git(repository, L"switch -q main") == 0);
    CHECK(run_git(repository, L"branch -d topic") != 0);
    CHECK(run_git(repository, L"branch -D topic") == 0);

    /* Publishing establishes the selected remote as the branch upstream. */
    WCHAR arguments[MAX_PATH * 2];
    swprintf(arguments, MAX_PATH * 2, L"init -q --bare \"%ls\"", remote);
    CHECK(run_git(NULL, arguments) == 0);
    swprintf(arguments, MAX_PATH * 2, L"remote add origin \"%ls\"", remote);
    CHECK(run_git(repository, arguments) == 0);
    CHECK(run_git(repository, L"push -q --set-upstream origin main") == 0);
    CHECK(run_git(repository, L"rev-parse --verify @{u}") == 0);

    /* Stash includes untracked files and can be applied and dropped by ref. */
    CHECK(write_file(tracked, "stashed\n"));
    CHECK(write_file(untracked, "stash-new\n"));
    CHECK(run_git(repository, L"stash push -q -u -m workflow") == 0);
    CHECK(read_file_equals(tracked, "base\n"));
    CHECK(GetFileAttributesW(untracked) == INVALID_FILE_ATTRIBUTES);
    CHECK(run_git(repository, L"stash apply -q stash@{0}") == 0);
    CHECK(read_file_equals(tracked, "stashed\n"));
    CHECK(GetFileAttributesW(untracked) != INVALID_FILE_ATTRIBUTES);
    CHECK(run_git(repository, L"reset --hard -q HEAD") == 0);
    CHECK(run_git(repository, L"clean -fdq") == 0);
    CHECK(run_git(repository, L"stash drop -q stash@{0}") == 0);

    /* A conflicted merge can be aborted back to its pre-merge state. */
    CHECK(run_git(repository, L"switch -q -c merge-topic") == 0);
    CHECK(write_file(tracked, "merge-topic\n"));
    CHECK(run_git(repository, L"add -- tracked.txt") == 0);
    CHECK(run_git(repository,
        L"-c user.name=TinkerEditor -c user.email=tinkeredit-tests@example.invalid "
        L"-c commit.gpgsign=false commit --no-verify -q -m merge-topic") == 0);
    CHECK(run_git(repository, L"switch -q main") == 0);
    CHECK(write_file(tracked, "merge-main\n"));
    CHECK(run_git(repository, L"add -- tracked.txt") == 0);
    CHECK(run_git(repository,
        L"-c user.name=TinkerEditor -c user.email=tinkeredit-tests@example.invalid "
        L"-c commit.gpgsign=false commit --no-verify -q -m merge-main") == 0);
    CHECK(run_git(repository, L"merge merge-topic") != 0);
    CHECK(run_git_retrying(repository, L"merge --abort") == 0);
    CHECK(read_file_equals(tracked, "merge-main\n"));

    /* The same conflict lifecycle is available for rebase. */
    CHECK(run_git(repository, L"switch -q -c rebase-topic") == 0);
    CHECK(write_file(tracked, "rebase-topic\n"));
    CHECK(run_git(repository, L"add -- tracked.txt") == 0);
    CHECK(run_git(repository,
        L"-c user.name=TinkerEditor -c user.email=tinkeredit-tests@example.invalid "
        L"-c commit.gpgsign=false commit --no-verify -q -m rebase-topic") == 0);
    CHECK(run_git(repository, L"switch -q main") == 0);
    CHECK(write_file(tracked, "rebase-main\n"));
    CHECK(run_git(repository, L"add -- tracked.txt") == 0);
    CHECK(run_git(repository,
        L"-c user.name=TinkerEditor -c user.email=tinkeredit-tests@example.invalid "
        L"-c commit.gpgsign=false commit --no-verify -q -m rebase-main") == 0);
    CHECK(run_git(repository, L"switch -q rebase-topic") == 0);
    CHECK(run_git(repository, L"rebase main") != 0);
    CHECK(run_git_retrying(repository, L"rebase --abort") == 0);
    CHECK(read_file_equals(tracked, "rebase-topic\n"));

    remove_tree(sandbox);
    if (failures) return 1;
    puts("All Git workflow tests passed.");
    return 0;
}
