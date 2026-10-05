#include "workspace_text.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

static WCHAR *wide_from_utf8_n(const char *text, size_t length, int *wideLength) {
    if (length > INT_MAX) return NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, NULL, 0);
    if (count <= 0 && length) return NULL;
    WCHAR *wide = (WCHAR *)malloc(((size_t)count + 1) * sizeof(WCHAR));
    if (!wide) return NULL;
    if (count && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, wide, count)) {
        free(wide);
        return NULL;
    }
    wide[count] = 0;
    if (wideLength) *wideLength = count;
    return wide;
}

const char *utf8_case_find(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!needle[0]) return haystack;
    int asciiNeedle = 1;
    for (const unsigned char *scan = (const unsigned char *)needle; *scan; scan++)
        if (*scan >= 0x80) { asciiNeedle = 0; break; }
    if (asciiNeedle) {
        size_t needleBytes = strlen(needle);
        for (const unsigned char *candidate = (const unsigned char *)haystack; *candidate; candidate++) {
            size_t i = 0;
            while (i < needleBytes && candidate[i]) {
                unsigned char left = candidate[i], right = (unsigned char)needle[i];
                if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + ('a' - 'A'));
                if (right >= 'A' && right <= 'Z') right = (unsigned char)(right + ('a' - 'A'));
                if (left != right) break;
                i++;
            }
            if (i == needleBytes) return (const char *)candidate;
        }
        return NULL;
    }
    int hayLength = 0, needleLength = 0;
    WCHAR *wideHaystack = wide_from_utf8_n(haystack, strlen(haystack), &hayLength);
    WCHAR *wideNeedle = wide_from_utf8_n(needle, strlen(needle), &needleLength);
    if (!wideHaystack || !wideNeedle) {
        free(wideHaystack); free(wideNeedle);
        return NULL;
    }
    int foundLength = 0;
    int found = FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | NORM_IGNORECASE,
                                wideHaystack, hayLength, wideNeedle, needleLength,
                                &foundLength, NULL, NULL, 0);
    const char *result = NULL;
    if (found >= 0) {
        int byteOffset = WideCharToMultiByte(CP_UTF8, 0, wideHaystack, found,
                                             NULL, 0, NULL, NULL);
        if (byteOffset >= 0) result = haystack + byteOffset;
    }
    free(wideHaystack); free(wideNeedle);
    return result;
}

char *text_line_break(char *start, size_t *terminatorLength) {
    if (terminatorLength) *terminatorLength = 0;
    if (!start) return NULL;
    char *cursor = start;
    while (*cursor && *cursor != '\r' && *cursor != '\n') cursor++;
    if (!*cursor) return cursor;
    size_t length = (*cursor == '\r' && cursor[1] == '\n') ? 2 : 1;
    if (terminatorLength) *terminatorLength = length;
    return cursor;
}

static int utf8_case_equal_n(const char *left, size_t leftLength, const char *right) {
    int leftWideLength = 0, rightWideLength = 0;
    WCHAR *leftWide = wide_from_utf8_n(left, leftLength, &leftWideLength);
    WCHAR *rightWide = wide_from_utf8_n(right, strlen(right), &rightWideLength);
    int equal = leftWide && rightWide && leftWideLength == rightWideLength &&
        CompareStringOrdinal(leftWide, leftWideLength, rightWide, rightWideLength, TRUE) == CSTR_EQUAL;
    free(leftWide); free(rightWide);
    return equal;
}

static int append_bytes(char **output, size_t *length, size_t *capacity,
                        const char *bytes, size_t count) {
    if (count > SIZE_MAX - *length - 1) return 0;
    size_t wanted = *length + count + 1;
    if (wanted > *capacity) {
        size_t next = *capacity ? *capacity : 256;
        while (next < wanted) {
            if (next > SIZE_MAX / 2) { next = wanted; break; }
            next *= 2;
        }
        char *grown = (char *)realloc(*output, next);
        if (!grown) return 0;
        *output = grown;
        *capacity = next;
    }
    if (count) memcpy(*output + *length, bytes, count);
    *length += count;
    (*output)[*length] = 0;
    return 1;
}

static const char *destination_start(const char *text, const char *position) {
    if (position > text && position[-1] == ']' && position[0] == '(') {
        position++;
    } else {
        const char *line = position;
        while (line > text && line[-1] != '\r' && line[-1] != '\n') line--;
        const char *scan = line;
        int spaces = 0;
        while ((*scan == ' ' || *scan == '\t') && spaces < 4) { scan++; spaces++; }
        if (*scan != '[') return NULL;
        const char *close = strchr(scan + 1, ']');
        if (!close || close + 1 != position || *position != ':') return NULL;
        position++;
    }
    while (*position == ' ' || *position == '\t') position++;
    return position;
}

char *markdown_rewrite_link_targets(const char *text, const char *oldTarget,
                                    const char *newTarget, int *changeCount) {
    if (changeCount) *changeCount = 0;
    if (!text || !oldTarget || !oldTarget[0] || !newTarget) return NULL;
    char *output = NULL;
    size_t outputLength = 0, outputCapacity = 0;
    const char *copiedThrough = text;
    const char *cursor = text;
    int changes = 0;
    while (*cursor) {
        if (*cursor != '(' && *cursor != ':') { cursor++; continue; }
        const char *start = destination_start(text, cursor);
        if (!start) { cursor++; continue; }
        int angled = *start == '<';
        if (angled) start++;
        const char *end = start;
        while (*end && (angled ? *end != '>' : *end != ')' && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')) end++;
        const char *pathEnd = start;
        while (pathEnd < end && *pathEnd != '#' && *pathEnd != '?') pathEnd++;
        if (pathEnd > start && utf8_case_equal_n(start, (size_t)(pathEnd - start), oldTarget)) {
            if (!append_bytes(&output, &outputLength, &outputCapacity,
                              copiedThrough, (size_t)(start - copiedThrough)) ||
                !append_bytes(&output, &outputLength, &outputCapacity,
                              newTarget, strlen(newTarget))) {
                free(output);
                return NULL;
            }
            copiedThrough = pathEnd;
            changes++;
        }
        cursor = end > cursor ? end : cursor + 1;
    }
    if (!changes) { free(output); return NULL; }
    if (!append_bytes(&output, &outputLength, &outputCapacity,
                      copiedThrough, strlen(copiedThrough))) {
        free(output);
        return NULL;
    }
    if (changeCount) *changeCount = changes;
    return output;
}
