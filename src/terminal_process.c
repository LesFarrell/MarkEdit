#include "terminal_process.h"

#include <process.h>
#include <stdlib.h>
#include <string.h>

/* How long freeing waits for the reader thread before cancelling its read;
   see output_process.c's OUTPUT_READER_WAIT_MS, which this mirrors. */
#define TERMINAL_READER_WAIT_MS 2000
#define TERMINAL_READ_SIZE 4096

/* CreatePseudoConsole/ResizePseudoConsole/ClosePseudoConsole are resolved
   with GetProcAddress rather than called directly, even though the headers
   declare them: ConPTY only exists on Windows 10 1809+, and a direct import
   would keep markedit.exe from loading at all on an older Windows, rather
   than just this one feature being unavailable. */
typedef HRESULT(WINAPI *PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef HRESULT(WINAPI *PFN_ResizePseudoConsole)(HPCON, COORD);
typedef void(WINAPI *PFN_ClosePseudoConsole)(HPCON);

static PFN_CreatePseudoConsole s_createPseudoConsole;
static PFN_ResizePseudoConsole s_resizePseudoConsole;
static PFN_ClosePseudoConsole s_closePseudoConsole;
static int s_resolved;

static void resolve_conpty(void) {
    if (s_resolved) return;
    s_resolved = 1;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) return;
    s_createPseudoConsole = (PFN_CreatePseudoConsole)(void *)GetProcAddress(kernel32, "CreatePseudoConsole");
    s_resizePseudoConsole = (PFN_ResizePseudoConsole)(void *)GetProcAddress(kernel32, "ResizePseudoConsole");
    s_closePseudoConsole = (PFN_ClosePseudoConsole)(void *)GetProcAddress(kernel32, "ClosePseudoConsole");
}

int terminal_process_available(void) {
    resolve_conpty();
    return s_createPseudoConsole && s_resizePseudoConsole && s_closePseudoConsole;
}

struct TerminalProcess {
    HANDLE process, job, readerThread, watcherThread;
    HANDLE inputWrite; /* our end; keystrokes go to the pseudoconsole here */
    HANDLE outputRead; /* our end; the pseudoconsole's rendered output arrives here */
    HPCON hpc;
    HWND window;
    UINT message;
    int session;
};

static void post_chunk(TerminalProcess *process, const char *data, size_t length,
                       int finished, DWORD exitCode) {
    TerminalChunk *chunk = (TerminalChunk *)malloc(sizeof(TerminalChunk) + length);
    if (!chunk) return;
    chunk->session = process->session;
    chunk->finished = finished;
    chunk->exitCode = exitCode;
    chunk->length = length;
    if (length) memcpy(chunk->data, data, length);
    if (!PostMessageW(process->window, process->message, 0, (LPARAM)chunk)) free(chunk);
}

/* Unlike output_process.c's plain pipes, ConPTY's output pipe only ends when
   ClosePseudoConsole() runs (terminal_process_free): the pseudoconsole is a
   terminal, not a single program's stdout, so it stays open after the shell
   it's hosting exits - same as a real terminal window doesn't close itself
   when you type "exit". So reading output and noticing the shell is done are
   two independent threads here, not one: this one just drains output for as
   long as the pseudoconsole lives. */
static unsigned __stdcall read_output(void *argument) {
    TerminalProcess *process = (TerminalProcess *)argument;
    char buffer[TERMINAL_READ_SIZE];
    DWORD read = 0;
    while (ReadFile(process->outputRead, buffer, sizeof(buffer), &read, NULL) && read > 0)
        post_chunk(process, buffer, read, 0, 0);
    return 0;
}

/* Reports the shell's exit the moment it happens, rather than waiting for
   the pseudoconsole itself to close. */
static unsigned __stdcall watch_process(void *argument) {
    TerminalProcess *process = (TerminalProcess *)argument;
    WaitForSingleObject(process->process, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process->process, &exitCode);
    post_chunk(process, NULL, 0, 1, exitCode);
    return 0;
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

static void close_handle(HANDLE *handle) {
    if (*handle) CloseHandle(*handle);
    *handle = NULL;
}

TerminalProcess *terminal_process_start(const WCHAR *commandLine, const WCHAR *directory,
                                        int cols, int rows, HWND window, UINT message,
                                        int session) {
    if (!terminal_process_available()) return NULL;

    HANDLE inputRead = NULL, outputWrite = NULL;
    TerminalProcess *process = (TerminalProcess *)calloc(1, sizeof(*process));
    WCHAR *command = _wcsdup(commandLine);
    int ok = process && command &&
             (process->job = create_kill_on_close_job()) != NULL &&
             CreatePipe(&inputRead, &process->inputWrite, NULL, 0) &&
             CreatePipe(&process->outputRead, &outputWrite, NULL, 0);

    HPCON hpc = NULL;
    if (ok) {
        COORD size = { (SHORT)(cols > 0 ? cols : 1), (SHORT)(rows > 0 ? rows : 1) };
        ok = s_createPseudoConsole(size, inputRead, outputWrite, 0, &hpc) == S_OK;
    }
    /* ConPTY duplicates these for itself; our copies aren't needed again. */
    close_handle(&inputRead);
    close_handle(&outputWrite);

    SIZE_T attributeListSize = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attributeList = NULL;
    if (ok) {
        InitializeProcThreadAttributeList(NULL, 1, 0, &attributeListSize);
        attributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attributeListSize);
        ok = attributeList && InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeListSize) &&
             UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       hpc, sizeof(hpc), NULL, NULL);
    }

    PROCESS_INFORMATION info = {0};
    if (ok) {
        process->hpc = hpc;
        process->window = window;
        process->message = message;
        process->session = session;
        STARTUPINFOEXW startup = {0};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributeList;
        /* Without this, a shell launched from a process that itself has a
           console (e.g. MarkEdit started from a command prompt rather than
           by double-clicking it) inherits that console's standard handles
           for its own stdout/stderr - CreateProcess copies them by default
           whenever STARTF_USESTDHANDLES is absent, regardless of
           bInheritHandles. The shell then writes some of its startup output
           (echoed commands, banners) there instead of through the
           pseudoconsole, invisibly to this control. Null standard handles
           make it fall back to the pseudoconsole for all of its I/O, the
           same as it would with no inherited console at all. */
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        /* Suspended until it is in the job, so nothing it starts escapes. */
        ok = CreateProcessW(NULL, command, NULL, NULL, FALSE,
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                            NULL, directory, &startup.StartupInfo, &info) != 0;
    }
    if (ok && !AssignProcessToJobObject(process->job, info.hProcess)) {
        TerminateProcess(info.hProcess, 1);
        ok = 0;
    }
    if (info.hThread) {
        if (ok) ResumeThread(info.hThread);
        CloseHandle(info.hThread);
    }
    if (attributeList) {
        DeleteProcThreadAttributeList(attributeList);
        free(attributeList);
    }
    free(command);

    if (ok) {
        process->process = info.hProcess;
        process->readerThread = (HANDLE)_beginthreadex(NULL, 0, read_output, process, 0, NULL);
        process->watcherThread = process->readerThread
            ? (HANDLE)_beginthreadex(NULL, 0, watch_process, process, 0, NULL) : NULL;
        ok = process->readerThread != NULL && process->watcherThread != NULL;
        if (!ok) TerminateJobObject(process->job, 1);
    } else if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    if (!ok) {
        if (hpc) s_closePseudoConsole(hpc);
        if (process) {
            close_handle(&process->inputWrite);
            /* Closing outputRead first unblocks a reader thread that managed
               to start even though its watcher sibling didn't (the only way
               to reach this path with one running) - an edge case rare
               enough that this isn't worth a cleaner shutdown than that. */
            close_handle(&process->outputRead);
            if (process->readerThread) {
                WaitForSingleObject(process->readerThread, TERMINAL_READER_WAIT_MS);
                close_handle(&process->readerThread);
            }
            close_handle(&process->job);
            free(process);
        }
        return NULL;
    }
    return process;
}

int terminal_process_write(TerminalProcess *process, const char *bytes, size_t length) {
    while (length > 0) {
        DWORD written = 0;
        if (!process->inputWrite || !WriteFile(process->inputWrite, bytes, (DWORD)length, &written, NULL))
            return 0;
        bytes += written;
        length -= written;
    }
    return 1;
}

void terminal_process_resize(TerminalProcess *process, int cols, int rows) {
    if (!process || !process->hpc || cols < 1 || rows < 1) return;
    COORD size = { (SHORT)cols, (SHORT)rows };
    s_resizePseudoConsole(process->hpc, size);
}

void terminal_process_stop(TerminalProcess *process) {
    if (process && process->job) TerminateJobObject(process->job, 1);
}

void terminal_process_free(TerminalProcess *process) {
    if (!process) return;
    terminal_process_stop(process); /* signals the watcher thread promptly */
    if (process->hpc) s_closePseudoConsole(process->hpc); /* ends the reader thread's ReadFile with EOF */
    close_handle(&process->inputWrite);
    if (WaitForSingleObject(process->readerThread, TERMINAL_READER_WAIT_MS) == WAIT_TIMEOUT) {
        CancelSynchronousIo(process->readerThread);
        WaitForSingleObject(process->readerThread, INFINITE);
    }
    close_handle(&process->readerThread);
    WaitForSingleObject(process->watcherThread, TERMINAL_READER_WAIT_MS);
    close_handle(&process->watcherThread);
    close_handle(&process->outputRead);
    close_handle(&process->process);
    close_handle(&process->job);
    free(process);
}
