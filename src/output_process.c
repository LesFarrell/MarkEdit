#include "output_process.h"

#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Large enough that a line typed into the panel never blocks the UI thread
   while the program is busy elsewhere. */
#define OUTPUT_PIPE_SIZE (64 * 1024)
#define OUTPUT_READ_SIZE 4096
/* How long freeing waits for the reader thread before cancelling its read,
   which a program that left the job could otherwise keep open. */
#define OUTPUT_READER_WAIT_MS 2000

struct OutputProcess {
    HANDLE process, job, thread;
    HANDLE input;  /* our end of the program's standard input */
    HANDLE output; /* our end of its standard output and error */
    HWND window;
    UINT message;
    int session;
};

static void post_chunk(OutputProcess *process, const char *data, size_t length,
                       int finished, DWORD exitCode) {
    OutputChunk *chunk = (OutputChunk *)malloc(sizeof(OutputChunk) + length);
    if (!chunk) return;
    chunk->session = process->session;
    chunk->finished = finished;
    chunk->exitCode = exitCode;
    chunk->length = length;
    if (length) memcpy(chunk->data, data, length);
    if (!PostMessageW(process->window, process->message, 0, (LPARAM)chunk)) free(chunk);
}

static unsigned __stdcall read_output(void *argument) {
    OutputProcess *process = (OutputProcess *)argument;
    char buffer[OUTPUT_READ_SIZE];
    DWORD read = 0;
    while (ReadFile(process->output, buffer, sizeof(buffer), &read, NULL) && read > 0)
        post_chunk(process, buffer, read, 0, 0);
    DWORD exitCode = 0;
    WaitForSingleObject(process->process, INFINITE);
    GetExitCodeProcess(process->process, &exitCode);
    post_chunk(process, NULL, 0, 1, exitCode);
    return 0;
}

/* MarkEdit's environment with `additions` merged in, as a block for
   CreateProcessW, or NULL when out of memory. */
static WCHAR *merged_environment(const WCHAR *additions) {
    WCHAR *current = GetEnvironmentStringsW();
    if (!current) return NULL;
    size_t currentLength = 0, additionsLength = 0;
    while (current[currentLength]) currentLength += wcslen(current + currentLength) + 1;
    while (additions[additionsLength]) additionsLength += wcslen(additions + additionsLength) + 1;
    WCHAR *block = (WCHAR *)malloc((currentLength + additionsLength + 1) * sizeof(WCHAR));
    if (!block) { FreeEnvironmentStringsW(current); return NULL; }
    size_t used = 0;
    for (const WCHAR *entry = current; *entry; entry += wcslen(entry) + 1) {
        /* Names such as "=C:" (a drive's current folder) begin with "=". */
        const WCHAR *equals = wcschr(entry + 1, L'=');
        size_t nameLength = equals ? (size_t)(equals - entry) : wcslen(entry);
        int replaced = 0;
        for (const WCHAR *added = additions; *added && !replaced; added += wcslen(added) + 1)
            replaced = _wcsnicmp(entry, added, nameLength) == 0 && added[nameLength] == L'=';
        if (replaced) continue;
        size_t length = wcslen(entry) + 1;
        memcpy(block + used, entry, length * sizeof(WCHAR));
        used += length;
    }
    memcpy(block + used, additions, additionsLength * sizeof(WCHAR));
    used += additionsLength;
    block[used] = 0;
    FreeEnvironmentStringsW(current);
    return block;
}

static HANDLE create_kill_on_close_job(void) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) return NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

/* Creates a pipe whose `childEnd` the program inherits and whose other end
   stays private to MarkEdit. childReads picks the direction. */
static int create_pipe(HANDLE *ourEnd, HANDLE *childEnd, int childReads) {
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    HANDLE readEnd, writeEnd;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, OUTPUT_PIPE_SIZE)) return 0;
    *ourEnd = childReads ? writeEnd : readEnd;
    *childEnd = childReads ? readEnd : writeEnd;
    SetHandleInformation(*ourEnd, HANDLE_FLAG_INHERIT, 0);
    return 1;
}

static void close_handle(HANDLE *handle) {
    if (*handle) CloseHandle(*handle);
    *handle = NULL;
}

OutputProcess *output_process_start(const WCHAR *commandLine, const WCHAR *directory,
                                    const WCHAR *environment, HWND window, UINT message,
                                    int session) {
    OutputProcess *process = (OutputProcess *)calloc(1, sizeof(*process));
    WCHAR *command = _wcsdup(commandLine);
    WCHAR *block = environment ? merged_environment(environment) : NULL;
    HANDLE childInput = NULL, childOutput = NULL;
    int ok = process && command && (!environment || block) &&
             (process->job = create_kill_on_close_job()) != NULL &&
             create_pipe(&process->input, &childInput, 1) &&
             create_pipe(&process->output, &childOutput, 0);
    PROCESS_INFORMATION info = {0};
    if (ok) {
        process->window = window;
        process->message = message;
        process->session = session;
        STARTUPINFOW startup = {0};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childInput;
        startup.hStdOutput = childOutput;
        startup.hStdError = childOutput;
        /* Suspended until it is in the job, so nothing it starts escapes. */
        ok = CreateProcessW(NULL, command, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                            block, directory, &startup, &info) != 0;
    }
    if (ok && !AssignProcessToJobObject(process->job, info.hProcess)) {
        TerminateProcess(info.hProcess, 1);
        ok = 0;
    }
    if (info.hThread) {
        if (ok) ResumeThread(info.hThread);
        CloseHandle(info.hThread);
    }
    /* The reader sees the end of the output only once every copy of the
       pipe's write end is closed, including MarkEdit's copy of the child's. */
    close_handle(&childInput);
    close_handle(&childOutput);
    free(command);
    free(block);
    if (ok) {
        process->process = info.hProcess;
        process->thread = (HANDLE)_beginthreadex(NULL, 0, read_output, process, 0, NULL);
        ok = process->thread != NULL;
        if (!ok) TerminateJobObject(process->job, 1);
    } else if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    if (!ok && process) {
        close_handle(&process->process);
        close_handle(&process->input);
        close_handle(&process->output);
        close_handle(&process->job);
        free(process);
        return NULL;
    }
    return process;
}

int output_process_write(OutputProcess *process, const char *bytes, size_t length) {
    while (length > 0) {
        DWORD written = 0;
        DWORD chunk = length > OUTPUT_PIPE_SIZE ? OUTPUT_PIPE_SIZE : (DWORD)length;
        if (!process->input || !WriteFile(process->input, bytes, chunk, &written, NULL)) return 0;
        bytes += written;
        length -= written;
    }
    return 1;
}

void output_process_stop(OutputProcess *process) {
    if (process && process->job) TerminateJobObject(process->job, 1);
}

void output_process_free(OutputProcess *process) {
    if (!process) return;
    output_process_stop(process);
    close_handle(&process->input);
    if (WaitForSingleObject(process->thread, OUTPUT_READER_WAIT_MS) == WAIT_TIMEOUT) {
        CancelSynchronousIo(process->thread);
        WaitForSingleObject(process->thread, INFINITE);
    }
    close_handle(&process->thread);
    close_handle(&process->output);
    close_handle(&process->process);
    close_handle(&process->job);
    free(process);
}
