#include "json_formatter.h"

#include <stdlib.h>
#include <string.h>

/* Validate before removing whitespace: otherwise malformed input such as
   [1 2] would silently become a different, valid value ([12]). */
typedef struct JsonReader {
    const char *text;
    int length;
    int position;
} JsonReader;

static void json_skip_space(JsonReader *reader) {
    while (reader->position < reader->length &&
           strchr(" \t\r\n", reader->text[reader->position]) &&
           reader->text[reader->position] != 0) reader->position++;
}

static int json_take(JsonReader *reader, char character) {
    if (reader->position >= reader->length ||
        reader->text[reader->position] != character) return 0;
    reader->position++;
    return 1;
}

static int json_read_string(JsonReader *reader) {
    if (!json_take(reader, '"')) return 0;
    while (reader->position < reader->length) {
        unsigned char c = (unsigned char)reader->text[reader->position++];
        if (c == '"') return 1;
        if (c < 0x20) return 0;
        if (c != '\\') continue;
        if (reader->position >= reader->length) return 0;
        char escape = reader->text[reader->position++];
        if (escape == 'u') {
            for (int i = 0; i < 4; i++) {
                if (reader->position >= reader->length) return 0;
                char digit = reader->text[reader->position++];
                if (!((digit >= '0' && digit <= '9') ||
                      (digit >= 'a' && digit <= 'f') ||
                      (digit >= 'A' && digit <= 'F'))) return 0;
            }
        } else if (!escape || !strchr("\"\\/bfnrt", escape)) return 0;
    }
    return 0;
}

static int json_read_digits(JsonReader *reader) {
    int start = reader->position;
    while (reader->position < reader->length &&
           reader->text[reader->position] >= '0' &&
           reader->text[reader->position] <= '9') reader->position++;
    return reader->position > start;
}

static int json_read_value(JsonReader *reader, int depth) {
    json_skip_space(reader);
    if (reader->position >= reader->length) return 0;
    char c = reader->text[reader->position];
    if (c == '"') return json_read_string(reader);
    if (c == '{' || c == '[') {
        if (depth >= 256) return 0;
        reader->position++;
        char closer = c == '{' ? '}' : ']';
        json_skip_space(reader);
        if (json_take(reader, closer)) return 1;
        for (;;) {
            if (c == '{') {
                json_skip_space(reader);
                if (!json_read_string(reader)) return 0;
                json_skip_space(reader);
                if (!json_take(reader, ':')) return 0;
            }
            if (!json_read_value(reader, depth + 1)) return 0;
            json_skip_space(reader);
            if (json_take(reader, closer)) return 1;
            if (!json_take(reader, ',')) return 0;
        }
    }
    const char *literal = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : NULL;
    if (literal) {
        int length = (int)strlen(literal);
        if (reader->length - reader->position < length ||
            memcmp(reader->text + reader->position, literal, (size_t)length)) return 0;
        reader->position += length;
        return 1;
    }
    json_take(reader, '-');
    if (!json_take(reader, '0') && !json_read_digits(reader)) return 0;
    if (json_take(reader, '.') && !json_read_digits(reader)) return 0;
    if (json_take(reader, 'e') || json_take(reader, 'E')) {
        if (!json_take(reader, '+')) json_take(reader, '-');
        if (!json_read_digits(reader)) return 0;
    }
    return 1;
}

typedef struct JsonPrinter {
    char *output;
    size_t length;
    size_t capacity;
    int failed;
} JsonPrinter;

static void json_emit(JsonPrinter *printer, const char *text, size_t length) {
    if (printer->failed) return;
    if (printer->length + length + 1 > printer->capacity) {
        size_t wanted = (printer->length + length + 1) * 2;
        char *grown = (char *)realloc(printer->output, wanted);
        if (!grown) {
            printer->failed = 1;
            return;
        }
        printer->output = grown;
        printer->capacity = wanted;
    }
    memcpy(printer->output + printer->length, text, length);
    printer->length += length;
    printer->output[printer->length] = 0;
}

static void json_emit_indent(JsonPrinter *printer, const char *eol, int depth) {
    json_emit(printer, eol, strlen(eol));
    for (int i = 0; i < depth; i++) json_emit(printer, "  ", 2);
}

static int json_copy_string(JsonPrinter *printer, const char *text, int index, int length) {
    int start = index++;
    while (index < length && text[index] != '"')
        index += (text[index] == '\\' && index + 1 < length) ? 2 : 1;
    if (index >= length) return -1;
    index++;
    json_emit(printer, text + start, (size_t)(index - start));
    return index;
}

char *json_reformat(const char *text, int length, const char *eol, int indent) {
    if (!text || length <= 0 || !eol) return NULL;
    JsonReader reader = {text, length, 0};
    if (!json_read_value(&reader, 0)) return NULL;
    json_skip_space(&reader);
    if (reader.position != length) return NULL;
    JsonPrinter printer = {NULL, 0, 0, 0};
    int depth = 0;
    int sawValue = 0;
    int closed = 0; /* the outermost container has closed - anything but trailing whitespace after this is invalid */
    /* One opener per nesting level, so a closer is checked against the kind
       of bracket that's actually open at this depth (a bare depth counter
       would otherwise accept "[1,2}" as balanced, since ']' and '}' both
       just decrement it). 256 levels is far past any realistic document. */
    char openers[256];

    for (int i = 0; i < length;) {
        char character = text[i];
        if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
            i++;
            continue;
        }
        if (closed) {
            free(printer.output);
            return NULL;
        }
        if (character == '"') {
            int next = json_copy_string(&printer, text, i, length);
            if (next < 0) {
                free(printer.output);
                return NULL;
            }
            i = next;
            sawValue = 1;
            continue;
        }

        switch (character) {
            case '{':
            case '[':
                if (depth >= (int)(sizeof(openers) / sizeof(openers[0]))) {
                    free(printer.output);
                    return NULL;
                }
                openers[depth] = character;
                json_emit(&printer, &character, 1);
                depth++;
                sawValue = 1;
                break;
            case '}':
            case ']':
                if (depth == 0 || openers[depth - 1] != (character == '}' ? '{' : '[')) {
                    free(printer.output);
                    return NULL;
                }
                depth--;
                if (depth == 0) closed = 1;
                if (printer.length && printer.output[printer.length - 1] != '{' &&
                    printer.output[printer.length - 1] != '[' && indent)
                    json_emit_indent(&printer, eol, depth);
                json_emit(&printer, &character, 1);
                break;
            case ':':
                json_emit(&printer, indent ? ": " : ":", indent ? 2 : 1);
                break;
            case ',': {
                /* A comma directly followed by a closing bracket (a trailing
                   comma) is invalid JSON - reject it here rather than
                   formatting it, which would otherwise emit a stray blank
                   line (this comma's own indent, then the closer's). */
                int next = i + 1;
                while (next < length && strchr(" \t\r\n", text[next])) next++;
                if (next >= length || text[next] == '}' || text[next] == ']') {
                    free(printer.output);
                    return NULL;
                }
                json_emit(&printer, ",", 1);
                if (indent) json_emit_indent(&printer, eol, depth);
                break;
            }
            default: {
                int start = i;
                while (i < length && !strchr(" \t\r\n,:{}[]\"", text[i])) i++;
                if (i == start) {
                    free(printer.output);
                    return NULL;
                }
                json_emit(&printer, text + start, (size_t)(i - start));
                sawValue = 1;
                continue;
            }
        }

        i++;
        if (indent && (character == '{' || character == '[')) {
            int next = i;
            while (next < length && strchr(" \t\r\n", text[next])) next++;
            if (next < length && text[next] != '}' && text[next] != ']')
                json_emit_indent(&printer, eol, depth);
        }
    }

    if (printer.failed || depth != 0 || !sawValue) {
        free(printer.output);
        return NULL;
    }
    return printer.output;
}
