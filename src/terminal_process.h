#ifndef MARKEDIT_TERMINAL_PROCESS_H
#define MARKEDIT_TERMINAL_PROCESS_H

#include <windows.h>

/* A shell run on a real Windows pseudoconsole (ConPTY), so full-screen
   programs (vim, less, PSReadLine's live completion) work the way they would
   in any other terminal - unlike output_process.c's plain pipes, which only
   suit a line-oriented log. It runs in a job that ends the shell and
   everything it started when it is stopped or MarkEdit exits, the same as
   output_process.c.

   A reader thread posts each piece of output - already UTF-8 VT/xterm text,
   ConPTY's own documented format - to the window as `message`, with a
   TerminalChunk in lParam that the window must free. The last chunk, once
   the shell has exited and all its output has been read, has `finished` set.
   Chunks may still arrive after terminal_process_free(), so `session` tells
   the window which run they belong to - the same contract as OutputChunk. */

typedef struct TerminalChunk {
    int session;
    int finished;
    DWORD exitCode; /* when finished */
    size_t length;
    char data[];
} TerminalChunk;

typedef struct TerminalProcess TerminalProcess;

/* Whether this Windows build exposes the ConPTY API at all (Windows 10
   1809+). Resolved with GetProcAddress rather than linked directly, so
   markedit.exe still loads on an older Windows - terminal_process_start()
   just returns NULL there instead of failing to launch at all. */
int terminal_process_available(void);

/* Runs commandLine in directory on a pseudoconsole sized cols x rows.
   Returns NULL when ConPTY isn't available or the shell can't be started. */
TerminalProcess *terminal_process_start(const WCHAR *commandLine, const WCHAR *directory,
                                        int cols, int rows, HWND window, UINT message,
                                        int session);
/* Queues bytes (keystrokes, already VT-encoded) for the shell. The pipe write
   happens on a worker so a busy console program cannot block the UI thread.
   Returns 0 once it has closed or when the input cannot be queued. */
int terminal_process_write(TerminalProcess *process, const char *bytes, size_t length);
/* Tells the pseudoconsole the terminal view resized; the shell sees this the
   way any console program learns its window changed size. */
void terminal_process_resize(TerminalProcess *process, int cols, int rows);
/* Ends the shell and everything it started. Its output still finishes with
   a `finished` chunk. */
void terminal_process_stop(TerminalProcess *process);
/* Stops the shell if it is running and releases everything. */
void terminal_process_free(TerminalProcess *process);

#endif
