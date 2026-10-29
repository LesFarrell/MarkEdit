#ifndef MARKEDIT_OUTPUT_TEXT_H
#define MARKEDIT_OUTPUT_TEXT_H

#include <stddef.h>

/* ---- what programs write to the output panel ----
   A decoder turns the bytes into styled UTF-8 text. ANSI colour sequences
   become styles and every other escape sequence is dropped. Bytes that
   aren't UTF-8 are read in the OEM code page, as a console would show them.
   The stream may be fed in pieces split anywhere. */

#define OUTPUT_STYLE_DEFAULT 0
/* OUTPUT_STYLE_COLOUR + 0..15: the ANSI colours black, red, green, yellow,
   blue, magenta, cyan and white, then their bright forms. */
#define OUTPUT_STYLE_COLOUR 1
#define OUTPUT_STYLE_COUNT 17

typedef struct {
    /* Text in one style; never empty. Every line break is "\n". */
    void (*text)(void *context, int style, const char *utf8, size_t length);
    /* A carriage return without a line feed: what follows replaces the line. */
    void (*carriage_return)(void *context);
} OutputSink;

typedef struct {
    unsigned char pending[4]; /* the start of a character split between feeds */
    int pendingLength;
    char escape[32];          /* the escape sequence being read */
    int escapeLength;         /* -1 once it is too long to keep */
    int escapeState;
    int colour;               /* 0..7, or -1 for the default */
    int bold;
    int carriageReturn;       /* a CR waiting to see whether LF follows */
} OutputDecoder;

void output_decoder_init(OutputDecoder *decoder);
void output_decoder_feed(OutputDecoder *decoder, const char *bytes, size_t length,
                         const OutputSink *sink, void *context);
/* Emits what feed held back, at the end of the stream. */
void output_decoder_finish(OutputDecoder *decoder, const OutputSink *sink, void *context);

/* ---- file locations in compiler and interpreter messages ----
   Finds the first "path:line[:column]" (GCC, Clang, Lua, PowerShell),
   "path(line[,column])" (MSVC) or 'File "path", line N' (Python) in a line
   of output. The path may begin with words that aren't part of it, as in
   "In file included from x.c:3"; the caller finds which suffix names a file
   (see output_location_next_candidate()). Returns 0 when there is none. */
typedef struct {
    size_t pathStart, pathLength; /* bytes of line */
    int line, column;             /* column is 0 when not given */
} OutputLocation;

int output_find_location(const char *line, size_t length, OutputLocation *location);
/* Finds the working-tree path and new-file line represented by a unified
   diff line. `position` may point at a context, added, removed or @@ line;
   the nearest preceding +++ header and @@ hunk are used. */
int output_find_diff_location(const char *text, size_t length, size_t position,
                              OutputLocation *location);
/* Drops the first word of a location's path, for when the whole of it
   isn't a file. Returns 0 when there's no shorter candidate. */
int output_location_next_candidate(const char *line, OutputLocation *location);

#endif
