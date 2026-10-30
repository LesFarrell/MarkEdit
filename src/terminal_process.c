#include "terminal_process.h"

#include <process.h>
#include <stdlib.h>
#include <string.h>

/* How long freeing waits for the reader thread before cancelling its read;
   see output_process.c's OUTPUT_READER_WAIT_MS, which this mirrors. */
#define TERMINAL_READER_WAIT_MS 2000
#define TERMINAL_WRITER_WAIT_MS 2000
#define TERMINAL_INPUT_BATCH_SIZE 4096
/* Caps how much a single read loop batches before posting, so one very busy
   shell can't grow a chunk (and the allocation behind it) without bound. */
#define TERMINAL_BATCH_SIZE (64 * 1024)

/* CreatePseudoConsole/ResizePseudoConsole/ClosePseudoConsole are resolved
   with GetProcAddress rather than called directly, even though the headers
   declare them: ConPTY only exists on Windows 10 1809+, and a direct import
   would keep tinkeredit.exe from loading at all on an older Windows, rather
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
    HANDLE process, job, readerThread, writerThread, watcherThread;
    HANDLE inputWrite; /* our end; keystrokes go to the pseudoconsole here */
    HANDLE inputEvent;
    HANDLE outputRead; /* our end; the pseudoconsole's rendered output arrives here */
    CRITICAL_SECTION inputLock;
    char *inputBuffer;
    size_t inputOffset, inputLength, inputCapacity;
    int inputLockInitialized, inputStopping;
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
   long as the pseudoconsole lives.

   Each posted chunk costs the UI thread a parse and a repaint, and a message
   already posted queues behind nothing - not even a keystroke typed after it.
   A program that prints quickly (a prompt redrawing, a directory listing, a
   build's log) can otherwise have ReadFile come back with a few hundred bytes
   at a time, turning into a few hundred messages a second that each take
   their turn ahead of input typed in between. PeekNamedPipe lets this thread
   keep folding in whatever is already waiting, up to TERMINAL_BATCH_SIZE,
   before posting - so a burst of fast output becomes a handful of large
   chunks instead of a flood of small ones. */
static unsigned __stdcall read_output(void *argument) {
    TerminalProcess *process = (TerminalProcess *)argument;
    char fallback[4096];
    char *buffer = (char *)malloc(TERMINAL_BATCH_SIZE);
    size_t capacity = buffer ? TERMINAL_BATCH_SIZE : sizeof(fallback);
    if (!buffer) buffer = fallback;
    size_t used = 0;
    DWORD read = 0;
    while (ReadFile(process->outputRead, buffer + used, (DWORD)(capacity - used),
                    &read, NULL) && read > 0) {
        used += read;
        DWORD available = 0;
        while (used < capacity &&
               PeekNamedPipe(process->outputRead, NULL, 0, NULL, &available, NULL) && available > 0 &&
               ReadFile(process->outputRead, buffer + used, (DWORD)(capacity - used),
                       &read, NULL) && read > 0)
            used += read;
        post_chunk(process, buffer, used, 0, 0);
        used = 0;
    }
    if (buffer != fallback) free(buffer);
    return 0;
}

/* Pipe writes can block when the program in the pseudoconsole is busy and
   not reading input. Keep that wait off the UI thread: keyboard handling only
   appends to this queue, while one writer preserves byte order and combines
   bursts of small key messages into larger writes. */
static unsigned __stdcall write_input(void *argument) {
    TerminalProcess *process = (TerminalProcess *)argument;
    char batch[TERMINAL_INPUT_BATCH_SIZE];
    for (;;) {
        WaitForSingleObject(process->inputEvent, INFINITE);
        for (;;) {
            EnterCriticalSection(&process->inputLock);
            int stopping = process->inputStopping;
            size_t length = process->inputLength < sizeof(batch)
                ? process->inputLength : sizeof(batch);
            if (!stopping && length) {
                memcpy(batch, process->inputBuffer + process->inputOffset, length);
                process->inputOffset += length;
                process->inputLength -= length;
                if (!process->inputLength) process->inputOffset = 0;
            }
            LeaveCriticalSection(&process->inputLock);
            if (stopping) return 0;
            if (!length) break;

            size_t offset = 0;
            while (offset < length) {
                DWORD written = 0;
                if (!WriteFile(process->inputWrite, batch + offset,
                               (DWORD)(length - offset), &written, NULL) || !written) {
                    EnterCriticalSection(&process->inputLock);
                    process->inputStopping = 1;
                    LeaveCriticalSection(&process->inputLock);
                    return 0;
                }
                offset += written;
            }
        }
    }
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

static void free_input_writer(TerminalProcess *process) {
    if (!process) return;
    if (process->inputLockInitialized) {
        EnterCriticalSection(&process->inputLock);
        process->inputStopping = 1;
        LeaveCriticalSection(&process->inputLock);
    }
    if (process->inputEvent) SetEvent(process->inputEvent);
    if (process->writerThread &&
        WaitForSingleObject(process->writerThread, TERMINAL_WRITER_WAIT_MS) == WAIT_TIMEOUT) {
        CancelSynchronousIo(process->writerThread);
        WaitForSingleObject(process->writerThread, INFINITE);
    }
    close_handle(&process->writerThread);
    close_handle(&process->inputWrite);
    close_handle(&process->inputEvent);
    free(process->inputBuffer);
    process->inputBuffer = NULL;
    process->inputOffset = process->inputLength = process->inputCapacity = 0;
    if (process->inputLockInitialized) {
        DeleteCriticalSection(&process->inputLock);
        process->inputLockInitialized = 0;
    }
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
        int attributeListInitialized = attributeList &&
            InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeListSize);
        ok = attributeListInitialized &&
             UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       hpc, sizeof(hpc), NULL, NULL);
        if (!attributeListInitialized) {
            free(attributeList);
            attributeList = NULL;
        }
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
           console (e.g. Tinker Editor started from a command prompt rather than
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
        process->inputEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        process->inputLockInitialized = InitializeCriticalSectionAndSpinCount(&process->inputLock, 4000);
        process->readerThread = process->inputEvent && process->inputLockInitialized
            ? (HANDLE)_beginthreadex(NULL, 0, read_output, process, 0, NULL) : NULL;
        process->writerThread = process->readerThread
            ? (HANDLE)_beginthreadex(NULL, 0, write_input, process, 0, NULL) : NULL;
        process->watcherThread = process->writerThread
            ? (HANDLE)_beginthreadex(NULL, 0, watch_process, process, 0, NULL) : NULL;
        ok = process->readerThread != NULL && process->writerThread != NULL &&
             process->watcherThread != NULL;
        if (!ok) TerminateJobObject(process->job, 1);
    } else if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    if (!ok) {
        if (hpc) s_closePseudoConsole(hpc);
        if (process) {
            free_input_writer(process);
            /* Closing outputRead first unblocks a reader thread that managed
               to start even though its watcher sibling didn't (the only way
               to reach this path with one running) - an edge case rare
               enough that this isn't worth a cleaner shutdown than that. */
            close_handle(&process->outputRead);
            if (process->readerThread) {
                WaitForSingleObject(process->readerThread, TERMINAL_READER_WAIT_MS);
                close_handle(&process->readerThread);
            }
            close_handle(&process->process);
            close_handle(&process->job);
            free(process);
        }
        return NULL;
    }
    return process;
}

int terminal_process_write(TerminalProcess *process, const char *bytes, size_t length) {
    if (!process || !bytes) return 0;
    if (!length) return 1;
    EnterCriticalSection(&process->inputLock);
    if (process->inputStopping || !process->inputWrite ||
        length > SIZE_MAX - process->inputLength) {
        LeaveCriticalSection(&process->inputLock);
        return 0;
    }
    size_t required = process->inputLength + length;
    size_t tail = process->inputOffset + process->inputLength;
    if (length > process->inputCapacity - tail && process->inputOffset) {
        memmove(process->inputBuffer, process->inputBuffer + process->inputOffset,
                process->inputLength);
        process->inputOffset = 0;
    }
    if (required > process->inputCapacity) {
        size_t capacity = process->inputCapacity ? process->inputCapacity : 256;
        while (capacity < required && capacity <= SIZE_MAX / 2) capacity *= 2;
        if (capacity < required) capacity = required;
        char *grown = (char *)realloc(process->inputBuffer, capacity);
        if (!grown) {
            LeaveCriticalSection(&process->inputLock);
            return 0;
        }
        process->inputBuffer = grown;
        process->inputCapacity = capacity;
    }
    memcpy(process->inputBuffer + process->inputOffset + process->inputLength, bytes, length);
    process->inputLength += length;
    LeaveCriticalSection(&process->inputLock);
    return SetEvent(process->inputEvent) != 0;
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
    free_input_writer(process);
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
