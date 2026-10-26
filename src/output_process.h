#ifndef MARKEDIT_OUTPUT_PROCESS_H
#define MARKEDIT_OUTPUT_PROCESS_H

#include <windows.h>

/* A program run for the output panel, with its standard input, output and
   error on pipes (output and error share one, so they stay in order). It
   runs without a console window, in a job that ends the program and
   everything it starts when it is stopped or MarkEdit exits.

   A reader thread posts each piece of output to the window as `message`,
   with an OutputChunk in lParam that the window must free. The last chunk,
   once the program has exited and all its output has been read, has
   `finished` set. Chunks may still arrive after output_process_free(), so
   `session` tells the window which run they belong to. */

typedef struct OutputChunk {
    int session;
    int finished;
    DWORD exitCode;   /* when finished */
    size_t length;
    char data[];
} OutputChunk;

typedef struct OutputProcess OutputProcess;

/* Runs commandLine in directory. environment, when not NULL, is a block of
   "NAME=value" strings each ending in a NUL, with one more NUL at the end;
   they are added to MarkEdit's own environment, replacing any variable of
   the same name. Returns NULL when the program can't be started. */
OutputProcess *output_process_start(const WCHAR *commandLine, const WCHAR *directory,
                                    const WCHAR *environment, HWND window, UINT message,
                                    int session);
/* Sends bytes to the program's standard input. Returns 0 when it has closed. */
int output_process_write(OutputProcess *process, const char *bytes, size_t length);
/* Ends the program and everything it started. Its output still finishes
   with a `finished` chunk. */
void output_process_stop(OutputProcess *process);
/* Stops the program if it is running and releases everything. */
void output_process_free(OutputProcess *process);

#endif
