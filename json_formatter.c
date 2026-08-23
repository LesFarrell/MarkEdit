#include "json_formatter.h"

#include <stdlib.h>
#include <string.h>

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
    JsonPrinter printer = {NULL, 0, 0, 0};
    int depth = 0;
    int containers = 0;
    int sawValue = 0;

    for (int i = 0; i < length;) {
        char character = text[i];
        if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
            i++;
            continue;
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
                json_emit(&printer, &character, 1);
                depth++;
                containers++;
                sawValue = 1;
                break;
            case '}':
            case ']':
                if (depth == 0) {
                    free(printer.output);
                    return NULL;
                }
                depth--;
                if (printer.length && printer.output[printer.length - 1] != '{' &&
                    printer.output[printer.length - 1] != '[' && indent)
                    json_emit_indent(&printer, eol, depth);
                json_emit(&printer, &character, 1);
                break;
            case ':':
                json_emit(&printer, indent ? ": " : ":", indent ? 2 : 1);
                break;
            case ',':
                json_emit(&printer, ",", 1);
                if (indent) json_emit_indent(&printer, eol, depth);
                break;
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

    if (printer.failed || depth != 0 || !sawValue || !containers) {
        free(printer.output);
        return NULL;
    }
    return printer.output;
}
