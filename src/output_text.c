#include "output_text.h"

#include <windows.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---- decoding ---- */

enum { ESCAPE_NONE, ESCAPE_START, ESCAPE_CSI, ESCAPE_OSC, ESCAPE_OSC_END };

/* Text waiting to be passed on in one style. */
typedef struct {
    char text[1024];
    size_t length;
    int style;
    const OutputSink *sink;
    void *context;
} OutputRun;

static void flush_run(OutputRun *run) {
    if (run->length) run->sink->text(run->context, run->style, run->text, run->length);
    run->length = 0;
}

static void emit(OutputRun *run, int style, const char *text, size_t length) {
    if (style != run->style || run->length + length > sizeof(run->text)) {
        flush_run(run);
        run->style = style;
    }
    memcpy(run->text + run->length, text, length);
    run->length += length;
}

static int current_style(const OutputDecoder *decoder) {
    if (decoder->colour < 0) return OUTPUT_STYLE_DEFAULT;
    /* Like most terminals, bold makes the normal colours bright. */
    return OUTPUT_STYLE_COLOUR + decoder->colour + (decoder->bold ? 8 : 0);
}

/* Select Graphic Rendition: colours and bold. Backgrounds, 256-colour and
   true-colour values are read past, apart from the 16 basic 256-colour ones. */
static void apply_sgr(OutputDecoder *decoder, const char *parameters, int length) {
    int values[16], count = 0, value = 0, any = 0;
    for (int i = 0; i <= length; i++) {
        if (i == length || parameters[i] == ';' || parameters[i] == ':') {
            if (count < 16) values[count++] = any ? value : 0;
            value = 0;
            any = 0;
        } else if (parameters[i] >= '0' && parameters[i] <= '9') {
            value = value * 10 + (parameters[i] - '0');
            if (value > 1000) value = 1000;
            any = 1;
        }
    }
    for (int i = 0; i < count; i++) {
        int code = values[i];
        if (code == 0) { decoder->colour = -1; decoder->bold = 0; }
        else if (code == 1) decoder->bold = 1;
        else if (code == 22) decoder->bold = 0;
        else if (code >= 30 && code <= 37) decoder->colour = code - 30;
        else if (code == 39) decoder->colour = -1;
        else if (code >= 90 && code <= 97) { decoder->colour = code - 90; decoder->bold = 1; }
        else if ((code == 38 || code == 48) && i + 1 < count) {
            if (values[i + 1] == 5 && i + 2 < count) {
                if (code == 38 && values[i + 2] < 16) {
                    decoder->colour = values[i + 2] % 8;
                    decoder->bold = values[i + 2] >= 8;
                }
                i += 2;
            } else if (values[i + 1] == 2) {
                i += 4;
            }
        }
    }
}

/* One byte of an escape sequence. Returns 1 when the sequence has ended. */
static int read_escape(OutputDecoder *decoder, unsigned char byte) {
    switch (decoder->escapeState) {
        case ESCAPE_START:
            if (byte == '[') { decoder->escapeState = ESCAPE_CSI; return 0; }
            if (byte == ']') { decoder->escapeState = ESCAPE_OSC; return 0; }
            return 1; /* a two-byte sequence, such as ESC = */
        case ESCAPE_CSI:
            if (byte >= 0x40 && byte <= 0x7e) {
                if (byte == 'm' && decoder->escapeLength >= 0)
                    apply_sgr(decoder, decoder->escape, decoder->escapeLength);
                return 1;
            }
            /* A sequence too long to keep is read past without being applied. */
            if (decoder->escapeLength >= (int)sizeof(decoder->escape)) decoder->escapeLength = -1;
            else if (decoder->escapeLength >= 0) decoder->escape[decoder->escapeLength++] = (char)byte;
            return 0;
        case ESCAPE_OSC:
            /* A title or hyperlink, ended by BEL or ESC \. */
            if (byte == 7) return 1;
            if (byte == 0x1b) decoder->escapeState = ESCAPE_OSC_END;
            return 0;
        default: /* ESCAPE_OSC_END */
            return 1;
    }
}

static int utf8_sequence_length(unsigned char lead) {
    if (lead >= 0xc2 && lead <= 0xdf) return 2;
    if (lead >= 0xe0 && lead <= 0xef) return 3;
    if (lead >= 0xf0 && lead <= 0xf4) return 4;
    return 0;
}

static int plain_output_byte(unsigned char byte) {
    return byte == '\n' || byte == '\t' || (byte >= 0x20 && byte < 0x7f);
}

/* A byte that isn't part of a UTF-8 character, read in the OEM code page. */
static void emit_oem_byte(OutputRun *run, int style, unsigned char byte) {
    WCHAR wide;
    char utf8[4];
    if (MultiByteToWideChar(CP_OEMCP, 0, (const char *)&byte, 1, &wide, 1) != 1) return;
    int length = WideCharToMultiByte(CP_UTF8, 0, &wide, 1, utf8, sizeof(utf8), NULL, NULL);
    if (length > 0) emit(run, style, utf8, (size_t)length);
}

/* Emits pending as UTF-8 if it is a whole character, otherwise byte by
   byte in the OEM code page. */
static void emit_pending(OutputDecoder *decoder, OutputRun *run) {
    int style = current_style(decoder);
    int expected = utf8_sequence_length(decoder->pending[0]);
    if (expected && expected == decoder->pendingLength) {
        emit(run, style, (const char *)decoder->pending, (size_t)expected);
    } else {
        for (int i = 0; i < decoder->pendingLength; i++) emit_oem_byte(run, style, decoder->pending[i]);
    }
    decoder->pendingLength = 0;
}

void output_decoder_init(OutputDecoder *decoder) {
    memset(decoder, 0, sizeof(*decoder));
    decoder->colour = -1;
}

void output_decoder_feed(OutputDecoder *decoder, const char *bytes, size_t length,
                         const OutputSink *sink, void *context) {
    OutputRun run = { .style = OUTPUT_STYLE_DEFAULT, .sink = sink, .context = context };
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)bytes[i];
        if (decoder->escapeState != ESCAPE_NONE) {
            if (read_escape(decoder, byte)) decoder->escapeState = ESCAPE_NONE;
            continue;
        }
        if (decoder->pendingLength) {
            if (byte >= 0x80 && byte <= 0xbf) {
                decoder->pending[decoder->pendingLength++] = byte;
                if (decoder->pendingLength == utf8_sequence_length(decoder->pending[0]))
                    emit_pending(decoder, &run);
                continue;
            }
            emit_pending(decoder, &run); /* cut short: not UTF-8 */
        }
        if (decoder->carriageReturn) {
            decoder->carriageReturn = 0;
            if (byte != '\n') {
                flush_run(&run);
                sink->carriage_return(context);
            }
        }
        if (byte == '\r') {
            decoder->carriageReturn = 1;
        } else if (byte == 0x1b) {
            decoder->escapeState = ESCAPE_START;
            decoder->escapeLength = 0;
        } else if (plain_output_byte(byte)) {
            /* Console output is overwhelmingly plain ASCII. Copy a bounded
               run at once instead of calling emit() for every byte. */
            size_t count = 1;
            while (count < sizeof(run.text) && i + count < length &&
                   plain_output_byte((unsigned char)bytes[i + count]))
                count++;
            emit(&run, current_style(decoder), bytes + i, count);
            i += count - 1;
        } else if (byte >= 0x80) {
            if (utf8_sequence_length(byte)) {
                decoder->pending[0] = byte;
                decoder->pendingLength = 1;
            } else {
                emit_oem_byte(&run, current_style(decoder), byte);
            }
        }
        /* Other control characters (BEL, backspace, ...) are dropped. */
    }
    flush_run(&run);
}

void output_decoder_finish(OutputDecoder *decoder, const OutputSink *sink, void *context) {
    OutputRun run = { .style = OUTPUT_STYLE_DEFAULT, .sink = sink, .context = context };
    if (decoder->pendingLength) emit_pending(decoder, &run);
    flush_run(&run);
    decoder->carriageReturn = 0;
    decoder->escapeState = ESCAPE_NONE;
}

/* ---- file locations ---- */

/* The number at line[*at], advancing past it; -1 when there isn't one. */
static int read_number(const char *line, size_t length, size_t *at) {
    if (*at >= length || !isdigit((unsigned char)line[*at])) return -1;
    int value = 0;
    while (*at < length && isdigit((unsigned char)line[*at])) {
        if (value < 10000000) value = value * 10 + (line[*at] - '0');
        (*at)++;
    }
    return value;
}

/* Where a path ending at `end` starts: after an opening quote or bracket,
   or after a ": " such as the one in "lua: x.lua:3:". */
static size_t path_start(const char *line, size_t end) {
    size_t start = end;
    while (start > 0) {
        char before = line[start - 1];
        if (before == '"' || before == '\'' || before == '(' || before == '[' || before == '<') break;
        if (before == ' ' && start >= 2 && line[start - 2] == ':') break;
        start--;
    }
    while (start < end && (line[start] == ' ' || line[start] == '\t')) start++;
    return start;
}

static int python_location(const char *line, size_t length, OutputLocation *location) {
    const char *file = NULL;
    for (size_t i = 0; i + 6 <= length; i++)
        if (memcmp(line + i, "File \"", 6) == 0) { file = line + i + 6; break; }
    if (!file) return 0;
    const char *close = memchr(file, '"', length - (size_t)(file - line));
    if (!close || close == file) return 0;
    size_t at = (size_t)(close - line) + 1;
    if (at + 7 > length || memcmp(line + at, ", line ", 7) != 0) return 0;
    at += 7;
    int number = read_number(line, length, &at);
    if (number <= 0) return 0;
    *location = (OutputLocation){ (size_t)(file - line), (size_t)(close - file), number, 0 };
    return 1;
}

int output_find_location(const char *line, size_t length, OutputLocation *location) {
    if (python_location(line, length, location)) return 1;
    for (size_t i = 1; i + 1 < length; i++) {
        size_t at = i + 1;
        int number = -1, column = 0;
        if (line[i] == ':' && isdigit((unsigned char)line[at])) {
            number = read_number(line, length, &at);
            /* A drive letter's colon ("C:\") is followed by a slash, not a
               digit, so it never gets here. */
            if (at < length && line[at] == ':' && at + 1 < length && isdigit((unsigned char)line[at + 1])) {
                at++;
                column = read_number(line, length, &at);
            }
            if (at < length && line[at] != ':' && line[at] != ' ' && line[at] != ',' && line[at] != ')')
                number = -1;
        } else if (line[i] == '(' && isdigit((unsigned char)line[at])) {
            number = read_number(line, length, &at);
            if (at < length && line[at] == ',') {
                at++;
                column = read_number(line, length, &at);
                if (column < 0) column = 0;
            }
            if (at >= length || line[at] != ')') number = -1;
        }
        if (number <= 0) continue;
        size_t start = path_start(line, i);
        if (start == i) continue;
        *location = (OutputLocation){ start, i - start, number, column };
        return 1;
    }
    return 0;
}

static size_t line_start_before(const char *text, size_t position) {
    while (position > 0 && text[position - 1] != '\n') position--;
    return position;
}

static size_t next_line_start(const char *text, size_t length, size_t position) {
    const char *newline = (const char *)memchr(text + position, '\n', length - position);
    return newline ? (size_t)(newline - text) + 1 : length;
}

static int diff_hunk_new_line(const char *line, size_t length, int *newLine) {
    if (length < 7 || line[0] != '@' || line[1] != '@') return 0;
    const char *plus = (const char *)memchr(line + 2, '+', length - 2);
    if (!plus || plus + 1 >= line + length || !isdigit((unsigned char)plus[1])) return 0;
    size_t at = (size_t)(plus + 1 - line);
    int value = read_number(line, length, &at);
    if (value < 0) return 0;
    *newLine = value > 0 ? value : 1;
    return 1;
}

int output_find_diff_location(const char *text, size_t length, size_t position,
                              OutputLocation *location) {
    if (!text || !location || position > length) return 0;
    if (position == length && position) position--;
    size_t clicked = line_start_before(text, position);
    size_t clickedEnd = next_line_start(text, length, clicked);
    size_t clickedLength = clickedEnd - clicked;
    if (clickedLength && text[clicked + clickedLength - 1] == '\n') clickedLength--;
    int hunkHeader = clickedLength >= 2 && text[clicked] == '@' && text[clicked + 1] == '@';
    int hunkRow = clickedLength &&
                  (text[clicked] == ' ' || text[clicked] == '+' || text[clicked] == '-' ||
                   text[clicked] == '\\');
    if ((!hunkHeader && !hunkRow) ||
        (clickedLength >= 4 && (memcmp(text + clicked, "+++ ", 4) == 0 ||
                                memcmp(text + clicked, "--- ", 4) == 0))) return 0;
    size_t hunk = clicked;
    int targetLine = 0;
    for (;;) {
        size_t end = next_line_start(text, length, hunk);
        size_t lineLength = end - hunk;
        if (lineLength && text[hunk + lineLength - 1] == '\n') lineLength--;
        if (lineLength && text[hunk + lineLength - 1] == '\r') lineLength--;
        if (diff_hunk_new_line(text + hunk, lineLength, &targetLine)) break;
        if (lineLength >= 11 && memcmp(text + hunk, "diff --git ", 11) == 0) return 0;
        if (!hunk) return 0;
        hunk = line_start_before(text, hunk - 1);
    }

    /* Removed rows do not consume a line in the working-tree file. */
    for (size_t line = next_line_start(text, length, hunk); line < clicked;) {
        char prefix = text[line];
        if (prefix != '-' && prefix != '\\') targetLine++;
        line = next_line_start(text, length, line);
    }

    size_t header = hunk;
    for (;;) {
        if (header + 4 <= length && memcmp(text + header, "+++ ", 4) == 0) {
            size_t start = header + 4;
            size_t end = next_line_start(text, length, start);
            while (end > start && (text[end - 1] == '\n' || text[end - 1] == '\r')) end--;
            if (end - start >= 2 && text[start] == 'b' && text[start + 1] == '/') start += 2;
            if (end == start || (end - start == 9 && memcmp(text + start, "/dev/null", 9) == 0))
                return 0;
            *location = (OutputLocation){ start, end - start, targetLine, 0 };
            return 1;
        }
        if (header + 11 <= length && memcmp(text + header, "diff --git ", 11) == 0) return 0;
        if (!header) return 0;
        header = line_start_before(text, header - 1);
    }
}

int output_location_next_candidate(const char *line, OutputLocation *location) {
    for (size_t i = 0; i + 1 < location->pathLength; i++) {
        if (line[location->pathStart + i] == ' ') {
            size_t skip = i + 1;
            location->pathStart += skip;
            location->pathLength -= skip;
            return 1;
        }
    }
    return 0;
}

